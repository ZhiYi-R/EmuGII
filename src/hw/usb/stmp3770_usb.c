/*
 * STMP3770 USB OTG controller emulation
 *
 * Based on STMP3770 Reference Manual Chapter 18
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
#include "hw/usb/stmp3770_usb.h"

/* Register offsets */
#define REG_ID              0x000
#define REG_HWGENERAL       0x004
#define REG_HWHOST          0x008
#define REG_HWDEVICE        0x00C
#define REG_HWTXBUF         0x010
#define REG_HWRXBUF         0x014
#define REG_GPTIMER0LD      0x080
#define REG_GPTIMER0CTRL    0x084
#define REG_GPTIMER1LD      0x088
#define REG_GPTIMER1CTRL    0x08C
#define REG_SBUSCFG         0x090
#define REG_CAPLENGTH       0x100
#define REG_HCIVERSION      0x102
#define REG_HCSPARAMS       0x104
#define REG_HCCPARAMS       0x108
#define REG_DCIVERSION      0x120
#define REG_DCCPARAMS       0x124
#define REG_USBCMD          0x140
#define REG_USBSTS          0x144
#define REG_USBINTR         0x148
#define REG_FRINDEX         0x14C
#define REG_DEVICEADDR      0x154
#define REG_ENDPTLISTADDR   0x158
#define REG_ASYNCLISTADDR   0x158
#define REG_TTCTRL          0x15C
#define REG_BURSTSIZE       0x160
#define REG_TXFILLTUNING    0x164
#define REG_ULPIVIEWPORT    0x170
#define REG_ENDPTNAK        0x178
#define REG_ENDPTNAKEN      0x17C
#define REG_CONFIGFLAG      0x180
#define REG_PORTSC1         0x184
#define REG_OTGSC           0x1A4
#define REG_USBMODE         0x1A8
#define REG_ENDPTSETUPSTAT  0x1AC
#define REG_ENDPTPRIME      0x1B0
#define REG_ENDPTFLUSH      0x1B4
#define REG_ENDPTSTAT       0x1B8
#define REG_ENDPTCOMPLETE   0x1BC
#define REG_ENDPTCTRL0      0x1C0
#define REG_ENDPTCTRL1      0x1C4
#define REG_ENDPTCTRL2      0x1C8
#define REG_ENDPTCTRL3      0x1CC
#define REG_ENDPTCTRL4      0x1D0

#define USBCMD_RST          (1U << 1)
#define USBCMD_RUN          (1U << 0)
#define USBCMD_WRITABLE_MASK        0x00FFEB7FU

#define USBSTS_W1C_MASK             0x030D05FF
#define USBINTR_WRITABLE_MASK        0x030D05FF
#define USBSTS_URI                  (1U << 6)
#define GPTIMER_RUN                  (1U << 31)
#define GPTIMER_RESET                (1U << 30)
#define GPTIMER_REPEAT               (1U << 24)
#define GPTIMER_CONTROL_WRITABLE_MASK (GPTIMER_RUN | GPTIMER_RESET | GPTIMER_REPEAT)
#define GPTIMER_COUNT_MASK           0x00FFFFFF
#define USBSTS_GPTIMER0              (1U << 24)
#define USBSTS_GPTIMER1              (1U << 25)

#define USBCTRL_USBCMD_DEVICE_RESET     0x00080000
#define USBCTRL_BURSTSIZE_RESET         0x00001010
#define USBCTRL_OTGSC_DEVICE_RESET      0x00000020
#define USBCTRL_USBINTR_WRITABLE_MASK   USBINTR_WRITABLE_MASK
#define USBCTRL_DEVICEADDR_WRITABLE_MASK 0xFF000000
#define USBCTRL_ENDPTLISTADDR_WRITABLE_MASK 0xFFFFF800
#define USBCTRL_TTCTRL_WRITABLE_MASK    0x7F000000
#define USBCTRL_BURSTSIZE_WRITABLE_MASK 0x0000FFFF
#define USBCTRL_TXFILLTUNING_WRITABLE_MASK 0x003F007F
#define USBCTRL_USBMODE_WRITABLE_MASK   0x0000003F
#define USBCTRL_ENDPOINT_BITMAP_MASK     0x001F001F
#define USBCTRL_ENDPTSETUP_MASK          0x0000001F
#define USBCTRL_ENDPTCTRL0_WRITABLE_MASK 0x000D000D
#define USBCTRL_ENDPTCTRL0_RESET         0x00800080
#define USBCTRL_ENDPTCTRLN_ACTION_MASK   0x00400040
#define USBCTRL_ENDPTCTRLN_WRITABLE_MASK 0x00EF00EF

