/*
 * STMP3770 Low-Resolution Analog-to-Digital Converter (LRADC)
 *
 * Based on STMP3770 Reference Manual Chapter 28
 *
 * Copyright (C) 2024
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/misc/stmp3770_lradc.h"
#include "hw/timer/stmp3770_pwm.h"

struct STMP3770LRADCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    uint32_t ctrl0;
    uint32_t ctrl1;
    uint32_t ctrl2;
    uint32_t ctrl3;
    uint32_t ctrl4;
    uint32_t status;
    uint32_t channel[8];
    uint32_t delay[4];
    uint32_t debug0;
    uint32_t debug1;
    uint32_t conversion;
    uint32_t version;
    STMP3770PWMState *pwm;
};

#define LRADC_VERSION   0x01010000

/* Register offsets */
#define REG_CTRL0       0x000
#define REG_CTRL0_SET   0x004
#define REG_CTRL0_CLR   0x008
#define REG_CTRL0_TOG   0x00C
#define REG_CTRL1       0x010
#define REG_CTRL1_SET   0x014
#define REG_CTRL1_CLR   0x018
#define REG_CTRL1_TOG   0x01C
#define REG_CTRL2       0x020
#define REG_CTRL2_SET   0x024
#define REG_CTRL2_CLR   0x028
#define REG_CTRL2_TOG   0x02C
#define REG_CTRL3       0x030
#define REG_CTRL3_SET   0x034
#define REG_CTRL3_CLR   0x038
#define REG_CTRL3_TOG   0x03C
#define REG_STATUS      0x040
#define REG_STATUS_SET  0x044
#define REG_STATUS_CLR  0x048
#define REG_STATUS_TOG  0x04C
#define REG_CH0         0x050
#define REG_CH1         0x060
#define REG_CH2         0x070
#define REG_CH3         0x080
#define REG_CH4         0x090
#define REG_CH5         0x0A0
#define REG_CH6         0x0B0
#define REG_CH7         0x0C0
#define REG_DELAY0      0x0D0
#define REG_DELAY1      0x0E0
#define REG_DELAY2      0x0F0
#define REG_DELAY3      0x100
#define REG_DEBUG0      0x110
#define REG_DEBUG1      0x120
#define REG_CONVERSION  0x130
#define REG_CTRL4       0x140
#define REG_VERSION     0x150

/* CTRL0 bits */
#define CTRL0_SFTRST    (1U << 31)
#define CTRL0_CLKGATE   (1U << 30)
#define CTRL0_SCHEDULE_MASK 0xFF
#define CTRL2_BL_ENABLE  (1U << 22)

/* Channel result bits */
#define CH_TOGGLE       (1U << 31)
#define CH_ACCUMULATE   (1U << 29)
#define CH_NUM_SAMPLES_SHIFT 24
#define CH_NUM_SAMPLES_MASK  0x1F
#define CH_VALUE_MASK   0x3FFFF

/* STATUS bits */
#define STATUS_CHANNEL_PRESENT_MASK 0x07FF0000

static uint64_t lradc_read_subword(uint32_t value, hwaddr offset, unsigned size)
{
    unsigned shift = (offset & 3) * 8;
    uint32_t mask = (size >= 4) ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1u);

    return (value >> shift) & mask;
}

static void lradc_apply_sct(uint32_t *reg, uint32_t value, int sct,
                            hwaddr offset, unsigned size)
{
    unsigned shift = (offset & 3) * 8;
    uint32_t mask = (size >= 4) ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1u);
    uint32_t cur = (*reg >> shift) & mask;

    switch (sct) {
    case 0:
        cur = value & mask;
        break;
    case 1:
        cur |= value;
        break;
    case 2:
        cur &= ~value;
        break;
    case 3:
        cur ^= value;
        break;
    }

    *reg = (*reg & ~(mask << shift)) | ((cur & mask) << shift);
}

static void lradc_complete_scheduled(STMP3770LRADCState *s)
{
    int ch;
    uint32_t schedule = s->ctrl0 & CTRL0_SCHEDULE_MASK;

    for (ch = 0; ch < 8; ch++) {
        if (schedule & (1U << ch)) {
            /* Instant conversion: set TOGGLE and a plausible 12-bit value */
            s->channel[ch] &= ~CH_VALUE_MASK;
            s->channel[ch] |= CH_TOGGLE | 0x00000ABC;
        }
    }

    /* Hardware clears SCHEDULE bits when conversions are done */
    s->ctrl0 &= ~CTRL0_SCHEDULE_MASK;
}

