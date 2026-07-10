/*
 * STMP3770 I2C controller
 *
 * Based on STMP3770 Reference Manual Chapter 24
 *
 * Features:
 * - Minimal register stub sufficient for firmware probing
 * - Reports bus free and acknowledges all writes
 * - Supports SET/CLR/TOG aliases
 *
 * Copyright (C) 2024
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/i2c/stmp3770_i2c.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

/* SET/CLR/TOG sub-offsets */
#define SCT_SET 0x4
#define SCT_CLR 0x8
#define SCT_TOG 0xC

static inline bool stmp3770_i2c_enabled(STMP3770I2CState *s)
{
    return (s->ctrl0 & (I2C_CTRL0_SFTRST | I2C_CTRL0_CLKGATE)) == 0;
}

static void stmp3770_i2c_apply_sct(uint32_t *reg, uint32_t value, int sct,
                                    uint32_t writable_mask)
{
    value &= writable_mask;

    switch (sct) {
    case SCT_SET:
        *reg |= value;
        break;
    case SCT_CLR:
        *reg &= ~value;
        break;
    case SCT_TOG:
        *reg ^= value;
        break;
    default:
        *reg = (*reg & ~writable_mask) | value;
        break;
    }
}

static void stmp3770_i2c_update_irq(STMP3770I2CState *s)
{
    uint32_t pending = (s->ctrl1 & I2C_CTRL1_STATUS_MASK) &
                       ((s->ctrl1 & I2C_CTRL1_ENABLE_MASK) >> 8);

    qemu_set_irq(s->irq_error, pending != 0);
}

static void stmp3770_i2c_reset(DeviceState *dev)
{
    STMP3770I2CState *s = STMP3770_I2C(dev);

    s->ctrl0 = I2C_CTRL0_SFTRST | I2C_CTRL0_CLKGATE;
    s->ctrl1 = 0x00860000;
    s->timing0 = 0x00780030;
    s->timing1 = 0x00800030;
    s->timing2 = 0x00300030;
    s->data = 0;
    s->debug0 = 0;
    s->debug1 = 0;

    stmp3770_i2c_update_irq(s);
}

static uint64_t stmp3770_i2c_read(void *opaque, hwaddr offset, unsigned size)
{
    STMP3770I2CState *s = STMP3770_I2C(opaque);

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-i2c: unsupported read size %u at offset "
                      HWADDR_FMT_plx "\n", size, offset);
        return 0;
    }

    switch (offset) {
    case I2C_CTRL0:
    case I2C_CTRL0 + SCT_SET:
    case I2C_CTRL0 + SCT_CLR:
    case I2C_CTRL0 + SCT_TOG:
        return s->ctrl0;
    case I2C_TIMING0:
    case I2C_TIMING0 + SCT_SET:
    case I2C_TIMING0 + SCT_CLR:
    case I2C_TIMING0 + SCT_TOG:
        return s->timing0;
    case I2C_TIMING1:
    case I2C_TIMING1 + SCT_SET:
    case I2C_TIMING1 + SCT_CLR:
    case I2C_TIMING1 + SCT_TOG:
        return s->timing1;
    case I2C_TIMING2:
    case I2C_TIMING2 + SCT_SET:
    case I2C_TIMING2 + SCT_CLR:
    case I2C_TIMING2 + SCT_TOG:
        return s->timing2;
    case I2C_CTRL1:
    case I2C_CTRL1 + SCT_SET:
    case I2C_CTRL1 + SCT_CLR:
    case I2C_CTRL1 + SCT_TOG:
        return s->ctrl1;
    case I2C_STAT: {
        uint32_t pending = (s->ctrl1 & I2C_CTRL1_STATUS_MASK) &
                           ((s->ctrl1 & I2C_CTRL1_ENABLE_MASK) >> 8);

        return 0xC0000000 | (pending ? (1U << 29) : 0) | pending;
    }
    case I2C_DATA:
        return s->data;
    case I2C_DEBUG0:
    case I2C_DEBUG0 + SCT_SET:
    case I2C_DEBUG0 + SCT_CLR:
    case I2C_DEBUG0 + SCT_TOG:
        return 0x00100000 | s->debug0;
    case I2C_DEBUG1:
    case I2C_DEBUG1 + SCT_SET:
    case I2C_DEBUG1 + SCT_CLR:
    case I2C_DEBUG1 + SCT_TOG:
        return 0xC0000000 | s->debug1;
    case I2C_VERSION:
        return I2C_VERSION_VALUE;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-i2c: read from unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        return 0;
    }
}

