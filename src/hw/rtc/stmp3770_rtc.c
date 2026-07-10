/*
 * STMP3770 Real-Time Clock (RTC)
 *
 * Based on STMP3770 Reference Manual Chapter 19
 *
 * Features:
 * - 32-bit seconds and milliseconds counters
 * - Alarm match interrupt
 * - 1 ms periodic interrupt
 * - Watchdog timer (guest reset when it expires)
 * - Persistent registers
 * - SET/CLR/TOG register variants
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
#include "hw/rtc/stmp3770_rtc.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/runstate.h"

/* Register offsets */
#define REG_CTRL            0x000
#define REG_STAT            0x010
#define REG_MILLISECONDS    0x020
#define REG_SECONDS         0x030
#define REG_ALARM           0x040
#define REG_WATCHDOG        0x050
#define REG_PERSISTENT0     0x060
#define REG_PERSISTENT1     0x070
#define REG_PERSISTENT2     0x080
#define REG_PERSISTENT3     0x090
#define REG_PERSISTENT4     0x0A0
#define REG_PERSISTENT5     0x0B0
#define REG_DEBUG           0x0C0
#define REG_VERSION         0x0D0

/* SET/CLR/TOG sub-offsets */
#define SCT_SET             0x4
#define SCT_CLR             0x8
#define SCT_TOG             0xC

/* CTRL bits */
#define CTRL_SFTRST                 (1U << 31)
#define CTRL_CLKGATE                (1U << 30)
#define CTRL_SUPPRESS_COPY2ANALOG   (1U << 6)
#define CTRL_FORCE_UPDATE           (1U << 5)
#define CTRL_WATCHDOGEN             (1U << 4)
#define CTRL_ONEMSEC_IRQ            (1U << 3)
#define CTRL_ALARM_IRQ              (1U << 2)
#define CTRL_ONEMSEC_IRQ_EN         (1U << 1)
#define CTRL_ALARM_IRQ_EN           (1U << 0)
#define CTRL_WRITABLE_MASK          (CTRL_SFTRST | CTRL_CLKGATE | \
                                     CTRL_SUPPRESS_COPY2ANALOG | \
                                     CTRL_FORCE_UPDATE | CTRL_WATCHDOGEN | \
                                     CTRL_ONEMSEC_IRQ | CTRL_ALARM_IRQ | \
                                     CTRL_ONEMSEC_IRQ_EN | CTRL_ALARM_IRQ_EN)

/* STAT bits */
#define STAT_RTC_PRESENT            (1U << 31)
#define STAT_ALARM_PRESENT          (1U << 30)
#define STAT_WATCHDOG_PRESENT       (1U << 29)
#define STAT_STALE_REGS_SHIFT       16
#define STAT_STALE_REGS_MASK        0xFF
#define STAT_NEW_REGS_SHIFT         8
#define STAT_NEW_REGS_MASK          0xFF

/* PERSISTENT0 bits */
#define PERSISTENT0_ALARM_WAKE      (1U << 7)
#define PERSISTENT0_XTAL32_FREQ     (1U << 6)
#define PERSISTENT0_XTAL32KHZ_PWRUP (1U << 5)
#define PERSISTENT0_XTAL24MHZ_PWRUP (1U << 4)
#define PERSISTENT0_LCK_SECS        (1U << 3)
#define PERSISTENT0_ALARM_EN        (1U << 2)
#define PERSISTENT0_ALARM_WAKE_EN   (1U << 1)
#define PERSISTENT0_CLOCKSOURCE     (1U << 0)
#define PERSISTENT0_WRITABLE_MASK   0xFFFFFFFFU

static inline bool stmp3770_rtc_enabled(STMP3770RTCState *s)
{
    return (s->ctrl & (CTRL_SFTRST | CTRL_CLKGATE)) == 0;
}

static void stmp3770_rtc_update_irq(STMP3770RTCState *s)
{
    bool alarm = (s->ctrl & CTRL_ALARM_IRQ) &&
                 (s->ctrl & CTRL_ALARM_IRQ_EN);
    bool msec = (s->ctrl & CTRL_ONEMSEC_IRQ) &&
                (s->ctrl & CTRL_ONEMSEC_IRQ_EN);

    qemu_set_irq(s->alarm_irq, alarm);
    qemu_set_irq(s->onemsec_irq, msec);
}

static void stmp3770_rtc_check_alarm(STMP3770RTCState *s)
{
    if ((s->persistent[0] & PERSISTENT0_ALARM_EN) &&
        s->seconds == s->alarm) {
        s->ctrl |= CTRL_ALARM_IRQ;
        s->persistent[0] |= PERSISTENT0_ALARM_WAKE;
        stmp3770_rtc_update_irq(s);
    }
}