#define USBCTRL_ID_RESET            0x0042FA05
#define USBCTRL_ARC_GENERAL_RESET   0x00000015
#define USBCTRL_HWHOST_RESET        0x10020001
#define USBCTRL_HWDEVICE_RESET      0x0000000B
#define USBCTRL_HWTXBUF_RESET       0x00050810
#define USBCTRL_HWRXBUF_RESET       0x00000610
#define USBCTRL_HCSPARAMS_RESET     0x00010011
#define USBCTRL_HCCPARAMS_RESET     0x00000006
#define USBCTRL_DCCPARAMS_RESET     0x00000185

#define PORTSC1_PP          (1U << 12)
#define PORTSC1_PR          (1U << 8)
#define PORTSC1_PE          (1U << 2)
#define PORTSC1_CCS         (1U << 0)

typedef struct STMP3770USBGPTimerCBInfo {
    STMP3770USBState *s;
    unsigned int idx;
} STMP3770USBGPTimerCBInfo;

static void usb_update_irq(STMP3770USBState *s)
{
    qemu_set_irq(s->irq, (s->usbsts & s->usbintr &
                          USBINTR_WRITABLE_MASK) != 0);
}

static void usb_gptimer_configure(STMP3770USBState *s, unsigned int idx,
                                  bool reload)
{
    ptimer_state *ptimer = s->gptimer_ptimer[idx];
    uint32_t control = s->gptimer[idx];
    uint64_t count;

    ptimer_transaction_begin(ptimer);
    if (!(control & GPTIMER_RUN)) {
        ptimer_stop(ptimer);
    }
    if (reload) {
        ptimer_set_limit(ptimer, (uint64_t)s->gptimer_load[idx] + 1, 1);
        ptimer_set_count(ptimer, (uint64_t)s->gptimer_load[idx] + 1);
    }
    if (control & GPTIMER_RUN) {
        count = ptimer_get_count(ptimer);
        if (reload || count != 0) {
            ptimer_run(ptimer, (control & GPTIMER_REPEAT) == 0);
        } else {
            s->gptimer[idx] &= ~GPTIMER_RUN;
        }
    }
    ptimer_transaction_commit(ptimer);
}

static void usb_gptimer_tick(void *opaque)
{
    STMP3770USBGPTimerCBInfo *info = opaque;
    STMP3770USBState *s = info->s;
    unsigned int idx = info->idx;

    s->usbsts |= USBSTS_GPTIMER0 << idx;
    if (!(s->gptimer[idx] & GPTIMER_REPEAT)) {
        s->gptimer[idx] &= ~GPTIMER_RUN;
    }
    usb_update_irq(s);
}

static void usb_controller_reset(STMP3770USBState *s)
{
    s->usbcmd = USBCTRL_USBCMD_DEVICE_RESET;
    s->usbsts = 0;
    s->usbintr = 0;
    s->frindex = 0;
    s->device_addr = 0;
    s->endptlistaddr = 0;
    s->asynclistaddr = 0;
    s->ttctrl = 0;
    s->burstsize = USBCTRL_BURSTSIZE_RESET;
    s->txfilltuning = 0;
    s->endptnak = 0;
    s->endptnaken = 0;
    s->portsc1 = 0;
    s->otgsc = USBCTRL_OTGSC_DEVICE_RESET;
    s->usbmode = 0;
    s->usbmode_written = false;
    s->endptsetupstat = 0;
    s->endptprime = 0;
    s->endptflush = 0;
    s->endptstat = 0;
    s->endptcomplete = 0;
    memset(s->endptctrl, 0, sizeof(s->endptctrl));
    s->endptctrl[0] = USBCTRL_ENDPTCTRL0_RESET;
    memset(s->gptimer_load, 0, sizeof(s->gptimer_load));
    memset(s->gptimer, 0, sizeof(s->gptimer));
    for (unsigned int i = 0; i < 2; i++) {
        ptimer_transaction_begin(s->gptimer_ptimer[i]);
        ptimer_stop(s->gptimer_ptimer[i]);
        ptimer_set_limit(s->gptimer_ptimer[i], 0, 1);
        ptimer_transaction_commit(s->gptimer_ptimer[i]);
    }
}

