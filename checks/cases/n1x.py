from cases._common import boot_kernel, check_file, check_report_clean, entry, kernel_file
from harness.api import check
from harness.media import HostFile, Tree
from harness.ports import Drive


@check(ports=['uefi-aarch64'], tags=['n1x', 'arm64', 'memory'], timeout=240)
def n1x_spark_arm64_high_ram(t):
    """Simulate NVIDIA N1X Spark platform with high RAM:
    - AArch64 UEFI with 8GB RAM spanning across and well above the 4GiB boundary.
    - SMP topology: 4 cores across 2 clusters (clusters=2, smp='4,clusters=2').
    - GICv3 with ITS enabled, -cpu max.
    - Real SBSA watchdog hardware simulation (-device sbsa-gwdt), verified present and disabled by firmware.
    - Synchronous timer verification: verifies that mrs cntvct_el0 executes without generating
      synchronous EL1/EL2 traps (verifying CNTHCTL_EL2 hypervisor configuration) and monotonically advances.
    - Real W^X selftest: verifies data and UEFI claimed memory (EfiLoaderCode) are writable and executable without
      fault, whereas code memory (.text) is read-only and triggers a synchronous Data Abort (EC=0x25).
    - ACPI RSDP verification: table response present, readable, valid revision >= 2.
    - Verifies executable_file Limine file structure and GPT disk match via check_file.
    - Verifies memory map parsing with exact overlap calculation strictly above 4GiB."""
    machine_kwargs = {
        'cpu': 'max',
        'machine_opts': ['gic-version=3', 'its=on'],
        'extra': ['-device', 'sbsa-gwdt,id=watchdog0'],
    }
    report, m, disk = boot_kernel(t, 'full', memory='8G', smp='4,clusters=2',
                                  machine_kwargs=machine_kwargs, timeout=180)
    check_report_clean(t, report)

    # Verify CPUs started
    t.expect_eq(report.int('mp.cpu_count'), 4, 'CPU count')
    t.expect_eq(report.int('mp.aps_started'), 3, 'APs started')

    # Verify timer frequency was read via system registers (generic timer CNTFRQ_EL0)
    t.check(report.int('tsc_frequency') > 0, 'Generic timer frequency (tsc_frequency) is non-zero')
    t.check(report.int('aarch64.cntfrq') > 0, 'Generic timer frequency (aarch64.cntfrq) is non-zero')
    t.check(report.int('performance.init_usec') > 0, 'performance init timer is non-zero')

    # Verify CNTVCT trap count and monotonic advancement
    t.expect_eq(report.int('aarch64.cntvct_traps'), 0, 'CNTVCT synchronous trap count is 0')
    t.expect_eq(report.int('aarch64.cntvct_advances'), 1, 'CNTVCT advances monotonically')

    # Verify SBSA watchdog timer is present and disabled by firmware
    t.expect_eq(report.int('aarch64.sbsa_gwdt.present'), 1, 'SBSA watchdog present')
    t.expect_eq(report.int('aarch64.sbsa_gwdt.enabled'), 0, 'SBSA watchdog disabled')
    t.check('WARNING: Failed to disable watchdog timer!' not in m.stream('serial').text(),
            'No firmware watchdog disable failure warning in serial')

    # Verify ACPI RSDP
    t.expect_eq(report.int('rsdp.response'), 1, 'ACPI RSDP response present')
    t.expect_eq(report.int('rsdp.readable'), 1, 'ACPI RSDP table readable')
    t.check(report.int('rsdp.revision') >= 2, 'ACPI RSDP revision >= 2')

    # Verify W^X enforcement selftest (data and claimed usable memory writable, code read-only)
    t.expect_eq(report.get('aarch64.wx_selftest'), 'ok', 'W^X enforcement selftest passed')
    t.expect_eq(report.int('aarch64.wx.data_writable'), 1, 'Data memory is writable')
    t.expect_eq(report.int('aarch64.wx.usable_writable'), 1, 'Claimed UEFI usable memory (EfiLoaderCode) is writable')
    t.expect_eq(report.int('aarch64.wx.usable_executable'), 1, 'Claimed UEFI usable memory (EfiLoaderCode) is executable')
    t.expect_eq(report.int('aarch64.wx.high_ram_writable'), 1, 'Claimed high RAM above 4GiB is writable')
    t.expect_eq(report.int('aarch64.wx.code_ro'), 1, 'Code memory is read-only (Data Abort EC=0x25 triggered)')
    t.expect_eq(report.int('aarch64.wx.pt_enforced'), 1, 'Page table translation permissions match W^X')

    # Verify executable file Limine structure and GPT disk match
    with open(kernel_file(t, 'full'), 'rb') as f:
        kdata = f.read()
    check_file(t, report, 'executable_file', path='/boot/kernel.elf', data=kdata, string='hello world', disk=disk)

    # Verify memory map spans above 4GiB with exact overlap calculation
    memmap_count = report.int('memmap.count')
    has_above_4g = False
    usable_above_4g = 0
    for i in range(memmap_count):
        entry_str = report.get(f'memmap.{i}', '')
        parts = entry_str.split()
        if not t.check(len(parts) >= 3, f'memmap entry {i} format valid (expected >= 3 fields): {entry_str!r}'):
            continue
        try:
            base = int(parts[0], 16)
            length = int(parts[1], 16)
        except ValueError as err:
            t.check(False, f'memmap entry {i} has invalid hex numeric fields: {entry_str!r} ({err})')
            continue
        type_str = parts[2]
        top = base + length
        overlap = max(0, top - max(base, 1 << 32))
        if overlap > 0:
            has_above_4g = True
            if type_str == 'usable':
                usable_above_4g += overlap
    t.check(has_above_4g, 'Memory map has entries above 4GiB')
    t.check(usable_above_4g > 0, f'Has usable memory above 4GiB ({usable_above_4g} bytes)')


