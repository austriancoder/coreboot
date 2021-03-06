/*
 * This file is part of the coreboot project.
 *
 * Copyright (C) 2015 Intel Corp.
 * (Written by Alexandru Gagniuc <alexandrux.gagniuc@intel.com> for Intel Corp.)
 * (Written by Andrey Petrov <andrey.petrov@intel.com> for Intel Corp.)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <arch/cpu.h>
#include <arch/early_variables.h>
#include <arch/io.h>
#include <arch/symbols.h>
#include <assert.h>
#include <bootmode.h>
#include <cbfs.h>
#include <cbmem.h>
#include <console/console.h>
#include <cpu/x86/mtrr.h>
#include <device/pci_def.h>
#include <device/resource.h>
#include <intelblocks/lpc_lib.h>
#include <fsp/api.h>
#include <fsp/memmap.h>
#include <fsp/util.h>
#include <intelblocks/cpulib.h>
#include <intelblocks/smm.h>
#include <intelblocks/systemagent.h>
#include <intelblocks/pmclib.h>
#include <mrc_cache.h>
#include <reset.h>
#include <soc/cpu.h>
#include <soc/iomap.h>
#include <soc/systemagent.h>
#include <soc/pci_devs.h>
#include <soc/pm.h>
#include <soc/romstage.h>
#include <soc/uart.h>
#include <spi_flash.h>
#include <string.h>
#include <timestamp.h>
#include <timer.h>
#include <delay.h>
#include <compiler.h>
#include "chip.h"

static const uint8_t hob_variable_guid[16] = {
	0x7d, 0x14, 0x34, 0xa0, 0x0c, 0x69, 0x54, 0x41,
	0x8d, 0xe6, 0xc0, 0x44, 0x64, 0x1d, 0xe9, 0x42,
};

static uint32_t fsp_version CAR_GLOBAL;

/* High Performance Event Timer Configuration */
#define P2SB_HPTC				0x60
#define P2SB_HPTC_ADDRESS_ENABLE		(1 << 7)
/*
 * ADDRESS_SELECT            ENCODING_RANGE
 *      0                 0xFED0 0000 - 0xFED0 03FF
 *      1                 0xFED0 1000 - 0xFED0 13FF
 *      2                 0xFED0 2000 - 0xFED0 23FF
 *      3                 0xFED0 3000 - 0xFED0 33FF
 */
#define P2SB_HPTC_ADDRESS_SELECT_0		(0 << 0)
#define P2SB_HPTC_ADDRESS_SELECT_1		(1 << 0)
#define P2SB_HPTC_ADDRESS_SELECT_2		(2 << 0)
#define P2SB_HPTC_ADDRESS_SELECT_3		(3 << 0)

/*
 * Enables several BARs and devices which are needed for memory init
 * - MCH_BASE_ADDR is needed in order to talk to the memory controller
 * - HPET is enabled because FSP wants to store a pointer to global data in the
 *   HPET comparator register
 */
static void soc_early_romstage_init(void)
{
	static const struct sa_mmio_descriptor soc_fixed_pci_resources[] = {
		{ MCHBAR, MCH_BASE_ADDRESS, MCH_BASE_SIZE, "MCHBAR" },
	};

	/* Set Fixed MMIO address into PCI configuration space */
	sa_set_pci_bar(soc_fixed_pci_resources,
			ARRAY_SIZE(soc_fixed_pci_resources));

	/* Enable decoding for HPET. Needed for FSP global pointer storage */
	pci_write_config8(PCH_DEV_P2SB, P2SB_HPTC, P2SB_HPTC_ADDRESS_SELECT_0 |
						P2SB_HPTC_ADDRESS_ENABLE);

	if (IS_ENABLED(CONFIG_DRIVERS_UART_8250IO))
		lpc_io_setup_comm_a_b();
}

/*
 * Punit Initialization code. This all isn't documented, but
 * this is the recipe.
 */
