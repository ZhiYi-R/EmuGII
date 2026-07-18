/*
 * STMP3770 LCD Interface (LCDIF) emulation
 *
 * Based on STMP3770 Reference Manual Chapter 18
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

#ifndef STMP3770_LCDIF_H
#define STMP3770_LCDIF_H

#include "hw/sysbus.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "ui/input.h"
#include "hw/display/framebuffer.h"
#include "hw/dma/stmp3770_dma.h"
#include "hw/gpio/stmp3770_pinctrl.h"

#define TYPE_STMP3770_LCDIF "stmp3770-lcdif"
OBJECT_DECLARE_SIMPLE_TYPE(STMP3770LCDIFState, STMP3770_LCDIF)

#define STMP3770_LCDIF_PANEL_WIDTH  258
#define STMP3770_LCDIF_PANEL_HEIGHT 137
#define STMP3770_LCDIF_PANEL_SIZE \
    (STMP3770_LCDIF_PANEL_WIDTH * STMP3770_LCDIF_PANEL_HEIGHT)
#define STMP3770_LCDIF_VIEW_WIDTH  256
#define STMP3770_LCDIF_VIEW_HEIGHT 128
#define STMP3770_LCDIF_VIEW_Y      8

struct STMP3770LCDIFState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    QemuConsole *con;
    QEMUTimer *refresh_timer;
    MemoryRegionSection fbsection;
    MemoryRegion *system_memory;

    uint32_t ctrl0;
    uint32_t ctrl1;
    uint32_t cur_buf;
    uint32_t next_buf;
    uint32_t timing[4];
    uint32_t vdctrl0;
    uint32_t vdctrl1;
    uint32_t vdctrl2;
    uint32_t vdctrl3;
    uint32_t hw_timing;
    uint32_t vdctrl[4];
    uint32_t dvctrl[4];
    uint32_t irq;
    uint32_t irq_en;

    qemu_irq irq_out;

    STMP3770DMAState *dma;
    int dma_channel;

    uint32_t dma_pio_ctrl;
    uint8_t panel_cmd;
    uint8_t panel_param_cmd;
    uint8_t panel_param_len;
    uint8_t panel_param[4];
    uint16_t panel_x_start;
    uint16_t panel_x_end;
    uint16_t panel_y_start;
    uint16_t panel_y_end;
    uint16_t panel_x;
    uint16_t panel_y;
    bool first_read_dummy_pending;
    uint8_t panel_vram[STMP3770_LCDIF_PANEL_SIZE];
    bool panel_dirty;
    bool display_on;

    STMP3770PINCTRLState *pinctrl;
    QemuInputHandlerState *input_handler;
    uint64_t frontpanel_key_state[2];
    int frontpanel_mouse_x;
    int frontpanel_mouse_y;
    int frontpanel_mouse_key;

    int width;
    int height;
    pixman_format_code_t surface_format;
};

void stmp3770_lcdif_set_dma(STMP3770LCDIFState *s, STMP3770DMAState *dma,
                            int channel);
void stmp3770_lcdif_set_pinctrl(STMP3770LCDIFState *s,
                                STMP3770PINCTRLState *pinctrl);

#endif /* STMP3770_LCDIF_H */
