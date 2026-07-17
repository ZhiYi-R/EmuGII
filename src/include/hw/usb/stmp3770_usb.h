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

/* Device-mode dQH/dTD geometry (ChipIdea ARC USB IP). */
#define USB_DQH_SIZE          64    /* bytes per queue head */
#define USB_DTD_SIZE          32    /* bytes per transfer descriptor */
#define USB_DTD_MAX_PAGES     5     /* page pointers per dTD */
#define USB_DTD_PAGE_SIZE     4096  /* 4 KiB per page pointer */

/* Virtual USB host test-injection registers (offset 0x800+, non-architectural). */
#define REG_VH_ACTION         0x800
#define REG_VH_SETUP_LO       0x804
#define REG_VH_SETUP_HI       0x808
#define REG_VH_OUT_ADDR       0x80C
#define REG_VH_OUT_LEN        0x810
#define REG_VH_IN_STATUS      0x814
#define VH_ACTION_TYPE_SHIFT  5
#define VH_ACTION_TRIGGER     (1U << 31)
#define VH_ACTION_SETUP       0
#define VH_ACTION_IN          1
#define VH_ACTION_OUT         2
#define VH_IN_STATUS_BYTES    0x0000FFFF
#define VH_IN_STATUS_ERROR    (1U << 16)

struct STMP3770USBState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t usbcmd;
    uint32_t usbsts;
    uint32_t usbintr;
    uint32_t frindex;
    uint32_t device_addr;
    uint32_t device_addr_staged;  /* USBADRA staged USBADR value */
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

    /* Virtual USB host test-injection state (non-architectural). */
    uint32_t vh_setup_lo;
    uint32_t vh_setup_hi;
    uint32_t vh_out_addr;
    uint32_t vh_out_len;
    uint32_t vh_in_status;
};

#endif /* STMP3770_USB_H */
