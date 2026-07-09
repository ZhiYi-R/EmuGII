/*
 * STMP3770 Real-Time Clock (RTC)
 *
 * Based on STMP3770 Reference Manual Chapter 19
 *
 * Copyright (C) 2024
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef STMP3770_RTC_H
#define STMP3770_RTC_H

#include "hw/sysbus.h"
#include "hw/ptimer.h"

#define TYPE_STMP3770_RTC "stmp3770-rtc"
OBJECT_DECLARE_SIMPLE_TYPE(STMP3770RTCState, STMP3770_RTC)

#define STMP3770_RTC_NUM_PERSISTENT 6

struct STMP3770RTCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq alarm_irq;
    qemu_irq onemsec_irq;

    /* Registers */
    uint32_t ctrl;
    uint32_t stat;
    uint32_t milliseconds;
    uint32_t seconds;
    uint32_t alarm;
    uint32_t watchdog;
    uint32_t persistent[STMP3770_RTC_NUM_PERSISTENT];
    uint32_t debug;
    uint32_t version;

    /* 1 kHz tick timer (drives ms counter, watchdog, seconds, alarm) */
    ptimer_state *tick;
};

#endif /* STMP3770_RTC_H */
