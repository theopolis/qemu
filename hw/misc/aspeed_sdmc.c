/*
 * ASPEED SDRAM Memory Controller
 *
 * Copyright (C) 2016 IBM Corp.
 *
 * This code is licensed under the GPL version 2 or later.  See
 * the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "hw/misc/aspeed_sdmc.h"
#include "hw/misc/aspeed_scu.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "trace.h"

/* Protection Key Register */
#define R_PROT            (0x00 / 4)
#define   PROT_KEY_UNLOCK     0xFC600309

/* Configuration Register */
#define R_CONF            (0x04 / 4)

/*
 * Configuration register Ox4 (for Aspeed AST2400 SOC)
 *
 * These are for the record and future use. ASPEED_SDMC_DRAM_SIZE is
 * what we care about right now as it is checked by U-Boot to
 * determine the RAM size.
 */

#define ASPEED_SDMC_RESERVED            0xFFFFF800 /* 31:11 reserved */
#define ASPEED_SDMC_AST2300_COMPAT      (1 << 10)
#define ASPEED_SDMC_SCRAMBLE_PATTERN    (1 << 9)
#define ASPEED_SDMC_DATA_SCRAMBLE       (1 << 8)
#define ASPEED_SDMC_ECC_ENABLE          (1 << 7)
#define ASPEED_SDMC_VGA_COMPAT          (1 << 6) /* readonly */
#define ASPEED_SDMC_DRAM_BANK           (1 << 5)
#define ASPEED_SDMC_DRAM_BURST          (1 << 4)
#define ASPEED_SDMC_VGA_APERTURE(x)     ((x & 0x3) << 2) /* readonly */
#define     ASPEED_SDMC_VGA_8MB             0x0
#define     ASPEED_SDMC_VGA_16MB            0x1
#define     ASPEED_SDMC_VGA_32MB            0x2
#define     ASPEED_SDMC_VGA_64MB            0x3
#define ASPEED_SDMC_DRAM_SIZE(x)        (x & 0x3)
#define     ASPEED_SDMC_DRAM_64MB           0x0
#define     ASPEED_SDMC_DRAM_128MB          0x1
#define     ASPEED_SDMC_DRAM_256MB          0x2
#define     ASPEED_SDMC_DRAM_512MB          0x3

#define ASPEED_SDMC_READONLY_MASK                       \
    (ASPEED_SDMC_RESERVED | ASPEED_SDMC_VGA_COMPAT |    \
     ASPEED_SDMC_VGA_APERTURE(ASPEED_SDMC_VGA_64MB))
/*
 * Configuration register Ox4 (for Aspeed AST2500 SOC and higher)
 *
 * Incompatibilities are annotated in the list. ASPEED_SDMC_HW_VERSION
 * should be set to 1 for the AST2500 SOC.
 */
#define ASPEED_SDMC_HW_VERSION(x)       ((x & 0xf) << 28) /* readonly */
#define ASPEED_SDMC_SW_VERSION          ((x & 0xff) << 20)
#define ASPEED_SDMC_CACHE_INITIAL_DONE  (1 << 19)  /* readonly */
#define ASPEED_SDMC_AST2500_RESERVED    0x7C000 /* 18:14 reserved */
#define ASPEED_SDMC_CACHE_DDR4_CONF     (1 << 13)
#define ASPEED_SDMC_CACHE_INITIAL       (1 << 12)
#define ASPEED_SDMC_CACHE_RANGE_CTRL    (1 << 11)
#define ASPEED_SDMC_CACHE_ENABLE        (1 << 10) /* differs from AST2400 */
#define ASPEED_SDMC_DRAM_TYPE           (1 << 4)  /* differs from AST2400 */

/* DRAM size definitions differs */
#define     ASPEED_SDMC_AST2500_128MB       0x0
#define     ASPEED_SDMC_AST2500_256MB       0x1
#define     ASPEED_SDMC_AST2500_512MB       0x2
#define     ASPEED_SDMC_AST2500_1024MB      0x3

#define ASPEED_SDMC_AST2500_READONLY_MASK                               \
    (ASPEED_SDMC_HW_VERSION(0xf) | ASPEED_SDMC_CACHE_INITIAL_DONE |     \
     ASPEED_SDMC_AST2500_RESERVED | ASPEED_SDMC_VGA_COMPAT |            \
     ASPEED_SDMC_VGA_APERTURE(ASPEED_SDMC_VGA_64MB))

/*
 * Other registers of the Aspeed AST2400 SOC
 *
 */

/* MCR08: Graphics Memory Protection Register */
#define R_GRAPHIC_MEM_PROT              (0x8 / 4)

/* MCR0C: Refresh Timing Register */
#define R_REFRESH_TIMING                (0x0C / 4)

