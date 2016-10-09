/*
 * OpenPOWER Palmetto BMC
 *
 * Andrew Jeffery <andrew@aj.id.au>
 *
 * Copyright 2016 IBM Corp.
 *
 * This code is licensed under the GPL version 2 or later.  See
 * the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "cpu.h"
#include "exec/address-spaces.h"
#include "hw/arm/arm.h"
#include "hw/arm/ast2400.h"
#include "hw/boards.h"
#include "qemu/log.h"
#include "sysemu/block-backend.h"
#include "sysemu/blockdev.h"
#include "hw/block/flash.h"

static struct arm_boot_info aspeed_binfo = {
    .nb_cpus = 1,
};

typedef struct AspeedBoardState {
    AST2400State soc;
    MemoryRegion ram;
    MemoryRegion sdram;
} AspeedBoardState;

static bool aspeed_has_flash0;

typedef struct AspeedBoardConfig {
    uint32_t hw_strap1;
    uint32_t silicon_rev;
    hwaddr sdram_base;
} AspeedBoardConfig;

enum { PALMETTO_BMC, AST2500_EDK };

/*
#define AST2500_EVB_HW_STRAP1 ((                                        \
        AST2500_HW_STRAP1_DEFAULTS |                                    \
        SCU_AST2500_HW_STRAP_SPI_AUTOFETCH_ENABLE |                     \
        SCU_AST2500_HW_STRAP_GPIO_STRAP_ENABLE |                        \
        SCU_AST2500_HW_STRAP_UART_DEBUG |                               \
        SCU_AST2500_HW_STRAP_DDR4_ENABLE |                              \
        SCU_HW_STRAP_MAC1_RGMII |                                       \
        SCU_HW_STRAP_MAC0_RGMII) &                                      \
        ~SCU_HW_STRAP_2ND_BOOT_WDT)
*/

static const AspeedBoardConfig aspeed_boards[] = {
    [PALMETTO_BMC] = {AST2400_HW_STRAP1, AST2400_A0_SILICON_REV,
              AST2400_SDRAM_BASE},
    [AST2500_EDK] = {0xF100C2E6, AST2500_A1_SILICON_REV,
             AST2500_SDRAM_BASE},
};

static void aspeed_init_flashes(AspeedSMCState *s, const char *flashtype,
                Error **errp)
{
    int i;

    for (i = 0; i < s->num_cs; ++i) {
        AspeedSMCFlash *fl = &s->flashes[i];
        DriveInfo *dinfo = drive_get_next(IF_MTD);
        qemu_irq cs_line;

        /*
         * FIXME: check that we are not using a flash module exceeding
         * the controller segment size
         */
        fl->flash = ssi_create_slave_no_init(s->spi, flashtype);
        if (dinfo) {
            qdev_prop_set_drive(fl->flash, "drive",
                        blk_by_legacy_dinfo(dinfo), errp);
            aspeed_has_flash0 = true;
        }
        m25p80_set_rom_storage(fl->flash, &fl->mmio);
        qdev_init_nofail(fl->flash);

        cs_line = qdev_get_gpio_in_named(fl->flash, SSI_GPIO_CS, 0);
        sysbus_connect_irq(SYS_BUS_DEVICE(s), i + 1, cs_line);
    }
}

static void aspeed_init(MachineState *machine, int board_model)
{
    AspeedBoardState *bmc;

    bmc = g_new0(AspeedBoardState, 1);
    object_initialize(&bmc->soc, (sizeof(bmc->soc)), TYPE_AST2400);
    object_property_add_child(OBJECT(machine), "soc", OBJECT(&bmc->soc),
                  &error_abort);

    /* Create RAM region */
    memory_region_allocate_system_memory(&bmc->ram, NULL, "ram", ram_size);
    memory_region_add_subregion(get_system_memory(),
                    aspeed_boards[board_model].sdram_base,
                    &bmc->ram);
    object_property_add_const_link(OBJECT(&bmc->soc), "ram",
                       OBJECT(&bmc->ram), &error_abort);

    /* Create SDRAM region */
    memory_region_allocate_system_memory(&bmc->sdram, NULL, "aspeed.sdram",
                         0x8000);
    memory_region_add_subregion(get_system_memory(), 0x1E720000,
                    &bmc->sdram);
    object_property_add_const_link(OBJECT(&bmc->soc), "aspeed.sdram",
                       OBJECT(&bmc->sdram), &error_abort);

    object_property_set_int(OBJECT(&bmc->soc),
                aspeed_boards[board_model].hw_strap1,
                "hw-strap1", &error_abort);
    object_property_set_int(OBJECT(&bmc->soc),
                aspeed_boards[board_model].silicon_rev,
                "silicon-rev", &error_abort);
    object_property_set_bool(OBJECT(&bmc->soc), true, "realized",
                 &error_abort);

    aspeed_init_flashes(&bmc->soc.smc, "n25q256a", &error_abort);
    // aspeed_init_flashes(&bmc->soc.spi, "n25q256a", &error_abort);

    /*
     * Install first SMC/FMC flash content as a rom.
     */
    if (aspeed_has_flash0) {
        AspeedSMCFlash *flash0 = &bmc->soc.smc.flashes[0];
        MemoryRegion *flash0alias = g_new(MemoryRegion, 1);

        memory_region_init_alias(flash0alias, OBJECT(&bmc->soc.smc),
                     "flash0alias", &flash0->mmio, 0,
                     flash0->size);
        memory_region_add_subregion(get_system_memory(), 0,
                        flash0alias);
    }

    aspeed_binfo.kernel_filename = machine->kernel_filename;
    aspeed_binfo.initrd_filename = machine->initrd_filename;
    aspeed_binfo.kernel_cmdline = machine->kernel_cmdline;
    aspeed_binfo.ram_size = ram_size;
    aspeed_binfo.loader_start = aspeed_boards[board_model].sdram_base,
    aspeed_binfo.board_id = aspeed_boards[board_model].silicon_rev,
    arm_load_kernel(ARM_CPU(first_cpu), &aspeed_binfo);
}

static void palmetto_bmc_init(MachineState *machine)
{
    machine->cpu_model = "arm926";
    aspeed_init(machine, PALMETTO_BMC);
}

static void palmetto_bmc_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "OpenPOWER Palmetto BMC (ARM926EJ-S)";
    mc->init = palmetto_bmc_init;
    mc->max_cpus = 1;
    mc->no_sdcard = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_sdcard = 1;
    mc->no_parallel = 1;
}

static const TypeInfo palmetto_bmc_type = {
    .name = MACHINE_TYPE_NAME("palmetto-bmc"),
    .parent = TYPE_MACHINE,
    .class_init = palmetto_bmc_class_init,
};

static void ast2500_edk_init(MachineState *machine)
{
    machine->cpu_model = "arm1176";
    aspeed_init(machine, AST2500_EDK);
}

static void ast2500_edk_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Aspeed AST2500 EDK (ARM1176)";
    mc->init = ast2500_edk_init;
    mc->max_cpus = 1;
    mc->no_sdcard = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_sdcard = 1;
    mc->no_parallel = 1;
}

static const TypeInfo ast2500_edk_type = {
    .name = MACHINE_TYPE_NAME("ast2500-edk"),
    .parent = TYPE_MACHINE,
    .class_init = ast2500_edk_class_init,
};

static void aspeed_machine_init(void)
{
    type_register_static(&palmetto_bmc_type);
    type_register_static(&ast2500_edk_type);
}

type_init(aspeed_machine_init)
