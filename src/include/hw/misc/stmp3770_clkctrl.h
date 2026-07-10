/*
 * STMP3770 Clock Control (CLKCTRL) header
 *
 * Copyright (C) 2024
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef HW_MISC_STMP3770_CLKCTRL_H
#define HW_MISC_STMP3770_CLKCTRL_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_STMP3770_CLKCTRL "stmp3770-clkctrl"
OBJECT_DECLARE_SIMPLE_TYPE(STMP3770CLKCTRLState, STMP3770_CLKCTRL)

typedef void (*STMP3770CLKCTRLDigResetFn)(void *opaque);
typedef void (*STMP3770CLKCTRLHclkRateFn)(void *opaque, uint32_t hclk_hz);

void stmp3770_clkctrl_set_dig_reset_callback(STMP3770CLKCTRLState *s,
                                             STMP3770CLKCTRLDigResetFn cb,
                                             void *opaque);
void stmp3770_clkctrl_set_hclk_rate_callback(STMP3770CLKCTRLState *s,
                                              STMP3770CLKCTRLHclkRateFn cb,
                                              void *opaque);
uint32_t stmp3770_clkctrl_get_hclk_rate(STMP3770CLKCTRLState *s);

#endif /* HW_MISC_STMP3770_CLKCTRL_H */
