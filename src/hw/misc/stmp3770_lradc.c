/*
 * STMP3770 Low-Resolution Analog-to-Digital Converter (LRADC)
 *
 * Based on STMP3770 Reference Manual Chapter 28
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
#include "qemu/timer.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/misc/stmp3770_lradc.h"
#include "hw/timer/stmp3770_pwm.h"

typedef struct STMP3770LRADCDelayCtx {
    STMP3770LRADCState *s;
    int n;
} STMP3770LRADCDelayCtx;

struct STMP3770LRADCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    uint32_t ctrl0;
    uint32_t ctrl1;
    uint32_t ctrl2;
    uint32_t ctrl3;
    uint32_t ctrl4;
    uint32_t status;
    uint32_t channel[8];
    uint32_t delay[4];
    uint32_t debug0;
    uint32_t debug1;
    uint32_t conversion;
    uint32_t version;
    STMP3770PWMState *pwm;

    /* IRQ lines: index 0 = touch, 1..8 = LRADC channels 0..7 */
    qemu_irq irq[9];

    /* Scheduler/accumulate/delay state */
    QEMUTimer *delay_timer[4];
    STMP3770LRADCDelayCtx delay_ctx[4];
    uint8_t delay_running[4];
    uint8_t delay_loop_remaining[4];
    int64_t delay_remaining_ns[4];
    uint8_t sample_count[8];

    /* Touch detection: set by external (UI) input; gated by TOUCH_DETECT_ENABLE */
    bool touch_detect;

    /* CTRL3 analog power/discard state */
    bool analog_powered;
    uint8_t discard_remaining;
};

#define LRADC_VERSION   0x01010000

/* Register offsets */
#define REG_CTRL0       0x000
#define REG_CTRL0_SET   0x004
#define REG_CTRL0_CLR   0x008
#define REG_CTRL0_TOG   0x00C
#define REG_CTRL1       0x010
#define REG_CTRL1_SET   0x014
#define REG_CTRL1_CLR   0x018
#define REG_CTRL1_TOG   0x01C
#define REG_CTRL2       0x020
#define REG_CTRL2_SET   0x024
#define REG_CTRL2_CLR   0x028
#define REG_CTRL2_TOG   0x02C
#define REG_CTRL3       0x030
#define REG_CTRL3_SET   0x034
#define REG_CTRL3_CLR   0x038
#define REG_CTRL3_TOG   0x03C
#define REG_STATUS      0x040
#define REG_STATUS_SET  0x044
#define REG_STATUS_CLR  0x048
#define REG_STATUS_TOG  0x04C
#define REG_CH0         0x050
#define REG_CH1         0x060
#define REG_CH2         0x070
#define REG_CH3         0x080
#define REG_CH4         0x090
#define REG_CH5         0x0A0
#define REG_CH6         0x0B0
#define REG_CH7         0x0C0
#define REG_DELAY0      0x0D0
#define REG_DELAY1      0x0E0
#define REG_DELAY2      0x0F0
#define REG_DELAY3      0x100
#define REG_DEBUG0      0x110
#define REG_DEBUG1      0x120
#define REG_CONVERSION  0x130
#define REG_CTRL4       0x140
#define REG_VERSION     0x150

/* CTRL0 bits */
#define CTRL0_SFTRST    (1U << 31)
#define CTRL0_CLKGATE   (1U << 30)
#define CTRL0_SCHEDULE_MASK 0xFF
#define CTRL0_TOUCH_DETECT_ENABLE (1U << 20)
#define CTRL0_RW_MASK   (CTRL0_SFTRST | CTRL0_CLKGATE | (0x3FU << 16) | CTRL0_SCHEDULE_MASK)
#define CTRL2_BL_ENABLE  (1U << 22)
#define CTRL2_TEMPSENSE_PWD (1U << 15)
#define CTRL2_TEMP_SENSOR_IENABLE0 (1U << 8)
#define CTRL2_TEMP_SENSOR_IENABLE1 (1U << 9)
#define CTRL3_FORCE_ANALOG_PWUP (1U << 23)
#define CTRL3_FORCE_ANALOG_PWDN (1U << 22)
#define DELAY_KICK       (1U << 20)
#define DELAY_TICK_NS    500000

