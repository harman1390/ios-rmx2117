/*
 * SWEETXNU loader smoke test: a stub "kernel". Runs wherever the loader
 * copied it (phys 0x100004000, MMU off, x0 = boot_args PA). Paints the
 * whole screen GREEN and kicks DSI_START so the frame reaches the
 * command-mode panel, then parks. Green = chainload works.
 */
	.text
	.globl _start
_start:
	/* Fill fb 0x77e20000 with green, pitch 4352, 2400 rows. */
	mov	x1, #0
	movk	x1, #0x77e2, lsl #16
	mov	w2, #0xff00
	movk	w2, #0xff00, lsl #16	/* 0xff00ff00 green */
	mov	w3, #0x7800
	movk	w3, #0x28, lsl #16	/* 0x287800 words ~= 2400*1088 */
1:	str	w2, [x1], #4
	subs	w3, w3, #1
	b.ne	1b
	/* Second visible layer (logo) 0x78816000 pitch 4320. */
	mov	x1, #0x6000
	movk	x1, #0x7881, lsl #16
	mov	w3, #0x8000
	movk	w3, #0x28, lsl #16
2:	str	w2, [x1], #4
	subs	w3, w3, #1
	b.ne	2b
	/* dsi0 @ 0x14013000: DSI_START 0->1 pushes one frame. */
	mov	x1, #0x3000
	movk	x1, #0x1401, lsl #16
	str	wzr, [x1, #0xc]
	mov	w2, #1
	str	w2, [x1, #0xc]
3:	wfe
	b	3b