/* MCR10: AC Timing Register #1 */
#define R_AC_TIMING1                    (0x10 / 4)

/* MCR14: AC Timing Register #2 */
#define R_AC_TIMING2                    (0x14 / 4)

/* MCR18: CK/DQS Delay Control Register */
#define R_CQDQS_DELAY_CTRL              (0x18 / 4)

/* MCR1C: MCLK to MCLK2X Calibration(CBR) Status Register */
#define R_MCLK_CALIB_STATUS             (0x1C / 4)
#define   MCLK2X_PHASE                      (0x96 << 16)

/* MCR20: DQS Input Phase Calibration Control and Status */
#define R_DQS_INPUT_CALIB_STATUS        (0x20 / 4)

/* MCR24: DQS Input DLL Delay Calibration Control and Status */
#define R_DQS_INPUT_DLL_CALIB_STATUS    (0x24 / 4)

/* MCR28: Mode Setting Control Register */
#define R_MODE_SETTING_CTRL             (0x28 / 4)

/* MCR2C: MRS/EMRS2 Mode Setting Register */
#define R_MRS_MODE_SETTING_CTRL         (0x2C / 4)

/* MCR30: EMRS/EMRS3 Mode Setting Register */
#define R_EMRS_MODE_SETTING_CTRL        (0x30 / 4)

/* MCR34: Power Control Register */
#define R_POWER_CTRL                    (0x34 / 4)

/* MCR38: Page Miss Latency Mask Register */
#define R_PAGE_MISS_MASK                (0x38 / 4)

/* MCR40: Maximum Grant Length Register #1 */
#define R_MAX_GRANT_LEN1                (0x40 / 4)

/* MCR44: Maximum Grant Length Register #2 */
#define R_MAX_GRANT_LEN2                (0x44 / 4)

/* MCR48: Maximum Grant Length Register #3 */
#define R_MAX_GRANT_LEN3                (0x48 / 4)

/* MCR4C: Maximum Grant Length Register #4 */
#define R_MAX_GRANT_LEN4                (0x4C / 4)

/* MCR50: Interrupt Control/Status Register */
#define R_IRQ_STATUS_CTRL               (0x50 / 4)

/* MCR54: ECC Protection Address Range Register */
#define R_ECC_PROTECT                   (0x54 / 4)

/* MCR58: ????  */
#define R_MCR58                         (0x58 / 4)

/* MCR60: IO Buffer Mode Register */
#define R_IO_BUFFER_MODE                (0x60 / 4)

/* MCR64: DLL Control Register #1 */
#define R_DLL_CTRL                      (0x64 / 4)

/* MCR68: DLL Control Register #2 */
#define R_DLL_CTRL2                     (0x68 / 4)

/* MCR6C: DDR IO Impedance Calibration Control */
#define R_DDR_IO_IMPEDANCE_CTRL         (0x6C / 4)

/* MCR70: ECC Testing Control/Status Register */
#define R_ECC_STATUS_CTRL               (0x70 / 4)
#define   ECC_TEST_FINISH                  (1 << 12)

/* MCR74: Testing Start Address and Length Register */
#define R_START_ADDR_LEN                (0x74 / 4)

/* MCR78: Testing Fail DQ Bit Register */
#define R_FAIL_DQ                       (0x78 / 4)

/* MCR7C: Test Initial Value Register */
#define R_TEST_INIT_VALUE               (0x7C / 4)

/* MCR80: DQ Input Delay Calibration Control #1 */
#define R_DQ_DELAY_CALIB_CTRL1          (0x80 / 4)
#define   DQ_DELAY_CALIB_COUNT_DONE        (1 << 31)

/* MCR84: DQ Input Delay Calibration Control #2 */
#define R_DQ_DELAY_CALIB_CTRL2          (0x84 / 4)

/* MCR88: DQ Input Delay Calibration Control #3 */
#define R_DQ_DELAY_CALIB_CTRL3          (0x88 / 4)

/* MCR8C: CK Duty Counter Value */
#define R_CK_DUTY_VALUE                 (0x8C / 4)

/* MCR100: AST2000 Backward Compatible SCU Password */
#define R_COMPAT_SCU_PASSWORD           (0x100 / 4)

/* MCR120: AST2000 Backward Compatible SCU MPLL Parameter */
#define R_COMPAT_SCU_MPLL               (0x120 / 4)

/* MCR170: AST2000 Backward Compatible SCU Hardware Strapping Value */
#define R_COMPAT_SCU_HW_STRAPPING       (0x170 / 4)

