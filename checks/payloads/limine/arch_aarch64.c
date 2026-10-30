#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>
#include "common.h"
#include "state.h"
#include "arch.h"

#define READ_SYSREG(r) ({ uint64_t v; asm volatile ("mrs %0, " #r : "=r"(v)); v; })
#define WRITE_SYSREG(r, v) asm volatile ("msr " #r ", %0" :: "r"((uint64_t)(v)) : "memory")

// QEMU virt's PL011.
#define UART_BASE 0x09000000
static volatile uint32_t *uart;

static uint64_t tables[4][512] __attribute__((aligned(4096)));

static int current_el(void) {
    return (READ_SYSREG(CurrentEL) >> 2) & 3;
}

static inline void set_exception_sp(uint64_t val) {
    asm volatile (
        "msr SPSel, #1\n\t"
        "mov sp, %0\n\t"
        "msr SPSel, #0"
        :: "r" (val)
        : "memory"
    );
}

static uint64_t at_phys(uint64_t va) {
    uint64_t par;
    if (current_el() == 2) {
        asm volatile ("at s1e2r, %1\n\tisb\n\tmrs %0, par_el1" : "=r"(par) : "r"(va) : "memory");
    } else {
        asm volatile ("at s1e1r, %1\n\tisb\n\tmrs %0, par_el1" : "=r"(par) : "r"(va) : "memory");
    }
    if (par & 1) {
        return 0;
    }
    return (par & 0x0000fffffffff000) | (va & 0xfff);
}

// TTBR0 is the executable's to use from base revision 1: map the UART there,
// as Device-nGnRnE through MAIR attribute 2, which the protocol leaves unused.
void arch_init_output(void) {
    uint64_t tcr = READ_SYSREG(tcr_el1);
    unsigned t0sz = tcr & 0x3f;
    unsigned va_bits = 64 - t0sz;
    // 4KiB granule: each level resolves 9 bits above the 12 of the page.
    int start = va_bits > 48 ? -1 : va_bits > 39 ? 0 : va_bits > 30 ? 1 : 2;
    uint64_t va = UART_BASE;
    int t = 0;
    for (int level = start; level < 2; level++) {
        int shift = 12 + 9 * (3 - level);
        uint64_t idx = (va >> shift) & (level == -1 ? 0xf : 511);
        tables[t][idx] = at_phys((uint64_t)(uintptr_t)tables[t + 1]) | 3;
        if (level == 1) {
            // Map 1GiB block at 0x40_0000_0000 covering highmem PCIe ECAM (0x4010000000)
            tables[t][256] = (uint64_t)0x4000000000ULL | (2 << 2) | (1 << 10)
                            | ((uint64_t)3 << 53) | 1;
        }
        t++;
    }
    // A 2MiB block at level 2: AttrIndx 2, AF, PXN, UXN.
    tables[t][(va >> 21) & 511] = (va & ~(uint64_t)0x1fffff) | (2 << 2) | (1 << 10)
                                 | ((uint64_t)3 << 53) | 1;
    // Map SBSA GWDT at 0x0f000000 (control frame at 0x0f001000)
    uint64_t gwdt_va = 0x0f000000;
    tables[t][(gwdt_va >> 21) & 511] = (gwdt_va & ~(uint64_t)0x1fffff) | (2 << 2) | (1 << 10)
                                     | ((uint64_t)3 << 53) | 1;
    // Map low PCIe ECAM at 0x3f000000 - 0x3fffffff (16MiB = 8 x 2MiB blocks)
    for (int i = 504; i < 512; i++) {
        uint64_t pa = (uint64_t)i << 21;
        tables[t][i] = pa | (2 << 2) | (1 << 10) | ((uint64_t)3 << 53) | 1;
    }
    uint64_t mair = READ_SYSREG(mair_el1);
    mair &= ~((uint64_t)0xff << 16);
    WRITE_SYSREG(mair_el1, mair);
    uint64_t ttbr0 = at_phys((uint64_t)(uintptr_t)tables[0]);
    asm volatile ("dsb ishst" ::: "memory");
    WRITE_SYSREG(ttbr0_el1, ttbr0);
    asm volatile ("isb\n\ttlbi vmalle1\n\tdsb ish\n\tisb" ::: "memory");
    uart = (volatile uint32_t *)UART_BASE;

    extern char exception_vector_table[];
    extern char bsp_exception_stack_top[];
    set_exception_sp((uint64_t)(uintptr_t)bsp_exception_stack_top);
    if (current_el() == 2) {
        WRITE_SYSREG(vbar_el2, (uint64_t)(uintptr_t)exception_vector_table);
    } else {
        WRITE_SYSREG(vbar_el1, (uint64_t)(uintptr_t)exception_vector_table);
    }
    asm volatile ("isb" ::: "memory");
}

