/*
 * STMP3770 Digital Control (DIGCTL) header
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

#ifndef HW_MISC_STMP3770_DIGCTL_H
#define HW_MISC_STMP3770_DIGCTL_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_STMP3770_DIGCTL "stmp3770-digctl"
OBJECT_DECLARE_SIMPLE_TYPE(STMP3770DIGCTLState, STMP3770_DIGCTL)

uint32_t stmp3770_digctl_get_mpte_loc(STMP3770DIGCTLState *s, int idx);
void stmp3770_digctl_dig_reset(STMP3770DIGCTLState *s);
void stmp3770_digctl_set_hclk_rate(void *opaque, uint32_t hclk_hz);

#endif /* HW_MISC_STMP3770_DIGCTL_H */