static bool punit_init(void)
{
	uint32_t reg;
	uint32_t data;
	struct stopwatch sw;

	/*
	 * Software Core Disable Mask (P_CR_CORE_DISABLE_MASK_0_0_0_MCHBAR).
	 * Enable all cores here.
	 */
	MCHBAR32(CORE_DISABLE_MASK) = 0x0;

	/* P-Unit bring up */
	reg = MCHBAR32(BIOS_RESET_CPL);
	if (reg == 0xffffffff) {
		/* P-unit not found */
		printk(BIOS_DEBUG, "Punit MMIO not available\n");
		return false;
	}
	/* Set Punit interrupt pin IPIN offset 3D */
	pci_write_config8(SA_DEV_PUNIT, PCI_INTERRUPT_PIN, 0x2);

	/* Set PUINT IRQ to 24 and INTPIN LOCK */
	MCHBAR32(PUNIT_THERMAL_DEVICE_IRQ) =
			PUINT_THERMAL_DEVICE_IRQ_VEC_NUMBER |
			PUINT_THERMAL_DEVICE_IRQ_LOCK;

	if (!IS_ENABLED(CONFIG_SOC_INTEL_GLK)) {
		data = MCHBAR32(0x7818);
		data &= 0xFFFFE01F;
		data |= 0x20 | 0x200;
		MCHBAR32(0x7818) = data;
	}

	/* Stage0 BIOS Reset Complete (RST_CPL) */
	enable_bios_reset_cpl();

	/*
	 * Poll for bit 8 to check if PCODE has completed its action
	 * in reponse to BIOS Reset complete.
	 * We wait here till 1 ms for the bit to get set.
	 */
	stopwatch_init_msecs_expire(&sw, 1);
	while (!(MCHBAR32(BIOS_RESET_CPL) & PCODE_INIT_DONE)) {
		if (stopwatch_expired(&sw)) {
			printk(BIOS_DEBUG, "PCODE Init Done Failure\n");
			return false;
		}
		udelay(100);
	}

	return true;
}

void set_max_freq(void)
{
	if (cpu_get_burst_mode_state() == BURST_MODE_UNAVAILABLE) {
		/* Burst Mode has been factory configured as disabled
		 * and is not available in this physical processor
		 * package.
		 */
		printk(BIOS_DEBUG, "Burst Mode is factory disabled\n");
		return;
	}

	/* Enable burst mode */
	cpu_enable_burst_mode();

	/* Enable speed step. */
	cpu_enable_eist();

	/* Set P-State ratio */
	cpu_set_p_state_to_turbo_ratio();
}

asmlinkage void car_stage_entry(void)
{
	struct postcar_frame pcf;
	uintptr_t top_of_ram;
	bool s3wake;
	struct chipset_power_state *ps = pmc_get_power_state();
	void *smm_base;
	size_t smm_size, var_size;
	const void *new_var_data;
	uintptr_t tseg_base;

	timestamp_add_now(TS_START_ROMSTAGE);

	soc_early_romstage_init();

	console_init();

	s3wake = pmc_fill_power_state(ps) == ACPI_S3;
	fsp_memory_init(s3wake);

	if (punit_init())
		set_max_freq();
	else
		printk(BIOS_DEBUG, "Punit failed to initialize properly\n");

	/* Stash variable MRC data and let cache system update it later */
	new_var_data = fsp_find_extension_hob_by_guid(hob_variable_guid,
							&var_size);
	if (new_var_data)
		mrc_cache_stash_data(MRC_VARIABLE_DATA,
				car_get_var(fsp_version), new_var_data,
				var_size);
	else
		printk(BIOS_ERR, "Failed to determine variable data\n");

	if (postcar_frame_init(&pcf, 1*KiB))
		die("Unable to initialize postcar frame.\n");

	mainboard_save_dimm_info();

	/*
	 * We need to make sure ramstage will be run cached. At this point exact
	 * location of ramstage in cbmem is not known. Instruct postcar to cache
	 * 16 megs under cbmem top which is a safe bet to cover ramstage.
	 */
	top_of_ram = (uintptr_t) cbmem_top();
	/* cbmem_top() needs to be at least 16 MiB aligned */
	assert(ALIGN_DOWN(top_of_ram, 16*MiB) == top_of_ram);
	postcar_frame_add_mtrr(&pcf, top_of_ram - 16*MiB, 16*MiB,
		MTRR_TYPE_WRBACK);

	/* Cache the memory-mapped boot media. */
	if (IS_ENABLED(CONFIG_BOOT_DEVICE_MEMORY_MAPPED))
		postcar_frame_add_mtrr(&pcf, -CONFIG_ROM_SIZE, CONFIG_ROM_SIZE,
					MTRR_TYPE_WRPROT);

	/*
	* Cache the TSEG region at the top of ram. This region is
	* not restricted to SMM mode until SMM has been relocated.
	* By setting the region to cacheable it provides faster access
	* when relocating the SMM handler as well as using the TSEG
	* region for other purposes.
	*/
	smm_region_info(&smm_base, &smm_size);
	tseg_base = (uintptr_t)smm_base;
	postcar_frame_add_mtrr(&pcf, tseg_base, smm_size, MTRR_TYPE_WRBACK);

	run_postcar_phase(&pcf);
}

