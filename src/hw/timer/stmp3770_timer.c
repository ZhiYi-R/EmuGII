/*
 * STMP3770 Timers and Rotary Decoder (TIMROT)
 *
 * Based on STMP3770 Reference Manual Chapter 18
 *
 * Features:
 * - Four 16-bit down-counters with reload/oneshot modes
 * - Programmable clock source and prescaler
 * - Interrupt generation on count-to-zero
 * - Rotary decoder registers (minimal stub)
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
#include "hw/timer/stmp3770_timer.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

/* Register offsets */
#define REG_ROTCTRL         0x000
#define REG_ROTCOUNT        0x010
#define REG_TIMCTRL(n)      (0x020 + ((n) * 0x20))
#define REG_TIMCOUNT(n)     (0x030 + ((n) * 0x20))
#define REG_VERSION         0x0A0

/* SET/CLR/TOG sub-offsets */
#define SCT_SET             0x4
#define SCT_CLR             0x8
#define SCT_TOG             0xC

/* ROTCTRL bits */
#define ROTCTRL_SFTRST              (1U << 31)
#define ROTCTRL_CLKGATE             (1U << 30)
#define ROTCTRL_ROTARY_PRESENT      (1U << 29)
#define ROTCTRL_TIM3_PRESENT        (1U << 28)
#define ROTCTRL_TIM2_PRESENT        (1U << 27)
#define ROTCTRL_TIM1_PRESENT        (1U << 26)
#define ROTCTRL_TIM0_PRESENT        (1U << 25)
#define ROTCTRL_PRESENT_MASK        (0x1FU << 25)
#define ROTCTRL_RELATIVE            (1U << 12)
#define ROTCTRL_OVERSAMPLE_SHIFT    10
#define ROTCTRL_OVERSAMPLE_MASK     0x3
#define ROTCTRL_POLARITY_B          (1U << 9)
#define ROTCTRL_POLARITY_A          (1U << 8)
#define ROTCTRL_DIVIDER_SHIFT       16
#define ROTCTRL_DIVIDER_MASK        0x3F
#define ROTCTRL_SELECT_B_SHIFT      4
#define ROTCTRL_SELECT_B_MASK       0x7
#define ROTCTRL_SELECT_A_SHIFT      0
#define ROTCTRL_SELECT_A_MASK       0x7
#define ROTCTRL_WRITABLE_MASK       (ROTCTRL_SFTRST | ROTCTRL_CLKGATE | \
                                     ROTCTRL_RELATIVE | \
                                     (ROTCTRL_OVERSAMPLE_MASK << ROTCTRL_OVERSAMPLE_SHIFT) | \
                                     ROTCTRL_POLARITY_B | ROTCTRL_POLARITY_A | \
                                     (ROTCTRL_DIVIDER_MASK << ROTCTRL_DIVIDER_SHIFT) | \
                                     (ROTCTRL_SELECT_B_MASK << ROTCTRL_SELECT_B_SHIFT) | \
                                     (ROTCTRL_SELECT_A_MASK << ROTCTRL_SELECT_A_SHIFT))

/* TIMCTRL bits */
#define TIMCTRL_IRQ             (1U << 15)
#define TIMCTRL_IRQ_EN          (1U << 14)
#define TIMCTRL_POLARITY        (1U << 8)
#define TIMCTRL_UPDATE          (1U << 7)
#define TIMCTRL_RELOAD          (1U << 6)
#define TIMCTRL_PRESCALE_SHIFT  4
#define TIMCTRL_PRESCALE_MASK   0x3
#define TIMCTRL_SELECT_SHIFT    0
#define TIMCTRL_SELECT_MASK     0xF

/* Timer3 extra bits */
#define TIMCTRL3_TEST_SIGNAL_SHIFT  16
#define TIMCTRL3_TEST_SIGNAL_MASK   0xF
#define TIMCTRL3_DUTY_VALID         (1U << 10)
#define TIMCTRL3_DUTY_CYCLE         (1U << 9)