/* Channel result bits */
#define CH_TOGGLE       (1U << 31)
#define CH_TESTMODE_TOGGLE (1U << 30)
#define CH_ACCUMULATE   (1U << 29)
#define CH_NUM_SAMPLES_SHIFT 24
#define CH_NUM_SAMPLES_MASK  0x1F
#define CH_VALUE_MASK   0x3FFFF

/* CH0-6 write/read mask (bit 30 and 23:18 are reserved) */
#define CH_RW_MASK      (CH_TOGGLE | CH_ACCUMULATE | \
                         (CH_NUM_SAMPLES_MASK << CH_NUM_SAMPLES_SHIFT) | CH_VALUE_MASK)
/* CH7 read mask also exposes TESTMODE_TOGGLE (bit 30) */
#define CH7_R_MASK      (CH_TOGGLE | CH_TESTMODE_TOGGLE | CH_ACCUMULATE | \
                         (CH_NUM_SAMPLES_MASK << CH_NUM_SAMPLES_SHIFT) | CH_VALUE_MASK)

/* Remaining LRADC register write/read masks */
#define CTRL1_RW_MASK   0x01FF01FFU
#define CTRL2_RW_MASK   0xFFFFB3FFU
#define CTRL3_RW_MASK   0x03C00333U
#define CONVERSION_RW_MASK 0x001303FFU
#define DEBUG1_RW_MASK  0x00001F07U
#define DELAY_RW_MASK   0xFF1FFFFFU

/* STATUS bits */
#define STATUS_CHANNEL_PRESENT_MASK 0x07FF0000
#define STATUS_R_MASK   0x07FF0001U

static uint64_t lradc_read_subword(uint32_t value, hwaddr offset, unsigned size)
{
    unsigned shift = (offset & 3) * 8;
    uint32_t mask = (size >= 4) ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1u);

    return (value >> shift) & mask;
}

static void lradc_apply_sct(uint32_t *reg, uint32_t value, int sct,
                            hwaddr offset, unsigned size)
{
    unsigned shift = (offset & 3) * 8;
    uint32_t mask = (size >= 4) ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1u);
    uint32_t cur = (*reg >> shift) & mask;

    switch (sct) {
    case 0:
        cur = value & mask;
        break;
    case 1:
        cur |= value;
        break;
    case 2:
        cur &= ~value;
        break;
    case 3:
        cur ^= value;
        break;
    }

    *reg = (*reg & ~(mask << shift)) | ((cur & mask) << shift);
}

static void lradc_update_status(STMP3770LRADCState *s);
static void lradc_update_irq(STMP3770LRADCState *s);
static void lradc_complete_scheduled(STMP3770LRADCState *s);
static void lradc_start_delay(STMP3770LRADCState *s, int n, bool reload_loop);
static void lradc_stop_delay(STMP3770LRADCState *s, int n);
static void lradc_stop_all_delays(STMP3770LRADCState *s);
static void lradc_resume_all_delays(STMP3770LRADCState *s);
static void lradc_update_analog_power(STMP3770LRADCState *s);
static void lradc_update_delay_remaining(STMP3770LRADCState *s);