@check(ports=['uefi-aarch64'], tags=['n1x', 'arm64', 'nvme'], timeout=240)
def n1x_spark_arm64_nvme(t):
    """Simulate NVIDIA N1X Spark platform booting from NVMe storage:
    - Verifies UEFI boot from NVMe controller on AArch64.
    - Uses memory='4G': standard enterprise RAM size distinct from the 8GB high-RAM test,
      sufficient for mass storage and EDK2 NVMe driver binding without artificial allocation pressure.
    - GICv3 with ITS enabled, -cpu max, 4 SMP cores across 2 clusters.
    - Verifies multi-core SMP and kernel execution.
    - Verifies kernel report confirms boot medium is NVMe (PCI vendor/device ID and class code).
    - Verifies firmware BDS serial log shows boot from NVMe device path.
    - Verifies QEMU QMP query-pci contains NVMe controller (PCI class 0x0108 / 264).
    - Verifies executable_file Limine file structure and NVMe GPT disk match via check_file."""
    tree = Tree()
    with open(kernel_file(t, 'full'), 'rb') as f:
        kdata = f.read()
    tree.add('/boot/kernel.elf', HostFile(kernel_file(t, 'full')))
    config = 'timeout: 0\n' + entry('Test', path='boot():/boot/kernel.elf', cmdline='x')
    t.limine_files(config, tree)
    disk = t.boot_disk(tree)

    # Boot using NVMe drive with modern ARM64 enterprise machine configuration
    m = t.boot(drives=[Drive(disk.path, kind='nvme', bootindex=1)], memory='4G', smp='4,clusters=2',
               cpu='max', machine_opts=['gic-version=3', 'its=on'])
    report = t.kernel_report(m, timeout=180)
    check_report_clean(t, report)
    t.expect_eq(report.int('mp.cpu_count'), 4, 'CPU count')
    t.expect_eq(report.int('mp.aps_started'), 3, 'APs started')

    # Verify boot medium came from NVMe:
    # 1. Kernel report confirms boot device is NVMe and detects PCI controller
    t.expect_eq(report.get('boot.device'), 'nvme', 'Kernel report boot.device is nvme')
    t.expect_eq(report.get('boot_device'), 'nvme', 'Kernel report boot_device is nvme')
    t.expect_eq(report.int('aarch64.pci.nvme.present'), 1, 'Kernel detected NVMe PCI controller')
    t.expect_eq(report.hex('aarch64.pci.nvme.vendor_id'), 0x1b36, 'NVMe PCI vendor ID is 0x1b36 (QEMU/Red Hat)')
    t.expect_eq(report.hex('aarch64.pci.nvme.device_id'), 0x0010, 'NVMe PCI device ID is 0x0010')
    t.expect_eq(report.hex('aarch64.pci.nvme.class'), 0x010802, 'NVMe PCI class code (0x010802 - Mass Storage / NVM)')

    # 2. Firmware BDS serial log shows NVMe device path
    serial_text = m.stream('serial').text()
    t.check(any('NVMe(' in line for line in serial_text.splitlines() if 'BdsDxe' in line),
            'Firmware BDS serial log records boot from NVMe device path')

    # 3. QMP PCI bus query verifies presence of NVMe controller (Class 0x0108 / 264)
    pci_buses = m.qmp.cmd('query-pci')
    has_nvme_pci = False
    for bus in pci_buses:
        for dev in bus.get('devices', []):
            if dev.get('class_info', {}).get('class') == 264:  # PCI Class 0x0108 (NVMe)
                has_nvme_pci = True
                break
    t.check(has_nvme_pci, 'PCI bus contains NVMe controller (class 0x0108)')

    # 4. Limine file protocol verifies executable loaded from NVMe GPT disk
    check_file(t, report, 'executable_file', path='/boot/kernel.elf', data=kdata, string='x', disk=disk)


arm64_high_ram_boot = n1x_spark_arm64_high_ram
arm64_nvme_boot = n1x_spark_arm64_nvme