static void stmp3770_rtc_tick(void *opaque)
{
    STMP3770RTCState *s = STMP3770_RTC(opaque);

    if (!stmp3770_rtc_enabled(s)) {
        return;
    }

    s->milliseconds++;

    if (s->ctrl & CTRL_ONEMSEC_IRQ_EN) {
        s->ctrl |= CTRL_ONEMSEC_IRQ;
    }

    if ((s->milliseconds % 1000) == 0) {
        s->seconds++;
        stmp3770_rtc_check_alarm(s);
    }

    if (s->ctrl & CTRL_ONEMSEC_IRQ_EN) {
        stmp3770_rtc_update_irq(s);
    }

    if ((s->ctrl & CTRL_WATCHDOGEN) && s->watchdog != 0) {
        s->watchdog--;
        if (s->watchdog == 0) {
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        }
    }
}

static uint64_t rtc_read_subword(uint32_t value, hwaddr offset, unsigned size)
{
    unsigned shift = (offset & 3) * 8;
    uint32_t mask = (size >= 4) ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1u);

    return (value >> shift) & mask;
}

static void stmp3770_rtc_apply_write(uint32_t *reg, uint32_t mask,
                                     uint32_t value, int sct,
                                     hwaddr offset, unsigned size)
{
    unsigned shift = (offset & 3) * 8;
    uint32_t smask = (size >= 4) ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1u);
    uint32_t full_mask = smask << shift;
    uint32_t cur = (*reg >> shift) & smask;

    value &= mask;
    switch (sct) {
    case SCT_SET:
        cur |= value;
        break;
    case SCT_CLR:
        cur &= ~value;
        break;
    case SCT_TOG:
        cur ^= value;
        break;
    default:
        cur = (value & mask) | (cur & ~mask);
        break;
    }

    *reg = (*reg & ~full_mask) | ((cur & smask) << shift);
}

static void stmp3770_rtc_rearm(STMP3770RTCState *s)
{
    ptimer_transaction_begin(s->tick);
    if (stmp3770_rtc_enabled(s)) {
        ptimer_run(s->tick, 0);
    } else {
        ptimer_stop(s->tick);
    }
    ptimer_transaction_commit(s->tick);
}

static uint64_t stmp3770_rtc_read(void *opaque, hwaddr offset, unsigned size)
{
    STMP3770RTCState *s = STMP3770_RTC(opaque);
    hwaddr base = offset & ~0xF;
    int idx;
    uint32_t ret;

    switch (base) {
    case REG_CTRL:
        ret = s->ctrl;
        break;

    case REG_STAT:
        ret = s->stat | STAT_RTC_PRESENT | STAT_ALARM_PRESENT |
              STAT_WATCHDOG_PRESENT;
        break;

    case REG_MILLISECONDS:
        ret = s->milliseconds;
        break;

    case REG_SECONDS:
        ret = s->seconds;
        break;

    case REG_ALARM:
        ret = s->alarm;
        break;

    case REG_WATCHDOG:
        ret = s->watchdog;
        break;

    case REG_DEBUG:
        ret = s->debug;
        break;

    case REG_VERSION:
        ret = s->version;
        break;

    default:
        ret = 0;
        break;
    }

    if (base >= REG_PERSISTENT0 &&
        base < REG_PERSISTENT0 + (STMP3770_RTC_NUM_PERSISTENT * 0x10)) {
        idx = (base - REG_PERSISTENT0) >> 4;
        ret = s->persistent[idx];
    }

    if (base != REG_CTRL && base != REG_STAT && base != REG_MILLISECONDS &&
        base != REG_SECONDS && base != REG_ALARM && base != REG_WATCHDOG &&
        base != REG_DEBUG && base != REG_VERSION &&
        !(base >= REG_PERSISTENT0 &&
          base < REG_PERSISTENT0 + (STMP3770_RTC_NUM_PERSISTENT * 0x10))) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-rtc: read from unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
    }

    return rtc_read_subword(ret, offset, size);
}