static uint32_t lradc_sample_value(STMP3770LRADCState *s, int ch)
{
    uint32_t physical = (s->ctrl4 >> (ch * 4)) & 0xF;
    uint32_t value = 0;

    if (!s->analog_powered) {
        return 0;
    }

    if (s->discard_remaining > 0) {
        s->discard_remaining--;
        return 0;
    }

    if (physical < 8) {
        if (physical == 0 && (s->ctrl2 & CTRL2_TEMP_SENSOR_IENABLE0)) {
            value = (s->ctrl2 & 0xFU) << 8;
        } else if (physical == 1 && (s->ctrl2 & CTRL2_TEMP_SENSOR_IENABLE1)) {
            value = ((s->ctrl2 >> 4) & 0xFU) << 8;
        } else {
            value = 0xABC;
        }
    } else if (physical == 8 || physical == 9) {
        if (s->ctrl2 & CTRL2_TEMPSENSE_PWD) {
            return 0;
        }
        value = (physical == 8) ? 0x400 : 0x800;
    }

    /* CTRL2.DIVIDE_BY_TWO applies to the selected logical channel */
    if ((s->ctrl2 >> (24 + ch)) & 1) {
        value >>= 1;
    }

    return value;
}

static void lradc_complete_channel(STMP3770LRADCState *s, int ch)
{
    uint32_t num_samples = (s->channel[ch] >> CH_NUM_SAMPLES_SHIFT) &
                           CH_NUM_SAMPLES_MASK;
    uint32_t target = num_samples ? num_samples : 1;
    uint32_t value = lradc_sample_value(s, ch);
    uint32_t current_value = s->channel[ch] & CH_VALUE_MASK;

    if (s->channel[ch] & CH_ACCUMULATE) {
        current_value += value;
        current_value &= CH_VALUE_MASK;
        s->sample_count[ch]++;
        if (s->sample_count[ch] >= target) {
            s->sample_count[ch] = 0;
            s->ctrl1 |= (1U << ch);
        }
    } else {
        s->sample_count[ch] = 0;
        current_value = value;
        s->ctrl1 |= (1U << ch);
    }

    s->channel[ch] = (s->channel[ch] & ~CH_VALUE_MASK) | current_value;
    s->channel[ch] ^= CH_TOGGLE;
}

static void lradc_complete_scheduled(STMP3770LRADCState *s)
{
    int ch;
    uint32_t schedule = s->ctrl0 & CTRL0_SCHEDULE_MASK;

    if (schedule == 0 || (s->ctrl0 & CTRL0_CLKGATE)) {
        return;
    }

    for (ch = 0; ch < 8; ch++) {
        if (schedule & (1U << ch)) {
            lradc_complete_channel(s, ch);
        }
    }

    /* Hardware clears SCHEDULE bits when conversions are done */
    s->ctrl0 &= ~CTRL0_SCHEDULE_MASK;
    lradc_update_irq(s);
}

static void lradc_update_pwm2_analog_enable(STMP3770LRADCState *s)
{
    if (s->pwm) {
        stmp3770_pwm_set_pwm2_analog_enable(s->pwm,
                                             s->ctrl2 & CTRL2_BL_ENABLE);
    }
}

static void lradc_update_status(STMP3770LRADCState *s)
{
    s->status = STATUS_CHANNEL_PRESENT_MASK;
    if (s->touch_detect && (s->ctrl0 & CTRL0_TOUCH_DETECT_ENABLE)) {
        s->status |= 1;  /* TOUCH_DETECT_RAW */
    }
}

static void lradc_update_irq(STMP3770LRADCState *s)
{
    int i;

    lradc_update_status(s);

    /* Hardware sets TOUCH_DETECT_IRQ when a touch is detected and enabled */
    if (s->status & 1) {
        s->ctrl1 |= (1U << 8);
    }

    for (i = 0; i < 8; i++) {
        bool pending = (s->ctrl1 >> i) & 1;
        bool enabled = (s->ctrl1 >> (16 + i)) & 1;
        qemu_set_irq(s->irq[i + 1], pending && enabled);
    }

    qemu_set_irq(s->irq[0], ((s->ctrl1 >> 8) & 1) && ((s->ctrl1 >> 24) & 1));
}

void stmp3770_lradc_set_touch_detect(STMP3770LRADCState *s, bool detect)
{
    s->touch_detect = detect;
    lradc_update_irq(s);
}