void arch_out_char(char c) {
    if (uart == NULL) {
        return;
    }
    while (uart[0x18 / 4] & (1 << 5)) {
    }
    uart[0] = (uint8_t)c;
}

const char *arch_name(void) {
    return "aarch64";
}

void arch_cpu_relax(void) {
    asm volatile ("yield");
}

uint64_t arch_counter(void) {
    asm volatile ("isb" ::: "memory");
    return READ_SYSREG(cntvct_el0);
}

uint64_t arch_counter_hw_freq(void) {
    return READ_SYSREG(cntfrq_el0) & 0xffffffff;
}

bool arch_has_nx(void) {
    return true;
}

int arch_cache_normal(void) {
    return 0;
}

int arch_cache_fb(void) {
    return 1;
}

uint64_t arch_paging_mode(void) {
    uint64_t tcr = READ_SYSREG(tcr_el1);
    return ((tcr >> 16) & 0x3f) < 16 ? 1 : 0;
}

bool arch_walk(uint64_t va, struct pt_walk *w) {
    if (!have_hhdm) {
        return false;
    }
    uint64_t tcr = READ_SYSREG(tcr_el1);
    bool upper = va >> 63;
    unsigned tsz = upper ? (tcr >> 16) & 0x3f : tcr & 0x3f;
    uint64_t ttbr = upper ? READ_SYSREG(ttbr1_el1) : READ_SYSREG(ttbr0_el1);
    unsigned va_bits = 64 - tsz;
    int start = va_bits > 48 ? -1 : va_bits > 39 ? 0 : va_bits > 30 ? 1 : 2;
    uint64_t table = ttbr & 0x0000fffffffffffe;
    if ((tcr >> 59) & 1) {
        // DS: table address bits 51:48 are in TTBR bits 5:2.
        table = (ttbr & 0x0000ffffffffffc0) | (((ttbr >> 2) & 0xf) << 48);
    }
    bool ap_ro = false, xn = false;
    for (int level = start; level <= 3; level++) {
        int shift = 12 + 9 * (3 - level);
        uint64_t idx = (va >> shift) & (level == -1 ? 0xf : 511);
        uint64_t e = ((volatile uint64_t *)(uintptr_t)(hhdm_offset + table))[idx];
        if (!(e & 1)) {
            return false;
        }
        bool is_table = level < 3 && (e & 2);
        if (is_table) {
            ap_ro = ap_ro || ((e >> 62) & 1);
            xn = xn || ((e >> 59) & 1);
            table = e & 0x0000fffffffff000;
            continue;
        }
        if (level == 3 && !(e & 2)) {
            return false;
        }
        uint64_t size = (uint64_t)1 << shift;
        w->phys = (e & 0x0000fffffffff000 & ~(size - 1)) + (va & (size - 1));
        w->page_size = size;
        w->writable = !ap_ro && !((e >> 7) & 1);
        w->executable = !xn && !((e >> 53) & 1);
        w->cache = (e >> 2) & 7;
        return true;
    }
    return false;
}

