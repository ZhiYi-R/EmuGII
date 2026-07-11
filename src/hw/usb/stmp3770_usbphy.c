/*
 * STMP3770 USB PHY emulation
 *
 * Based on STMP3770 Reference Manual Chapter 10
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
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/usb/stmp3770_usbphy.h"

#define REG_PWD             0x000
#define REG_TX              0x010
#define REG_RX              0x020
#define REG_CTRL            0x030
#define REG_STATUS          0x040
#define REG_DEBUG           0x050
#define REG_DEBUG0_STATUS   0x060
#define REG_DEBUG1          0x070
#define REG_VERSION         0x080

#define PWD_WRITABLE_MASK   0x001F7C00U
#define TX_WRITABLE_MASK    0x1FAF2F8FU
#define RX_WRITABLE_MASK    0x00400033U
#define CTRL_SFTRST         (1U << 31)
#define CTRL_CLKGATE        (1U << 30)
#define CTRL_UTMI_SUSPENDM  (1U << 29)
#define CTRL_WRITABLE_MASK  0xD0003EBFU
#define DEBUG_WRITABLE_MASK 0x7F1F1F3FU
#define DEBUG1_WRITABLE_MASK 0x0000700FU
#define STATUS_OTGID_STATUS (1U << 8)

#define PWD_RESET_VALUE     0x001F7C00U
#define TX_RESET_VALUE      0x10060607U
#define RX_RESET_VALUE      0x00000000U
#define CTRL_RESET_VALUE    0xC0000001U
#define DEBUG_RESET_VALUE   0x7F180000U
#define DEBUG0_STATUS_RESET_VALUE 0x0000900DU
#define DEBUG1_RESET_VALUE  0x00001000U

static void usbphy_apply_sct(uint32_t *reg, uint32_t value, int sct)
{
    switch (sct) {
    case 0:
        *reg = value;
        break;
    case 1:
        *reg |= value;
        break;
    case 2:
        *reg &= ~value;
        break;
    case 3:
        *reg ^= value;
        break;
    }
}

static void usbphy_reset_registers(STMP3770USBPHYState *s)
{
    s->pwd = PWD_RESET_VALUE;
    s->tx = TX_RESET_VALUE;
    s->rx = RX_RESET_VALUE;
    s->ctrl = CTRL_RESET_VALUE;
}

static void usbphy_cold_reset(STMP3770USBPHYState *s)
{
    usbphy_reset_registers(s);
    s->status = 0;
    s->debug = DEBUG_RESET_VALUE;
    s->debug1 = DEBUG1_RESET_VALUE;
}

static uint32_t usbphy_ctrl_read(STMP3770USBPHYState *s)
{
    uint32_t ctrl = s->ctrl & ~CTRL_UTMI_SUSPENDM;

    if ((s->pwd & PWD_WRITABLE_MASK) != PWD_WRITABLE_MASK) {
        ctrl |= CTRL_UTMI_SUSPENDM;
    }
    return ctrl;
}

static uint64_t usbphy_read(void *opaque, hwaddr offset, unsigned size)
{
    STMP3770USBPHYState *s = opaque;

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-usbphy: unsupported read size %u at offset "
                      HWADDR_FMT_plx "\n", size, offset);
        return 0;
    }

    switch (offset) {
    case REG_PWD:
        return s->pwd;
    case REG_TX:
        return s->tx;
    case REG_RX:
        return s->rx;
    case REG_CTRL:
        return usbphy_ctrl_read(s);
    case REG_STATUS:
        /* Report a connected B-device (peripheral) */
        return s->status;
    case REG_DEBUG:
        return s->debug;
    case REG_DEBUG0_STATUS:
        return DEBUG0_STATUS_RESET_VALUE;
    case REG_DEBUG1:
        return s->debug1;
    case REG_VERSION:
        return 0x03000000;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-usbphy: read from unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        return 0;
    }
}

static void usbphy_write(void *opaque, hwaddr offset,
                         uint64_t value, unsigned size)
{
    STMP3770USBPHYState *s = opaque;
    int sct = (offset >> 2) & 3;

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-usbphy: unsupported write size %u at offset "
                      HWADDR_FMT_plx "\n", size, offset);
        return;
    }

    if ((offset & ~0xC) == REG_PWD) {
        usbphy_apply_sct(&s->pwd, (uint32_t)value & PWD_WRITABLE_MASK, sct);
        return;
    }

    if ((offset & ~0xC) == REG_TX) {
        usbphy_apply_sct(&s->tx, (uint32_t)value & TX_WRITABLE_MASK, sct);
        return;
    }

    if ((offset & ~0xC) == REG_RX) {
        usbphy_apply_sct(&s->rx, (uint32_t)value & RX_WRITABLE_MASK, sct);
        return;
    }

    if ((offset & ~0xC) == REG_CTRL) {
        usbphy_apply_sct(&s->ctrl, (uint32_t)value & CTRL_WRITABLE_MASK,
                         sct);
        if (s->ctrl & CTRL_SFTRST) {
            usbphy_reset_registers(s);
        }
        return;
    }

    if ((offset & ~0xC) == REG_DEBUG) {
        usbphy_apply_sct(&s->debug, (uint32_t)value & DEBUG_WRITABLE_MASK,
                         sct);
        return;
    }

    if ((offset & ~0xC) == REG_DEBUG1) {
        usbphy_apply_sct(&s->debug1,
                         (uint32_t)value & DEBUG1_WRITABLE_MASK, sct);
        return;
    }

    if (offset == REG_STATUS) {
        s->status = (s->status & ~STATUS_OTGID_STATUS) |
                    ((uint32_t)value & STATUS_OTGID_STATUS);
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "stmp3770-usbphy: write to unimplemented offset "
                  HWADDR_FMT_plx "\n", offset);
}

static const MemoryRegionOps usbphy_ops = {
    .read = usbphy_read,
    .write = usbphy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void usbphy_reset(DeviceState *dev)
{
    STMP3770USBPHYState *s = STMP3770_USBPHY(dev);

    usbphy_cold_reset(s);
}

static void usbphy_realize(DeviceState *dev, Error **errp)
{
    STMP3770USBPHYState *s = STMP3770_USBPHY(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &usbphy_ops, s,
                          TYPE_STMP3770_USBPHY, 0x2000);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_usbphy = {
    .name = "stmp3770-usbphy",
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, STMP3770USBPHYState),
        VMSTATE_UINT32(status, STMP3770USBPHYState),
        VMSTATE_UINT32(debug, STMP3770USBPHYState),
        VMSTATE_UINT32_V(pwd, STMP3770USBPHYState, 2),
        VMSTATE_UINT32_V(tx, STMP3770USBPHYState, 2),
        VMSTATE_UINT32_V(rx, STMP3770USBPHYState, 2),
        VMSTATE_UINT32_V(debug1, STMP3770USBPHYState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void usbphy_init(Object *obj)
{
    STMP3770USBPHYState *s = STMP3770_USBPHY(obj);

    usbphy_cold_reset(s);
}

static void usbphy_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = usbphy_realize;
    device_class_set_legacy_reset(dc, usbphy_reset);
    dc->vmsd = &vmstate_usbphy;
}

static const TypeInfo usbphy_type_info = {
    .name          = TYPE_STMP3770_USBPHY,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STMP3770USBPHYState),
    .instance_init = usbphy_init,
    .class_init    = usbphy_class_init,
};

static void usbphy_register_types(void)
{
    type_register_static(&usbphy_type_info);
}

type_init(usbphy_register_types)