static void lradc_touch_detect_set(void *opaque, int n, int level)
{
    STMP3770LRADCState *s = STMP3770_LRADC(opaque);

    (void)n;
    stmp3770_lradc_set_touch_detect(s, !!level);
}

static void lradc_reset(DeviceState *dev);

static void lradc_update_delay_remaining(STMP3770LRADCState *s)
{
    int n;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    for (n = 0; n < 4; n++) {
        if (s->delay_running[n] && s->delay_timer[n]) {
            int64_t remaining = s->delay_timer[n]->expire_time - now;
            s->delay_remaining_ns[n] = remaining > 0 ? remaining : 0;
        } else {
            s->delay_remaining_ns[n] = 0;
        }
    }
}

static void lradc_stop_delay(STMP3770LRADCState *s, int n)
{
    if (s->delay_running[n]) {
        lradc_update_delay_remaining(s);
        timer_del(s->delay_timer[n]);
        s->delay_running[n] = false;
    }
}

static void lradc_stop_all_delays(STMP3770LRADCState *s)
{
    int n;

    for (n = 0; n < 4; n++) {
        lradc_stop_delay(s, n);
    }
}

static void lradc_start_delay(STMP3770LRADCState *s, int n, bool reload_loop)
{
    uint32_t delay_ticks = s->delay[n] & 0x7FF;
    int64_t now;
    int64_t expire;

    if (delay_ticks == 0) {
        s->delay_running[n] = false;
        return;
    }

    s->delay_running[n] = true;
    if (reload_loop) {
        s->delay_loop_remaining[n] = (s->delay[n] >> 11) & 0x1F;
    }

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    expire = now + (int64_t)delay_ticks * DELAY_TICK_NS;
    timer_mod(s->delay_timer[n], expire);
}

static void lradc_resume_all_delays(STMP3770LRADCState *s)
{
    int n;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    for (n = 0; n < 4; n++) {
        if (s->delay_running[n]) {
            int64_t remaining = s->delay_remaining_ns[n];
            if (remaining <= 0) {
                remaining = (s->delay[n] & 0x7FF) * DELAY_TICK_NS;
                if (remaining == 0) {
                    continue;
                }
            }
            timer_mod(s->delay_timer[n], now + remaining);
            s->delay_remaining_ns[n] = 0;
        }
    }
}

static void lradc_delay_expire(void *opaque)
{
    STMP3770LRADCDelayCtx *ctx = opaque;
    STMP3770LRADCState *s = ctx->s;
    int n = ctx->n;
    uint32_t trigger_lradcs = (s->delay[n] >> 24) & 0xFF;
    uint32_t trigger_delays = (s->delay[n] >> 16) & 0xF;
    uint32_t self_trigger = trigger_delays & (1U << n);
    int m;

    s->delay_running[n] = false;

    if (trigger_lradcs) {
        s->ctrl0 |= trigger_lradcs & CTRL0_SCHEDULE_MASK;
        lradc_complete_scheduled(s);
    }

    for (m = 0; m < 4; m++) {
        if (trigger_delays & (1U << m)) {
            lradc_start_delay(s, m, true);
        }
    }

    if (!self_trigger) {
        if (s->delay_loop_remaining[n] > 0) {
            s->delay_loop_remaining[n]--;
            lradc_start_delay(s, n, false);
        }
    }
}

static void lradc_handle_clockgate(STMP3770LRADCState *s)
{
    if (s->ctrl0 & (CTRL0_SFTRST | CTRL0_CLKGATE)) {
        lradc_stop_all_delays(s);
    } else {
        lradc_resume_all_delays(s);
    }
}