static void check_gprs(const char *name, uint64_t *st, int keep_x0, uint64_t x0_want) {
    bool ok = true;
    for (int i = 0; i <= 30; i++) {
        if (i == 0 && keep_x0) {
            check(st[0] == x0_want, "aarch64.ap.x0", "x0 %lx, want %lx", st[0], x0_want);
            continue;
        }
        if (st[ST_X0 + i] != 0) {
            ok = false;
            kprintf("C FAIL %s: x%d=%lx\n", name, i, st[ST_X0 + i]);
        }
    }
    check(ok, name, "see above");
}

static uint64_t expected_tcr(uint64_t tsz, bool ds) {
    uint64_t pa = READ_SYSREG(id_aa64mmfr0_el1) & 0xf;
    return ((uint64_t)ds << 59) | (pa << 32) | ((uint64_t)2 << 30) | ((uint64_t)3 << 28)
         | ((uint64_t)1 << 26) | ((uint64_t)1 << 24) | (tsz << 16) | ((uint64_t)3 << 12)
         | ((uint64_t)1 << 10) | ((uint64_t)1 << 8) | tsz;
}

static void check_state(const char *who, uint64_t *st, int base_rev, bool is_bsp) {
    char name[48];
    size_t n = 0;
    while (who[n] != 0) {
        name[n] = who[n];
        n++;
    }
#define NAME(S) (memcpy(name + n, S, sizeof(S)), name)
    int el = (st[ST_CURRENTEL] >> 2) & 3;
    bool vhe = (READ_SYSREG(id_aa64mmfr1_el1) >> 8) & 0xf;
    check(st[ST_DAIF] == 0x3c0, NAME(".daif"), "DAIF %lx", st[ST_DAIF]);
    uint64_t sctlr = st[ST_SCTLR];
    check((sctlr & 1) && (sctlr & (1 << 2)) && (sctlr & (1 << 12)) && !(sctlr & 2)
       && (sctlr & (1 << 3)) && (sctlr & (1 << 4)), NAME(".sctlr_required"), "SCTLR %lx", sctlr);
    uint64_t tcr = st[ST_TCR];
    uint64_t t0sz = tcr & 0x3f, t1sz = (tcr >> 16) & 0x3f;
    bool five = arch_paging_mode() == 1;
    check(t0sz == (five ? 12 : 16) && t1sz == (five ? 12 : 16) && ((tcr >> 59) & 1) == five,
          NAME(".tcr_size"), "TCR %lx", tcr);
    check(((tcr >> 14) & 3) == 0 && ((tcr >> 30) & 3) == 2, NAME(".tcr_granule"), "TCR %lx", tcr);
    check((st[ST_MAIR] & 0xff) == 0xff, NAME(".mair_attr0"), "MAIR %lx", st[ST_MAIR]);
    if (base_rev >= 6) {
        check(el == 1 || (el == 2 && vhe), NAME(".el"), "entered at EL%d (VHE %d)", el, vhe);
        if (is_bsp) {
            check(bsp_state[ST_NZCV] == 0, NAME(".nzcv"), "NZCV %lx", bsp_state[ST_NZCV]);
        }
        check(st[ST_SPSEL] == 0, NAME(".spsel"), "SPSel %lx", st[ST_SPSEL]);
        check(sctlr == 0x30d0199d, NAME(".sctlr_exact"), "SCTLR %lx, want 30d0199d", sctlr);
        check(st[ST_CPACR] == 0, NAME(".cpacr"), "CPACR %lx", st[ST_CPACR]);
        uint64_t want = expected_tcr(five ? 12 : 16, five);
        check(tcr == want, NAME(".tcr_exact"), "TCR %lx, want %lx", tcr, want);
        if (el == 2) {
            check(st[ST_HCR_EL2] == 0x488000002, NAME(".hcr_el2"), "HCR_EL2 %lx", st[ST_HCR_EL2]);
            check(st[ST_CPTR_EL2] == 0, NAME(".cptr_el2"), "CPTR_EL2 %lx", st[ST_CPTR_EL2]);
            check(st[ST_CNTHCTL_EL2] == 3, NAME(".cnthctl_el2"), "CNTHCTL_EL2 %lx", st[ST_CNTHCTL_EL2]);
            check(st[ST_HSTR_EL2] == 0, NAME(".hstr_el2"), "HSTR_EL2 %lx", st[ST_HSTR_EL2]);
        }
    }
#undef NAME
}