static uint64_t usb_read(void *opaque, hwaddr offset, unsigned size)
{
    STMP3770USBState *s = opaque;

    switch (offset) {
    case REG_CAPLENGTH:
        if (size == 1) {
            return 0x40;
        }
        break;
    case REG_HCIVERSION:
        if (size == 2) {
            return 0x0100;
        }
        break;
    case REG_DCIVERSION:
        if (size == 2) {
            return 0x0001;
        }
        break;
    default:
        if (size == 4) {
            break;
        }
        /* Fall through to the common invalid-access report below. */
        break;
    }

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-usb: unsupported read size %u at offset "
                      HWADDR_FMT_plx "\n", size, offset);
        return 0;
    }

    switch (offset) {
    case REG_ID:
        return USBCTRL_ID_RESET;
    case REG_HWGENERAL:
        return USBCTRL_ARC_GENERAL_RESET;
    case REG_HWHOST:
        return USBCTRL_HWHOST_RESET;
    case REG_HWDEVICE:
        return USBCTRL_HWDEVICE_RESET;
    case REG_HWTXBUF:
        return USBCTRL_HWTXBUF_RESET;
    case REG_HWRXBUF:
        return USBCTRL_HWRXBUF_RESET;
    case REG_GPTIMER0LD:
    case REG_GPTIMER1LD:
        return s->gptimer_load[(offset - REG_GPTIMER0LD) / 8];
    case REG_GPTIMER0CTRL:
    case REG_GPTIMER1CTRL:
    {
        unsigned int idx = (offset - REG_GPTIMER0CTRL) / 8;
        uint32_t count;

        ptimer_transaction_begin(s->gptimer_ptimer[idx]);
        count = ptimer_get_count(s->gptimer_ptimer[idx]);
        ptimer_transaction_commit(s->gptimer_ptimer[idx]);
        return (s->gptimer[idx] & (GPTIMER_RUN | GPTIMER_REPEAT)) |
               ((count ? count - 1 : 0) & GPTIMER_COUNT_MASK);
    }
    case REG_SBUSCFG:
        return 0;
    case REG_HCSPARAMS:
        return USBCTRL_HCSPARAMS_RESET;
    case REG_HCCPARAMS:
        return USBCTRL_HCCPARAMS_RESET;
    case REG_DCCPARAMS:
        return USBCTRL_DCCPARAMS_RESET;
    case REG_USBCMD:
        return s->usbcmd;
    case REG_USBSTS:
        return s->usbsts;
    case REG_USBINTR:
        return s->usbintr;
    case REG_FRINDEX:
        return s->frindex;
    case REG_DEVICEADDR:
        return s->device_addr;
    case REG_ENDPTLISTADDR:
        return s->endptlistaddr;
    case REG_TTCTRL:
        return s->ttctrl;
    case REG_BURSTSIZE:
        return s->burstsize;
    case REG_TXFILLTUNING:
        return s->txfilltuning;
    case REG_ULPIVIEWPORT:
        return 0;
    case REG_ENDPTNAK:
        return s->endptnak;
    case REG_ENDPTNAKEN:
        return s->endptnaken;
    case REG_CONFIGFLAG:
        return 0;
    case REG_PORTSC1:
        return s->portsc1;
    case REG_OTGSC:
        return s->otgsc;
    case REG_USBMODE:
        return s->usbmode;
    case REG_ENDPTSETUPSTAT:
        return s->endptsetupstat;
    case REG_ENDPTPRIME:
        return s->endptprime;
    case REG_ENDPTFLUSH:
        return s->endptflush;
    case REG_ENDPTSTAT:
        return s->endptstat;
    case REG_ENDPTCOMPLETE:
        return s->endptcomplete;
    case REG_ENDPTCTRL0 ... REG_ENDPTCTRL4:
        return s->endptctrl[(offset - REG_ENDPTCTRL0) / 4];
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-usb: read from unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        return 0;
    }
}