/* Clock sources */
#define TIMCLK_NEVER        0
#define TIMCLK_PWM0         1
#define TIMCLK_PWM1         2
#define TIMCLK_PWM2         3
#define TIMCLK_PWM3         4
#define TIMCLK_PWM4         5
#define TIMCLK_ROTARYA      6
#define TIMCLK_ROTARYB      7
#define TIMCLK_32KHZ        8
#define TIMCLK_8KHZ         9
#define TIMCLK_4KHZ         10
#define TIMCLK_1KHZ         11
#define TIMCLK_TICK_ALWAYS  12

/* APBX clock frequency used for TICK_ALWAYS / PWM sources */
#define STMP3770_TIMROT_APB_FREQ    24000000

static void stmp3770_timer_reset(DeviceState *dev);

static inline int timctrl_idx_from_base(hwaddr base)
{
    if (base >= REG_TIMCTRL(0) && base < REG_TIMCTRL(STMP3770_NUM_TIMERS)) {
        return (base - REG_TIMCTRL(0)) >> 5;
    }
    return -1;
}

static inline int timcount_idx_from_base(hwaddr base)
{
    if (base >= REG_TIMCOUNT(0) && base < REG_TIMCOUNT(STMP3770_NUM_TIMERS)) {
        return (base - REG_TIMCOUNT(0)) >> 5;
    }
    return -1;
}

static uint32_t stmp3770_timer_get_freq(STMP3770TimerState *s, int idx)
{
    STMP3770TimerChannel *t = &s->timer[idx];
    int select;
    int prescale;
    uint32_t f;

    if (s->rotctrl & (ROTCTRL_SFTRST | ROTCTRL_CLKGATE)) {
        return 0;
    }

    select = (t->timctrl >> TIMCTRL_SELECT_SHIFT) & TIMCTRL_SELECT_MASK;
    prescale = 1 << ((t->timctrl >> TIMCTRL_PRESCALE_SHIFT) & TIMCTRL_PRESCALE_MASK);

    switch (select) {
    case TIMCLK_NEVER:
        return 0;
    case TIMCLK_PWM0:
    case TIMCLK_PWM1:
    case TIMCLK_PWM2:
    case TIMCLK_PWM3:
    case TIMCLK_PWM4:
    case TIMCLK_ROTARYA:
    case TIMCLK_ROTARYB:
        /* PWM and rotary inputs are not modelled */
        return 0;
    case TIMCLK_32KHZ:
        f = 32768;
        break;
    case TIMCLK_8KHZ:
        f = 8192;
        break;
    case TIMCLK_4KHZ:
        f = 4096;
        break;
    case TIMCLK_1KHZ:
        f = 1024;
        break;
    case TIMCLK_TICK_ALWAYS:
        f = STMP3770_TIMROT_APB_FREQ;
        break;
    default:
        return 0;
    }

    return f / prescale;
}

static void stmp3770_timer_update_irq(STMP3770TimerState *s, int idx)
{
    STMP3770TimerChannel *t = &s->timer[idx];
    int level = (t->timctrl & (TIMCTRL_IRQ | TIMCTRL_IRQ_EN)) ==
                (TIMCTRL_IRQ | TIMCTRL_IRQ_EN);
    qemu_set_irq(s->irq[idx], level);
}