static void check_pstate_extras(int base_rev) {
    // Read where implemented; reading an absent one would fault.
    uint64_t mmfr1 = READ_SYSREG(id_aa64mmfr1_el1);
    uint64_t mmfr2 = READ_SYSREG(id_aa64mmfr2_el1);
    uint64_t pfr0 = READ_SYSREG(id_aa64pfr0_el1);
    uint64_t pfr1 = READ_SYSREG(id_aa64pfr1_el1);
    if ((mmfr1 >> 20) & 0xf) {
        uint64_t v;
        asm volatile ("mrs %0, s3_0_c4_c2_3" : "=r"(v));
        fact("aarch64.pan", "%lx", v);
        if (base_rev >= 6) {
            check(v == 0, "aarch64.pstate_pan", "PAN %lx", v);
        }
    }
    if ((mmfr2 >> 4) & 0xf) {
        uint64_t v;
        asm volatile ("mrs %0, s3_0_c4_c2_4" : "=r"(v));
        fact("aarch64.uao", "%lx", v);
        if (base_rev >= 6) {
            check(v == 0, "aarch64.pstate_uao", "UAO %lx", v);
        }
    }
    if ((pfr0 >> 48) & 0xf) {
        uint64_t v;
        asm volatile ("mrs %0, s3_3_c4_c2_5" : "=r"(v));
        fact("aarch64.dit", "%lx", v);
        if (base_rev >= 6) {
            check(v == 0, "aarch64.pstate_dit", "DIT %lx", v);
        }
    }
    // MRS of SSBS needs FEAT_SSBS2.
    if (((pfr1 >> 4) & 0xf) >= 2) {
        uint64_t v;
        asm volatile ("mrs %0, s3_3_c4_c2_6" : "=r"(v));
        fact("aarch64.ssbs", "%lx", v);
        if (base_rev >= 6) {
            check(v == 0, "aarch64.pstate_ssbs", "SSBS %lx", v);
        }
    }
    if (((pfr1 >> 8) & 0xf) >= 1) {
        uint64_t v;
        asm volatile ("mrs %0, s3_3_c4_c2_7" : "=r"(v));
        fact("aarch64.tco", "%lx", v);
        if (base_rev >= 6) {
            check(v == 0, "aarch64.pstate_tco", "TCO %lx", v);
        }
    }
}

void arch_check_entry(int base_rev, uint64_t entry_point) {
    (void)entry_point;
    uint64_t *st = bsp_state;
    fact("aarch64.el", "%lu", (st[ST_CURRENTEL] >> 2) & 3);
    fact("aarch64.vhe", "%lu", (READ_SYSREG(id_aa64mmfr1_el1) >> 8) & 0xf);
    kprintf("K aarch64.entry=daif %lx nzcv %lx spsel %lx sctlr %lx tcr %lx mair %lx cpacr %lx\n",
            st[ST_DAIF], st[ST_NZCV], st[ST_SPSEL], st[ST_SCTLR], st[ST_TCR], st[ST_MAIR],
            st[ST_CPACR]);
    kprintf("K aarch64.ttbr=ttbr0 %lx ttbr1 %lx vbar %lx\n", st[ST_TTBR0], st[ST_TTBR1], st[ST_VBAR]);
    if (((st[ST_CURRENTEL] >> 2) & 3) == 2) {
        kprintf("K aarch64.el2=hcr %lx cptr %lx cnthctl %lx hstr %lx\n", st[ST_HCR_EL2],
                st[ST_CPTR_EL2], st[ST_CNTHCTL_EL2], st[ST_HSTR_EL2]);
    }
    check_gprs("aarch64.bsp.gprs_zero", st, 0, 0);
    check(st[ST_SP] % 16 == 0, "aarch64.bsp.sp_alignment", "SP %lx", st[ST_SP]);
    check_state("aarch64.bsp", st, base_rev, true);
    check_pstate_extras(base_rev);
}