static void stmp3770_i2c_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    STMP3770I2CState *s = STMP3770_I2C(opaque);
    int sct = offset & 0xF;

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-i2c: unsupported write size %u at offset "
                      HWADDR_FMT_plx "\n", size, offset);
        return;
    }

    switch (offset & ~0xFULL) {
    case I2C_CTRL0:
        stmp3770_i2c_apply_sct(&s->ctrl0, (uint32_t)value, sct,
                                I2C_CTRL0_RW_MASK);
        if (s->ctrl0 & I2C_CTRL0_SFTRST) {
            stmp3770_i2c_reset(DEVICE(s));
        }
        if ((s->ctrl0 & I2C_CTRL0_RUN) && stmp3770_i2c_enabled(s)) {
            /* Stub: complete the transfer immediately */
            s->ctrl0 &= ~I2C_CTRL0_RUN;
            s->ctrl1 |= I2C_CTRL1_DATA_ENGINE_CMPLT_IRQ;
        }
        break;
    case I2C_TIMING0:
        stmp3770_i2c_apply_sct(&s->timing0, (uint32_t)value, sct,
                                0x03FF03FF);
        break;
    case I2C_TIMING1:
        stmp3770_i2c_apply_sct(&s->timing1, (uint32_t)value, sct,
                                0x03FF03FF);
        break;
    case I2C_TIMING2:
        stmp3770_i2c_apply_sct(&s->timing2, (uint32_t)value, sct,
                                0x03FF03FF);
        break;
    case I2C_CTRL1:
        stmp3770_i2c_apply_sct(&s->ctrl1, (uint32_t)value, sct,
                                I2C_CTRL1_RW_MASK);
        stmp3770_i2c_update_irq(s);
        break;
    case I2C_DATA:
        if (sct == 0) {
            s->data = (uint32_t)value;
        }
        break;
    case I2C_DEBUG0:
        stmp3770_i2c_apply_sct(&s->debug0, (uint32_t)value, sct,
                                0x1C000800);
        break;
    case I2C_DEBUG1:
        stmp3770_i2c_apply_sct(&s->debug1, (uint32_t)value, sct,
                                0x0000073F);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-i2c: write to unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        break;
    }
}

static const MemoryRegionOps stmp3770_i2c_ops = {
    .read = stmp3770_i2c_read,
    .write = stmp3770_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void stmp3770_i2c_init(Object *obj)
{
    STMP3770I2CState *s = STMP3770_I2C(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &stmp3770_i2c_ops, s,
                          TYPE_STMP3770_I2C, 0x2000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq_error);
}

static const VMStateDescription vmstate_stmp3770_i2c = {
    .name = "stmp3770-i2c",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl0, STMP3770I2CState),
        VMSTATE_UINT32(ctrl1, STMP3770I2CState),
        VMSTATE_UINT32(timing0, STMP3770I2CState),
        VMSTATE_UINT32(timing1, STMP3770I2CState),
        VMSTATE_UINT32(timing2, STMP3770I2CState),
        VMSTATE_UINT32(data, STMP3770I2CState),
        VMSTATE_UINT32(debug0, STMP3770I2CState),
        VMSTATE_UINT32(debug1, STMP3770I2CState),
        VMSTATE_END_OF_LIST()
    }
};

static void stmp3770_i2c_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, stmp3770_i2c_reset);
    dc->vmsd = &vmstate_stmp3770_i2c;
}

static const TypeInfo stmp3770_i2c_type_info = {
    .name          = TYPE_STMP3770_I2C,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STMP3770I2CState),
    .instance_init = stmp3770_i2c_init,
    .class_init    = stmp3770_i2c_class_init,
};

static void stmp3770_i2c_register_types(void)
{
    type_register_static(&stmp3770_i2c_type_info);
}

type_init(stmp3770_i2c_register_types)
