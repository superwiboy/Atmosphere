#include <stdint.h>
#include <stdbool.h>

#include "utils.h"
#include "bootup.h"

#include "fuse.h"
#include "flow.h"
#include "pmc.h"
#include "mc.h"
#include "car.h"
#include "se.h"
#include "masterkey.h"
#include "configitem.h"
#include "timers.h"
#include "misc.h"
#include "bpmp.h"
#include "sysreg.h"
#include "interrupt.h"
#include "cpu_context.h"
#include "actmon.h"
#include "syscrt0.h"

static bool g_has_booted_up = false;

void bootup_misc_mmio(void) {
    /* Initialize Fuse registers. */
    fuse_init();

    /* Verify Security Engine sanity. */
    se_set_in_context_save_mode(false);
    /* TODO: se_verify_keys_unreadable(); */
    se_validate_stored_vector();

    for (unsigned int i = 0; i < KEYSLOT_SWITCH_SESSIONKEY; i++) {
        clear_aes_keyslot(i);
    }

    for (unsigned int i = 0; i < KEYSLOT_RSA_MAX; i++) {
        clear_rsa_keyslot(i);
    }
    se_initialize_rng(KEYSLOT_SWITCH_RNGKEY);
    se_generate_random_key(KEYSLOT_SWITCH_SRKGENKEY, KEYSLOT_SWITCH_RNGKEY);
    se_generate_srk(KEYSLOT_SWITCH_SRKGENKEY);

    /* TODO: Why does this DRAM write occur? */
    if (!g_has_booted_up && mkey_get_revision() >= MASTERKEY_REVISION_400_CURRENT) {
        /* 4.x writes this magic number into DRAM. Why? */
        (*(volatile uint32_t *)(0x8005FFFC)) = 0xC0EDBBCC;
    }

    /* Todo: What? */
    MAKE_TIMERS_REG(0x1A4) = 0xF1E0;

    FLOW_CTLR_BPMP_CLUSTER_CONTROL_0 = 4; /* ACTIVE_CLUSTER_LOCK. */
    FLOW_CTLR_FLOW_DBG_QUAL_0 = 0x10000000; /* Enable FIQ2CCPLEX */

    /* Disable Deep Power Down. */
    APBDEV_PMC_DPD_ENABLE_0 = 0;

    /* Setup MC. */
    /* TODO: What are these MC reg writes? */
    MAKE_MC_REG(0x984) = 1;
    MAKE_MC_REG(0x648) = 0;
    MAKE_MC_REG(0x64C) = 0;
    MAKE_MC_REG(0x650) = 1;
    MAKE_MC_REG(0x670) = 0;
    MAKE_MC_REG(0x674) = 0;
    MAKE_MC_REG(0x678) = 1;
    MAKE_MC_REG(0x9A0) = 0;
    MAKE_MC_REG(0x9A4) = 0;
    MAKE_MC_REG(0x9A8) = 0;
    MAKE_MC_REG(0x9AC) = 1;
    MC_SECURITY_CFG0_0 = 0;
    MC_SECURITY_CFG1_0 = 0;
    MC_SECURITY_CFG3_0 = 3;
    configure_default_carveouts();

    /* Mark registers secure world only. */
    /* Mark SATA_AUX, DTV, QSPI, SE, SATA, LA secure only. */
    APB_MISC_SECURE_REGS_APB_SLAVE_SECURITY_ENABLE_REG0_0 = 0x504244;

    /* By default, mark SPI1, SPI2, SPI3, SPI5, SPI6, I2C6 secure only. */
    uint32_t sec_disable_1 = 0x83700000;
    /* By default, mark SDMMC3, DDS, DP2 secure only. */
    uint32_t sec_disable_2 = 0x304;
    uint64_t hardware_type = configitem_get_hardware_type();
    if (hardware_type != 1) {
        /* Also mark I2C5 secure only, */
        sec_disable_1 |= 0x20000000;
    }
    if (hardware_type != 0 && mkey_get_revision() >= MASTERKEY_REVISION_400_CURRENT) {
        /* Starting on 4.x on non-dev units, mark UARTB, UARTC, SPI4, I2C3 secure only. */
        sec_disable_1 |= 0x10806000;
        /* Starting on 4.x on non-dev units, mark SDMMC1 secure only. */
        sec_disable_2 |= 1;
    }
    APB_MISC_SECURE_REGS_APB_SLAVE_SECURITY_ENABLE_REG1_0 = sec_disable_1;
    APB_MISC_SECURE_REGS_APB_SLAVE_SECURITY_ENABLE_REG2_0 = sec_disable_2;

    /* TODO: What are these MC reg writes? */
    MAKE_MC_REG(0x228) = 0xFFFFFFFF;
    MAKE_MC_REG(0x22C) = 0xFFFFFFFF;
    MAKE_MC_REG(0x230) = 0xFFFFFFFF;
    MAKE_MC_REG(0x234) = 0xFFFFFFFF;
    MAKE_MC_REG(0xB98) = 0xFFFFFFFF;
    MAKE_MC_REG(0x038) = 0;
    MAKE_MC_REG(0x03C) = 0;
    MAKE_MC_REG(0x0E0) = 0;
    MAKE_MC_REG(0x0E4) = 0;
    MAKE_MC_REG(0x0E8) = 0;
    MAKE_MC_REG(0x0EC) = 0;
    MAKE_MC_REG(0x0F0) = 0;
    MAKE_MC_REG(0x0F4) = 0;
    MAKE_MC_REG(0x020) = 0;
    MAKE_MC_REG(0x014) = 0x30000030;
    MAKE_MC_REG(0x018) = 0x2800003F;
    MAKE_MC_REG(0x034) = 0;
    MAKE_MC_REG(0x030) = 0;
    MAKE_MC_REG(0x010) = 0;

    /* Clear RESET Vector, setup CPU Secure Boot RESET Vectors. */
    uint32_t reset_vec = TZRAM_GET_SEGMENT_PA(TZRAM_SEGMENT_ID_WARMBOOT_CRT0_AND_MAIN);
    EVP_CPU_RESET_VECTOR_0 = 0;
    SB_AA64_RESET_LOW_0 = reset_vec | 1;
    SB_AA64_RESET_HIGH_0 = 0;

    /* Lock Non-Secure writes to Secure Boot RESET Vector. */
    SB_CSR_0 = 2;
    
    /* Setup PMC Secure Scratch RESET Vector for warmboot. */
    APBDEV_PMC_SECURE_SCRATCH34_0 = reset_vec;
    APBDEV_PMC_SECURE_SCRATCH35_0 = 0;
    APBDEV_PMC_SEC_DISABLE3_0 = 0x500000;

    /* Setup FIQs. */
    

    /* And assign "se_operation_completed" to Interrupt 0x5A. */
    intr_set_priority(INTERRUPT_ID_SECURITY_ENGINE, 0);
    intr_set_group(INTERRUPT_ID_SECURITY_ENGINE, 0);
    intr_set_enabled(INTERRUPT_ID_SECURITY_ENGINE, 1);
    intr_set_cpu_mask(INTERRUPT_ID_SECURITY_ENGINE, 8);
    intr_set_edge_level(INTERRUPT_ID_SECURITY_ENGINE, 0);
    intr_set_priority(INTERRUPT_ID_ACTIVITY_MONITOR_4X, 0);
    intr_set_group(INTERRUPT_ID_ACTIVITY_MONITOR_4X, 0);
    intr_set_enabled(INTERRUPT_ID_ACTIVITY_MONITOR_4X, 1);
    intr_set_cpu_mask(INTERRUPT_ID_ACTIVITY_MONITOR_4X, 8);
    intr_set_edge_level(INTERRUPT_ID_ACTIVITY_MONITOR_4X, 0);

    if (!g_has_booted_up) {
        intr_register_handler(INTERRUPT_ID_SECURITY_ENGINE, se_operation_completed);
        intr_register_handler(INTERRUPT_ID_ACTIVITY_MONITOR_4X, actmon_interrupt_handler);
        for (unsigned int core = 1; core < NUM_CPU_CORES; core++) {
            set_core_is_active(core, false);
        }
        g_has_booted_up = true;
    } else if (mkey_get_revision() < MASTERKEY_REVISION_400_CURRENT) {
        /* TODO: What are these MC reg writes? */
        MAKE_MC_REG(0x65C) = 0xFFFFF000;
        MAKE_MC_REG(0x660) = 0;
        MAKE_MC_REG(0x964) |= 1;
        CLK_RST_CONTROLLER_LVL2_CLK_GATE_OVRD_0 &= 0xFFF7FFFF;
    }
}

