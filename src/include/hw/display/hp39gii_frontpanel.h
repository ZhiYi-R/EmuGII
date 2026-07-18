/*
 * HP 39gII front panel (case, LCD glass, annunciators, keypad) rendering
 *
 * Visual design is based on photographs of the real HP 39gII hardware.
 * The annunciator strip at the top of the LCD glass consists of
 * independent LCD segments that live outside the 256x128 pixel area;
 * their on/off state is decoded from the panel controller's memory
 * (rows 0..7, see hp39gii_frontpanel.c) rather than from screen pixels.
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

#ifndef HP39GII_FRONTPANEL_H
#define HP39GII_FRONTPANEL_H

#include <stdbool.h>
#include <stdint.h>
#include "ui/input.h"

typedef struct QemuConsole QemuConsole;

/* Front panel canvas size, in host pixels. */
#define HP39GII_FP_WIDTH  576
#define HP39GII_FP_HEIGHT 1152

/* Panel glass geometry (hardware facts of the LCD module). */
#define HP39GII_PANEL_WIDTH  258 /* bytes per row in panel memory */
#define HP39GII_PANEL_HEIGHT 137
#define HP39GII_PANEL_VIEW_Y 8   /* first row of the 256x128 pixel area */
#define HP39GII_PANEL_VIEW_W 256
#define HP39GII_PANEL_VIEW_H 128

/* Calculator keyboard matrix code, as wired to the GPIO scan matrix. */
#define HP39GII_FP_KEY(row, col) (((row) << 3) | (col))
#define HP39GII_FP_KEY_NONE (-1)

/*
 * Render the whole front panel (case, glass with annunciator strip and
 * pixel area, keypad with pressed-key feedback) into the console and
 * flush it to the display. @vram is the panel controller memory
 * (HP39GII_PANEL_WIDTH * HP39GII_PANEL_HEIGHT bytes, 8bpp grayscale),
 * @key_state two 64-bit words indexed by matrix code, @display_on the
 * panel controller's display-on flag (glass renders blank when false).
 */
void hp39gii_fp_render(QemuConsole *con, const uint8_t *vram,
                       const uint64_t *key_state, bool display_on);

/* Front panel hit testing: matrix code under canvas point (x, y). */
int hp39gii_fp_key_at(int x, int y);

/* PC keyboard mapping: matrix code for a QKeyCode. */
int hp39gii_fp_key_for_qcode(QKeyCode qcode);

#endif /* HP39GII_FRONTPANEL_H */