static void lradc_update_pwm2_analog_enable(STMP3770LRADCState *s)
{
    if (s->pwm) {
        stmp3770_pwm_set_pwm2_analog_enable(s->pwm,
                                             s->ctrl2 & CTRL2_BL_ENABLE);
    }
}

static uint64_t lradc_read(void *opaque, hwaddr offset, unsigned size)
{
    STMP3770LRADCState *s = opaque;
    hwaddr base = offset & ~0xC;
    uint32_t ret;

    switch (base) {
    case REG_CTRL0:
        ret = s->ctrl0;
        break;
    case REG_CTRL1:
        ret = s->ctrl1;
        break;
    case REG_CTRL2:
        ret = s->ctrl2;
        break;
    case REG_CTRL3:
        ret = s->ctrl3;
        break;
    case REG_STATUS:
        /* Report all LRADC channels as present */
        ret = STATUS_CHANNEL_PRESENT_MASK;
        break;
    case REG_CH0 ... REG_CH7:
        {
            int ch = (base - REG_CH0) >> 4;
            /*
             * Channel 7: Battery voltage (based on ExistOS BSP analysis)
             * ExistOS stmp_power.cpp:158-170 expects:
             *   ADC_val = (Vbatt / 4) / 3.3 * 4095
             *   For 3.7V battery: ADC ≈ 460
             *   For 0V (disconnected): ADC ≈ 2748 (ExistOS log shows this)
             *
             * Simulate disconnected battery (USB powered):
             */
            ret = s->channel[ch];
            if (ch == 7) {
                /* Replace VALUE field with simulated battery ADC, keep control bits */
                ret = (ret & ~CH_VALUE_MASK) | (2748 & CH_VALUE_MASK);
            }
        }
        break;
    case REG_DELAY0:
        ret = s->delay[0];
        break;
    case REG_DELAY1:
        ret = s->delay[1];
        break;
    case REG_DELAY2:
        ret = s->delay[2];
        break;
    case REG_DELAY3:
        ret = s->delay[3];
        break;
    case REG_DEBUG0:
        ret = 0x43210000;
        break;
    case REG_DEBUG1:
        ret = s->debug1;
        break;
    case REG_CONVERSION:
        ret = s->conversion;
        break;
    case REG_CTRL4:
        ret = s->ctrl4;
        break;
    case REG_VERSION:
        ret = LRADC_VERSION;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-lradc: read from unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        ret = 0;
        break;
    }

    return lradc_read_subword(ret, offset, size);
}

static void lradc_write(void *opaque, hwaddr offset,
                        uint64_t value, unsigned size)
{
    STMP3770LRADCState *s = opaque;
    int sct = (offset >> 2) & 3;
    hwaddr base = offset & ~0xC;

    switch (base) {
    case REG_CTRL0:
        lradc_apply_sct(&s->ctrl0, (uint32_t)value, sct, offset, size);
        /* Hardware ties CLKGATE to SFTRST */
        if (s->ctrl0 & CTRL0_SFTRST) {
            s->ctrl0 |= CTRL0_CLKGATE;
        } else {
            s->ctrl0 &= ~CTRL0_CLKGATE;
        }
        /* Complete any scheduled conversions immediately */
        if (s->ctrl0 & CTRL0_SCHEDULE_MASK) {
            lradc_complete_scheduled(s);
        }
        break;
    case REG_CTRL1:
        lradc_apply_sct(&s->ctrl1, (uint32_t)value, sct, offset, size);
        break;
    case REG_CTRL2:
        lradc_apply_sct(&s->ctrl2, (uint32_t)value, sct, offset, size);
        lradc_update_pwm2_analog_enable(s);
        break;
    case REG_CTRL3:
        lradc_apply_sct(&s->ctrl3, (uint32_t)value, sct, offset, size);
        break;
    case REG_CTRL4:
        lradc_apply_sct(&s->ctrl4, (uint32_t)value, sct, offset, size);
        break;
    case REG_CH0 ... REG_CH7:
        lradc_apply_sct(&s->channel[(base - REG_CH0) >> 4], (uint32_t)value,
                        sct, offset, size);
        break;
    case REG_DELAY0:
        lradc_apply_sct(&s->delay[0], (uint32_t)value, sct, offset, size);
        break;
    case REG_DELAY1:
        lradc_apply_sct(&s->delay[1], (uint32_t)value, sct, offset, size);
        break;
    case REG_DELAY2:
        lradc_apply_sct(&s->delay[2], (uint32_t)value, sct, offset, size);
        break;
    case REG_DELAY3:
        lradc_apply_sct(&s->delay[3], (uint32_t)value, sct, offset, size);
        break;
    case REG_DEBUG0:
        /* Read-only */
        break;
    case REG_DEBUG1:
        lradc_apply_sct(&s->debug1, (uint32_t)value, sct, offset, size);
        break;
    case REG_CONVERSION:
        lradc_apply_sct(&s->conversion, (uint32_t)value, sct, offset, size);
        break;
    case REG_STATUS:
    case REG_VERSION:
        /* Read-only; SET/CLR/TOG aliases are harmless no-ops */
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-lradc: write to unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        break;
    }
}