static void usb_write(void *opaque, hwaddr offset,
                      uint64_t value, unsigned size)
{
    STMP3770USBState *s = opaque;

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-usb: unsupported write size %u at offset "
                      HWADDR_FMT_plx "\n", size, offset);
        return;
    }

    switch (offset) {
    case REG_USBCMD:
        s->usbcmd = (uint32_t)value & USBCMD_WRITABLE_MASK;
        if (s->usbcmd & USBCMD_RST) {
            usb_controller_reset(s);
        }
        usb_update_irq(s);
        break;
    case REG_USBSTS:
        /* Write-1-to-clear */
        s->usbsts &= ~((uint32_t)value & USBSTS_W1C_MASK);
        usb_update_irq(s);
        break;
    case REG_USBINTR:
        s->usbintr = (uint32_t)value & USBCTRL_USBINTR_WRITABLE_MASK;
        usb_update_irq(s);
        break;
    case REG_FRINDEX:
        s->frindex = (uint32_t)value & 0x3FFF;
        break;
    case REG_DEVICEADDR:
        s->device_addr = (uint32_t)value & USBCTRL_DEVICEADDR_WRITABLE_MASK;
        break;
    case REG_ENDPTLISTADDR:
        s->endptlistaddr = (uint32_t)value &
                           USBCTRL_ENDPTLISTADDR_WRITABLE_MASK;
        break;
    case REG_TTCTRL:
        s->ttctrl = (uint32_t)value & USBCTRL_TTCTRL_WRITABLE_MASK;
        break;
    case REG_BURSTSIZE:
        s->burstsize = (uint32_t)value & USBCTRL_BURSTSIZE_WRITABLE_MASK;
        break;
    case REG_TXFILLTUNING:
        s->txfilltuning = (uint32_t)value & USBCTRL_TXFILLTUNING_WRITABLE_MASK;
        break;
    case REG_ENDPTNAK:
        /* Write-1-to-clear */
        s->endptnak &= ~((uint32_t)value & USBCTRL_ENDPOINT_BITMAP_MASK);
        break;
    case REG_ENDPTNAKEN:
        s->endptnaken = (uint32_t)value & USBCTRL_ENDPOINT_BITMAP_MASK;
        break;
    case REG_PORTSC1:
        /* ClearPortFeature / SetPortFeature emulation */
        if (value & PORTSC1_PR) {
            value &= ~PORTSC1_PR;
            s->usbsts |= USBSTS_URI;
            usb_update_irq(s);
        }
        s->portsc1 = (s->portsc1 & ~0x0075002C) | ((uint32_t)value & 0x0075002C);
        s->portsc1 |= PORTSC1_PP | PORTSC1_PE | PORTSC1_CCS;
        break;
    case REG_OTGSC:
        s->otgsc = (uint32_t)value;
        break;
    case REG_USBMODE:
        if (!s->usbmode_written) {
            s->usbmode = (uint32_t)value & USBCTRL_USBMODE_WRITABLE_MASK;
            s->usbmode_written = true;
        }
        break;
    case REG_ENDPTSETUPSTAT:
        s->endptsetupstat &= ~((uint32_t)value & USBCTRL_ENDPTSETUP_MASK);
        break;
    case REG_ENDPTPRIME:
        /* For stub, immediately clear prime and set status */
        s->endptprime = 0;
        s->endptstat |= (uint32_t)value & USBCTRL_ENDPOINT_BITMAP_MASK;
        break;
    case REG_ENDPTFLUSH:
        s->endptflush = 0;
        s->endptstat &= ~((uint32_t)value & USBCTRL_ENDPOINT_BITMAP_MASK);
        break;
    case REG_ENDPTCOMPLETE:
        s->endptcomplete &= ~((uint32_t)value & USBCTRL_ENDPOINT_BITMAP_MASK);
        break;
    case REG_ENDPTCTRL0:
        s->endptctrl[0] = USBCTRL_ENDPTCTRL0_RESET |
                          ((uint32_t)value & USBCTRL_ENDPTCTRL0_WRITABLE_MASK);
        break;
    case REG_ENDPTCTRL1 ... REG_ENDPTCTRL4:
        s->endptctrl[(offset - REG_ENDPTCTRL0) / 4] =
            (uint32_t)value & USBCTRL_ENDPTCTRLN_WRITABLE_MASK;
        s->endptctrl[(offset - REG_ENDPTCTRL0) / 4] &=
            ~USBCTRL_ENDPTCTRLN_ACTION_MASK;
        break;
    case REG_GPTIMER0LD:
    case REG_GPTIMER1LD:
        s->gptimer_load[(offset - REG_GPTIMER0LD) / 8] =
            (uint32_t)value & GPTIMER_COUNT_MASK;
        break;
    case REG_GPTIMER0CTRL:
    case REG_GPTIMER1CTRL:
    {
        unsigned int idx = (offset - REG_GPTIMER0CTRL) / 8;
        uint32_t control = (uint32_t)value;
        bool reload = (control & GPTIMER_RESET) != 0;

        s->gptimer[idx] = control &
                          (GPTIMER_CONTROL_WRITABLE_MASK & ~GPTIMER_RESET);
        usb_gptimer_configure(s, idx, reload);
        break;
    }
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-usb: write to unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        break;
    }
}

