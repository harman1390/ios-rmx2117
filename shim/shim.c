/*
 * Milestone 2: one boot answers the questions that decide the XNU port.
 *   - CPU feature registers (16K granule, GIC sysreg interface)
 *   - PSCI/SMCCC version and which PSCI calls the Qualcomm TZ offers
 *   - SMP: PSCI CPU_ON for the 7 secondaries, each reports its MPIDR
 *   - GICv3 Group 1 IRQ from the virtual timer (INTID 19 on Qualcomm)
 * Any unexpected exception paints a blue panic screen.
 */
#include <stdint.h>
#include "console.h"
#include "mmu.h"
#include "loader.h"
#include "ufs.h"
#include "fdt.h"

/* TODO(rmx2117): no Qualcomm IMEM on MTK. Reboot-reason/reporting lives in
 * the preloader SRAM/RTC block; PSCI SYSTEM_RESET is the reset path.
 * If a fastboot-boot menu return is needed later, find the MTK
 * toprgu/watchdog or ATF restart hook. */
#define IMEM_RESTART_REASON	0UL
#define RESTART_BOOTLOADER	0x77665500U
#define PSHOLD			0UL

#define GICD_BASE		0x0c000000UL	/* mt6853 GIC-600 distributor */
#define GICR_BASE		0x0c040000UL
#define GICR_SIZE		0x200000UL
#define GICR_PE_SIZE		0x20000UL
#define GICR_TYPER		0x08
#define GICR_WAKER		0x14
#define GICR_SGI		0x10000
#define GICR_IGROUPR0		(GICR_SGI + 0x080)
#define GICR_ISENABLER0		(GICR_SGI + 0x100)
#define GICR_ICENABLER0		(GICR_SGI + 0x180)
#define GICR_IPRIORITYR		(GICR_SGI + 0x400)
#define GICR_ISPENDR0		(GICR_SGI + 0x200)
#define GICR_ISACTIVER0		(GICR_SGI + 0x300)
#define GICR_ICFGR1		(GICR_SGI + 0xc04)
#define VTIMER_INTID		27	/* mt6853 arch_timer PPI 11 -> INTID 27 */
#define PTIMER_INTID		30	/* mt6853 arch_timer PPI 14 -> INTID 30 */
#define SGI_INTID		1

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_SUSPEND	0xc4000001UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_MIGRATE_INFO_TYPE	0x84000006UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_SUSPEND	0xc400000eUL
#define PSCI_SYSTEM_RESET2	0xc4000012UL
#define SMCCC_VERSION		0x80000000UL

#define NR_CPUS			8

/* Debug mailbox in start.S — DRAM copy only. The 0x11b000/0x11b500 SRAM
 * mirrors faulted at MSTEP and were clobbered by BROM anyway. */
extern volatile uint64_t mbox[24];
#define MB(n, v)	do { mbox[n] = (v); } while (0)
#define MSTEP(n)	MB(1, n)

static void log_flush(void);

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

/* Warm-reset-surviving progress marker: TOPRGU WDT_NONRST spare reg
 * (MTK's reboot-reason slot — the earlier 0x1001A0F0 target was infracfg,
 * not TOPRGU; same 0x10007000 block as the WDT whose mode write works).
 * Read post-mortem in BROM: `mtk.py peek 0x100070E0 4`.
 * Value = 0x53484D00 ('SHM') | code. */
#define MARKER_REG	0x100070E0UL
void mark(uint32_t code)
{
	*(volatile uint32_t *)MARKER_REG = 0x53484D00u | (code & 0xff);
}

extern char secondary_entry[];

static const char *stage;	/* set at runtime: no absolute pointers in data */
static volatile uint64_t sec_mpidr[NR_CPUS];
static volatile uint64_t sec_probe[4];	/* CPU1 redistributor write test */
static volatile uint32_t irq_count, irq_other, irq_last;
static volatile int irq_src;

