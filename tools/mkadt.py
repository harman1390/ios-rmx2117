#!/usr/bin/env python3
"""Build the minimal Apple Device Tree that the SWEET xnu needs to boot.

Only what the early boot path actually reads (see NOTES.md); every MMIO
address comes from the sweet live FDT. Usage:  mkadt.py OUT.adt [--dump] [--qemu] [--highmem]
"""
import hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

# Kernel-managed RAM. MT6853 DRAM starts at 0x40000000; the Narzo 30 Pro
# has ~6GB. LK puts the framebuffer at 0x7e605000 (k6853v1_64.dts atag);
# most MTK reserve-memory regions are dynamic alloc-ranges below
# 0xc0000000. 0x80000000+1GB keeps clear of LK/kernel/fb areas.
PHYS_BASE = 0x80000000
MEM_SIZE = 0x40000000
# --highmem (phone only): inside the upper RAM bank (the 6GB device has
# ~0x40000000..0x1_c0000000; staying in 0x1_00000000..0x1_80000000 = 2GB).
HIGH_PHYS_BASE = 0x100000000
HIGH_MEM_SIZE = 0x80000000
# /arm-io children are relative to this; it must not be 0 (serial_init bails)
SOC_BASE = 0x01000000

GICD, GICD_SIZE = 0x0c000000, 0x40000   # mt6853 GIC-600 (live dtb)
GICR, GICR_SIZE = 0x0c040000, 0x200000
# LK log (expdb): mblock 'framebuffer' + OVL layer0 = 0x77e20000,
# pitch 4352 eBGRA8888. On-device verified: shim console writes there.
FB, FB_SIZE = 0x77e20000, 0x27e0000
RMX2117_ECID = 0x0052303131370001       # 'R2117\0\0' + 1, synthetic
FB_W, FB_H, FB_STRIDE = 1080, 2400, 4352  # OVL pitch (1088px), eBGRA8888
# Internal UFS host (ufshci@11270000, mediatek,ufshci); QEMU virt has none
UFS, UFS_SIZE = 0x11270000, 0x2300
TIMEBASE = 13000000  # mt6853 arch_timer clock-frequency (13 MHz)
VTIMER_INTID = 27    # mt6853 arch_timer PPI 11 -> INTID 27
# QEMU virt (-M virt,gic-version=3 -cpu cortex-a76): GICD/GICR and the generic timer's CNTFRQ
QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE = 0x08000000, 0x080a0000, 62500000
QEMU_VTIMER_INTID = 27

BOOT_MANIFEST_HASH = hashlib.sha384(b'rmx2117 boot manifest').digest()

# The iOS userland is the iPhone X (iPhone10,3 / D22AP) IPSW. MobileGestalt
# derives product type, device class, home-button-less UI, artwork etc. from
# the root identity and /product of the real D22AP device tree. That tree is
# an Apple artifact (kept out of git): extract it from the IPSW with
#   tools/remotezip.py URL DeviceTree.d22ap.im4p DIR
#   ipsw img4 im4p extract DIR/Firmware/all_flash/DeviceTree.d22ap.im4p -o d22ap.adt
D22AP_ADT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../research/ipsw-x/d22ap.adt')