static uint64_t aspeed_sdmc_read(void *opaque, hwaddr addr, unsigned size)
{
    AspeedSDMCState *s = ASPEED_SDMC(opaque);
    uint64_t val = 0;

    addr >>= 2;

    switch (addr) {
    case R_PROT:
        val = s->unlocked;
        break;
    case R_CONF:
    case R_GRAPHIC_MEM_PROT:
    case R_REFRESH_TIMING:
    case R_AC_TIMING1:
    case R_AC_TIMING2:
    case R_CQDQS_DELAY_CTRL:
    case R_MCLK_CALIB_STATUS:
    case R_DQS_INPUT_CALIB_STATUS:
    case R_DQS_INPUT_DLL_CALIB_STATUS:
    case R_MODE_SETTING_CTRL:
    case R_MRS_MODE_SETTING_CTRL:
    case R_EMRS_MODE_SETTING_CTRL:
    case R_POWER_CTRL:
    case R_PAGE_MISS_MASK:
    case R_MAX_GRANT_LEN1:
    case R_MAX_GRANT_LEN2:
    case R_MAX_GRANT_LEN3:
    case R_MAX_GRANT_LEN4:
    case R_IRQ_STATUS_CTRL:
    case R_ECC_PROTECT:
    case R_MCR58:
    case R_IO_BUFFER_MODE:
    case R_DLL_CTRL:
    case R_DLL_CTRL2:
    case R_DDR_IO_IMPEDANCE_CTRL:
    case R_START_ADDR_LEN:
    case R_FAIL_DQ:
    case R_TEST_INIT_VALUE:
    case R_DQ_DELAY_CALIB_CTRL2:
    case R_DQ_DELAY_CALIB_CTRL3:
    case R_CK_DUTY_VALUE:
    case R_COMPAT_SCU_PASSWORD:
    case R_COMPAT_SCU_MPLL:
    case R_COMPAT_SCU_HW_STRAPPING:
        val = s->regs[addr];
        break;
    case R_DQ_DELAY_CALIB_CTRL1:
        val = s->regs[addr] | DQ_DELAY_CALIB_COUNT_DONE;
        break;
    case R_ECC_STATUS_CTRL:
        val = s->regs[addr] | ECC_TEST_FINISH;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Out-of-bounds read at offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }

    return val;
}

static void aspeed_sdmc_write(void *opaque, hwaddr addr, uint64_t data,
                             unsigned int size)
{
    AspeedSDMCState *s = ASPEED_SDMC(opaque);

    addr >>= 2;

    if (addr == R_PROT) {
        s->unlocked = (data == PROT_KEY_UNLOCK);
        return;
    }

    if (!s->unlocked) { /* TODO protect : MCR04 ∼ MCR7C */
        qemu_log_mask(LOG_GUEST_ERROR, "%s: SDMC is locked!\n", __func__);
        return;
    }

    switch (addr) {
    case R_CONF:
        /* Make sure readonly bits are kept */
        switch (s->silicon_rev) {
        case AST2400_A0_SILICON_REV:
        case AST2400_A1_SILICON_REV:
            data &= ~ASPEED_SDMC_READONLY_MASK;
            break;
        case AST2500_A1_SILICON_REV:
        case AST2500_A0_SILICON_REV:
            data &= ~ASPEED_SDMC_AST2500_READONLY_MASK;
            break;
        default:
            g_assert_not_reached();
        }
        break;
    case R_GRAPHIC_MEM_PROT:
    case R_REFRESH_TIMING:
    case R_AC_TIMING1:
    case R_AC_TIMING2:
    case R_CQDQS_DELAY_CTRL:
    case R_MCLK_CALIB_STATUS:
    case R_DQS_INPUT_CALIB_STATUS:
    case R_DQS_INPUT_DLL_CALIB_STATUS:
    case R_MODE_SETTING_CTRL:
    case R_MRS_MODE_SETTING_CTRL:
    case R_EMRS_MODE_SETTING_CTRL:
    case R_POWER_CTRL:
    case R_PAGE_MISS_MASK:
    case R_MAX_GRANT_LEN1:
    case R_MAX_GRANT_LEN2:
    case R_MAX_GRANT_LEN3:
    case R_MAX_GRANT_LEN4:
    case R_IRQ_STATUS_CTRL:
    case R_ECC_PROTECT:
    case R_MCR58:
    case R_IO_BUFFER_MODE:
    case R_DLL_CTRL:
    case R_DLL_CTRL2:
    case R_DDR_IO_IMPEDANCE_CTRL:
    case R_ECC_STATUS_CTRL:
    case R_START_ADDR_LEN:
    case R_FAIL_DQ:
    case R_TEST_INIT_VALUE:
    case R_DQ_DELAY_CALIB_CTRL1:
    case R_DQ_DELAY_CALIB_CTRL2:
    case R_DQ_DELAY_CALIB_CTRL3:
    case R_CK_DUTY_VALUE:
    case R_COMPAT_SCU_PASSWORD:
    case R_COMPAT_SCU_MPLL:
    case R_COMPAT_SCU_HW_STRAPPING:
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Out-of-bounds write at offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr << 2);
        return;
    }

    s->regs[addr] = data;
}