/* ICC_SGI1R_EL1 value targeting the calling core only. */
static uint64_t sgi_self(unsigned id)
{
	uint64_t m = rsr("mpidr_el1");
	return ((m >> 16) & 0xff) << 32 |	/* Aff2 */
	       ((m >> 8) & 0xff) << 16 |	/* Aff1 */
	       (uint64_t)id << 24 |
	       (1U << (m & 0xf));		/* target list: Aff0 */
}

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	/* Fixed-iteration spin: cntpct can't be trusted at our EL — a dead
	 * timer must not turn the exception/reset path into a hang.
	 * ~5e5 iters ≈ 1 ms. */
	for (volatile uint64_t i = 0; i < (uint64_t)ms * 500000ULL; i++)
		;
}

static void __attribute__((noreturn)) reset(unsigned after_s)
{
	con_skip();
	con_str("RESET", after_s > 20 ? "in 40s" : "in 20s");
	delay_ms(after_s * 1000);
	if (IMEM_RESTART_REASON)
		wr32(IMEM_RESTART_REASON, RESTART_BOOTLOADER);
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	if (PSHOLD)
		wr32(PSHOLD, 0);
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- exceptions ---- */

static void irq_handler(void)
{
	uint64_t iar = rsr("icc_iar1_el1");
	uint32_t id = iar & 0xffffff;

	if (id >= 1020 && id <= 1023)
		return;	/* spurious */
	irq_last = id;
	if (id == (uint32_t)irq_src)
		irq_count++;
	else
		irq_other++;
	if (id == VTIMER_INTID)
		wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);	/* 10 ms */
	else if (id == PTIMER_INTID)
		wsr("cntp_tval_el0", rsr("cntfrq_el0") / 100);
	wsr("icc_eoir1_el1", iar);
	isb();
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	if (slot == 5) {	/* IRQ from current EL, SPx */
		irq_handler();
		return;
	}
	mark(0xE0 | (slot & 0x1f));	/* exception slot, survives reset */
	/* Mailbox dump before anything that could fault again. Pick the
	 * right EL's syndrome regs — we may have been entered at EL2. */
	int el2 = (rsr("CurrentEL") >> 2) == 2;
	MB(6, el2 ? rsr("esr_el2") : rsr("esr_el1"));
	MB(7, el2 ? rsr("elr_el2") : rsr("elr_el1"));
	MB(8, el2 ? rsr("far_el2") : rsr("far_el1"));
	MB(9, el2 ? rsr("spsr_el2") : rsr("spsr_el1"));
	MB(10, regs[0]);
	MB(11, regs[29]);
	if (ufs_ready())
		log_flush();
	con_clear(BLUE);
	switch (slot & 3) {
	case 0: con_str("PANIC", "SYNC"); break;
	case 1: con_str("PANIC", "IRQ"); break;
	case 2: con_str("PANIC", "FIQ"); break;
	default: con_str("PANIC", "SERROR"); break;
	}
	con_hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
	con_str("STAGE", stage);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("SPSR", rsr("spsr_el1"));
	con_hex("X0", regs[0]);
	con_hex("X1", regs[1]);
	con_hex("X30", regs[29]);	/* stashed lr is just above the frame */
	reset(40);
}

/* ---- SMP ---- */

static uintptr_t find_gicr(void);

void secondary_main(uint64_t idx)
{
	/* Minimal touch: report MPIDR and leave. The earlier build poked
	 * this core's redistributor from here and the box reset inside the
	 * settle window — isolate whether plain CPU_ON/OFF is clean first. */
	sec_mpidr[idx] = rsr("mpidr_el1") | (1ULL << 63);
	sec_probe[0] = 1;
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_CPU_OFF, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");	/* CPU_OFF failed -> stay put */
}

