/*
 * STMP3770 Pin Control and GPIO
 *
 * Based on STMP3770 Reference Manual Chapter 33
 *
 * Copyright (C) 2024
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef STMP3770_PINCTRL_H
#define STMP3770_PINCTRL_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_STMP3770_PINCTRL "stmp3770-pinctrl"
OBJECT_DECLARE_SIMPLE_TYPE(STMP3770PINCTRLState, STMP3770_PINCTRL)

#define STMP3770_PINCTRL_NUM_BANKS 4
#define STMP3770_PINCTRL_PWM_HI_Z  2

struct STMP3770PINCTRLState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq[STMP3770_PINCTRL_NUM_BANKS];

    /* Control */
    uint32_t ctrl;

    /* Pin muxing: 2 bits per pin, 16 pins per register */
    uint32_t muxsel[8];

    /* Drive strength/voltage: 4 bits per pin (3 bits effective), 8 pins per reg */
    uint32_t drive[15];

    /* Pull-up / pad keeper */
    uint32_t pull[4];

    /* GPIO data */
    uint32_t dout[4];
    uint32_t din[4];
    uint32_t doe[4];
    uint32_t prev_din[4];
    uint8_t pwm_output[5];

    /* GPIO interrupts */
    uint32_t pin2irq[4];
    uint32_t irqen[4];
    uint32_t irqlevel[4];
    uint32_t irqpol[4];
    uint32_t irqstat[4];

    /*
     * HP39GII keyboard matrix host state.  Key ids use the ExistOS encoding:
     * (row << 3) + col, with rows 0..10 and columns 0..4.
     */
    uint64_t key_state[2];
};

void stmp3770_pinctrl_set_key(STMP3770PINCTRLState *s,
                              unsigned int key, bool down);
void stmp3770_pinctrl_set_pwm_output(STMP3770PINCTRLState *s,
                                     unsigned int channel, uint8_t level);

#endif /* STMP3770_PINCTRL_H */