static void fill_console_params(FSPM_UPD *mupd)
{
	if (IS_ENABLED(CONFIG_CONSOLE_SERIAL)) {
		if (IS_ENABLED(CONFIG_SOC_UART_DEBUG)) {
			mupd->FspmConfig.SerialDebugPortDevice =
					CONFIG_UART_FOR_CONSOLE;
			/* use MMIO port type */
			mupd->FspmConfig.SerialDebugPortType = 2;
			/* use 4 byte register stride */
			mupd->FspmConfig.SerialDebugPortStrideSize = 2;
			/* used only for port type set to external */
			mupd->FspmConfig.SerialDebugPortAddress = 0;
		} else if (IS_ENABLED(CONFIG_DRIVERS_UART_8250IO)) {
			/* use external UART for debug */
			mupd->FspmConfig.SerialDebugPortDevice = 3;
			/* use I/O port type */
			mupd->FspmConfig.SerialDebugPortType = 1;
			/* use 1 byte register stride */
			mupd->FspmConfig.SerialDebugPortStrideSize = 0;
			/* used only for port type set to external */
			mupd->FspmConfig.SerialDebugPortAddress =
					CONFIG_TTYS0_BASE;
		}
	} else {
		mupd->FspmConfig.SerialDebugPortType = 0;
	}
}

static void check_full_retrain(const FSPM_UPD *mupd)
{
	struct chipset_power_state *ps;

	if (mupd->FspmArchUpd.BootMode != FSP_BOOT_WITH_FULL_CONFIGURATION)
		return;

	ps = pmc_get_power_state();

	if (ps->gen_pmcon1 & WARM_RESET_STS) {
		printk(BIOS_INFO, "Full retrain unsupported on warm reboot.\n");
		hard_reset();
	}
}

static void soc_memory_init_params(FSPM_UPD *mupd)
{
#if IS_ENABLED(CONFIG_SOC_INTEL_GLK)
	/* Only for GLK */
	const struct device *dev = dev_find_slot(0, PCH_DEVFN_LPC);
	assert(dev != NULL);
	const config_t *config = dev->chip_info;
	FSP_M_CONFIG *m_cfg = &mupd->FspmConfig;

	if (!config)
		die("Can not find SoC devicetree\n");

	m_cfg->PrmrrSize = config->PrmrrSize;

	/* FSP performs a PERST# signal deassertion for PCIe ports with
	 * the GPIO address specified in these UPDs. Over-ride the default
	 * addresses with 0 to bypass PERST# signal deassertion in FSP.
	 */
	m_cfg->RootPort0Perst = 0;
	m_cfg->RootPort1Perst = 0;
	m_cfg->RootPort2Perst = 0;
	m_cfg->RootPort3Perst = 0;
	m_cfg->RootPort4Perst = 0;
	m_cfg->RootPort5Perst = 0;
#endif
}

void platform_fsp_memory_init_params_cb(FSPM_UPD *mupd, uint32_t version)
{
	struct region_device rdev;

	check_full_retrain(mupd);

	fill_console_params(mupd);

	if (IS_ENABLED(CONFIG_SOC_INTEL_GLK))
		soc_memory_init_params(mupd);

	mainboard_memory_init_params(mupd);

	/* Do NOT let FSP do any GPIO pad configuration */
	mupd->FspmConfig.PreMemGpioTablePtr = (uintptr_t) NULL;

	/*
	 * Tell CSE we do not need to use Ring Buffer Protocol (RBP) to fetch
	 * firmware for us if we are using memory-mapped SPI. This lets CSE
	 * state machine transition to next boot state, so that it can function
	 * as designed.
	 */
	mupd->FspmConfig.SkipCseRbp =
		IS_ENABLED(CONFIG_BOOT_DEVICE_MEMORY_MAPPED);

	/*
	 * Converged Security Engine (CSE) has secure storage functionality.
	 * HECI2 device can be used to access that functionality. However, part
	 * of S3 resume flow involves resetting HECI2 which takes 136ms. Since
	 * coreboot does not use secure storage functionality, instruct FSP to
	 * skip HECI2 reset.
	 */
	mupd->FspmConfig.EnableS3Heci2 = 0;

	/*
	 * Apollolake splits MRC cache into two parts: constant and variable.
	 * The constant part is not expected to change often and variable is.
	 * Currently variable part consists of parameters that change on cold
	 * boots such as scrambler seed and some memory controller registers.
	 * Scrambler seed is vital for S3 resume case because attempt to use
	 * wrong/missing key renders DRAM contents useless.
	 */

	if (mrc_cache_get_current(MRC_VARIABLE_DATA, version, &rdev) == 0) {
		/* Assume leaking is ok. */
		assert(IS_ENABLED(CONFIG_BOOT_DEVICE_MEMORY_MAPPED));
		mupd->FspmConfig.VariableNvsBufferPtr = rdev_mmap_full(&rdev);
	}

	car_set_var(fsp_version, version);
}

__attribute__((weak))
void mainboard_memory_init_params(FSPM_UPD *mupd)
{
	printk(BIOS_DEBUG, "WEAK: %s/%s called\n", __FILE__, __func__);
}

__attribute__((weak))
void mainboard_save_dimm_info(void)
{
	printk(BIOS_DEBUG, "WEAK: %s/%s called\n", __FILE__, __func__);
}