static void __attribute__((unused)) test_smp(void)
{
	stage = "smp";
	uint64_t self = rsr("mpidr_el1") & 0xffffff;

	/* mark(0x50|i) before each CPU_ON: the TOPRGU marker survives reset,
	 * so `mtk.py peek 0x100070E0` names the call that killed us. */
	for (int i = 1; i < NR_CPUS; i++) {
		char label[] = "CPU0";
		label[3] = '0' + i;
		if (((uint64_t)i << 8) == self) {
			con_str(label, "is boot cpu");
			continue;
		}
		mark(0x50 | i);
		uint64_t ret = smc(PSCI_CPU_ON, (uint64_t)i << 8, (uintptr_t)secondary_entry, i);
		con_hex2(label, "ret=", (uint32_t)ret, "mpidr=", 0);
	}
	mark(0x5f);
	delay_ms(500);		/* let secondaries run secondary_main */
	for (int i = 1; i < NR_CPUS; i++) {
		char label[] = "CPUn";
		label[3] = '0' + i;
		if (((uint64_t)i << 8) != self)
			con_hex2(label, "mpidr=", (uint32_t)sec_mpidr[i],
				 "seen=", sec_mpidr[i] ? 1 : 0);
	}
}

/* ---- GIC + timer ---- */

static uintptr_t find_gicr(void)
{
	uint64_t aff = rsr("mpidr_el1");
	uint32_t want = (aff & 0xffffff) | ((aff >> 8) & 0xff000000);

	for (uintptr_t off = 0; off < GICR_SIZE; off += GICR_PE_SIZE) {
		uint64_t typer = rd64(GICR_BASE + off + GICR_TYPER);
		if ((uint32_t)(typer >> 32) == want)
			return GICR_BASE + off;
		if (typer & (1 << 4))	/* Last */
			break;
	}
	return 0;
}

static void gicd_wait_rwp(void)
{
	for (int i = 0; i < 1000000 && (rd32(GICD_BASE) & (1U << 31)); i++)
		;
}

/* Run one interrupt source for ~300 ms and report what the GIC saw. */
static void irq_window(const char *label, uintptr_t r, int src)
{
	uint64_t f = rsr("cntfrq_el0");
	uint32_t pend = 0, act = 0, isr = 0, hppi = 0x3ff, ist = 0;

	irq_count = irq_other = 0;
	irq_last = 0x3ff;
	irq_src = src;
	if (src == VTIMER_INTID) {
		wsr("cntv_tval_el0", f / 100);
		wsr("cntv_ctl_el0", 1);
	} else if (src == PTIMER_INTID) {
		wsr("cntp_tval_el0", f / 100);
		wsr("cntp_ctl_el0", 1);
	}
	isb();
	/* sample state with IRQs still masked, after the first period */
	uint64_t t = now() + f / 50;
	while (now() < t)
		;
	if (src == SGI_INTID)
		wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
	isb();
	ist = src == PTIMER_INTID ? rsr("cntp_ctl_el0") : rsr("cntv_ctl_el0");
	pend = rd32(r + GICR_ISPENDR0);
	act = rd32(r + GICR_ISACTIVER0);
	isr = rsr("isr_el1");
	hppi = rsr("icc_hppir1_el1");

	__asm__ volatile("msr daifclr, #2" ::: "memory");
	t = now() + f * 3 / 10;
	/* SGI: one already sent above, send 9 more 10 ms apart -> expect 10 */
	uint64_t next = now() + f / 100;
	for (int sent = 1; now() < t;)
		if (src == SGI_INTID && sent < 10 && now() >= next) {
			wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
			isb();
			sent++;
			next += f / 100;
		}
	__asm__ volatile("msr daifset, #2" ::: "memory");
	wsr("cntv_ctl_el0", 0);
	wsr("cntp_ctl_el0", 0);
	isb();

	con_hex2(label, "irqs=", irq_count, "other=", irq_other);
	con_hex2(" pre", "pend=", pend, "act=", act);
	con_hex2(" pre", "isr=", isr, "hppi=", hppi);
	con_hex2(" pre", "tctl=", ist, "last=", irq_last);
}

