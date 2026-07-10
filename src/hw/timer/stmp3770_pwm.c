/*
 * STMP3770 PWM controller emulation
 *
 * Based on STMP3770 Reference Manual Chapter 20
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
#include "hw/gpio/stmp3770_pinctrl.h"
#include "hw/timer/stmp3770_pwm.h"

#define PWM_VERSION     0x01010000

#define REG_CTRL0       0x000
#define REG_CTRL0_SET   0x004
#define REG_CTRL0_CLR   0x008
#define REG_CTRL0_TOG   0x00C
#define REG_ACTIVE(ch)  (0x010 + ((ch) * 0x20))
#define REG_PERIOD(ch)  (0x020 + ((ch) * 0x20))
#define REG_VERSION     0x0B0

#define CTRL0_SFTRST    (1U << 31)
#define CTRL0_CLKGATE   (1U << 30)
#define CTRL0_PRESENT   (0x1FU << 25)
#define CTRL0_RW_MASK   (CTRL0_SFTRST | CTRL0_CLKGATE | 0x3FU)
#define CTRL0_RESET     (CTRL0_SFTRST | CTRL0_CLKGATE | CTRL0_PRESENT)
#define PERIOD_RW_MASK  0x00FFFFFF
#define CTRL0_PWM_ENABLE(chan) (1U << (chan))

#define PWM_CLOCK_HZ 24000000
#define PERIOD_MATT  (1U << 23)
#define PERIOD_CDIV_SHIFT 20
#define PERIOD_ACTIVE_STATE_SHIFT 16
#define PERIOD_INACTIVE_STATE_SHIFT 18

static const uint16_t pwm_dividers[] = {
    1, 2, 4, 8, 16, 64, 256, 1024,
};

static void pwm_reset(DeviceState *dev);
static void pwm_rearm_channel(STMP3770PWMState *s, unsigned ch);
static int pwm_post_load(void *opaque, int version_id);

static uint8_t pwm_state_to_level(uint32_t state)
{
    switch (state) {
    case 2:
        return 0;
    case 3:
        return 1;
    default:
        return STMP3770_PINCTRL_PWM_HI_Z;
    }
}

static bool pwm_channel_enabled(const STMP3770PWMState *s, unsigned ch)
{
    return !(s->ctrl0 & (CTRL0_SFTRST | CTRL0_CLKGATE)) &&
           (s->ctrl0 & CTRL0_PWM_ENABLE(ch));
}

static void pwm_update_output(STMP3770PWMState *s, unsigned ch)
{
    STMP3770PWMChannel *channel = &s->channel[ch];
    uint32_t active;
    uint32_t inactive;
    uint32_t state;
    uint8_t level = STMP3770_PINCTRL_PWM_HI_Z;

    if (pwm_channel_enabled(s, ch) && channel->latched) {
        if (channel->latched_period & PERIOD_MATT) {
            /* MATT uses the 24 MHz crystal path, not the PWM phase logic. */
            level = STMP3770_PINCTRL_PWM_HI_Z;
        } else {
            active = channel->latched_active & 0xffff;
            inactive = channel->latched_active >> 16;
            state = (channel->counter >= active &&
                     channel->counter <= inactive) ?
                    (channel->latched_period >> PERIOD_ACTIVE_STATE_SHIFT) :
                    (channel->latched_period >> PERIOD_INACTIVE_STATE_SHIFT);
            level = pwm_state_to_level(state & 0x3);
        }
    }

    if (s->pinctrl) {
        stmp3770_pinctrl_set_pwm_output(s->pinctrl, ch, level);
    }
}

static void pwm_latch_channel(STMP3770PWMState *s, unsigned ch,
                              bool in_ptimer_callback)
{
    STMP3770PWMChannel *channel = &s->channel[ch];
    uint32_t cdiv = (channel->pending_period >> PERIOD_CDIV_SHIFT) & 0x7;

    channel->latched_active = channel->pending_active;
    channel->latched_period = channel->pending_period;
    channel->pending = false;
    channel->latched = true;
    channel->counter = 0;

    if (!in_ptimer_callback) {
        ptimer_transaction_begin(channel->ptimer);
    }
    ptimer_set_freq(channel->ptimer, PWM_CLOCK_HZ / pwm_dividers[cdiv]);
    ptimer_set_limit(channel->ptimer, 1, 1);
    if (!in_ptimer_callback) {
        ptimer_transaction_commit(channel->ptimer);
    }
}

static void pwm_channel_tick(void *opaque)
{
    STMP3770PWMCallbackInfo *info = opaque;
    STMP3770PWMState *s = info->s;
    unsigned ch = info->channel;
    STMP3770PWMChannel *channel = &s->channel[ch];
    uint16_t period;

    if (!pwm_channel_enabled(s, ch) || !channel->running ||
        !channel->latched) {
        return;
    }

    period = channel->latched_period & 0xffff;
    if (channel->counter >= period) {
        if (channel->pending) {
            pwm_latch_channel(s, ch, true);
        } else {
            channel->counter = 0;
        }
    } else {
        channel->counter++;
    }

    pwm_update_output(s, ch);
}