static void lradc_update_analog_power(STMP3770LRADCState *s)
{
    bool new_powered = !(s->ctrl0 & (CTRL0_SFTRST | CTRL0_CLKGATE)) &&
                       ((s->ctrl3 & CTRL3_FORCE_ANALOG_PWUP) ||
                        !(s->ctrl3 & CTRL3_FORCE_ANALOG_PWDN));

    if (new_powered && !s->analog_powered) {
        static const uint8_t discard_count[4] = {0, 1, 2, 3};
        s->discard_remaining = discard_count[(s->ctrl3 >> 24) & 0x3];
    }

    s->analog_powered = new_powered;
    lradc_handle_clockgate(s);
}

static uint64_t lradc_read(void *opaque, hwaddr offset, unsigned size)
{
    STMP3770LRADCState *s = opaque;
    hwaddr base = offset & ~0xC;
    uint32_t ret;

    switch (base) {
    case REG_CTRL0:
        ret = s->ctrl0 & CTRL0_RW_MASK;
        break;
    case REG_CTRL1:
        ret = s->ctrl1 & CTRL1_RW_MASK;
        break;
    case REG_CTRL2:
        ret = s->ctrl2 & CTRL2_RW_MASK;
        break;
    case REG_CTRL3:
        ret = s->ctrl3 & CTRL3_RW_MASK;
        break;
    case REG_STATUS:
        ret = s->status & STATUS_R_MASK;
        break;
    case REG_CH0 ... REG_CH7:
        {
            int ch = (base - REG_CH0) >> 4;
            ret = s->channel[ch];
            if (ch == 7) {
                ret &= CH7_R_MASK;
            } else {
                ret &= CH_RW_MASK;
            }
        }
        break;
    case REG_DELAY0:
        ret = s->delay[0] & DELAY_RW_MASK;
        break;
    case REG_DELAY1:
        ret = s->delay[1] & DELAY_RW_MASK;
        break;
    case REG_DELAY2:
        ret = s->delay[2] & DELAY_RW_MASK;
        break;
    case REG_DELAY3:
        ret = s->delay[3] & DELAY_RW_MASK;
        break;
    case REG_DEBUG0:
        ret = 0x43210000;
        break;
    case REG_DEBUG1:
        ret = s->debug1 & DEBUG1_RW_MASK;
        break;
    case REG_CONVERSION:
        ret = s->conversion & CONVERSION_RW_MASK;
        break;
    case REG_CTRL4:
        ret = s->ctrl4;
        break;
    case REG_VERSION:
        ret = LRADC_VERSION;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-lradc: read from unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        ret = 0;
        break;
    }

    return lradc_read_subword(ret, offset, size);
}

