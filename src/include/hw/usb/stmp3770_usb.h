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

#ifndef STMP3770_USB_H
#define STMP3770_USB_H

#include "hw/sysbus.h"
#include "hw/ptimer.h"

#define TYPE_STMP3770_USB "stmp3770-usb"
OBJECT_DECLARE_SIMPLE_TYPE(STMP3770USBState, STMP3770_USB)

#define STMP3770_USB_NUM_ENDPOINTS 5

/* Keep the pre-v3 migration stream layout while exposing only EP0 through EP4. */
#define STMP3770_USB_MIGRATION_ENDPOINTS 8

struct STMP3770USBState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t usbcmd;
    uint32_t usbsts;
    uint32_t usbintr;
    uint32_t frindex;
    uint32_t device_addr;
    uint32_t endptlistaddr;
    uint32_t asynclistaddr;
    uint32_t ttctrl;
    uint32_t burstsize;
    uint32_t txfilltuning;
    uint32_t endptnak;
    uint32_t endptnaken;
    uint32_t portsc1;
    uint32_t otgsc;
    uint32_t usbmode;
    bool usbmode_written;
    uint32_t endptsetupstat;
    uint32_t endptprime;
    uint32_t endptflush;
    uint32_t endptstat;
    uint32_t endptcomplete;
    uint32_t endptctrl[STMP3770_USB_MIGRATION_ENDPOINTS];

    uint32_t gptimer_load[2];
    uint32_t gptimer[2];
    ptimer_state *gptimer_ptimer[2];
    struct STMP3770USBGPTimerCBInfo *gptimer_cb_info;

    ptimer_state *port_reset_ptimer;
    ptimer_state *otgsc_1ms_ptimer;
    ptimer_state *frindex_ptimer;
};

#endif /* STMP3770_USB_H */