void arch_prepare_aps(void) {
}

void arch_ap_hook(unsigned slot) {
    extern char exception_vector_table[];
    extern char ap_exception_stacks[];
    if (slot < 32) {
        set_exception_sp((uint64_t)(uintptr_t)&ap_exception_stacks[(slot + 1) * 4096]);
    }
    if (current_el() == 2) {
        WRITE_SYSREG(vbar_el2, (uint64_t)(uintptr_t)exception_vector_table);
    } else {
        WRITE_SYSREG(vbar_el1, (uint64_t)(uintptr_t)exception_vector_table);
    }
    asm volatile ("isb" ::: "memory");
}

void arch_check_ap(int base_rev, unsigned slot, struct limine_mp_info *info) {
    uint64_t *st = ap_states[slot];
    char who[24] = "aarch64.ap.";
    who[11] = '0' + (slot / 10) % 10;
    who[12] = '0' + slot % 10;
    who[13] = 0;
    check_gprs("aarch64.ap.gprs_zero", st, 1, (uint64_t)(uintptr_t)info);
    check(st[ST_SP] % 16 == 0, "aarch64.ap.sp_alignment", "AP %u SP %lx", slot, st[ST_SP]);
    check_state(who, st, base_rev, false);
    check(st[ST_TTBR1] == bsp_state[ST_TTBR1] && st[ST_MAIR] == bsp_state[ST_MAIR]
       && st[ST_TCR] == bsp_state[ST_TCR] && st[ST_SCTLR] == bsp_state[ST_SCTLR],
          "aarch64.ap.same_as_bsp", "AP %u differs from the BSP", slot);
    check(((st[ST_CURRENTEL] >> 2) & 3) == ((bsp_state[ST_CURRENTEL] >> 2) & 3), "aarch64.ap.el",
          "AP %u at EL%lu, BSP at EL%lu", slot, (st[ST_CURRENTEL] >> 2) & 3,
          (bsp_state[ST_CURRENTEL] >> 2) & 3);
}

volatile uint64_t aarch64_sync_trap_count = 0;
volatile uint64_t aarch64_last_esr = 0;
volatile uint64_t aarch64_last_far = 0;
volatile int aarch64_probe_active = 0;
volatile int aarch64_probe_faulted = 0;

void aarch64_handle_sync(void) {
    uint64_t esr, far, elr;
    if (current_el() == 2) {
        esr = READ_SYSREG(esr_el2);
        far = READ_SYSREG(far_el2);
        elr = READ_SYSREG(elr_el2);
    } else {
        esr = READ_SYSREG(esr_el1);
        far = READ_SYSREG(far_el1);
        elr = READ_SYSREG(elr_el1);
    }

    aarch64_sync_trap_count++;
    aarch64_last_esr = esr;
    aarch64_last_far = far;

    if (aarch64_probe_active) {
        aarch64_probe_faulted = 1;
        if (current_el() == 2) {
            WRITE_SYSREG(elr_el2, elr + 4);
        } else {
            WRITE_SYSREG(elr_el1, elr + 4);
        }
        return;
    }

    kprintf("FATAL: Unhandled sync exception ESR=%lx FAR=%lx ELR=%lx (EC=0x%x)\n",
            esr, far, elr, (unsigned)(esr >> 26));
    halt();
}

static bool probe_write8(volatile uint8_t *addr, uint8_t val) {
    aarch64_probe_faulted = 0;
    aarch64_probe_active = 1;
    asm volatile (
        "dsb ish\n\t"
        "strb %w1, [%0]\n\t"
        "dsb ish\n\t"
        "isb"
        :: "r" (addr), "r" (val)
        : "memory"
    );
    aarch64_probe_active = 0;
    return aarch64_probe_faulted == 0;
}