static void stmp3770_rtc_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    STMP3770RTCState *s = STMP3770_RTC(opaque);
    hwaddr base = offset & ~0xF;
    int sct = offset & 0xF;
    int idx;
    uint32_t old_ctrl;

    switch (base) {
    case REG_CTRL:
        old_ctrl = s->ctrl;
        stmp3770_rtc_apply_write(&s->ctrl, CTRL_WRITABLE_MASK, (uint32_t)value,
                                sct, offset, size);
        /*
         * Hardware ties CLKGATE to SFTRST: asserting reset automatically
         * gates the clock, so firmware polls CLKGATE after setting SFTRST.
         */
        if (s->ctrl & CTRL_SFTRST) {
            s->ctrl |= CTRL_CLKGATE;
        } else {
            s->ctrl &= ~CTRL_CLKGATE;
        }
        if ((old_ctrl & CTRL_FORCE_UPDATE) == 0 &&
            (s->ctrl & CTRL_FORCE_UPDATE)) {
            /* Force-update completes immediately in this model */
            s->ctrl &= ~CTRL_FORCE_UPDATE;
        }
        stmp3770_rtc_rearm(s);
        stmp3770_rtc_update_irq(s);
        return;

    case REG_MILLISECONDS:
        stmp3770_rtc_apply_write(&s->milliseconds, 0xFFFFFFFF, (uint32_t)value,
                                sct, offset, size);
        return;

    case REG_SECONDS:
        if (!(s->persistent[0] & PERSISTENT0_LCK_SECS)) {
            stmp3770_rtc_apply_write(&s->seconds, 0xFFFFFFFF, (uint32_t)value,
                                    sct, offset, size);
            stmp3770_rtc_check_alarm(s);
        }
        return;

    case REG_ALARM:
        stmp3770_rtc_apply_write(&s->alarm, 0xFFFFFFFF, (uint32_t)value,
                                sct, offset, size);
        stmp3770_rtc_check_alarm(s);
        return;

    case REG_WATCHDOG:
        stmp3770_rtc_apply_write(&s->watchdog, 0xFFFFFFFF, (uint32_t)value,
                                sct, offset, size);
        return;

    case REG_DEBUG:
        stmp3770_rtc_apply_write(&s->debug, 0x3, (uint32_t)value,
                                sct, offset, size);
        return;

    default:
        break;
    }

    if (base >= REG_PERSISTENT0 &&
        base < REG_PERSISTENT0 + (STMP3770_RTC_NUM_PERSISTENT * 0x10)) {
        idx = (base - REG_PERSISTENT0) >> 4;
        if (idx == 0) {
            uint32_t old = s->persistent[0];
            stmp3770_rtc_apply_write(&s->persistent[0],
                                     PERSISTENT0_WRITABLE_MASK,
                                     (uint32_t)value, sct, offset, size);
            /* LCK_SECS is sticky once written */
            if (old & PERSISTENT0_LCK_SECS) {
                s->persistent[0] |= PERSISTENT0_LCK_SECS;
            }
        } else {
            stmp3770_rtc_apply_write(&s->persistent[idx], 0xFFFFFFFF,
                                     (uint32_t)value, sct, offset, size);
        }
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "stmp3770-rtc: write to unimplemented offset "
                  HWADDR_FMT_plx "\n", offset);
}

static const MemoryRegionOps stmp3770_rtc_ops = {
    .read = stmp3770_rtc_read,
    .write = stmp3770_rtc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void stmp3770_rtc_reset(DeviceState *dev)
{
    STMP3770RTCState *s = STMP3770_RTC(dev);

    ptimer_transaction_begin(s->tick);
    ptimer_stop(s->tick);
    ptimer_transaction_commit(s->tick);

    s->ctrl = CTRL_SFTRST | CTRL_CLKGATE | CTRL_FORCE_UPDATE;
    s->stat = STAT_STALE_REGS_MASK << STAT_STALE_REGS_SHIFT;
    s->milliseconds = 0;
    s->seconds = 0;
    s->alarm = 0;
    s->watchdog = 0xFFFFFFFF;
    memset(s->persistent, 0, sizeof(s->persistent));
    s->persistent[0] = 0x100; /* MSEC_RES = 1 ms */
    s->debug = 0;
    s->version = 0x02000000; /* RTC Block v2.0 */
    stmp3770_rtc_update_irq(s);
}

static void stmp3770_rtc_init(Object *obj)
{
    STMP3770RTCState *s = STMP3770_RTC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &stmp3770_rtc_ops, s,
        TYPE_STMP3770_RTC, 0x2000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->alarm_irq);
    sysbus_init_irq(sbd, &s->onemsec_irq);

    s->tick = ptimer_init(stmp3770_rtc_tick, s, PTIMER_POLICY_LEGACY);
    ptimer_transaction_begin(s->tick);
    ptimer_set_freq(s->tick, 1000);
    ptimer_set_limit(s->tick, 1, 1);
    ptimer_transaction_commit(s->tick);
}

static const VMStateDescription vmstate_stmp3770_rtc = {
    .name = TYPE_STMP3770_RTC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, STMP3770RTCState),
        VMSTATE_UINT32(stat, STMP3770RTCState),
        VMSTATE_UINT32(milliseconds, STMP3770RTCState),
        VMSTATE_UINT32(seconds, STMP3770RTCState),
        VMSTATE_UINT32(alarm, STMP3770RTCState),
        VMSTATE_UINT32(watchdog, STMP3770RTCState),
        VMSTATE_UINT32_ARRAY(persistent, STMP3770RTCState,
                             STMP3770_RTC_NUM_PERSISTENT),
        VMSTATE_UINT32(debug, STMP3770RTCState),
        VMSTATE_UINT32(version, STMP3770RTCState),
        VMSTATE_PTIMER(tick, STMP3770RTCState),
        VMSTATE_END_OF_LIST()
    }
};

static void stmp3770_rtc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, stmp3770_rtc_reset);
    dc->vmsd = &vmstate_stmp3770_rtc;
}

static const TypeInfo stmp3770_rtc_type_info = {
    .name          = TYPE_STMP3770_RTC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STMP3770RTCState),
    .instance_init = stmp3770_rtc_init,
    .class_init    = stmp3770_rtc_class_init,
};

static void stmp3770_rtc_register_types(void)
{
    type_register_static(&stmp3770_rtc_type_info);
}

type_init(stmp3770_rtc_register_types)