static void pwm_stop_channel(STMP3770PWMState *s, unsigned ch,
                             bool preserve_phase)
{
    STMP3770PWMChannel *channel = &s->channel[ch];

    ptimer_transaction_begin(channel->ptimer);
    ptimer_stop(channel->ptimer);
    if (!preserve_phase) {
        ptimer_set_limit(channel->ptimer, 1, 1);
        channel->counter = 0;
        channel->running = false;
    }
    ptimer_transaction_commit(channel->ptimer);

    if (!preserve_phase) {
        pwm_update_output(s, ch);
    }
}

static void pwm_rearm_channel(STMP3770PWMState *s, unsigned ch)
{
    STMP3770PWMChannel *channel = &s->channel[ch];

    if (s->ctrl0 & CTRL0_SFTRST) {
        pwm_stop_channel(s, ch, false);
        return;
    }
    if (s->ctrl0 & CTRL0_CLKGATE) {
        if (channel->running) {
            pwm_stop_channel(s, ch, true);
        }
        return;
    }
    if (!(s->ctrl0 & CTRL0_PWM_ENABLE(ch)) || !channel->latched) {
        pwm_stop_channel(s, ch, false);
        return;
    }

    ptimer_transaction_begin(channel->ptimer);
    ptimer_run(channel->ptimer, 0);
    ptimer_transaction_commit(channel->ptimer);
    channel->running = true;
    pwm_update_output(s, ch);
}

static void pwm_rearm_all(STMP3770PWMState *s)
{
    unsigned ch;

    for (ch = 0; ch < STMP3770_PWM_NUM_CHANNELS; ch++) {
        pwm_rearm_channel(s, ch);
    }
}

static uint32_t pwm_apply_sct(uint32_t current, uint32_t value, int sct)
{
    switch (sct) {
    case 0:
        return value;
    case 1:
        return current | value;
    case 2:
        return current & ~value;
    case 3:
        return current ^ value;
    }

    g_assert_not_reached();
}

static uint64_t pwm_read(void *opaque, hwaddr offset, unsigned size)
{
    STMP3770PWMState *s = opaque;
    int ch;

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-pwm: unsupported read size %u at offset "
                      HWADDR_FMT_plx "\n", size, offset);
        return 0;
    }

    if (offset == REG_CTRL0) {
        return s->ctrl0;
    }
    if (offset == REG_VERSION) {
        return PWM_VERSION;
    }

    for (ch = 0; ch < STMP3770_PWM_NUM_CHANNELS; ch++) {
        if (offset == REG_PERIOD(ch)) {
            return s->period[ch];
        }
        if (offset == REG_ACTIVE(ch)) {
            return s->active[ch];
        }
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "stmp3770-pwm: read from unimplemented offset "
                  HWADDR_FMT_plx "\n", offset);
    return 0;
}

static void pwm_write(void *opaque, hwaddr offset,
                      uint64_t value, unsigned size)
{
    STMP3770PWMState *s = opaque;
    int sct = (offset >> 2) & 3;
    int ch;

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-pwm: unsupported write size %u at offset "
                      HWADDR_FMT_plx "\n", size, offset);
        return;
    }

    if ((offset & ~0xC) == REG_CTRL0) {
        uint32_t current = s->ctrl0 & CTRL0_RW_MASK;
        uint32_t next = pwm_apply_sct(current, (uint32_t)value & CTRL0_RW_MASK,
                                      sct) & CTRL0_RW_MASK;

        s->ctrl0 = CTRL0_PRESENT | next;
        if (s->ctrl0 & CTRL0_SFTRST) {
            pwm_reset(DEVICE(s));
        } else {
            pwm_rearm_all(s);
        }
        return;
    }

    for (ch = 0; ch < STMP3770_PWM_NUM_CHANNELS; ch++) {
        if ((offset & ~0xC) == REG_PERIOD(ch)) {
            s->period[ch] =
                pwm_apply_sct(s->period[ch], (uint32_t)value & PERIOD_RW_MASK,
                              sct) & PERIOD_RW_MASK;
            s->channel[ch].pending_active = s->active[ch];
            s->channel[ch].pending_period = s->period[ch];
            s->channel[ch].pending = true;
            if (!s->channel[ch].latched) {
                pwm_latch_channel(s, ch, false);
            }
            pwm_rearm_channel(s, ch);
            return;
        }
        if ((offset & ~0xC) == REG_ACTIVE(ch)) {
            s->active[ch] = pwm_apply_sct(s->active[ch], (uint32_t)value,
                                          sct);
            return;
        }
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "stmp3770-pwm: write to unimplemented offset "
                  HWADDR_FMT_plx "\n", offset);
}