static const MemoryRegionOps usb_ops = {
    .read = usb_read,
    .write = usb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void usb_reset(DeviceState *dev)
{
    STMP3770USBState *s = STMP3770_USB(dev);

    usb_controller_reset(s);

    usb_update_irq(s);
}

static void usb_realize(DeviceState *dev, Error **errp)
{
    STMP3770USBState *s = STMP3770_USB(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &usb_ops, s,
                          TYPE_STMP3770_USB, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

}

static void usb_finalize(Object *obj)
{
    STMP3770USBState *s = STMP3770_USB(obj);

    for (unsigned int i = 0; i < 2; i++) {
        ptimer_free(s->gptimer_ptimer[i]);
    }
    g_free(s->gptimer_cb_info);
}

static const VMStateDescription vmstate_usb = {
    .name = "stmp3770-usb",
    .version_id = 4,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(usbcmd, STMP3770USBState),
        VMSTATE_UINT32(usbsts, STMP3770USBState),
        VMSTATE_UINT32(usbintr, STMP3770USBState),
        VMSTATE_UINT32(frindex, STMP3770USBState),
        VMSTATE_UINT32(device_addr, STMP3770USBState),
        VMSTATE_UINT32(endptlistaddr, STMP3770USBState),
        VMSTATE_UINT32(asynclistaddr, STMP3770USBState),
        VMSTATE_UINT32_V(ttctrl, STMP3770USBState, 2),
        VMSTATE_UINT32(burstsize, STMP3770USBState),
        VMSTATE_UINT32(txfilltuning, STMP3770USBState),
        VMSTATE_UINT32(endptnak, STMP3770USBState),
        VMSTATE_UINT32(endptnaken, STMP3770USBState),
        VMSTATE_UINT32(portsc1, STMP3770USBState),
        VMSTATE_UINT32(otgsc, STMP3770USBState),
        VMSTATE_UINT32(usbmode, STMP3770USBState),
        VMSTATE_BOOL_V(usbmode_written, STMP3770USBState, 2),
        VMSTATE_UINT32(endptsetupstat, STMP3770USBState),
        VMSTATE_UINT32(endptprime, STMP3770USBState),
        VMSTATE_UINT32(endptflush, STMP3770USBState),
        VMSTATE_UINT32(endptstat, STMP3770USBState),
        VMSTATE_UINT32(endptcomplete, STMP3770USBState),
        VMSTATE_UINT32_ARRAY(endptctrl, STMP3770USBState,
                             STMP3770_USB_MIGRATION_ENDPOINTS),
        VMSTATE_UINT32_ARRAY_V(gptimer_load, STMP3770USBState, 2, 4),
        VMSTATE_UINT32_ARRAY(gptimer, STMP3770USBState, 2),
        VMSTATE_ARRAY_OF_POINTER_TO_STRUCT(gptimer_ptimer,
                                           STMP3770USBState, 2, 4,
                                           vmstate_ptimer, ptimer_state),
        VMSTATE_END_OF_LIST()
    }
};

static void usb_init(Object *obj)
{
    STMP3770USBState *s = STMP3770_USB(obj);

    s->gptimer_cb_info = g_new0(STMP3770USBGPTimerCBInfo, 2);
    for (unsigned int i = 0; i < 2; i++) {
        s->gptimer_cb_info[i].s = s;
        s->gptimer_cb_info[i].idx = i;
        s->gptimer_ptimer[i] = ptimer_init(usb_gptimer_tick,
                                           &s->gptimer_cb_info[i],
                                           PTIMER_POLICY_NO_COUNTER_ROUND_DOWN |
                                           PTIMER_POLICY_TRIGGER_ONLY_ON_DECREMENT);
        ptimer_transaction_begin(s->gptimer_ptimer[i]);
        ptimer_set_freq(s->gptimer_ptimer[i], 1000000);
        ptimer_set_limit(s->gptimer_ptimer[i], 0, 1);
        ptimer_transaction_commit(s->gptimer_ptimer[i]);
    }
    usb_controller_reset(s);
}

static void usb_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = usb_realize;
    device_class_set_legacy_reset(dc, usb_reset);
    dc->vmsd = &vmstate_usb;
}

static const TypeInfo usb_type_info = {
    .name          = TYPE_STMP3770_USB,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STMP3770USBState),
    .instance_init = usb_init,
    .instance_finalize = usb_finalize,
    .class_init    = usb_class_init,
};

static void usb_register_types(void)
{
    type_register_static(&usb_type_info);
}

type_init(usb_register_types)
