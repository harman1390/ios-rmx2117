/*
 * sweet boot shim entry: arm64 Linux Image header so Qualcomm ABL treats
 * us like a kernel. ABL enters at EL1, MMU/D-cache off, x0 = DTB pointer.
 */
	.section .text.head, "ax"
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0x80000			/* text_offset: stock MT kernel entry offset */
	.quad	_end			/* image_size (linked at 0) */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0			/* res2 */
	.quad	0			/* res3 */
	.quad	0			/* res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 (PE offset) */

	.text
	.globl _start
/* Entry trampoline at file offset 0x80000 — LK jumps to
 * kernel_addr + text_offset(0x80000), landing here. */
	.section .entry2, "ax"
	.globl _entry_trampoline
_entry_trampoline:
	b	_start

/* Debug mailbox lives in our own image (DRAM) only. The old SRAM mirrors
 * (0x11b000/0x11b500) proved fatal: the stores faulted before vectors
 * were armed, and BROM re-entry reuses that SRAM anyway. Post-mortem
 * channel is the UFS expdb block written by test_ufs(). */
	.text
_start:
	/* Kill the MTK hardware watchdog, then immediately re-arm a short
	 * one: if _start executes, the LK reset loop stops AND any hang
	 * before the next checkpoint rearms still resets observably. */
	mov	x12, #0x7000
	movk	x12, #0x1000, lsl #16	/* WDT_MODE @ 0x10007000 */
	mov	w13, wzr
	movk	w13, #0x2200, lsl #16	/* 0x22000000: KEY | mode 0 = disable */
	str	w13, [x12]
	/* Stage-1 arm: if we hang before shim_main rearms (~0x2000 units,
	 * tens of seconds), the device resets on its own. */
	mov	w13, #0x2000
	movk	w13, #8, lsl #16
	str	w13, [x12, #4]		/* LENGTH */
	mov	w13, #0x31
	movk	w13, #0x2200, lsl #16
	str	w13, [x12]		/* MODE = key|enable|irq|ext */
	mov	w13, #0x1971
	movk	w13, #0x9, lsl #16
	str	w13, [x12, #8]		/* RESTART */
	adr	x9, mbox
	mov	w10, #0x4853		/* 'SH' */
	movk	w10, #0x4d42, lsl #16	/* 'MB' -> "SHBM" */
	str	w10, [x9]		/* magic */
	str	x0, [x9, #32]		/* dtb from LK */
	mrs	x10, CurrentEL
	str	x10, [x9, #16]
	mrs	x10, sctlr_el1
	str	x10, [x9, #24]

	/* Earliest visual breadcrumb: the panel is DSI command-mode
	 * (nt35595_fhd_dsi_cmd), so paint LK's real fb (OVL layer0
	 * @ 0x77e20000, pitch 4352) and kick DSI_START to push a frame.
	 * A magenta band at the top = _start reached here. */
	mov	w15, #0xff00
	movk	w15, #0xff00, lsl #16	/* magenta */
	mov	w16, #0x8000		/* 128KB -> ~30 rows @4352, @4320 */
	mov	x14, #0
	movk	x14, #0x77e2, lsl #16	/* layer0 0x77e20000 */
1:	str	w15, [x14], #4
	subs	w16, w16, #1
	b.ne	1b
	mov	w16, #0x8000
	mov	x14, #0x6000
	movk	x14, #0x7881, lsl #16	/* layer1 0x78816000 (logo, on top) */
2:	str	w15, [x14], #4
	subs	w16, w16, #1
	b.ne	2b
	mov	x14, #0x3000
	movk	x14, #0x1401, lsl #16	/* dsi0 @ 0x14013000 */
	str	wzr, [x14, #0xc]	/* DSI_START = 0 */
	mov	w15, #1
	str	w15, [x14, #0xc]	/* DSI_START = 1: push one frame */

	msr	daifset, #0xf
	mov	x19, x0			/* DTB pointer from ABL */
	mov	w10, #1
	str	w10, [x9, #8]
	msr	spsel, #1
	adr	x1, _stack_top
	mov	sp, x1
	mov	w10, #2
	str	w10, [x9, #8]
	adr	x1, vectors
	msr	vbar_el1, x1
	mrs	x10, CurrentEL		/* re-read: x10 was clobbered above */
	lsr	x10, x10, #2		/* 1=EL1, 2=EL2 */
	cmp	x10, #2
	b.ne	4f
	/* Entered at EL2 (LK runs in hypervisor mode on this SoC): hook the
	 * EL2 vectors too, then drop to S-EL1h — the shim's ICC/timer paths
	 * and any XNU payload expect EL1 registers. */
	msr	vbar_el2, x1
	mov	x10, #(1 << 31)
	msr	hcr_el2, x10		/* RW: lower EL executes AArch64 */
	mov	x10, #3
	msr	cnthctl_el2, x10	/* EL1PCTEN|EL1PCEN: phys timer for EL1 */
	msr	cptr_el2, xzr		/* no FP/SIMD traps */
	msr	hstr_el2, xzr		/* no sysreg access traps */
	mov	x10, #0x3c5		/* SPSR: EL1h, DAIF masked */
	msr	spsr_el2, x10
	adr	x10, 5f
	msr	elr_el2, x10
	adr	x10, _stack_top
	msr	sp_el1, x10
	eret
4:	isb
5:	mov	w10, #3
	str	w10, [x9, #8]
	/* zero .bss */
	adr	x1, _bss_start
	adr	x2, _bss_end
1:	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b
2:	adr	x9, mbox
	mov	w10, #4
	str	w10, [x9, #8]
	mov	x0, x19
	bl	shim_main
3:	wfe
	b	3b

/* PSCI CPU_ON target. x0 = context id = cpu index (1..7). */
	.globl secondary_entry
secondary_entry:
	msr	daifset, #0xf
	msr	spsel, #1
	adr	x1, _sec_stacks
	add	x1, x1, x0, lsl #12
	add	x1, x1, #0x1000
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	mrs	x2, CurrentEL
	lsr	x2, x2, #2
	cmp	x2, #2
	b.ne	9f
	msr	vbar_el2, x1
9:	isb
	bl	secondary_main
4:	wfe
	b	4b

/*
 * Exception vectors. Each slot stashes x29/x30, loads its slot number
 * and jumps to the common path, which saves x0-x28 and calls
 * exc_handler(slot, regs). IRQs return; everything else ends in the
 * on-screen panic.
 */
.macro vent slot
	.balign	0x80
	stp	x29, x30, [sp, #-16]!
	mov	x29, #\slot
	b	exc_common
.endm

	.balign	0x800
	.globl vectors
vectors:
	vent 0
	vent 1
	vent 2
	vent 3
	vent 4
	vent 5
	vent 6
	vent 7
	vent 8
	vent 9
	vent 10
	vent 11
	vent 12
	vent 13
	vent 14
	vent 15

exc_common:
	sub	sp, sp, #(30 * 8)
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	str	x28, [sp, #224]
	adr	x9, mbox
	str	x29, [x9, #40]		/* exception slot before C handler */
	mov	x0, x29
	mov	x1, sp
	bl	exc_handler
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldr	x28, [sp, #224]
	add	sp, sp, #(30 * 8)
	ldp	x29, x30, [sp], #16
	eret

/*
 * Debug mailbox, read post-mortem via `mtk.py da peek <phys> 0xc0`.
 * [0]=magic 'SHBM'  [1]=step  [2]=CurrentEL  [3]=SCTLR_EL1  [4]=dtb
 * [5]=exc slot  [6]=ESR  [7]=ELR  [8]=FAR  [9]=SPSR  [10]=x0  [11]=x30
 */
	.data
	.balign	16
	.globl	mbox
mbox:
	.fill	24, 8, 0