static bool probe_read8(volatile uint8_t *addr, uint8_t *out_val) {
    uint8_t val = 0;
    aarch64_probe_faulted = 0;
    aarch64_probe_active = 1;
    asm volatile (
        "dsb ish\n\t"
        "ldrb %w0, [%1]\n\t"
        "dsb ish\n\t"
        "isb"
        : "=r" (val)
        : "r" (addr)
        : "memory"
    );
    aarch64_probe_active = 0;
    if (aarch64_probe_faulted == 0) {
        if (out_val != NULL) {
            *out_val = val;
        }
        return true;
    }
    return false;
}

static bool probe_read32(volatile uint32_t *addr, uint32_t *out_val) {
    uint32_t val = 0;
    aarch64_probe_faulted = 0;
    aarch64_probe_active = 1;
    asm volatile (
        "dsb ish\n\t"
        "ldr %w0, [%1]\n\t"
        "dsb ish\n\t"
        "isb"
        : "=r" (val)
        : "r" (addr)
        : "memory"
    );
    aarch64_probe_active = 0;
    if (aarch64_probe_faulted == 0) {
        if (out_val != NULL) {
            *out_val = val;
        }
        return true;
    }
    return false;
}

__attribute__((section(".text.probe_target"), aligned(4)))
static const uint8_t probe_text_target[4] = { 0x1f, 0x20, 0x03, 0xd5 };

__attribute__((section(".data"), aligned(4)))
static volatile uint8_t probe_data_target = 0x5a;

