/*
 * Milestone 5 probe: can we talk to the internal UFS that ABL booted from?
 *
 * Runs with the MMU off (all memory Device, so DMA is coherent with no cache
 * maintenance). Dumps the UFSHCI registers ABL left behind and, if the host
	 * is enabled and the link is up, points the transfer request list at our own
	 * buffers and probes the LUs with SCSI. The only write goes to the first
	 * block of the "opporeserve6" GPT partition (OEM-reserved, unused) as a
	 * post-mortem mailbox; if it cannot be found, nothing is written at all.
 *
 * Register layout and descriptor formats: JEDEC UFSHCI (JESD223).
 */
#include <stdint.h>
#include "console.h"
#include "ufs.h"

#define UFS_BASE	0x11270000UL	/* ufshci@11270000, MT6853.dts */
/* NOTE: MT6853 wraps UFSHCI in MTK glue regs; the JEDEC host should be at
 * this base but may need MTK-specific power/clock enable first.
 * Stock userdata is FBE-encrypted; probe does raw SCSI on LUN 0 only. */

#define REG_CAP		0x00
#define REG_VER		0x08
#define REG_IS		0x20
#define REG_HCS		0x30
#define REG_HCE		0x34
#define REG_UTRLBA	0x50
#define REG_UTRLBAU	0x54
#define REG_UTRLDBR	0x58
#define REG_UTRLCLR	0x5c
#define REG_UTRLRSR	0x60
#define REG_UICCMD	0x38
#define REG_UICCMDARG1	0x3c
#define REG_UICCMDARG2	0x40
#define REG_UICCMDARG3	0x44
#define REG_UICCMDR	0x48
#define UIC_DME_LINKSTARTUP	0x16

#define HCS_DP		(1u << 0)
#define HCS_UTRLRDY	(1u << 1)
#define HCS_UCRDY	(1u << 3)

#define BLOCK		4096u

static inline uint32_t ufs_rd(uint32_t off) { return *(volatile uint32_t *)(UFS_BASE + off); }
static inline void ufs_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(UFS_BASE + off) = v; }

/* UTP transfer request descriptor (32 bytes). */
struct utrd {
	uint32_t dw0;		/* [31:28] CT, [26:25] DD, [24] I */
	uint32_t dw1;
	uint32_t dw2;		/* [7:0] OCS */
	uint32_t dw3;
	uint32_t ucdba;		/* command descriptor base, 128-byte aligned */
	uint32_t ucdbau;
	uint32_t resp;		/* [31:16] response UPIU offset, [15:0] length (DWORDs) */
	uint32_t prdt;		/* [31:16] PRDT offset, [15:0] entries (DWORDs/entries) */
};

/* UTP command descriptor: command UPIU, response UPIU, PRDT. */
struct ucd {
	uint8_t cmd[512];
	uint8_t resp[512];
	struct {
		uint32_t addr_lo, addr_hi, rsvd, dbc;	/* dbc = byte count - 1 */
	} prdt[1];
};

/* Host DMA only reaches SRAM in this context — the preloader's own
 * list sits at 0x103800 and DRAM-descriptor TURs never complete.
 * Fixed SRAM window (struct ucd is 1040 B -> give it 0x600):
 * UTRD 0x110000, UCD 0x110400..0x1109ff, data 0x110a00..0x1129ff,
 * GPT entries 0x112a00..0x1169ff (16K). */
#define SRAM_UTRL	((struct utrd *)0x110000UL)
#define SRAM_UCD	((struct ucd *)0x110400UL)
#define SRAM_DATA	((uint8_t *)0x110a00UL)
#define SRAM_ENTS	((uint8_t *)0x112a00UL)
static struct utrd *const utrl = SRAM_UTRL;
static struct ucd *const ucd = SRAM_UCD;
static uint8_t *const data = SRAM_DATA;

int strcmp(const char *a, const char *b)	/* freestanding: no libc */
{
	while (*a && *a == *b)
		a++, b++;
	return *a - *b;
}

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