void setup_4x_mmio(void) {
    /* TODO */
}

#define SET_SYSREG(reg, val) do { temp_reg = val; __asm__ __volatile__ ("msr " #reg ", %0" :: "r"(temp_reg) : "memory"); } while(false)

void setup_current_core_state(void) {
    uint64_t temp_reg;
    
    /* Setup system registers. */
    SET_SYSREG(actlr_el3, 0x73ull);
    SET_SYSREG(actlr_el2, 0x73ull);
    SET_SYSREG(hcr_el2, 0x80000000ull);
    SET_SYSREG(dacr32_el2, 0xFFFFFFFFull);
    SET_SYSREG(sctlr_el1, 0xC50838ull);
    SET_SYSREG(sctlr_el2, 0x30C50838ull);
    
    do { __asm__ __volatile__ ("isb"); } while (false);
    
    SET_SYSREG(cntfrq_el0, MAKE_SYSCRT0_REG(0x20)); /* TODO: Reg name. */
    SET_SYSREG(cnthctl_el2, 3ull);

    do { __asm__ __volatile__ ("isb"); } while (false);

    /* Setup Interrupts, flow. */
    flow_clear_csr0_and_events(get_core_id());
    intr_initialize_gic();
    intr_set_priority(INTERRUPT_ID_1C, 0);
    intr_set_group(INTERRUPT_ID_1C, 0);
    intr_set_enabled(INTERRUPT_ID_1C, 1);

    /* Restore current core context. */
    restore_current_core_context();
}