static void stmp3770_timer_configure(STMP3770TimerState *s, int idx,
                                     bool reload_count)
{
    STMP3770TimerChannel *t = &s->timer[idx];
    uint32_t freq = stmp3770_timer_get_freq(s, idx);
    uint32_t limit = t->fixed_count;
    bool periodic = (t->timctrl & TIMCTRL_RELOAD) != 0;

    ptimer_transaction_begin(t->ptimer);

    if (!freq) {
        ptimer_stop(t->ptimer);
        t->running = false;
        ptimer_transaction_commit(t->ptimer);
        stmp3770_timer_update_irq(s, idx);
        return;
    }

    ptimer_set_freq(t->ptimer, freq);

    if (reload_count || !t->running) {
        /* Load the running counter from the fixed count */
        ptimer_set_limit(t->ptimer, limit, 1);
    } else {
        /* Preserve the current count while changing limit or frequency */
        uint64_t count = ptimer_get_count(t->ptimer);
        ptimer_set_limit(t->ptimer, limit, 0);
        ptimer_set_count(t->ptimer, count);
    }

    /*
     * A limit of zero with periodic mode would fire continuously.
     * Model it as stopped to avoid interrupt storms.
     */
    if (limit == 0 && periodic) {
        ptimer_stop(t->ptimer);
        t->running = false;
    } else {
        ptimer_run(t->ptimer, periodic ? 0 : 1);
        t->running = true;
    }

    ptimer_transaction_commit(t->ptimer);
    stmp3770_timer_update_irq(s, idx);
}

static void stmp3770_timer_tick(void *opaque)
{
    STMP3770TimerCBInfo *info = opaque;
    STMP3770TimerState *s = info->s;
    int idx = info->idx;
    STMP3770TimerChannel *t = &s->timer[idx];

    t->timctrl |= TIMCTRL_IRQ;
    /* For oneshot mode the ptimer stops automatically */
    t->running = (t->timctrl & TIMCTRL_RELOAD) != 0;
    stmp3770_timer_update_irq(s, idx);
}

static void stmp3770_timer_apply_write(uint32_t *reg, uint32_t mask,
                                       uint32_t value, int sct)
{
    switch (sct) {
    case SCT_SET:
        *reg |= (value & mask);
        break;
    case SCT_CLR:
        *reg &= ~(value & mask);
        break;
    case SCT_TOG:
        *reg ^= (value & mask);
        break;
    default:
        *reg = (value & mask) | (*reg & ~mask);
        break;
    }
}

static uint64_t stmp3770_timer_read(void *opaque, hwaddr offset, unsigned size)
{
    STMP3770TimerState *s = STMP3770_TIMER(opaque);
    hwaddr base = offset & ~0xF;
    int idx;

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-timer: unsupported read size %u at "
                      HWADDR_FMT_plx "\n", size, offset);
        return 0;
    }

    switch (base) {
    case REG_ROTCTRL:
        return s->rotctrl | ROTCTRL_PRESENT_MASK;

    case REG_ROTCOUNT:
        return s->rotcount;

    case REG_VERSION:
        return s->version;

    default:
        break;
    }

    idx = timctrl_idx_from_base(base);
    if (idx >= 0 && idx < STMP3770_NUM_TIMERS) {
        return s->timer[idx].timctrl;
    }

    idx = timcount_idx_from_base(base);
    if (idx >= 0 && idx < STMP3770_NUM_TIMERS) {
        STMP3770TimerChannel *t = &s->timer[idx];
        uint32_t running;

        ptimer_transaction_begin(t->ptimer);
        running = (uint32_t)ptimer_get_count(t->ptimer);
        ptimer_transaction_commit(t->ptimer);

        return ((running & 0xFFFF) << 16) | (t->fixed_count & 0xFFFF);
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "stmp3770-timer: read from unimplemented offset "
                  HWADDR_FMT_plx "\n", offset);
    return 0;
}