static void test_gic(void)
{
	stage = "gic sre";
	uint64_t sre = rsr("icc_sre_el1");
	if (!(sre & 1)) {
		wsr("icc_sre_el1", sre | 1);
		isb();
	}
	if (!(rsr("icc_sre_el1") & 1)) {
		con_str("GIC", "no sysreg iface, skip");
		return;
	}

	stage = "gic dist";
	uint32_t dctlr = rd32(GICD_BASE);
	/* Non-secure view (DS=0): bit0 EnableGrp1, bit1 EnableGrp1A, bit4 ARE_NS; Linux writes 0x13 */
	wr32(GICD_BASE, dctlr | 0x13);
	gicd_wait_rwp();
	con_hex2("GICD", "was=", dctlr, "now=", rd32(GICD_BASE));

	stage = "gic redist";
	uintptr_t r = find_gicr();
	if (!r) {
		con_str("GICR", "not found");
		return;
	}
	uint32_t waker = rd32(r + GICR_WAKER);
	if (waker & 2) {
		wr32(r + GICR_WAKER, waker & ~2U);
		for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
			;
	}
	con_hex2("GICR", "base=", r, "waker=", rd32(r + GICR_WAKER));

	stage = "gic config";
	uint32_t grp = rd32(r + GICR_IGROUPR0);
	wr32(r + GICR_IGROUPR0, 0xffffffff);	/* NS view: secure bits are WI */
	uint32_t en_before = rd32(r + GICR_ISENABLER0);
	uint32_t pri_before = rd32(r + GICR_IPRIORITYR + 16);	/* INTIDs 16..19 */
	for (int id = 0; id < 32; id++)
		wr8(r + GICR_IPRIORITYR + id, 0xa0);
	wr32(r + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << PTIMER_INTID) | (1U << SGI_INTID));
	con_hex2("ENABLE", "was=", en_before, "now=", rd32(r + GICR_ISENABLER0));
	(void)grp;
	(void)pri_before;

	uint64_t ctlr = rsr("icc_ctlr_el1");
	wsr("icc_ctlr_el1", ctlr & ~2ULL);	/* EOImode 0: EOIR also deactivates */
	wsr("icc_pmr_el1", 0xf0);
	wsr("icc_bpr1_el1", 0);
	wsr("icc_igrpen1_el1", 1);
	isb();
	con_hex2("ICC", "ctlr=", ctlr, "grpen1=", rsr("icc_igrpen1_el1"));

	stage = "gic vtimer";
	irq_window("VTIMER", r, VTIMER_INTID);
	stage = "gic sgi";
	irq_window("SGI1", r, SGI_INTID);
	wr32(r + GICR_ICENABLER0, 0xffffffff);
	isb();
}

/* ---- main ---- */

static void psci_line(const char *label, uint64_t fn)
{
	con_hex2(label, "feat=", smc(PSCI_FEATURES, fn, 0, 0), 0, 0);
}

/* CPU feature dump: the answers the XNU port needs — EL, 16K/4K granule
 * support (ID_AA64MMFR0 TGran16/TGran4), PARange, PSCI/SMCCC versions,
 * and which PSCI calls TZ implements. */
static void test_cpu(void)
{
	stage = "cpu";
	con_hex2("CPU", "EL=", rsr("CurrentEL") >> 2, "midr=", (uint32_t)rsr("midr_el1"));
	con_hex("MMFR0", rsr("id_aa64mmfr0_el1"));
	con_hex2("PSCI", "ver=", smc(PSCI_VERSION, 0, 0, 0), "smcccv=", smc(SMCCC_VERSION, 0, 0, 0));
	psci_line("CPU_ON", PSCI_CPU_ON);
	psci_line("SYSRST", PSCI_SYSTEM_RESET);
	psci_line("CPU_OFF", PSCI_CPU_OFF);
}