static void lradc_write(void *opaque, hwaddr offset,
                        uint64_t value, unsigned size)
{
    STMP3770LRADCState *s = opaque;
    int sct = (offset >> 2) & 3;
    hwaddr base = offset & ~0xC;

    switch (base) {
    case REG_CTRL0:
        lradc_apply_sct(&s->ctrl0, (uint32_t)value, sct, offset, size);
        s->ctrl0 &= CTRL0_RW_MASK;
        /* SFTRST resets the entire LRADC block; it also gates the clock. */
        if (s->ctrl0 & CTRL0_SFTRST) {
            lradc_reset(DEVICE(s));
        }
        lradc_update_analog_power(s);
        /* Complete any scheduled conversions once the clock is running */
        if (!(s->ctrl0 & CTRL0_CLKGATE) && (s->ctrl0 & CTRL0_SCHEDULE_MASK)) {
            lradc_complete_scheduled(s);
        }
        break;
    case REG_CTRL1:
        lradc_apply_sct(&s->ctrl1, (uint32_t)value, sct, offset, size);
        s->ctrl1 &= CTRL1_RW_MASK;
        lradc_update_irq(s);
        break;
    case REG_CTRL2:
        lradc_apply_sct(&s->ctrl2, (uint32_t)value, sct, offset, size);
        s->ctrl2 &= CTRL2_RW_MASK;
        lradc_update_pwm2_analog_enable(s);
        break;
    case REG_CTRL3:
        lradc_apply_sct(&s->ctrl3, (uint32_t)value, sct, offset, size);
        s->ctrl3 &= CTRL3_RW_MASK;
        lradc_update_analog_power(s);
        break;
    case REG_CTRL4:
        lradc_apply_sct(&s->ctrl4, (uint32_t)value, sct, offset, size);
        break;
    case REG_CH0 ... REG_CH7:
        lradc_apply_sct(&s->channel[(base - REG_CH0) >> 4], (uint32_t)value,
                        sct, offset, size);
        s->channel[(base - REG_CH0) >> 4] &= CH_RW_MASK;
        break;
    case REG_DELAY0:
    case REG_DELAY1:
    case REG_DELAY2:
    case REG_DELAY3:
        {
            int n = (base - REG_DELAY0) >> 4;
            uint32_t old_kick = s->delay[n] & DELAY_KICK;

            lradc_apply_sct(&s->delay[n], (uint32_t)value, sct, offset, size);
            s->delay[n] &= DELAY_RW_MASK;

            if (s->delay[n] & DELAY_KICK) {
                lradc_start_delay(s, n, true);
            } else if (old_kick) {
                lradc_stop_delay(s, n);
            }
        }
        break;
    case REG_DEBUG0:
        /* Read-only */
        break;
    case REG_DEBUG1:
        lradc_apply_sct(&s->debug1, (uint32_t)value, sct, offset, size);
        s->debug1 &= DEBUG1_RW_MASK;
        break;
    case REG_CONVERSION:
        lradc_apply_sct(&s->conversion, (uint32_t)value, sct, offset, size);
        s->conversion &= CONVERSION_RW_MASK;
        break;
    case REG_STATUS:
    case REG_VERSION:
        /* Read-only; SET/CLR/TOG aliases are harmless no-ops */
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-lradc: write to unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        break;
    }
}

static const MemoryRegionOps lradc_ops = {
    .read = lradc_read,
    .write = lradc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void lradc_reset(DeviceState *dev)
{
    STMP3770LRADCState *s = STMP3770_LRADC(dev);

    lradc_stop_all_delays(s);
    s->ctrl0 = CTRL0_SFTRST | CTRL0_CLKGATE;
    s->ctrl1 = 0;
    s->ctrl2 = CTRL2_TEMPSENSE_PWD;
    lradc_update_pwm2_analog_enable(s);
    s->ctrl3 = 0;
    s->ctrl4 = 0x76543210;  /* default channel MUX mapping */
    s->conversion = 0x00000080;
    s->version = LRADC_VERSION;
    memset(s->channel, 0, sizeof(s->channel));
    /* Channel 7 is the battery monitor; default to disconnected battery (USB powered) */
    s->channel[7] = 2748;
    memset(s->delay, 0, sizeof(s->delay));
    memset(s->delay_running, 0, sizeof(s->delay_running));
    memset(s->delay_loop_remaining, 0, sizeof(s->delay_loop_remaining));
    memset(s->delay_remaining_ns, 0, sizeof(s->delay_remaining_ns));
    memset(s->sample_count, 0, sizeof(s->sample_count));
    s->touch_detect = false;
    s->debug0 = 0x43210000;
    s->debug1 = 0;
    s->analog_powered = false;
    s->discard_remaining = 0;
    lradc_update_analog_power(s);
    lradc_update_irq(s);
}

static void lradc_realize(DeviceState *dev, Error **errp)
{
    STMP3770LRADCState *s = STMP3770_LRADC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    int i;

    memory_region_init_io(&s->iomem, OBJECT(dev), &lradc_ops, s,
                          TYPE_STMP3770_LRADC, 0x2000);
    sysbus_init_mmio(sbd, &s->iomem);

    for (i = 0; i < 9; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }

    qdev_init_gpio_in_named(DEVICE(dev), lradc_touch_detect_set,
                            "touch-detect", 1);

    for (i = 0; i < 4; i++) {
        s->delay_ctx[i].s = s;
        s->delay_ctx[i].n = i;
        s->delay_timer[i] = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                         lradc_delay_expire,
                                         &s->delay_ctx[i]);
    }
}