static void stmp3770_timer_write_rotctrl(STMP3770TimerState *s,
                                         int sct, uint32_t value)
{
    uint32_t old = s->rotctrl;

    stmp3770_timer_apply_write(
        &s->rotctrl, ROTCTRL_WRITABLE_MASK, value, sct);

    /*
     * STMP3770 SFTRST protocol: when software writes SFTRST=1, hardware
     * resets the module and automatically sets CLKGATE=1. Software polls
     * CLKGATE until it reads 1, then clears both SFTRST and CLKGATE.
     *
     * Emulation: trigger reset when SFTRST transitions 0->1, then force
     * CLKGATE=1 so firmware sees the expected state.
     */
    if (!(old & ROTCTRL_SFTRST) && (s->rotctrl & ROTCTRL_SFTRST)) {
        stmp3770_timer_reset(DEVICE(s));
        /* After reset, ROTCTRL = SFTRST|CLKGATE|PRESENT. Preserve the
         * SFTRST bit that firmware just wrote, so next read sees both set. */
        s->rotctrl |= ROTCTRL_SFTRST;
    }

    if ((old ^ s->rotctrl) & ROTCTRL_CLKGATE) {
        int i;
        for (i = 0; i < STMP3770_NUM_TIMERS; i++) {
            stmp3770_timer_configure(s, i, false);
        }
    }
}

static void stmp3770_timer_write_timctrl(STMP3770TimerState *s, int idx,
                                         int sct, uint32_t value)
{
    STMP3770TimerChannel *t = &s->timer[idx];
    uint32_t writable_mask;
    uint32_t old = t->timctrl;

    if (idx == 3) {
        writable_mask = TIMCTRL_IRQ | TIMCTRL_IRQ_EN |
                        TIMCTRL_POLARITY | TIMCTRL_UPDATE | TIMCTRL_RELOAD |
                        (TIMCTRL_PRESCALE_MASK << TIMCTRL_PRESCALE_SHIFT) |
                        (TIMCTRL_SELECT_MASK << TIMCTRL_SELECT_SHIFT) |
                        TIMCTRL3_DUTY_CYCLE |
                        (TIMCTRL3_TEST_SIGNAL_MASK << TIMCTRL3_TEST_SIGNAL_SHIFT);
    } else {
        writable_mask = TIMCTRL_IRQ | TIMCTRL_IRQ_EN |
                        TIMCTRL_POLARITY | TIMCTRL_UPDATE | TIMCTRL_RELOAD |
                        (TIMCTRL_PRESCALE_MASK << TIMCTRL_PRESCALE_SHIFT) |
                        (TIMCTRL_SELECT_MASK << TIMCTRL_SELECT_SHIFT);
    }

    stmp3770_timer_apply_write(&t->timctrl, writable_mask, value, sct);

    /* Treat a 0->1 transition of UPDATE as an immediate reload request */
    if (!(old & TIMCTRL_UPDATE) && (t->timctrl & TIMCTRL_UPDATE)) {
        stmp3770_timer_configure(s, idx, true);
    } else if ((old ^ t->timctrl) & (TIMCTRL_SELECT_MASK |
                                      (TIMCTRL_PRESCALE_MASK << TIMCTRL_PRESCALE_SHIFT) |
                                      TIMCTRL_RELOAD | TIMCTRL_IRQ_EN)) {
        stmp3770_timer_configure(s, idx, false);
    }

    stmp3770_timer_update_irq(s, idx);
}

static void stmp3770_timer_write_timcount(STMP3770TimerState *s, int idx,
                                          uint32_t value)
{
    STMP3770TimerChannel *t = &s->timer[idx];

    /* Only the lower 16 bits (FIXED_COUNT) are writable */
    t->fixed_count = value & 0xFFFF;

    if (t->timctrl & TIMCTRL_UPDATE) {
        stmp3770_timer_configure(s, idx, true);
    } else if (t->timctrl & TIMCTRL_RELOAD) {
        stmp3770_timer_configure(s, idx, false);
    }
}