static const MemoryRegionOps aspeed_sdmc_ops = {
    .read = aspeed_sdmc_read,
    .write = aspeed_sdmc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static int ast2400_rambits(AspeedSDMCState *s)
{
    switch (s->ram_size >> 20) {
    case 64:
        return ASPEED_SDMC_DRAM_64MB;
    case 128:
        return ASPEED_SDMC_DRAM_128MB;
    case 256:
        return ASPEED_SDMC_DRAM_256MB;
    case 512:
        return ASPEED_SDMC_DRAM_512MB;
    default:
        break;
    }

    /* use a common default */
    error_report("warning: Invalid RAM size 0x%" PRIx64
                 ". Using default 256M", s->ram_size);
    s->ram_size = 256 << 20;
    return ASPEED_SDMC_DRAM_256MB;
}

static int ast2500_rambits(AspeedSDMCState *s)
{
    switch (s->ram_size >> 20) {
    case 128:
        return ASPEED_SDMC_AST2500_128MB;
    case 256:
        return ASPEED_SDMC_AST2500_256MB;
    case 512:
        return ASPEED_SDMC_AST2500_512MB;
    case 1024:
        return ASPEED_SDMC_AST2500_1024MB;
    default:
        break;
    }

    /* use a common default */
    error_report("warning: Invalid RAM size 0x%" PRIx64
                 ". Using default 512M", s->ram_size);
    s->ram_size = 512 << 20;
    return ASPEED_SDMC_AST2500_512MB;
}

static void aspeed_sdmc_reset(DeviceState *dev)
{
    AspeedSDMCState *s = ASPEED_SDMC(dev);

    memset(s->regs, 0, sizeof(s->regs));

    /* Set ram size bit and defaults values */
    switch (s->silicon_rev) {
    case AST2400_A0_SILICON_REV:
    case AST2400_A1_SILICON_REV:
        s->regs[R_CONF] |=
            ASPEED_SDMC_VGA_COMPAT |
            ASPEED_SDMC_DRAM_SIZE(s->ram_bits);
        break;

    case AST2500_A0_SILICON_REV:
    case AST2500_A1_SILICON_REV:
        s->regs[R_CONF] |=
            ASPEED_SDMC_HW_VERSION(1) |
            ASPEED_SDMC_VGA_APERTURE(ASPEED_SDMC_VGA_64MB) |
            ASPEED_SDMC_DRAM_SIZE(s->ram_bits);
        break;

    default:
        g_assert_not_reached();
    }

    s->regs[R_MCLK_CALIB_STATUS] = MCLK2X_PHASE;
}

static void aspeed_sdmc_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    AspeedSDMCState *s = ASPEED_SDMC(dev);

    if (!is_supported_silicon_rev(s->silicon_rev)) {
        error_setg(errp, "Unknown silicon revision: 0x%" PRIx32,
                s->silicon_rev);
        return;
    }

    switch (s->silicon_rev) {
    case AST2400_A0_SILICON_REV:
    case AST2400_A1_SILICON_REV:
        s->ram_bits = ast2400_rambits(s);
        break;
    case AST2500_A0_SILICON_REV:
    case AST2500_A1_SILICON_REV:
        s->ram_bits = ast2500_rambits(s);
        break;
    default:
        g_assert_not_reached();
    }

    memory_region_init_io(&s->iomem, OBJECT(s), &aspeed_sdmc_ops, s,
                          TYPE_ASPEED_SDMC, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_aspeed_sdmc = {
    .name = "aspeed.sdmc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, AspeedSDMCState, ASPEED_SDMC_NR_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static Property aspeed_sdmc_properties[] = {
    DEFINE_PROP_UINT32("silicon-rev", AspeedSDMCState, silicon_rev, 0),
    DEFINE_PROP_UINT64("ram-size", AspeedSDMCState, ram_size, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void aspeed_sdmc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = aspeed_sdmc_realize;
    dc->reset = aspeed_sdmc_reset;
    dc->desc = "ASPEED SDRAM Memory Controller";
    dc->vmsd = &vmstate_aspeed_sdmc;
    dc->props = aspeed_sdmc_properties;
}

static const TypeInfo aspeed_sdmc_info = {
    .name = TYPE_ASPEED_SDMC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AspeedSDMCState),
    .class_init = aspeed_sdmc_class_init,
};

static void aspeed_sdmc_register_types(void)
{
    type_register_static(&aspeed_sdmc_info);
}

type_init(aspeed_sdmc_register_types);