def d22ap_product(path=D22AP_ADT):
    """/product properties of the real D22AP tree, without the syscfg
    placeholders iBoot would fill in (flag bit set) and the node bookkeeping."""
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def build(seed=None, qemu=False, highmem=False):
    """qemu=True: same tree, but with QEMU virt's GIC addresses and CNTFRQ (tools/qemu-sweet.py)."""
    phys_base, mem_size = (HIGH_PHYS_BASE, HIGH_MEM_SIZE) if highmem else (PHYS_BASE, MEM_SIZE)
    gicd, gicr, timebase = (QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE) if qemu else (GICD, GICR, TIMEBASE)
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('firmware-version', cstr('sweet-shim')),
        ('system-firmware-version', cstr('sweet-shim')),
        # iBoot's SHA-384 of the boot manifest; /sbin/mount -P 1 exits unless
        # it exists (bind mounts /private/preboot/<hex>/usr/standalone/firmware,
        # tolerating a missing directory). Fixed stand-in value.
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        # libimg4 (cryptexd) aborts "failed to get crypto algorithm" without
        # it; real A11+ device trees say sha2-384.
        ('crypto-hash-method', cstr('sha2-384')),
        # MobileGestalt's screen class: display-scale (u32, default 0) comes
        # from /chosen, the pixel size from IOMobileFramebuffer. Scale 0 makes
        # SpringBoard's FBSDisplayMode "infxinf@0x" invalid. 1080x2400 @3x =
        # 360x800 points (like iPhone X class devices).
        ('display-scale', u32(3)),
        # XNU derives IOPlatformUUID (gethostuuid) from these on arm; without
        # them CFPreferences cannot use kCFPreferencesCurrentHost (no ByHost
        # files), so e.g. DataMigration never records LastSystemVersion and
        # migrates on every boot. Synthetic ECID (not the phone's); chip-id
        # of the A11 that the iPhone X userland expects.
        ('unique-chip-id', struct.pack('<Q', RMX2117_ECID)),
        ('chip-id', u32(0x8015)),
        # Security status as on a dev-fused, undemoted board (tuple 0,0,1,1):
        # MobileGestalt reads these from /chosen, and mobileactivationd's
        # DeviceType then takes its native "Hactivation" branch (state
        # Activated without an activation record; no SEP, no baseband here).
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        # Local boot policy (kern_csr.c): SIP relaxed like `csrutil disable`
        # (lp-sip0: untrusted kexts, unrestricted FS, task_for_pid, kernel
        # debugger, dtrace, nvram), lp-sip1: unauthenticated (unsealed) root.
        # The iOS shared cache is in no trust cache, so dyld can only map it
        # with CSR_ALLOW_UNRESTRICTED_FS (vm_unix.c shared region checks).
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # RMX2117 SMP: MT6853 (Dimensity 800U) 2+6: cores 0-5 Cortex-A55
    # @1.701GHz, 6-7 Cortex-A76 @2.171GHz. Live dtb: contiguous MPIDRs,
    # reg = core<<8; cluster0 = 0-5 (A55), cluster1 = 6-7 (A76).
    MPIDRS = [i << 8 for i in range(8)]
    MT6853_FREQS = [1701000000] * 6 + [2171000000] * 2
    CLUSTER = [0] * 6 + [1] * 2
    cpu_nodes = []
    for core in range(1 if qemu else 8):
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr('arm,cortex-a76' if MT6853_FREQS[core] > 2000000000 else 'arm,cortex-a55') + cstr('ARM,v8')),
            ('reg', u32(MPIDRS[core])),  # actual MPIDR, NOT logical ID
            ('cpu-id', u32(core)),   # descriptive: XNU assigns IDs by DT order
            ('cluster-id', u32(CLUSTER[core])),
            ('cluster-type', u32(0)),  # homogeneous CLUSTER_TYPE_SMP, not AMP
            ('die-cluster-id', u32(CLUSTER[core])),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(core)),  # distinct per-core L2 nodes in live FDT
            ('l3-cache-id', u32(0)),     # shared L3
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(MT6853_FREQS[core])),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(2133000000)),  # LPDDR4X
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)),
                         ('#size-cells', u32(0))], cpu_nodes)

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(QEMU_VTIMER_INTID if qemu else VTIMER_INTID)),
        ('reg', u64(gicd - SOC_BASE, GICD_SIZE, gicr - SOC_BASE, GICR_SIZE)),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('rmx2117-io')),
        ('compatible', cstr('rmx2117-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic,
        # No hardware: watchdogd checks IODeviceTree:/arm-io/wdt for no-pmu and
        # otherwise crashes trying to set the PMU boot stage (customer builds
        # ignore the -wdt_no_pmu boot-arg).
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        # Main display: IOMobileFramebuffer.framework counts /arm-io children
        # with this device_type and reads the canvas size from the node
        # (the presented iPhone X size, see SweetFramebuffer.cpp);
        # the kernel's AppleCLCD matches the compatible string.
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('rmx2117,display')),
            ('canvas-width', u32(1125)),
            ('canvas-height', u32(2436)),
        ])])

    # Name must stay 'sweet-fbcon': xnu patch 0003's fbcon_probe() does
    # SecureDTFindEntry("name", "sweet-fbcon").
    fbcon = node('sweet-fbcon', [
        ph(),
        ('reg', u64(FB, FB_SIZE)),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(FB_STRIDE)),
        # MT6853 cmd-mode panel: a frame only reaches the panel's GRAM
        # after dsi0 DSI_START (0x1401300C) 0->1. Kernel patch 0082
        # reads this phys address and kicks it after each fb update.
        ('kick-reg', u64(0x14013000)),
    ])

    # The UFS host LK leaves running (absolute physical reg). Name must
    # stay 'sweet-ufs': xnu patch 0028 does SecureDTLookupEntry("/sweet-ufs").
    ufs = node('sweet-ufs', [
        ph(),
        ('compatible', cstr('mediatek,ufshci')),
        ('reg', u64(UFS, UFS_SIZE)),
    ])

    # pmap_compute_max_asids requires the node even when no overrides are set.
    # An absent pmap-max-asids property selects XNU's own MAX_ASIDS default.
    # No effaceable storage: keybagd then reads /private/var/keybags/systembag.kb
    # as a plain plist instead of decrypting it with the effaceable key.
    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    # MobileGestalt HomeButtonType = IODeviceTree:/buttons home-button-type
    # (D22AP: 2, no Home button; absent = 0 = Home-button device).
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])

    product_props = d22ap_product()
    product = [node('product', [ph()] + product_props)] if product_props else []

    # Identity of the iPhone X whose userland runs here: compatible[0] is
    # hw.model/hw.target (D22AP), model is hw.machine/hw.product
    # (iPhone10,3). 'xiaomi,sweet' stays for SweetPlatformExpert matching.
    return node('device-tree', [
        ph(),
        ('compatible', cstr('D22AP') + cstr('iPhone10,3') + cstr('realme,RMX2117') + cstr('mediatek,mt6853') + cstr('AppleARM')),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons] + product + ([] if qemu else [ufs]))


if __name__ == '__main__':
    root = build(qemu='--qemu' in sys.argv, highmem='--highmem' in sys.argv)
    blob = adt.serialize(root)
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{sys.argv[1]}: {len(blob)} bytes', file=sys.stderr)