static void stmp3770_timer_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    STMP3770TimerState *s = STMP3770_TIMER(opaque);
    hwaddr base = offset & ~0xF;
    int sct = offset & 0xF;
    int idx;

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-timer: unsupported write size %u at "
                      HWADDR_FMT_plx "\n", size, offset);
        return;
    }

    if (base == REG_ROTCTRL) {
        stmp3770_timer_write_rotctrl(s, sct, (uint32_t)value);
        return;
    }

    if (base == REG_ROTCOUNT) {
        /* ROTCOUNT is read-only */
        return;
    }

    idx = timctrl_idx_from_base(base);
    if (idx >= 0 && idx < STMP3770_NUM_TIMERS) {
        stmp3770_timer_write_timctrl(s, idx, sct, (uint32_t)value);
        return;
    }

    idx = timcount_idx_from_base(base);
    if (idx >= 0 && idx < STMP3770_NUM_TIMERS) {
        stmp3770_timer_write_timcount(s, idx, (uint32_t)value);
        return;
    }

    if (base == REG_VERSION) {
        /* Version register is read-only */
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "stmp3770-timer: write to unimplemented offset "
                  HWADDR_FMT_plx "\n", offset);
}

static const MemoryRegionOps stmp3770_timer_ops = {
    .read = stmp3770_timer_read,
    .write = stmp3770_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void stmp3770_timer_reset(DeviceState *dev)
{
    STMP3770TimerState *s = STMP3770_TIMER(dev);
    int i;

    s->rotctrl = ROTCTRL_SFTRST | ROTCTRL_CLKGATE | ROTCTRL_PRESENT_MASK;
    s->rotcount = 0;

    for (i = 0; i < STMP3770_NUM_TIMERS; i++) {
        STMP3770TimerChannel *t = &s->timer[i];

        ptimer_transaction_begin(t->ptimer);
        ptimer_stop(t->ptimer);
        ptimer_set_limit(t->ptimer, 0, 1);
        ptimer_transaction_commit(t->ptimer);

        t->timctrl = 0;
        t->fixed_count = 0;
        t->running = false;
        stmp3770_timer_update_irq(s, i);
    }
}

static void stmp3770_timer_init(Object *obj)
{
    STMP3770TimerState *s = STMP3770_TIMER(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    int i;

    memory_region_init_io(
        &s->iomem, obj, &stmp3770_timer_ops, s,
        TYPE_STMP3770_TIMER, 0x2000);
    sysbus_init_mmio(sbd, &s->iomem);

    for (i = 0; i < STMP3770_NUM_TIMERS; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
        s->cb_info[i].s = s;
        s->cb_info[i].idx = i;
        s->timer[i].ptimer = ptimer_init(stmp3770_timer_tick,
                                         &s->cb_info[i], PTIMER_POLICY_LEGACY);
    }

    s->version = 0x01010000; /* TIMROT Block v1.1 */
}

static const VMStateDescription vmstate_stmp3770_timer_channel = {
    .name = "stmp3770-timer-channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(timctrl, STMP3770TimerChannel),
        VMSTATE_UINT32(fixed_count, STMP3770TimerChannel),
        VMSTATE_PTIMER(ptimer, STMP3770TimerChannel),
        VMSTATE_BOOL(running, STMP3770TimerChannel),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_stmp3770_timer = {
    .name = TYPE_STMP3770_TIMER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(rotctrl, STMP3770TimerState),
        VMSTATE_UINT32(rotcount, STMP3770TimerState),
        VMSTATE_UINT32(version, STMP3770TimerState),
        VMSTATE_STRUCT_ARRAY(timer, STMP3770TimerState, STMP3770_NUM_TIMERS,
                             0, vmstate_stmp3770_timer_channel,
                             STMP3770TimerChannel),
        VMSTATE_END_OF_LIST()
    }
};

static void stmp3770_timer_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, stmp3770_timer_reset);
    dc->vmsd = &vmstate_stmp3770_timer;
}

static const TypeInfo stmp3770_timer_type_info = {
    .name          = TYPE_STMP3770_TIMER,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STMP3770TimerState),
    .instance_init = stmp3770_timer_init,
    .class_init    = stmp3770_timer_class_init,
};

static void stmp3770_timer_register_types(void)
{
    type_register_static(&stmp3770_timer_type_info);
}

type_init(stmp3770_timer_register_types)
