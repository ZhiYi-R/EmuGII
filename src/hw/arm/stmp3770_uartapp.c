/*
 * STMP3770 Application UART
 *
 * Copyright (C) 2024
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/arm/stmp3770_uartapp.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define SCT_SET 0x4
#define SCT_CLR 0x8
#define SCT_TOG 0xc

#define UARTAPP_CTRL0     0x000
#define UARTAPP_CTRL1     0x010
#define UARTAPP_CTRL2     0x020
#define UARTAPP_LINECTRL  0x030
#define UARTAPP_LINECTRL2 0x040
#define UARTAPP_INTR      0x050
#define UARTAPP_DATA      0x060
#define UARTAPP_STAT      0x070
#define UARTAPP_DEBUG     0x080
#define UARTAPP_VERSION   0x090

#define UARTAPP_CTRL0_SFTRST (1U << 31)
#define UARTAPP_CTRL0_CLKGATE (1U << 30)
#define UARTAPP_CTRL0_RUN (1U << 29)
#define UARTAPP_CTRL1_RW_MASK 0x1000FFFFU
#define UARTAPP_CTRL2_RW_MASK 0xFF77FFC7U
#define UARTAPP_LINECTRL_RW_MASK 0xFFFF3FFFU
#define UARTAPP_LINECTRL2_RW_MASK 0xFFFF3FFEU
#define UARTAPP_INTR_RW_MASK 0x07FF07FFU
#define UARTAPP_INTR_STATUS_MASK 0x000007FFU
#define UARTAPP_INTR_ENABLE_MASK 0x07FF0000U
#define UARTAPP_STAT_WRITABLE_MASK 0x00F70000U

static void uartapp_apply_sct(uint32_t *reg, uint32_t value, int sct,
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

static void uartapp_update_irq(STMP3770UARTAppState *s)
{
    uint32_t pending = (s->intr & UARTAPP_INTR_STATUS_MASK) &
                       ((s->intr & UARTAPP_INTR_ENABLE_MASK) >> 16);

    qemu_set_irq(s->irq, pending != 0);
}

static void uartapp_reset(DeviceState *dev)
{
    STMP3770UARTAppState *s = STMP3770_UARTAPP(dev);

    s->ctrl0 = 0xC0030000;
    s->ctrl1 = 0;
    s->ctrl2 = 0x00220300;
    s->linectrl = 0;
    s->linectrl2 = 0;
    s->intr = 0;
    s->data = 0;
    s->stat_writable = 0x00F00000;
    uartapp_update_irq(s);
}

static uint64_t uartapp_read(void *opaque, hwaddr offset, unsigned size)
{
    STMP3770UARTAppState *s = STMP3770_UARTAPP(opaque);

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-uartapp: unsupported read size %u at offset "
                      HWADDR_FMT_plx "\n", size, offset);
        return 0;
    }

    switch (offset) {
    case UARTAPP_CTRL0:
    case UARTAPP_CTRL0 + SCT_SET:
    case UARTAPP_CTRL0 + SCT_CLR:
    case UARTAPP_CTRL0 + SCT_TOG:
        return s->ctrl0;
    case UARTAPP_CTRL1:
    case UARTAPP_CTRL1 + SCT_SET:
    case UARTAPP_CTRL1 + SCT_CLR:
    case UARTAPP_CTRL1 + SCT_TOG:
        return s->ctrl1;
    case UARTAPP_CTRL2:
    case UARTAPP_CTRL2 + SCT_SET:
    case UARTAPP_CTRL2 + SCT_CLR:
    case UARTAPP_CTRL2 + SCT_TOG:
        return s->ctrl2;
    case UARTAPP_LINECTRL:
    case UARTAPP_LINECTRL + SCT_SET:
    case UARTAPP_LINECTRL + SCT_CLR:
    case UARTAPP_LINECTRL + SCT_TOG:
        return s->linectrl;
    case UARTAPP_LINECTRL2:
    case UARTAPP_LINECTRL2 + SCT_SET:
    case UARTAPP_LINECTRL2 + SCT_CLR:
    case UARTAPP_LINECTRL2 + SCT_TOG:
        return s->linectrl2;
    case UARTAPP_INTR:
    case UARTAPP_INTR + SCT_SET:
    case UARTAPP_INTR + SCT_CLR:
    case UARTAPP_INTR + SCT_TOG:
        return s->intr;
    case UARTAPP_DATA:
        return s->data;
    case UARTAPP_STAT:
        return 0xC9000000 | s->stat_writable;
    case UARTAPP_DEBUG:
        return 0;
    case UARTAPP_VERSION:
        return 0x02000000;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-uartapp: read from unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        return 0;
    }
}

static void uartapp_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    STMP3770UARTAppState *s = STMP3770_UARTAPP(opaque);
    int sct = offset & 0xf;

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-uartapp: unsupported write size %u at offset "
                      HWADDR_FMT_plx "\n", size, offset);
        return;
    }

    switch (offset & ~0xfULL) {
    case UARTAPP_CTRL0:
        uartapp_apply_sct(&s->ctrl0, value, sct, UINT32_MAX);
        if (s->ctrl0 & UARTAPP_CTRL0_SFTRST) {
            uartapp_reset(DEVICE(s));
        }
        break;
    case UARTAPP_CTRL1:
        uartapp_apply_sct(&s->ctrl1, value, sct, UARTAPP_CTRL1_RW_MASK);
        break;
    case UARTAPP_CTRL2:
        uartapp_apply_sct(&s->ctrl2, value, sct, UARTAPP_CTRL2_RW_MASK);
        break;
    case UARTAPP_LINECTRL:
        uartapp_apply_sct(&s->linectrl, value, sct, UARTAPP_LINECTRL_RW_MASK);
        break;
    case UARTAPP_LINECTRL2:
        uartapp_apply_sct(&s->linectrl2, value, sct,
                          UARTAPP_LINECTRL2_RW_MASK);
        break;
    case UARTAPP_INTR:
        uartapp_apply_sct(&s->intr, value, sct, UARTAPP_INTR_RW_MASK);
        uartapp_update_irq(s);
        break;
    case UARTAPP_DATA:
        if (sct == 0) {
            s->data = value;
        }
        break;
    case UARTAPP_STAT:
        if (sct == 0) {
            s->stat_writable = value & UARTAPP_STAT_WRITABLE_MASK;
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-uartapp: write to unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        break;
    }
}

static const MemoryRegionOps uartapp_ops = {
    .read = uartapp_read,
    .write = uartapp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void uartapp_init(Object *obj)
{
    STMP3770UARTAppState *s = STMP3770_UARTAPP(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &uartapp_ops, s,
                          TYPE_STMP3770_UARTAPP, 0x2000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription vmstate_uartapp = {
    .name = "stmp3770-uartapp",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl0, STMP3770UARTAppState),
        VMSTATE_UINT32(ctrl1, STMP3770UARTAppState),
        VMSTATE_UINT32(ctrl2, STMP3770UARTAppState),
        VMSTATE_UINT32(linectrl, STMP3770UARTAppState),
        VMSTATE_UINT32(linectrl2, STMP3770UARTAppState),
        VMSTATE_UINT32(intr, STMP3770UARTAppState),
        VMSTATE_UINT32(data, STMP3770UARTAppState),
        VMSTATE_UINT32(stat_writable, STMP3770UARTAppState),
        VMSTATE_END_OF_LIST()
    }
};

static void uartapp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, uartapp_reset);
    dc->vmsd = &vmstate_uartapp;
}

static const TypeInfo uartapp_type_info = {
    .name = TYPE_STMP3770_UARTAPP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STMP3770UARTAppState),
    .instance_init = uartapp_init,
    .class_init = uartapp_class_init,
};

static void uartapp_register_types(void)
{
    type_register_static(&uartapp_type_info);
}

type_init(uartapp_register_types)
