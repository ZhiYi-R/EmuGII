/*
 * STMP3770 PWM controller emulation
 *
 * Based on STMP3770 Reference Manual Chapter 19
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
        return;
    }

    for (ch = 0; ch < STMP3770_PWM_NUM_CHANNELS; ch++) {
        if ((offset & ~0xC) == REG_PERIOD(ch)) {
            s->period[ch] =
                pwm_apply_sct(s->period[ch], (uint32_t)value & PERIOD_RW_MASK,
                              sct) & PERIOD_RW_MASK;
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

    s->ctrl0 = CTRL0_RESET;
    memset(s->period, 0, sizeof(s->period));
    memset(s->duty, 0, sizeof(s->duty));
    memset(s->active, 0, sizeof(s->active));
}

static void pwm_realize(DeviceState *dev, Error **errp)
{
    STMP3770PWMState *s = STMP3770_PWM(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &pwm_ops, s,
                          TYPE_STMP3770_PWM, 0x2000);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_pwm = {
    .name = "stmp3770-pwm",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl0, STMP3770PWMState),
        VMSTATE_UINT32_ARRAY(period, STMP3770PWMState, STMP3770_PWM_NUM_CHANNELS),
        VMSTATE_UINT32_ARRAY(duty, STMP3770PWMState, STMP3770_PWM_NUM_CHANNELS),
        VMSTATE_UINT32_ARRAY(active, STMP3770PWMState, STMP3770_PWM_NUM_CHANNELS),
        VMSTATE_END_OF_LIST()
    }
};

static void pwm_init(Object *obj)
{
    STMP3770PWMState *s = STMP3770_PWM(obj);

    s->ctrl0 = CTRL0_RESET;
}

static void pwm_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = pwm_realize;
    device_class_set_legacy_reset(dc, pwm_reset);
    dc->vmsd = &vmstate_pwm;
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
