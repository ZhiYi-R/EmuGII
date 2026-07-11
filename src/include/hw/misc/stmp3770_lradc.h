/*
 * STMP3770 Low-Resolution Analog-to-Digital Converter (LRADC)
 *
 * Based on STMP3770 Reference Manual Chapter 13
 *
 * Features:
 * - 12-bit low-resolution ADC with multiple channels
 * - Schedule registers, delay channels, conversion control
 * - Interrupt status and enable registers
 * - SET/CLR/TOG register variants on applicable registers
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

#ifndef HW_MISC_STMP3770_LRADC_H
#define HW_MISC_STMP3770_LRADC_H

#include "hw/sysbus.h"

#define TYPE_STMP3770_LRADC "stmp3770-lradc"
OBJECT_DECLARE_SIMPLE_TYPE(STMP3770LRADCState, STMP3770_LRADC)

typedef struct STMP3770PWMState STMP3770PWMState;

void stmp3770_lradc_set_pwm(STMP3770LRADCState *s, STMP3770PWMState *pwm);
void stmp3770_lradc_set_touch_detect(STMP3770LRADCState *s, bool detect);

#endif /* HW_MISC_STMP3770_LRADC_H */