static const MemoryRegionOps pwm_ops = {
    .read = pwm_read,
    .write = pwm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void pwm_reset(DeviceState *dev)
{
    STMP3770PWMState *s = STMP3770_PWM(dev);
    unsigned ch;

    s->ctrl0 = CTRL0_RESET;
    memset(s->period, 0, sizeof(s->period));
    memset(s->duty, 0, sizeof(s->duty));
    memset(s->active, 0, sizeof(s->active));
    for (ch = 0; ch < STMP3770_PWM_NUM_CHANNELS; ch++) {
        STMP3770PWMChannel *channel = &s->channel[ch];

        ptimer_transaction_begin(channel->ptimer);
        ptimer_stop(channel->ptimer);
        ptimer_set_limit(channel->ptimer, 1, 1);
        ptimer_transaction_commit(channel->ptimer);
        channel->latched_active = 0;
        channel->latched_period = 0;
        channel->pending_active = 0;
        channel->pending_period = 0;
        channel->counter = 0;
        channel->pending = false;
        channel->latched = false;
        channel->running = false;
        pwm_update_output(s, ch);
    }
}

static void pwm_realize(DeviceState *dev, Error **errp)
{
    STMP3770PWMState *s = STMP3770_PWM(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &pwm_ops, s,
                          TYPE_STMP3770_PWM, 0x2000);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_pwm_channel = {
    .name = "stmp3770-pwm-channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PTIMER(ptimer, STMP3770PWMChannel),
        VMSTATE_UINT32(latched_active, STMP3770PWMChannel),
        VMSTATE_UINT32(latched_period, STMP3770PWMChannel),
        VMSTATE_UINT32(pending_active, STMP3770PWMChannel),
        VMSTATE_UINT32(pending_period, STMP3770PWMChannel),
        VMSTATE_UINT16(counter, STMP3770PWMChannel),
        VMSTATE_BOOL(pending, STMP3770PWMChannel),
        VMSTATE_BOOL(latched, STMP3770PWMChannel),
        VMSTATE_BOOL(running, STMP3770PWMChannel),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_pwm = {
    .name = "stmp3770-pwm",
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = pwm_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl0, STMP3770PWMState),
        VMSTATE_UINT32_ARRAY(period, STMP3770PWMState, STMP3770_PWM_NUM_CHANNELS),
        VMSTATE_UINT32_ARRAY(duty, STMP3770PWMState, STMP3770_PWM_NUM_CHANNELS),
        VMSTATE_UINT32_ARRAY(active, STMP3770PWMState, STMP3770_PWM_NUM_CHANNELS),
        VMSTATE_STRUCT_ARRAY(channel, STMP3770PWMState,
                             STMP3770_PWM_NUM_CHANNELS, 2,
                             vmstate_pwm_channel, STMP3770PWMChannel),
        VMSTATE_END_OF_LIST()
    }
};

static void pwm_init(Object *obj)
{
    STMP3770PWMState *s = STMP3770_PWM(obj);
    unsigned ch;

    s->ctrl0 = CTRL0_RESET;
    for (ch = 0; ch < STMP3770_PWM_NUM_CHANNELS; ch++) {
        s->cb_info[ch].s = s;
        s->cb_info[ch].channel = ch;
        s->channel[ch].ptimer = ptimer_init(pwm_channel_tick, &s->cb_info[ch],
                                             PTIMER_POLICY_NO_COUNTER_ROUND_DOWN |
                                             PTIMER_POLICY_TRIGGER_ONLY_ON_DECREMENT);
    }
}

static int pwm_post_load(void *opaque, int version_id)
{
    STMP3770PWMState *s = STMP3770_PWM(opaque);
    unsigned ch;

    if (version_id < 2) {
        for (ch = 0; ch < STMP3770_PWM_NUM_CHANNELS; ch++) {
            s->channel[ch].pending_active = s->active[ch];
            s->channel[ch].pending_period = s->period[ch];
        }
    }
    pwm_rearm_all(s);
    return 0;
}

static void pwm_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = pwm_realize;
    device_class_set_legacy_reset(dc, pwm_reset);
    dc->vmsd = &vmstate_pwm;
}

void stmp3770_pwm_set_pinctrl(STMP3770PWMState *s,
                               STMP3770PINCTRLState *pinctrl)
{
    unsigned ch;

    s->pinctrl = pinctrl;
    for (ch = 0; ch < STMP3770_PWM_NUM_CHANNELS; ch++) {
        pwm_update_output(s, ch);
    }
}

static const TypeInfo pwm_type_info = {
    .name          = TYPE_STMP3770_PWM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STMP3770PWMState),
    .instance_init = pwm_init,
    .class_init    = pwm_class_init,
};

static void pwm_register_types(void)
{
    type_register_static(&pwm_type_info);
}

type_init(pwm_register_types)