static int lradc_post_load(void *opaque, int version_id)
{
    STMP3770LRADCState *s = STMP3770_LRADC(opaque);

    lradc_update_pwm2_analog_enable(s);
    lradc_update_analog_power(s);
    lradc_update_irq(s);
    return 0;
}

static int lradc_pre_save(void *opaque)
{
    STMP3770LRADCState *s = STMP3770_LRADC(opaque);

    lradc_update_delay_remaining(s);
    return 0;
}

static const VMStateDescription vmstate_lradc = {
    .name = "stmp3770-lradc",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = lradc_pre_save,
    .post_load = lradc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl0, STMP3770LRADCState),
        VMSTATE_UINT32(ctrl1, STMP3770LRADCState),
        VMSTATE_UINT32(ctrl2, STMP3770LRADCState),
        VMSTATE_UINT32(ctrl3, STMP3770LRADCState),
        VMSTATE_UINT32(ctrl4, STMP3770LRADCState),
        VMSTATE_UINT32(conversion, STMP3770LRADCState),
        VMSTATE_UINT32_ARRAY(channel, STMP3770LRADCState, 8),
        VMSTATE_UINT32_ARRAY(delay, STMP3770LRADCState, 4),
        VMSTATE_UINT32(debug1, STMP3770LRADCState),
        VMSTATE_UINT8_ARRAY(delay_running, STMP3770LRADCState, 4),
        VMSTATE_UINT8_ARRAY(delay_loop_remaining, STMP3770LRADCState, 4),
        VMSTATE_INT64_ARRAY(delay_remaining_ns, STMP3770LRADCState, 4),
        VMSTATE_UINT8_ARRAY(sample_count, STMP3770LRADCState, 8),
        VMSTATE_BOOL(touch_detect, STMP3770LRADCState),
        VMSTATE_BOOL(analog_powered, STMP3770LRADCState),
        VMSTATE_UINT8(discard_remaining, STMP3770LRADCState),
        VMSTATE_END_OF_LIST()
    }
};

static void lradc_init(Object *obj)
{
    STMP3770LRADCState *s = STMP3770_LRADC(obj);

    s->ctrl0 = CTRL0_SFTRST | CTRL0_CLKGATE;
    s->ctrl1 = 0;
    s->ctrl2 = CTRL2_TEMPSENSE_PWD;
    s->ctrl3 = 0;
    s->ctrl4 = 0x76543210;
    s->conversion = 0x00000080;
    s->version = LRADC_VERSION;
    memset(s->channel, 0, sizeof(s->channel));
    s->channel[7] = 2748;
    memset(s->delay, 0, sizeof(s->delay));
    memset(s->delay_running, 0, sizeof(s->delay_running));
    memset(s->delay_loop_remaining, 0, sizeof(s->delay_loop_remaining));
    memset(s->delay_remaining_ns, 0, sizeof(s->delay_remaining_ns));
    memset(s->sample_count, 0, sizeof(s->sample_count));
    s->touch_detect = false;
    s->analog_powered = false;
    s->discard_remaining = 0;
    lradc_update_status(s);
    s->debug0 = 0x43210000;
    s->debug1 = 0;
}

static void lradc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = lradc_realize;
    device_class_set_legacy_reset(dc, lradc_reset);
    dc->vmsd = &vmstate_lradc;
}

void stmp3770_lradc_set_pwm(STMP3770LRADCState *s, STMP3770PWMState *pwm)
{
    s->pwm = pwm;
    lradc_update_pwm2_analog_enable(s);
}

static const TypeInfo lradc_type_info = {
    .name          = TYPE_STMP3770_LRADC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STMP3770LRADCState),
    .instance_init = lradc_init,
    .class_init    = lradc_class_init,
};

static void lradc_register_types(void)
{
    type_register_static(&lradc_type_info);
}

type_init(lradc_register_types)