/* Dead-timer-proof delay: fixed iteration count. cntvct can't be trusted
 * at our EL — if it doesn't tick, every ticks()-based wait hangs
 * forever. ~5e8 iters ≈ 1s at typical A76/A55 clock. */
static void spin_s(unsigned secs)
{
	for (volatile uint64_t i = 0; i < (uint64_t)secs * 500000000ULL; i++)
		;
}

static void zero(void *p, unsigned n)
{
	volatile uint8_t *b = p;
	for (unsigned i = 0; i < n; i++)
		b[i] = 0;
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int g_ufs_ready;
static uint64_t g_log_lba;	/* first block of expdb, once located */

/* expdb = GPT sector 84488 (0x14A08) in 4K blocks, confirmed by DA dump.
 * Stage breadcrumbs go to blocks 0x14A08+n; each is self-contained
 * (magic + mailbox snapshot) so the last block present shows the
 * deepest stage reached before any hang.
 * NOTE: expdb itself is aee-owned — these crumbs now target the
 * GPT-located opporeserve6 LBA (g_log_lba) instead. */
#define EXPDB_LBA	g_log_lba

extern volatile uint64_t mbox[24];	/* debug mailbox from start.S */

/*
 * Issue one SCSI command in slot 0. wr=1 is WRITE(10) direction
 * (host->device, DD=01); otherwise read/no-data as before.
 * Returns 0 on success, otherwise a code describing the failure stage.
 */
static uint32_t scsi(uint8_t lun, const uint8_t *cdb, unsigned cdb_len, void *buf, uint32_t len,
    int wr, uint32_t *ocs_out, uint32_t *status_out)
{
	static uint8_t tag;
	zero(&utrl[0], sizeof(utrl[0]));
	zero(ucd, sizeof(*ucd));

	/* Command UPIU: header (12 bytes), expected length, CDB. */
	ucd->cmd[0] = 0x01;			/* transaction type: COMMAND */
	ucd->cmd[1] = wr ? 0x20 : (len ? 0x40 : 0x00);	/* W / R / no-data */
	ucd->cmd[2] = lun;
	ucd->cmd[3] = ++tag;
	ucd->cmd[12] = len >> 24;
	ucd->cmd[13] = len >> 16;
	ucd->cmd[14] = len >> 8;
	ucd->cmd[15] = len;
	for (unsigned i = 0; i < cdb_len; i++)
		ucd->cmd[16 + i] = cdb[i];

	uintptr_t ucd_pa = (uintptr_t)ucd;
	utrl[0].dw0 = (1u << 28) | ((len ? (wr ? 1u : 2u) : 0u) << 25) | (1u << 24);	/* UFS storage */
	utrl[0].dw2 = 0x0f;			/* OCS: invalid until the host updates it */
	utrl[0].ucdba = (uint32_t)ucd_pa;
	utrl[0].ucdbau = (uint32_t)(ucd_pa >> 32);
	utrl[0].resp = (512u / 4) << 16 | (512u / 4);
	if (len) {
		uintptr_t buf_pa = (uintptr_t)buf;
		ucd->prdt[0].addr_lo = (uint32_t)buf_pa;
		ucd->prdt[0].addr_hi = (uint32_t)(buf_pa >> 32);
		ucd->prdt[0].dbc = len - 1;
		utrl[0].prdt = (1024u / 4) << 16 | 1;
	}
	__asm__ volatile("dsb sy" ::: "memory");

	ufs_wr(REG_UTRLDBR, 1);
	for (uint64_t i = 0; ufs_rd(REG_UTRLDBR) & 1; i++) {
		if (i > 30000000) {		/* ~100ms-ish of polling */
			ufs_wr(REG_UTRLCLR, ~1u);	/* clear slot 0 (write 0 to the bit) */
			return 1;			/* timeout */
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	*ocs_out = utrl[0].dw2 & 0xff;
	*status_out = ucd->resp[7];		/* SCSI status in the response UPIU */
	if (*ocs_out)
		return 2;
	if (ucd->resp[0] != 0x21)		/* RESPONSE UPIU */
		return 3;
	return *status_out ? 4 : 0;
}

static int g_lun;	/* LU that owns the GPT; 0 until found */

static uint32_t read_blocks(uint32_t lba, uint32_t count, void *buf)
{
	uint8_t cdb[10] = { 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, count >> 8, count, 0 };
	uint32_t ocs, st;
	return scsi(g_lun, cdb, sizeof(cdb), buf, count * BLOCK, 0, &ocs, &st);
}

static uint32_t write_blocks(uint32_t lba, uint32_t count, const void *buf)
{
	uint8_t cdb[10] = { 0x2a, 0x08 /* FUA */, lba >> 24, lba >> 16, lba >> 8, lba, 0, count >> 8, count, 0 };
	uint32_t ocs, st;
	return scsi(g_lun, cdb, sizeof(cdb), (void *)buf, count * BLOCK, 1, &ocs, &st);
}

/* SYNCHRONIZE CACHE(10): force the LU's volatile write cache to flash.
 * Without this a reset can silently drop a WRITE(10) that returned
 * GOOD status — likely why the first beacon never reached expdb. */
static uint32_t sync_cache(void)
{
	uint8_t cdb[10] = { 0x35 };
	uint32_t ocs, st;
	return scsi(g_lun, cdb, sizeof(cdb), 0, 0, 0, &ocs, &st);
}

/* One 4K block of log/debug data into expdb — survives reset, read back
 * via `mtk.py r expdb`. Only called once the host is confirmed ours. */
uint32_t ufs_log_write(const void *buf, uint32_t len)
{
	if (!g_ufs_ready || !g_log_lba || len > BLOCK)
		return 1;
	return write_blocks(g_log_lba, 1, buf);
}

int ufs_ready(void)
{
	return g_ufs_ready;
}

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

/* Name the filesystem whose superblock starts at block lba (f2fs/ext4). */
static const char *fs_type(uint32_t lba)
{
	if (read_blocks(lba, 1, data))
		return "read error";
	const uint8_t *sb = data + 1024;
	if (sb[0] == 0x10 && sb[1] == 0x20 && sb[2] == 0xf5 && sb[3] == 0xf2)
		return "f2fs";
	if (sb[56] == 0x53 && sb[57] == 0xef)
		return "ext4";
	return "other";
}

/* One line per GPT entry of at least 64 MiB: name, first LBA, size in MiB
 * (LBAs count 4K blocks). Returns the first LBA of "expdb" — MTK's
 * crash-dump partition — or 0 if it isn't in the table. */
static uint64_t list_partitions(uint32_t entry_lba, uint32_t nent)
{
	uint8_t *const ents = SRAM_ENTS;
	uint64_t expdb = 0;
	uint32_t blocks = (nent * 128 + BLOCK - 1) / BLOCK;
	if (read_blocks(entry_lba, blocks, ents)) {
		con_str("UFS PART", "read error");
		return 0;
	}
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = ents + i * 128;
		uint64_t first = le64(e + 32), last = le64(e + 40);
		if (!first)
			continue;
		char name[17];
		for (int c = 0; c < 16; c++) {
			uint8_t ch = e[56 + 2 * c];
			name[c] = (ch >= 32 && ch < 127) ? ch : (ch ? '?' : 0);
		}
		name[16] = 0;
		/* expdb belongs to MTK aee (its crash index overwrote our beacon
		 * after a WDT reset). opporeserve6 is OEM-reserved and unused
		 * -> our persistent mailbox lives there instead. */
		if (!__builtin_strcmp(name, "opporeserve6"))
			expdb = first;
		if (((last - first + 1) >> 8) < 64)	/* keep the screen to the big ones */
			continue;
		con_hex2(name, "lba=", (uint32_t)first, "MiB=", (uint32_t)((last - first + 1) >> 8));
		if (!__builtin_strcmp(name, "userdata") || !__builtin_strcmp(name, "cust") ||
		    !__builtin_strcmp(name, "cache"))
			con_str(" fs", fs_type((uint32_t)first));
	}
	return expdb;
}

static void stage(uint32_t n)
{
	if (!g_log_lba)
		return;		/* mailbox not located yet — never write blind */
	volatile uint8_t *dst = data;
	const char m[8] = {'S','H','S','T','G','0'+n,0,0};
	for (int i = 0; i < 8; i++)
		dst[i] = m[i];
	for (int i = 0; i < 24; i++) {
		uint64_t v = mbox[i];
		for (int b = 0; b < 8; b++)
			dst[16 + i*8 + b] = (uint8_t)(v >> (8 * b));
	}
	write_blocks(EXPDB_LBA + n, 1, data);
	sync_cache();
}

static void hang(void)
{
	for (;;)
		__asm__ volatile("wfe");
}

/* MT6853 TOPRGU watchdog: LK's log proves it writes mode 0x22000030 to
 * 0x10007000 before handoff, and our mode-0 write at _start stopped the
 * reset loop. Re-arming gives a catch-all reset for the one failure
 * nothing else can observe: a stalled AXI read on a clock-gated host.
 * LENGTH low bits are in ~1ms-ish units (key 0x8); cap at max so even a
 * wrong unit guess stays bounded. */
#define WDT_MODE	0x10007000UL
#define WDT_LENGTH	0x10007004UL
#define WDT_RESTART	0x10007008UL

void wdt_set(unsigned unit16)
{
	*(volatile uint32_t *)WDT_LENGTH = 0x00080000 | unit16;
	*(volatile uint32_t *)WDT_MODE = 0x22000031;	/* key | LK's bits | enable */
	*(volatile uint32_t *)WDT_RESTART = 0x00091971;	/* kick: start counting */
}

void wdt_off(void)
{
	*(volatile uint32_t *)WDT_MODE = 0x22000000;
}

/* Timed-reset death: pause `secs` then PSCI SYSTEM_RESET so the reset
 * cadence identifies the bail site (8s=enc, 16s=takeover, 40s=exc,
 * ~60s=WDT stall, never=success park). */
static void die_reset(unsigned secs)
{
	spin_s(secs);
	__asm__ volatile("mov w0, #0x0009\n\tmovk w0, #0x8400, lsl #16\n\tsmc #0" ::: "x0");
	hang();		/* SMC reset unsupported -> WDT still fires */
}

void test_ufs(void)
{
	con_clear(CYAN);	/* cyan = reached test_ufs */
	con_str("UFS", "milestone 5 probe (read-only)");
	for (int i = 0; i < 0x5a00 / 4; i++)
		((volatile uint32_t *)0x110000UL)[i] = 0;	/* scrub SRAM window */
	wdt_set(0x8000);		/* catch-all: stalls reset in ~tens of sec */
	mark(0x40);			/* entered test_ufs, wdt armed */
	uint32_t cap = ufs_rd(REG_CAP), hcs = ufs_rd(REG_HCS), hce = ufs_rd(REG_HCE);
	mark(0x41);			/* first ufs_rd returned -> regs accessible */
	if (!(hce & 1)) {            /* LK may have powered the host down */
		ufs_wr(REG_HCE, 1);
		for (int i = 0; i < 50000 && !(ufs_rd(REG_HCE) & 1); i++)
			;
		hce = ufs_rd(REG_HCE);
		hcs = ufs_rd(REG_HCS);
	}
	if (!(hce & 1)) {		/* host dead -> ~8s reset cadence */
		mark(0x21);
		die_reset(8);
	}
	/* After re-enable the UniPro link is down: issue DME_LINKSTARTUP and
	 * wait for device-present + transfer-list-ready before going on. */
	if (!(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
		ufs_wr(REG_UICCMDARG1, 0);
		ufs_wr(REG_UICCMDARG2, 0);
		ufs_wr(REG_UICCMDARG3, 0);
		ufs_wr(REG_UICCMD, UIC_DME_LINKSTARTUP);
		for (int i = 0; i < 2000000 && (ufs_rd(REG_UICCMD) & 1); i++)
			;
		for (uint64_t i = 0; i < 500000000ULL; i++) {	/* ~1s */
			hcs = ufs_rd(REG_HCS);
			if ((hcs & (HCS_DP | HCS_UTRLRDY)) == (HCS_DP | HCS_UTRLRDY))
				break;
		}
	}
	con_hex2("UFS", "ver=", ufs_rd(REG_VER), "cap=", cap);
	con_hex2("UFS", "hce=", hce, "hcs=", hcs);
	con_hex2("UFS", "is=", ufs_rd(REG_IS), "rsr=", ufs_rd(REG_UTRLRSR));
	con_hex2("UFS", "utrlba=", ufs_rd(REG_UTRLBA), "hi=", ufs_rd(REG_UTRLBAU));
	con_hex2("UFS", "dbr=", ufs_rd(REG_UTRLDBR), "nutrs=", (cap & 0x1f) + 1);

	uint32_t enc = 0;
	if (!(hce & 1))            enc = 1;
	else if (!(hcs & HCS_DP))  enc = 2;
	else if (!(hcs & HCS_UTRLRDY)) enc = 3;
	else if (ufs_rd(REG_UTRLDBR)) enc = 4;
	if (enc == 4) {		/* stale doorbells from LK: stop + write-1-clear */
		ufs_wr(REG_UTRLRSR, 0);
		ufs_wr(REG_UTRLCLR, 0xffffffff);
		for (int i = 0; i < 100000 && ufs_rd(REG_UTRLDBR); i++)
			;
		enc = ufs_rd(REG_UTRLDBR) ? 6 : 0;
	}
	if (enc) {			/* bail: reset after ~8s (distinctive cadence) */
		mark(0x20 | enc);
		die_reset(8);
	}

	/* Take over the transfer request list: stop, point at ours, run. */
	ufs_wr(REG_UTRLRSR, 0);
	uintptr_t list = (uintptr_t)utrl;
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	con_hex2("UFS", "ours=", (uint32_t)list, "rsr=", ufs_rd(REG_UTRLRSR));
	/*
	 * Never ring a doorbell unless the host really uses our list: otherwise
	 * it would execute whatever ABL left in its own slot 0 (maybe a write).
	 */
	if (ufs_rd(REG_UTRLBA) != (uint32_t)list || ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1)) {
		mark(0x25);
		die_reset(16);		/* takeover rejected -> ~16s cadence */
	}

	g_ufs_ready = 1;	/* our list is live; ufs_log_write() may proceed */
	mark(0x42);			/* takeover verified */

	/* Clear UNIT ATTENTION first — the UFS device reports UA after any
	 * reset/power event (preloader + DA touched it before us) and rejects
	 * commands with CHECK CONDITION until a TUR cycle drains it. */
	uint32_t ocs = 0, st = 0, rc;
	static const uint8_t tur[6] = { 0x00 };
	for (int i = 0; i < 5; i++) {
		rc = scsi(0, tur, sizeof(tur), 0, 0, 0, &ocs, &st);
		con_hex2("UFS TUR", "rc=", rc, "ocs/st=", ocs << 8 | st);
		if (!rc)
			break;
	}
	mark(0x43);			/* TUR loop done */
	stage(1);				/* breadcrumb: takeover + UA cleared + write path */
	mark(0x44);			/* stage(1) write_blocks returned */

	/* Find the LU that owns the GPT: LUN0 turned out to be a 4 MiB
	 * boot LU returning erased data, so scan LUNs 0-3 by READ CAPACITY
	 * and take the first one big enough to be the user flash. */
	static const uint8_t cap10[10] = { 0x25 };	/* READ CAPACITY(10) */
	rc = 4;
	for (int lun = 0; lun < 4; lun++) {
		uint32_t lrc = scsi(lun, tur, sizeof(tur), 0, 0, 0, &ocs, &st);
		rc = scsi(lun, cap10, sizeof(cap10), data, 8, 0, &ocs, &st);
		con_hex2("UFS CAP", "lun=", lun << 24 | rc, "ocs/st=", ocs << 8 | st);
		if (rc)
			continue;
		con_hex2("UFS LUN", "id=", lun, "lastlba=", be32(data));
		if (be32(data) > 0x10000 && be32(data + 4) == BLOCK) {
			g_lun = lun;
			break;
		}
	}
	stage(2);				/* breadcrumb: LU scan done */
	mark(0x45);

	/* READ(10) LBA 0, 2 blocks on the chosen LU: protective MBR and the
	 * GPT header. The partition table decides where the mailbox block
	 * may be written — "expdb" is MTK's crash-dump partition and safe
	 * to overwrite. No expdb found -> no UFS write at all. */
	static const uint8_t rd10[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 2, 0 };
	rc = g_lun || 1 ? scsi(g_lun, rd10, sizeof(rd10), data, 2 * BLOCK, 0, &ocs, &st) : 4;
	con_hex2("UFS READ", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (!rc) {
		con_hex2("UFS MBR", "sig=", data[510] | data[511] << 8, "type=", data[450]);
		const uint8_t *gpt = data + BLOCK;
		char sig[9];
		for (int i = 0; i < 8; i++)
			sig[i] = (gpt[i] >= 32 && gpt[i] < 127) ? gpt[i] : '.';
		sig[8] = 0;
		con_str("UFS GPT", sig);
		uint32_t nent = gpt[80] | gpt[81] << 8 | gpt[82] << 16 | gpt[83] << 24;
		uint32_t esz = gpt[84] | gpt[85] << 8 | gpt[86] << 16 | gpt[87] << 24;
		uint32_t elba = gpt[72] | gpt[73] << 8 | gpt[74] << 16 | gpt[75] << 24;
		con_hex2("UFS GPT", "entries=", nent, "lba=", elba);
		if (esz == 128 && nent <= 128)
			g_log_lba = list_partitions(elba, nent);
	}
	con_hex2("UFS LOG", "mailbox=", (uint32_t)g_log_lba, "found=", g_log_lba != 0);
	stage(3);				/* breadcrumb: GPT parsed, expdb located (or not) */
	mark(0x46);

	if (g_log_lba) {
		/* Beacon: dump the mailbox into expdb block 0. Then read it
		 * back — verify the device actually committed the write. */
		uint32_t wrc;
		{
			volatile uint8_t *dst = data;
			const char m[8] = {'S','H','I','M','U','F','S',0};
			for (int i = 0; i < 8; i++)
				dst[i] = m[i];
			for (int i = 0; i < 24; i++) {
				uint64_t v = mbox[i];
				for (int b = 0; b < 8; b++)
					dst[16 + i * 8 + b] = (uint8_t)(v >> (8 * b));
			}
			ufs_wr(REG_UTRLDBR, 0);	/* in case */
			wrc = write_blocks((uint32_t)g_log_lba, 1, data);
			if (!wrc)
				wrc = sync_cache();
			con_hex2("UFS BEACON", "rc=", wrc, "lba=", (uint32_t)g_log_lba);
			stage(4);		/* breadcrumb: GPT-located beacon write done */
		}
		{
			volatile uint8_t *dst = data;
			for (int i = 0; i < 16; i++)
				dst[i] = 0;
			uint32_t rrc = read_blocks((uint32_t)g_log_lba, 1, data);
			int ok = !wrc && !rrc && data[0] == 'S' && data[1] == 'H';
			con_hex2("UFS VERIFY", "rc=", rrc, "ok=", ok);
			uint64_t dsec = ok ? 0 : (wrc ? 20 : 10);
			if (dsec)
				spin_s(dsec);
		}
	}

	/* Done: disarm the watchdog — a hang now freezes the last screen
	 * instead of erasing the evidence with a reset. */
	wdt_off();
	mark(0x3f);			/* full pass: all SCSI ops completed */
	return;
}