static uint64_t scratch[8192];	/* 64KB, lives in our (later cacheable) image */

static uint64_t ticks_fill(uint32_t c)
{
	uint64_t t = now();
	con_clear(c);
	return now() - t;
}

static uint64_t ticks_mem(void)
{
	uint64_t t = now();
	for (int pass = 0; pass < 16; pass++)
		for (int i = 0; i < 8192; i++)
			scratch[i] = scratch[i] * 3 + i;
	return now() - t;
}

static uint32_t to_us(uint64_t ticks)
{
	return ticks * 10 / 130;	/* 13 MHz */
}

/* Persistent log: one 4K block flushed into the expdb partition — the
 * only reliable output on this command-mode DSI panel (no DRAM FB). */
static uint8_t logblk[4096] __attribute__((aligned(4096)));

static void log_flush(void)
{
	static const char magic[8] = "SHIMLOG\0";
	for (int i = 0; i < 8; i++)
		logblk[i] = magic[i];
	for (int i = 0; i < 24; i++) {
		uint64_t v = mbox[i];
		for (int b = 0; b < 8; b++)
			logblk[16 + i * 8 + b] = v >> (8 * b);
	}
	for (const char *s = stage ? stage : "?"; *s; s++)
		logblk[0xc8 + (s - stage)] = *s;
	ufs_log_write(logblk, 4096);
}

void shim_main(uint64_t dtb)
{
	struct mmu_info mi;
	(void)mi;

	/* Instrumented probe build: fill the mailbox, then hang stably so a
	 * preloader-mode `mtk.py peek` can read it back from live DRAM. */
	stage = "probe";
	MSTEP(5);
	wdt_set(0x4000);	/* stage-2 arm: hang here resets at mid delay */
	MB(12, dtb);
	uint64_t fb = 0, fbh = 0;
	if (dtb >= 0x40000000 && dtb < 0x80000000 &&
	    __builtin_bswap32(rd32(dtb)) == 0xd00dfeed) {
		/* LK may publish the splash framebuffer either as one 64-bit
		 * property or split into low/high u32s — accept both forms. */
		if ((fdt_chosen_u64(dtb, "atag,videolfb-fb_base", &fb) && fb) ||
		    (fdt_chosen_u64(dtb, "atag,videolfb-fb_base_l", &fb) &&
		     fdt_chosen_u64(dtb, "atag,videolfb-fb_base_h", &fbh) && fb))
			MB(13, 1);
		else
			MB(13, 0);
	} else {
		MB(13, 0xbad);
	}
	MB(14, fb);
	MB(15, fbh);
	con_fb = fb ? (volatile uint32_t *)(fb | (fbh << 32))
		    : (volatile uint32_t *)FB_BASE;	/* LK's logo fb is our best guess */
	MSTEP(6);
	mark(fb ? 0x02 : 0x03);		/* fb prop found / not found */
	con_clear(YELLOW);	/* yellow = shim_main alive, fb found */
	con_str("SHIM", "probe alive");
	mark(0x10);

	/* Production shape: chainload first. With a real SWEETXNU payload
	 * appended we jump straight into the kernel; the whole diagnostic
	 * suite below only runs when loader_boot returns an error. */
	stage = "load";
	const char *err = loader_boot(dtb);	/* only returns on error */
	MSTEP(7);

	/* Diagnostics path: no payload found — bring UFS up for the
	 * persistent mailbox, then run the milestone-2 hardware probes. */
	stage = "ufs";
	test_ufs();			/* returns with UFS logging live */

	con_clear(MAGENTA);	/* magenta = probe phase */
	test_cpu();
	test_gic();
	test_smp();

	con_str("LOAD", err ? err : "returned");

	stage = "hold";
	wdt_off();
	for (;;)
		__asm__ volatile("wfe");
}