static const MemoryRegionOps lradc_ops = {
    .read = lradc_read,
    .write = lradc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void lradc_reset(DeviceState *dev)
{
    STMP3770LRADCState *s = STMP3770_LRADC(dev);

    s->ctrl0 = CTRL0_SFTRST | CTRL0_CLKGATE;
    s->ctrl1 = 0;
    s->ctrl2 = 0;
    lradc_update_pwm2_analog_enable(s);
    s->ctrl3 = 0;
    s->ctrl4 = 0x76543210;  /* default channel MUX mapping */
    s->conversion = 0x00000080;
    s->version = LRADC_VERSION;
    memset(s->channel, 0, sizeof(s->channel));
    memset(s->delay, 0, sizeof(s->delay));
    s->debug0 = 0x43210000;
    s->debug1 = 0;
}

static void lradc_realize(DeviceState *dev, Error **errp)
{
    STMP3770LRADCState *s = STMP3770_LRADC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &lradc_ops, s,
                          TYPE_STMP3770_LRADC, 0x2000);
    sysbus_init_mmio(sbd, &s->iomem);
}

static int lradc_post_load(void *opaque, int version_id)
{
    STMP3770LRADCState *s = STMP3770_LRADC(opaque);

    lradc_update_pwm2_analog_enable(s);
    return 0;
}

static const VMStateDescription vmstate_lradc = {
    .name = "stmp3770-lradc",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = lradc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl0, STMP3770LRADCState),
        VMSTATE_UINT32(ctrl1, STMP3770LRADCState),
        VMSTATE_UINT32(ctrl2, STMP3770LRADCState),
        VMSTATE_UINT32(ctrl3, STMP3770LRADCState),
        VMSTATE_UINT32(ctrl4, STMP3770LRADCState),
        VMSTATE_UINT32(conversion, STMP3770LRADCState),
        VMSTATE_UINT32_ARRAY(channel, STMP3770LRADCState, 8),
        VMSTATE_UINT32_ARRAY(delay, STMP3770LRADCState, 4),
        VMSTATE_UINT32(debug1, STMP3770LRADCState),
        VMSTATE_END_OF_LIST()
    }
};

static void lradc_init(Object *obj)
{
    STMP3770LRADCState *s = STMP3770_LRADC(obj);

    s->ctrl0 = CTRL0_SFTRST | CTRL0_CLKGATE;
    s->ctrl1 = 0;
    s->ctrl2 = 0;
    s->ctrl3 = 0;
    s->ctrl4 = 0x76543210;
    s->conversion = 0x00000080;
    s->version = LRADC_VERSION;
    memset(s->channel, 0, sizeof(s->channel));
    memset(s->delay, 0, sizeof(s->delay));
    s->debug0 = 0x43210000;
    s->debug1 = 0;
}

static void lradc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = lradc_realize;
    device_class_set_legacy_reset(dc, lradc_reset);
    dc->vmsd = &vmstate_lradc;
}

void stmp3770_lradc_set_pwm(STMP3770LRADCState *s, STMP3770PWMState *pwm)
{
    s->pwm = pwm;
    lradc_update_pwm2_analog_enable(s);
}

static const TypeInfo lradc_type_info = {
    .name          = TYPE_STMP3770_LRADC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STMP3770LRADCState),
    .instance_init = lradc_init,
    .class_init    = lradc_class_init,
};

static void lradc_register_types(void)
{
    type_register_static(&lradc_type_info);
}

type_init(lradc_register_types)