void arch_check_late(int base_rev, uint64_t rsdp) {
    (void)base_rev;
    (void)rsdp;
    fact("aarch64.mpidr", "%lx", READ_SYSREG(mpidr_el1) & 0xff00ffffff);

    // 1. Timer / CNTVCT trap verification
    uint64_t traps_before = aarch64_sync_trap_count;
    uint64_t c1 = READ_SYSREG(cntvct_el0);
    for (int i = 0; i < 100; i++) {
        asm volatile ("isb" ::: "memory");
        (void)READ_SYSREG(cntvct_el0);
    }
    uint64_t c2 = READ_SYSREG(cntvct_el0);
    uint64_t cntvct_traps = aarch64_sync_trap_count - traps_before;
    fact("aarch64.cntvct_traps", "%lu", cntvct_traps);
    fact("aarch64.cntvct_advances", "%d", c2 >= c1);
    fact("aarch64.cntfrq", "%lu", READ_SYSREG(cntfrq_el0));
    check(cntvct_traps == 0, "aarch64.cntvct_no_traps", "cntvct produced %lu traps", cntvct_traps);

    // 2. SBSA Watchdog check
    volatile uint32_t *wcs = (volatile uint32_t *)0x0f001000;
    uint32_t wcs_val = 0;
    if (probe_read32(wcs, &wcs_val)) {
        fact("aarch64.sbsa_gwdt.present", "%d", 1);
        fact("aarch64.sbsa_gwdt.enabled", "%d", (wcs_val & 1) != 0);
        fact("aarch64.sbsa_gwdt.wcs", "%x", wcs_val);
        check((wcs_val & 1) == 0, "aarch64.sbsa_gwdt.disabled", "WCS=%x", wcs_val);
    } else {
        fact("aarch64.sbsa_gwdt.present", "%d", 0);
    }

    // 3. W^X selftest (data writable/NX, code RO/executable, claimed UEFI memory RWX)
    bool data_writable = probe_write8(&probe_data_target, 0xa5);
    bool data_read_ok = (probe_data_target == 0xa5);
    fact("aarch64.wx.data_writable", "%d", data_writable && data_read_ok);

    bool code_write_succeeded = probe_write8((volatile uint8_t *)probe_text_target, 0x00);
    uint32_t fault_ec = (uint32_t)(aarch64_last_esr >> 26);
    bool code_ro_enforced = (!code_write_succeeded) && (fault_ec == 0x25);
    fact("aarch64.wx.code_ro", "%d", code_ro_enforced);

    // Verify UEFI claimed memory (EfiLoaderCode) and high RAM are writable and executable without fault
    bool usable_writable = false;
    bool high_ram_writable = false;
    bool has_high_ram = false;
    static struct pt_walk w_usable, w_high;
    bool walk_usable_ok = false, walk_high_ok = false;

    if (have_hhdm && memmap != NULL) {
        for (uint64_t i = 0; i < memmap->entry_count; i++) {
            struct limine_memmap_entry *e = memmap->entries[i];
            if (e->type != LIMINE_MEMMAP_USABLE || e->length < 4096) {
                continue;
            }
            if (!usable_writable) {
                uint64_t u_va = hhdm_offset + e->base;
                uint8_t orig = 0;
                if (probe_read8((volatile uint8_t *)(uintptr_t)u_va, &orig)) {
                    bool w_ok = probe_write8((volatile uint8_t *)(uintptr_t)u_va, 0xa5);
                    uint8_t readback = 0;
                    bool r_ok = probe_read8((volatile uint8_t *)(uintptr_t)u_va, &readback) && (readback == 0xa5);
                    if (w_ok) {
                        probe_write8((volatile uint8_t *)(uintptr_t)u_va, orig);
                    }
                    if (w_ok && r_ok) {
                        usable_writable = true;
                        walk_usable_ok = arch_walk(u_va, &w_usable);
                    }
                }
            }
            uint64_t top = e->base + e->length;
            if (top > (1ULL << 32) && !high_ram_writable) {
                has_high_ram = true;
                uint64_t high_base = e->base >= (1ULL << 32) ? e->base : (1ULL << 32);
                uint64_t h_va = hhdm_offset + high_base;
                uint8_t orig = 0;
                if (probe_read8((volatile uint8_t *)(uintptr_t)h_va, &orig)) {
                    bool w_ok = probe_write8((volatile uint8_t *)(uintptr_t)h_va, 0x5a);
                    uint8_t readback = 0;
                    bool r_ok = probe_read8((volatile uint8_t *)(uintptr_t)h_va, &readback) && (readback == 0x5a);
                    if (w_ok) {
                        probe_write8((volatile uint8_t *)(uintptr_t)h_va, orig);
                    }
                    if (w_ok && r_ok) {
                        high_ram_writable = true;
                        walk_high_ok = arch_walk(h_va, &w_high);
                    }
                }
            }
        }
    }

    fact("aarch64.wx.usable_writable", "%d", usable_writable);
    fact("aarch64.wx.usable_executable", "%d", walk_usable_ok && w_usable.executable);
    if (has_high_ram) {
        fact("aarch64.wx.high_ram_writable", "%d", high_ram_writable);
    }

    bool pt_ok = true;
    if (have_hhdm) {
        static struct pt_walk w_data, w_code;
        bool walk_data_ok = arch_walk((uint64_t)(uintptr_t)&probe_data_target, &w_data);
        bool walk_code_ok = arch_walk((uint64_t)(uintptr_t)probe_text_target, &w_code);
        pt_ok = walk_data_ok && walk_code_ok && w_data.writable && !w_data.executable
                && !w_code.writable && w_code.executable;
        if (usable_writable) {
            pt_ok = pt_ok && walk_usable_ok && w_usable.writable && w_usable.executable;
        }
        if (has_high_ram) {
            pt_ok = pt_ok && walk_high_ok && w_high.writable;
        }
    }
    fact("aarch64.wx.pt_enforced", "%d", pt_ok);

    bool wx_ok = data_writable && data_read_ok && code_ro_enforced && pt_ok;
    if (have_hhdm && memmap != NULL) {
        wx_ok = wx_ok && usable_writable;
    }
    if (has_high_ram) {
        wx_ok = wx_ok && high_ram_writable;
    }
    fact("aarch64.wx_selftest", "%s", wx_ok ? "ok" : "fail");
    check(wx_ok, "aarch64.wx_enforced", "W^X enforced: data/usable writable, code RO (fault EC=0x%x)", fault_ec);

    // 4. PCIe ECAM scan for NVMe storage controller
    uint64_t ecam_base = 0;
    if (have_hhdm && rsdp != 0) {
        const uint8_t *rsdp_ptr = (const uint8_t *)(uintptr_t)(hhdm_offset + rsdp);
        bool xsdt = rsdp_ptr[15] >= 2;
        uint64_t root_phys = 0;
        if (xsdt) {
            memcpy(&root_phys, rsdp_ptr + 24, 8);
        }
        else {
            uint32_t r32 = 0;
            memcpy(&r32, rsdp_ptr + 16, 4);
            root_phys = r32;
        }
        if (root_phys != 0) {
            const uint8_t *root = (const uint8_t *)(uintptr_t)(hhdm_offset + root_phys);
            uint32_t root_len = 0;
            memcpy(&root_len, root + 4, 4);
            size_t esize = xsdt ? 8 : 4;
            for (size_t off = 36; off + esize <= root_len; off += esize) {
                uint64_t tbl_phys = 0;
                if (xsdt) {
                    memcpy(&tbl_phys, root + off, 8);
                }
                else {
                    uint32_t t32 = 0;
                    memcpy(&t32, root + off, 4);
                    tbl_phys = t32;
                }
                if (tbl_phys == 0) {
                    continue;
                }
                const uint8_t *tbl = (const uint8_t *)(uintptr_t)(hhdm_offset + tbl_phys);
                if (memcmp(tbl, "MCFG", 4) == 0) {
                    memcpy(&ecam_base, tbl + 44, 8);
                    break;
                }
            }
        }
    }

    if (ecam_base == 0) {
        uint32_t test_val = 0;
        if (probe_read32((volatile uint32_t *)(uintptr_t)0x4010000000ULL, &test_val)) {
            ecam_base = 0x4010000000ULL;
        }
        else {
            ecam_base = 0x3f000000ULL;
        }
    }

    bool nvme_found = false;
    uint16_t nvme_vendor = 0;
    uint16_t nvme_device = 0;
    uint32_t nvme_class = 0;
    uint8_t nvme_slot = 0;

    for (uint8_t dev = 0; dev < 32; dev++) {
        volatile uint32_t *pci_cfg = (volatile uint32_t *)(uintptr_t)(ecam_base + ((uint64_t)dev << 15));
        uint32_t id_reg = 0;
        if (!probe_read32(pci_cfg, &id_reg) || id_reg == 0xffffffff || id_reg == 0) {
            continue;
        }
        uint16_t vendor = (uint16_t)(id_reg & 0xffff);
        uint16_t device = (uint16_t)(id_reg >> 16);

        uint32_t class_reg = 0;
        if (!probe_read32(pci_cfg + 2, &class_reg)) {
            continue;
        }
        uint32_t class_code = class_reg >> 8;
        uint8_t base_class = (uint8_t)(class_code >> 16);
        uint8_t sub_class = (uint8_t)(class_code >> 8);

        // Mass storage (0x01) and Non-Volatile Memory (0x08)
        if (base_class == 0x01 && sub_class == 0x08) {
            nvme_found = true;
            nvme_vendor = vendor;
            nvme_device = device;
            nvme_class = class_code;
            nvme_slot = dev;
            break;
        }
    }

    if (nvme_found) {
        fact("aarch64.pci.nvme.present", "%d", 1);
        fact("aarch64.pci.nvme.vendor_id", "%04x", nvme_vendor);
        fact("aarch64.pci.nvme.device_id", "%04x", nvme_device);
        fact("aarch64.pci.nvme.class", "%06x", nvme_class);
        fact("aarch64.pci.nvme.slot", "%u", nvme_slot);
        fact("boot.device", "nvme");
        fact("boot_device", "nvme");
    } else {
        fact("aarch64.pci.nvme.present", "%d", 0);
    }
}

void arch_dtb(const void *dtb) {
    (void)dtb;
}
