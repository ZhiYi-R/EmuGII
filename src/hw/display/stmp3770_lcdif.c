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

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "ui/pixel_ops.h"
#include "ui/vgafont.h"
#include "hw/display/framebuffer.h"
#include "hw/display/stmp3770_lcdif.h"

#define LCDIF_VERSION   0x02000000

/* Register offsets */
#define REG_CTRL0       0x000
#define REG_CTRL0_SET   0x004
#define REG_CTRL0_CLR   0x008
#define REG_CTRL0_TOG   0x00C
#define REG_CTRL1       0x010
#define REG_CTRL1_SET   0x014
#define REG_CTRL1_CLR   0x018
#define REG_CTRL1_TOG   0x01C
#define REG_TIMING      0x020
#define REG_VDCTRL0     0x030
#define REG_VDCTRL0_SET 0x034
#define REG_VDCTRL0_CLR 0x038
#define REG_VDCTRL0_TOG 0x03C
#define REG_VDCTRL1     0x040
#define REG_VDCTRL2     0x050
#define REG_VDCTRL3     0x060
#define REG_DVICTRL0    0x070
#define REG_DVICTRL1    0x080
#define REG_DVICTRL2    0x090
#define REG_DVICTRL3    0x0A0
#define REG_DATA        0x0B0
#define REG_STAT        0x0C0
#define REG_VERSION     0x0D0
#define REG_DEBUG0      0x0E0

/* CTRL0 bits */
#define CTRL0_SFTRST    (1U << 31)
#define CTRL0_CLKGATE   (1U << 30)
#define CTRL0_READ_WRITEB (1U << 29)
#define CTRL0_WAIT_FOR_VSYNC_EDGE (1U << 28)
#define CTRL0_DATA_SHIFT_DIR (1U << 27)
#define CTRL0_SHIFT_NUM_BITS_MASK (3U << 25)
#define CTRL0_DVI_MODE (1U << 24)
#define CTRL0_BYPASS_COUNT (1U << 23)
#define CTRL0_DATA_SWIZZLE_MASK (3U << 21)
#define CTRL0_VSYNC_MODE (1U << 20)
#define CTRL0_DOTCLK_MODE (1U << 19)
#define CTRL0_DATA_SELECT (1U << 18)
#define CTRL0_WORD_LENGTH (1U << 17)
#define CTRL0_RUN       (1U << 16)
#define CTRL0_COUNT_MASK 0xFFFF
#define CTRL0_WRITABLE_MASK 0xFFFFFFFFU

/* CTRL1 bits */
#define CTRL1_OVERFLOW_IRQ_EN       (1U << 15)
#define CTRL1_UNDERFLOW_IRQ_EN      (1U << 14)
#define CTRL1_CUR_FRAME_DONE_IRQ_EN (1U << 13)
#define CTRL1_VSYNC_EDGE_IRQ_EN     (1U << 12)
#define CTRL1_OVERFLOW_IRQ          (1U << 11)
#define CTRL1_UNDERFLOW_IRQ         (1U << 10)
#define CTRL1_CUR_FRAME_DONE_IRQ    (1U << 9)
#define CTRL1_VSYNC_EDGE_IRQ        (1U << 8)

#define CTRL1_IRQ_EN_MASK (CTRL1_OVERFLOW_IRQ_EN | CTRL1_UNDERFLOW_IRQ_EN | \
                           CTRL1_CUR_FRAME_DONE_IRQ_EN | \
                           CTRL1_VSYNC_EDGE_IRQ_EN)
#define CTRL1_IRQ_MASK    (CTRL1_OVERFLOW_IRQ | CTRL1_UNDERFLOW_IRQ | \
                           CTRL1_CUR_FRAME_DONE_IRQ | CTRL1_VSYNC_EDGE_IRQ)
#define CTRL1_WRITABLE_MASK 0x000FFFFFU
#define CTRL1_RESET_VALUE (0xFU << 16)

#define VDCTRL0_WRITABLE_MASK 0x3F3803FFU
#define VDCTRL3_WRITABLE_MASK 0x01FFF1FFU
#define DVICTRL1_WRITABLE_MASK 0x3FFFFFFFU
#define DVICTRL3_WRITABLE_MASK 0x03FF03FFU

/* IRQ bits */
#define IRQ_VSYNC           (1U << 0)
#define IRQ_CUR_FRAME_DONE  (1U << 1)
#define IRQ_UNDERFLOW       (1U << 2)
#define IRQ_OVERFLOW        (1U << 3)

/* STAT bits */
#define STAT_PRESENT        (1U << 31)
#define STAT_DMA_REQ        (1U << 30)
#define STAT_RXFIFO_FULL    (1U << 29)
#define STAT_RXFIFO_EMPTY   (1U << 28)
#define STAT_TXFIFO_FULL    (1U << 27)
#define STAT_TXFIFO_EMPTY   (1U << 26)
#define STAT_BUSY           (1U << 25)
#define STAT_DVI_CURRENT_FIELD (1U << 24)
#define STAT_RESET_VALUE (STAT_PRESENT | STAT_RXFIFO_EMPTY)
#define DEBUG0_RESET_VALUE 0x0E810000U

#define REFRESH_RATE_HZ     60
#define NS_PER_SEC          1000000000ULL

#define FP_LCD_X        40
#define FP_LCD_Y        50
#define FP_LCD_SCALE    2
#define FP_LCD_W        (STMP3770_LCDIF_VIEW_WIDTH * FP_LCD_SCALE)
#define FP_LCD_H        (STMP3770_LCDIF_VIEW_HEIGHT * FP_LCD_SCALE)

#define FP_STATUS_X     560
#define FP_STATUS_Y     42
#define FP_STATUS_W     430
#define FP_STATUS_H     58

#define FP_RIGHT_X      560
#define FP_RIGHT_Y      130
#define FP_KEY_W        72
#define FP_KEY_H        40
#define FP_KEY_GAP_X    20
#define FP_KEY_GAP_Y    22

#define FP_LEFT_X       64
#define FP_LEFT_F_Y     385
#define FP_LEFT_KEY_W   56
#define FP_LEFT_KEY_H   32
#define FP_LEFT_GAP_X   22

#define FP_NAV_X        325
#define FP_NAV_Y        410
#define FP_NAV_SIZE     150

#define HP39GII_KEY(row, col) (((row) << 3) | (col))
#define HP39GII_KEY_NONE (-1)

static inline bool lcdif_enabled(STMP3770LCDIFState *s)
{
    return (s->ctrl0 & (CTRL0_SFTRST | CTRL0_CLKGATE)) == 0 &&
           (s->ctrl0 & CTRL0_RUN) != 0;
}

static void lcdif_update_irq(STMP3770LCDIFState *s)
{
    bool pending = (s->irq & s->irq_en) != 0;
    qemu_set_irq(s->irq_out, pending);
}

static uint32_t lcdif_ctrl1_to_irq_en(uint32_t ctrl1)
{
    uint32_t irq_en = 0;

    if (ctrl1 & CTRL1_VSYNC_EDGE_IRQ_EN) {
        irq_en |= IRQ_VSYNC;
    }
    if (ctrl1 & CTRL1_CUR_FRAME_DONE_IRQ_EN) {
        irq_en |= IRQ_CUR_FRAME_DONE;
    }
    if (ctrl1 & CTRL1_UNDERFLOW_IRQ_EN) {
        irq_en |= IRQ_UNDERFLOW;
    }
    if (ctrl1 & CTRL1_OVERFLOW_IRQ_EN) {
        irq_en |= IRQ_OVERFLOW;
    }

    return irq_en;
}

static uint32_t lcdif_irq_to_ctrl1(uint32_t irq)
{
    uint32_t ctrl1 = 0;

    if (irq & IRQ_VSYNC) {
        ctrl1 |= CTRL1_VSYNC_EDGE_IRQ;
    }
    if (irq & IRQ_CUR_FRAME_DONE) {
        ctrl1 |= CTRL1_CUR_FRAME_DONE_IRQ;
    }
    if (irq & IRQ_UNDERFLOW) {
        ctrl1 |= CTRL1_UNDERFLOW_IRQ;
    }
    if (irq & IRQ_OVERFLOW) {
        ctrl1 |= CTRL1_OVERFLOW_IRQ;
    }

    return ctrl1;
}

static uint32_t lcdif_ctrl1_status_to_irq(uint32_t ctrl1)
{
    uint32_t irq = 0;

    if (ctrl1 & CTRL1_VSYNC_EDGE_IRQ) {
        irq |= IRQ_VSYNC;
    }
    if (ctrl1 & CTRL1_CUR_FRAME_DONE_IRQ) {
        irq |= IRQ_CUR_FRAME_DONE;
    }
    if (ctrl1 & CTRL1_UNDERFLOW_IRQ) {
        irq |= IRQ_UNDERFLOW;
    }
    if (ctrl1 & CTRL1_OVERFLOW_IRQ) {
        irq |= IRQ_OVERFLOW;
    }

    return irq;
}

static void lcdif_panel_seek_start(STMP3770LCDIFState *s)
{
    s->panel_x = s->panel_x_start;
    s->panel_y = s->panel_y_start;
}

static void lcdif_panel_reset_window(STMP3770LCDIFState *s)
{
    s->panel_x_start = 0;
    s->panel_x_end = STMP3770_LCDIF_PANEL_WIDTH - 1;
    s->panel_y_start = 0;
    s->panel_y_end = STMP3770_LCDIF_PANEL_HEIGHT - 1;
    lcdif_panel_seek_start(s);
}

static void lcdif_panel_advance(STMP3770LCDIFState *s)
{
    if (s->panel_x < s->panel_x_end) {
        s->panel_x++;
        return;
    }

    s->panel_x = s->panel_x_start;
    if (s->panel_y < s->panel_y_end) {
        s->panel_y++;
    }
}

static uint16_t lcdif_panel_clamp_x(uint32_t x)
{
    return MIN(x, STMP3770_LCDIF_PANEL_WIDTH - 1);
}

static uint16_t lcdif_panel_clamp_y(uint32_t y)
{
    return MIN(y, STMP3770_LCDIF_PANEL_HEIGHT - 1);
}

static void lcdif_panel_finish_param(STMP3770LCDIFState *s)
{
    uint32_t start = (s->panel_param[0] << 8) | s->panel_param[1];
    uint32_t end = (s->panel_param[2] << 8) | s->panel_param[3];

    if (end < start) {
        uint32_t tmp = start;
        start = end;
        end = tmp;
    }

    switch (s->panel_param_cmd) {
    case 0x2A: /* Column address set: panel columns are 3 byte-wide. */
        s->panel_x_start = lcdif_panel_clamp_x(start * 3);
        s->panel_x_end = lcdif_panel_clamp_x(end * 3 + 2);
        break;
    case 0x2B: /* Page/row address set. */
        s->panel_y_start = lcdif_panel_clamp_y(start);
        s->panel_y_end = lcdif_panel_clamp_y(end);
        break;
    default:
        break;
    }

    s->panel_param_cmd = 0;
    s->panel_param_len = 0;
    lcdif_panel_seek_start(s);
}

static void lcdif_panel_command_byte(STMP3770LCDIFState *s, uint8_t value)
{
    s->panel_cmd = value;

    switch (value) {
    case 0x2A: /* Column address set. */
    case 0x2B: /* Page/row address set. */
        s->panel_param_cmd = value;
        s->panel_param_len = 0;
        break;
    case 0x2C: /* Memory write. */
    case 0x2E: /* Memory read. */
        s->panel_param_cmd = 0;
        s->panel_param_len = 0;
        lcdif_panel_seek_start(s);
        break;
    default:
        s->panel_param_cmd = 0;
        s->panel_param_len = 0;
        break;
    }
}

static void lcdif_panel_write_pixel_byte(STMP3770LCDIFState *s, uint8_t value)
{
    if (s->panel_x < STMP3770_LCDIF_PANEL_WIDTH &&
        s->panel_y < STMP3770_LCDIF_PANEL_HEIGHT) {
        uint32_t offset = s->panel_y * STMP3770_LCDIF_PANEL_WIDTH +
                          s->panel_x;

        if (s->panel_vram[offset] != value) {
            s->panel_vram[offset] = value;
            s->panel_dirty = true;
        }
    }
    lcdif_panel_advance(s);
}

static uint8_t lcdif_panel_read_pixel_byte(STMP3770LCDIFState *s)
{
    uint8_t value = 0;

    if (s->panel_x < STMP3770_LCDIF_PANEL_WIDTH &&
        s->panel_y < STMP3770_LCDIF_PANEL_HEIGHT) {
        value = s->panel_vram[s->panel_y * STMP3770_LCDIF_PANEL_WIDTH +
                              s->panel_x];
    }
    lcdif_panel_advance(s);
    return value;
}

static void lcdif_panel_data_byte(STMP3770LCDIFState *s, uint8_t value)
{
    if (s->panel_param_cmd != 0) {
        if (s->panel_param_len < sizeof(s->panel_param)) {
            s->panel_param[s->panel_param_len++] = value;
        }
        if (s->panel_param_len == sizeof(s->panel_param)) {
            lcdif_panel_finish_param(s);
        }
        return;
    }

    if (s->panel_cmd == 0x2C) {
        lcdif_panel_write_pixel_byte(s, value);
    }
}

static uint8_t lcdif_panel_read_data_byte(STMP3770LCDIFState *s)
{
    switch (s->panel_cmd) {
    case 0x2E:
        return lcdif_panel_read_pixel_byte(s);
    case 0xDA:
        return 0x00;
    case 0xDB:
        return 0x80;
    case 0xDC:
        return 0x00;
    default:
        return 0;
    }
}

static void lcdif_panel_write(STMP3770LCDIFState *s, const uint8_t *buf,
                              size_t len, bool data_select)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (data_select) {
            lcdif_panel_data_byte(s, buf[i]);
        } else {
            lcdif_panel_command_byte(s, buf[i]);
        }
    }
}

static void lcdif_panel_read(STMP3770LCDIFState *s, uint8_t *buf, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        buf[i] = lcdif_panel_read_data_byte(s);
    }
}

static uint32_t lcdif_rgb_to_surface_pixel(STMP3770LCDIFState *s,
                                           unsigned int r, unsigned int g,
                                           unsigned int b)
{
    switch (s->surface_format) {
    case PIXMAN_r5g6b5:
        return rgb_to_pixel16(r, g, b);
    case PIXMAN_b5g6r5:
        return rgb_to_pixel16bgr(r, g, b);
    case PIXMAN_r8g8b8:
        return rgb_to_pixel24(r, g, b);
    case PIXMAN_b8g8r8:
        return rgb_to_pixel24bgr(r, g, b);
    case PIXMAN_x8b8g8r8:
    case PIXMAN_a8b8g8r8:
        return rgb_to_pixel32bgr(r, g, b);
    case PIXMAN_x8r8g8b8:
    case PIXMAN_a8r8g8b8:
    default:
        return rgb_to_pixel32(r, g, b);
    }
}

static void lcdif_store_surface_pixel(uint8_t *dest, int deststep,
                                      uint32_t val)
{
    switch (deststep) {
    case 1:
        dest[0] = (uint8_t)val;
        break;
    case 2:
        ((uint16_t *)dest)[0] = (uint16_t)val;
        break;
    case 3:
        dest[0] = val & 0xFF;
        dest[1] = (val >> 8) & 0xFF;
        dest[2] = (val >> 16) & 0xFF;
        break;
    case 4:
        ((uint32_t *)dest)[0] = val;
        break;
    default:
        memset(dest, 0, deststep);
        memcpy(dest, &val, MIN(deststep, (int)sizeof(val)));
        break;
    }
}

typedef struct STMP3770FrontpanelButton {
    int x;
    int y;
    int w;
    int h;
    int key;
    QKeyCode qcode;
    const char *primary;
    const char *secondary;
} STMP3770FrontpanelButton;

#define FP_RX(col) (FP_RIGHT_X + (col) * (FP_KEY_W + FP_KEY_GAP_X))
#define FP_RY(row) (FP_RIGHT_Y + (row) * (FP_KEY_H + FP_KEY_GAP_Y))
#define FP_LFX(idx) (FP_LEFT_X + (idx) * (FP_LEFT_KEY_W + FP_LEFT_GAP_X))

static const STMP3770FrontpanelButton frontpanel_buttons[] = {
    { FP_RX(0), FP_RY(0), FP_KEY_W, FP_KEY_H, HP39GII_KEY(3, 0), Q_KEY_CODE_UNMAPPED, "Vars", "Chars A" },
    { FP_RX(1), FP_RY(0), FP_KEY_W, FP_KEY_H, HP39GII_KEY(4, 1), Q_KEY_CODE_UNMAPPED, "Math", "Cmds B" },
    { FP_RX(2), FP_RY(0), FP_KEY_W, FP_KEY_H, HP39GII_KEY(3, 2), Q_KEY_CODE_UNMAPPED, "a b/c", "'' C" },
    { FP_RX(3), FP_RY(0), FP_KEY_W, FP_KEY_H, HP39GII_KEY(2, 3), Q_KEY_CODE_UNMAPPED, "x,T,N", "EEX D" },
    { FP_RX(4), FP_RY(0), FP_KEY_W, FP_KEY_H, HP39GII_KEY(3, 3), Q_KEY_CODE_BACKSPACE, "<-", "Clear" },

    { FP_RX(0), FP_RY(1), FP_KEY_W, FP_KEY_H, HP39GII_KEY(4, 0), Q_KEY_CODE_UNMAPPED, "SIN", "ASIN E" },
    { FP_RX(1), FP_RY(1), FP_KEY_W, FP_KEY_H, HP39GII_KEY(5, 1), Q_KEY_CODE_UNMAPPED, "COS", "ACOS F" },
    { FP_RX(2), FP_RY(1), FP_KEY_W, FP_KEY_H, HP39GII_KEY(4, 2), Q_KEY_CODE_UNMAPPED, "TAN", "ATAN G" },
    { FP_RX(3), FP_RY(1), FP_KEY_W, FP_KEY_H, HP39GII_KEY(4, 3), Q_KEY_CODE_UNMAPPED, "LN", "exp H" },
    { FP_RX(4), FP_RY(1), FP_KEY_W, FP_KEY_H, HP39GII_KEY(4, 4), Q_KEY_CODE_UNMAPPED, "LOG", "10^x I" },

    { FP_RX(0), FP_RY(2), FP_KEY_W, FP_KEY_H, HP39GII_KEY(5, 0), Q_KEY_CODE_UNMAPPED, "x^2", "sqrt J" },
    { FP_RX(1), FP_RY(2), FP_KEY_W, FP_KEY_H, HP39GII_KEY(6, 1), Q_KEY_CODE_UNMAPPED, "x^y", "root K" },
    { FP_RX(2), FP_RY(2), FP_KEY_W, FP_KEY_H, HP39GII_KEY(5, 2), Q_KEY_CODE_BRACKET_LEFT, "(", "Copy L" },
    { FP_RX(3), FP_RY(2), FP_KEY_W, FP_KEY_H, HP39GII_KEY(5, 3), Q_KEY_CODE_BRACKET_RIGHT, ")", "Paste M" },
    { FP_RX(4), FP_RY(2), FP_KEY_W, FP_KEY_H, HP39GII_KEY(5, 4), Q_KEY_CODE_KP_DIVIDE, "/", "x^-1 N" },

    { FP_RX(0), FP_RY(3), FP_KEY_W, FP_KEY_H, HP39GII_KEY(6, 0), Q_KEY_CODE_COMMA, ",", "Memo O" },
    { FP_RX(1), FP_RY(3), FP_KEY_W, FP_KEY_H, HP39GII_KEY(7, 1), Q_KEY_CODE_7, "7", "List P" },
    { FP_RX(2), FP_RY(3), FP_KEY_W, FP_KEY_H, HP39GII_KEY(6, 2), Q_KEY_CODE_8, "8", "{ Q" },
    { FP_RX(3), FP_RY(3), FP_KEY_W, FP_KEY_H, HP39GII_KEY(6, 3), Q_KEY_CODE_9, "9", "} R" },
    { FP_RX(4), FP_RY(3), FP_KEY_W, FP_KEY_H, HP39GII_KEY(6, 4), Q_KEY_CODE_KP_MULTIPLY, "*", "! S" },

    { FP_RX(0), FP_RY(4), FP_KEY_W, FP_KEY_H, HP39GII_KEY(7, 0), Q_KEY_CODE_A, "ALPHA", "alpha" },
    { FP_RX(1), FP_RY(4), FP_KEY_W, FP_KEY_H, HP39GII_KEY(8, 1), Q_KEY_CODE_4, "4", "Matrix T" },
    { FP_RX(2), FP_RY(4), FP_KEY_W, FP_KEY_H, HP39GII_KEY(7, 2), Q_KEY_CODE_5, "5", "U" },
    { FP_RX(3), FP_RY(4), FP_KEY_W, FP_KEY_H, HP39GII_KEY(7, 3), Q_KEY_CODE_6, "6", "V" },
    { FP_RX(4), FP_RY(4), FP_KEY_W, FP_KEY_H, HP39GII_KEY(7, 4), Q_KEY_CODE_KP_SUBTRACT, "-", "W" },

    { FP_RX(0), FP_RY(5), FP_KEY_W, FP_KEY_H, HP39GII_KEY(8, 0), Q_KEY_CODE_SHIFT, "SHIFT", "" },
    { FP_RX(1), FP_RY(5), FP_KEY_W, FP_KEY_H, HP39GII_KEY(9, 1), Q_KEY_CODE_1, "1", "Prgm X" },
    { FP_RX(2), FP_RY(5), FP_KEY_W, FP_KEY_H, HP39GII_KEY(8, 2), Q_KEY_CODE_2, "2", "i Y" },
    { FP_RX(3), FP_RY(5), FP_KEY_W, FP_KEY_H, HP39GII_KEY(8, 3), Q_KEY_CODE_3, "3", "pi Z" },
    { FP_RX(4), FP_RY(5), FP_KEY_W, FP_KEY_H, HP39GII_KEY(8, 4), Q_KEY_CODE_KP_ADD, "+", "sum" },

    { FP_RX(0), FP_RY(6), FP_KEY_W, FP_KEY_H, HP39GII_KEY(10, 0), Q_KEY_CODE_POWER, "ON/C", "OFF" },
    { FP_RX(1), FP_RY(6), FP_KEY_W, FP_KEY_H, HP39GII_KEY(9, 0), Q_KEY_CODE_0, "0", "Notes" },
    { FP_RX(2), FP_RY(6), FP_KEY_W, FP_KEY_H, HP39GII_KEY(9, 2), Q_KEY_CODE_DOT, ".", "=" },
    { FP_RX(3), FP_RY(6), FP_KEY_W, FP_KEY_H, HP39GII_KEY(9, 3), Q_KEY_CODE_UNMAPPED, "(-)", "ABS" },
    { FP_RX(4), FP_RY(6), FP_KEY_W, FP_KEY_H, HP39GII_KEY(9, 4), Q_KEY_CODE_RET, "ENTER", "ANS" },

    { FP_LFX(0), FP_LEFT_F_Y, FP_LEFT_KEY_W, FP_LEFT_KEY_H, HP39GII_KEY(0, 0), Q_KEY_CODE_F1, "F1", "" },
    { FP_LFX(1), FP_LEFT_F_Y, FP_LEFT_KEY_W, FP_LEFT_KEY_H, HP39GII_KEY(1, 1), Q_KEY_CODE_F2, "F2", "" },
    { FP_LFX(2), FP_LEFT_F_Y, FP_LEFT_KEY_W, FP_LEFT_KEY_H, HP39GII_KEY(0, 1), Q_KEY_CODE_F3, "F3", "" },
    { FP_LFX(3), FP_LEFT_F_Y, FP_LEFT_KEY_W, FP_LEFT_KEY_H, HP39GII_KEY(0, 2), Q_KEY_CODE_F4, "F4", "" },
    { FP_LFX(4), FP_LEFT_F_Y, FP_LEFT_KEY_W, FP_LEFT_KEY_H, HP39GII_KEY(0, 3), Q_KEY_CODE_F5, "F5", "" },
    { FP_LFX(5), FP_LEFT_F_Y, FP_LEFT_KEY_W, FP_LEFT_KEY_H, HP39GII_KEY(1, 3), Q_KEY_CODE_F6, "F6", "" },

    { FP_LEFT_X, FP_LEFT_F_Y + 62, FP_LEFT_KEY_W, FP_KEY_H, HP39GII_KEY(1, 0), Q_KEY_CODE_UNMAPPED, "Symb", "Setup" },
    { FP_LEFT_X + 78, FP_LEFT_F_Y + 62, FP_LEFT_KEY_W, FP_KEY_H, HP39GII_KEY(2, 1), Q_KEY_CODE_UNMAPPED, "Plot", "Setup" },
    { FP_LEFT_X + 156, FP_LEFT_F_Y + 62, FP_LEFT_KEY_W, FP_KEY_H, HP39GII_KEY(1, 2), Q_KEY_CODE_NUM_LOCK, "Num", "Setup" },
    { FP_LEFT_X, FP_LEFT_F_Y + 136, FP_LEFT_KEY_W, FP_KEY_H, HP39GII_KEY(2, 0), Q_KEY_CODE_HOME, "Home", "Modes" },
    { FP_LEFT_X + 78, FP_LEFT_F_Y + 136, FP_LEFT_KEY_W, FP_KEY_H, HP39GII_KEY(3, 1), Q_KEY_CODE_UNMAPPED, "Apps", "Info" },
    { FP_LEFT_X + 156, FP_LEFT_F_Y + 136, FP_LEFT_KEY_W, FP_KEY_H, HP39GII_KEY(2, 2), Q_KEY_CODE_UNMAPPED, "Views", "Help" },
};

#undef FP_RX
#undef FP_RY
#undef FP_LFX

static bool frontpanel_key_is_down(STMP3770LCDIFState *s, int key)
{
    if (key < 0 || key >= 128) {
        return false;
    }

    return (s->frontpanel_key_state[key >> 6] & (1ULL << (key & 63))) != 0;
}

static void frontpanel_set_key(STMP3770LCDIFState *s, int key, bool down)
{
    if (key < 0 || key >= 128) {
        return;
    }

    if (down) {
        s->frontpanel_key_state[key >> 6] |= 1ULL << (key & 63);
    } else {
        s->frontpanel_key_state[key >> 6] &= ~(1ULL << (key & 63));
    }

    if (s->pinctrl) {
        stmp3770_pinctrl_set_key(s->pinctrl, key, down);
    }
    s->panel_dirty = true;
}

static void frontpanel_put_pixel(STMP3770LCDIFState *s, DisplaySurface *surface,
                                 int x, int y, uint32_t pixel)
{
    int deststep;
    int stride;

    if (x < 0 || y < 0 ||
        x >= surface_width(surface) || y >= surface_height(surface)) {
        return;
    }

    deststep = surface_bytes_per_pixel(surface);
    stride = surface_stride(surface);
    if (deststep <= 0 || stride <= 0) {
        return;
    }

    lcdif_store_surface_pixel(surface_data(surface) + y * stride + x * deststep,
                              deststep, pixel);
}

static void frontpanel_fill_rect(STMP3770LCDIFState *s,
                                 DisplaySurface *surface,
                                 int x, int y, int w, int h,
                                 uint32_t pixel)
{
    int yy;
    int xx;

    for (yy = MAX(y, 0); yy < MIN(y + h, surface_height(surface)); yy++) {
        for (xx = MAX(x, 0); xx < MIN(x + w, surface_width(surface)); xx++) {
            frontpanel_put_pixel(s, surface, xx, yy, pixel);
        }
    }
}

static void frontpanel_draw_rect(STMP3770LCDIFState *s,
                                 DisplaySurface *surface,
                                 int x, int y, int w, int h,
                                 uint32_t pixel)
{
    int i;

    for (i = 0; i < w; i++) {
        frontpanel_put_pixel(s, surface, x + i, y, pixel);
        frontpanel_put_pixel(s, surface, x + i, y + h - 1, pixel);
    }
    for (i = 0; i < h; i++) {
        frontpanel_put_pixel(s, surface, x, y + i, pixel);
        frontpanel_put_pixel(s, surface, x + w - 1, y + i, pixel);
    }
}

static int frontpanel_text_width(const char *text, int scale)
{
    return (int)strlen(text) * 8 * scale;
}

static void frontpanel_draw_text(STMP3770LCDIFState *s,
                                 DisplaySurface *surface,
                                 int x, int y, const char *text,
                                 int scale, uint32_t pixel)
{
    const unsigned char *p = (const unsigned char *)text;
    int cx = x;

    while (*p) {
        const uint8_t *glyph = &vgafont16[*p * 16];
        int row;
        int bit;
        int sy;
        int sx;

        for (row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            for (bit = 0; bit < 8; bit++) {
                if ((bits & (0x80 >> bit)) == 0) {
                    continue;
                }
                for (sy = 0; sy < scale; sy++) {
                    for (sx = 0; sx < scale; sx++) {
                        frontpanel_put_pixel(s, surface,
                                             cx + bit * scale + sx,
                                             y + row * scale + sy,
                                             pixel);
                    }
                }
            }
        }
        cx += 8 * scale;
        p++;
    }
}

static void frontpanel_draw_button(STMP3770LCDIFState *s,
                                   DisplaySurface *surface,
                                   const STMP3770FrontpanelButton *button)
{
    bool down = frontpanel_key_is_down(s, button->key);
    uint32_t border = lcdif_rgb_to_surface_pixel(s, 28, 28, 28);
    uint32_t shadow = lcdif_rgb_to_surface_pixel(s, 80, 80, 80);
    uint32_t hi = lcdif_rgb_to_surface_pixel(s, down ? 120 : 232,
                                             down ? 145 : 232,
                                             down ? 165 : 232);
    uint32_t mid = lcdif_rgb_to_surface_pixel(s, down ? 92 : 190,
                                              down ? 118 : 196,
                                              down ? 138 : 198);
    uint32_t text = lcdif_rgb_to_surface_pixel(s, 0, 0, 0);
    uint32_t blue = lcdif_rgb_to_surface_pixel(s, 0, 76, 170);
    int y;
    int primary_scale;
    int tx;

    frontpanel_fill_rect(s, surface, button->x + 3, button->y + 3,
                         button->w, button->h, shadow);
    frontpanel_fill_rect(s, surface, button->x, button->y,
                         button->w, button->h, border);
    frontpanel_fill_rect(s, surface, button->x + 3, button->y + 3,
                         button->w - 6, button->h - 6, mid);
    for (y = button->y + 3; y < button->y + button->h / 2; y++) {
        frontpanel_fill_rect(s, surface, button->x + 4, y,
                             button->w - 8, 1, hi);
    }

    primary_scale = strlen(button->primary) <= 2 ? 2 : 1;
    tx = button->x + (button->w -
         frontpanel_text_width(button->primary, primary_scale)) / 2;
    frontpanel_draw_text(s, surface, tx, button->y + 6,
                         button->primary, primary_scale, text);

    if (button->secondary && button->secondary[0]) {
        tx = button->x + (button->w -
             frontpanel_text_width(button->secondary, 1)) / 2;
        frontpanel_draw_text(s, surface, tx, button->y + button->h - 18,
                             button->secondary, 1, blue);
    }
}

static void frontpanel_draw_lcd(STMP3770LCDIFState *s,
                                DisplaySurface *surface)
{
    uint32_t border = lcdif_rgb_to_surface_pixel(s, 24, 24, 24);
    uint32_t bezel = lcdif_rgb_to_surface_pixel(s, 75, 75, 75);
    int x;
    int y;
    int sx;
    int sy;

    frontpanel_fill_rect(s, surface, FP_LCD_X - 4, FP_LCD_Y - 4,
                         FP_LCD_W + 8, FP_LCD_H + 8, bezel);
    frontpanel_draw_rect(s, surface, FP_LCD_X - 5, FP_LCD_Y - 5,
                         FP_LCD_W + 10, FP_LCD_H + 10, border);

    for (y = 0; y < STMP3770_LCDIF_VIEW_HEIGHT; y++) {
        const uint8_t *src =
            &s->panel_vram[(y + STMP3770_LCDIF_VIEW_Y) *
                           STMP3770_LCDIF_PANEL_WIDTH];

        for (x = 0; x < STMP3770_LCDIF_VIEW_WIDTH; x++) {
            uint8_t gray = src[x];
            uint32_t pixel = lcdif_rgb_to_surface_pixel(s, gray, gray, gray);

            for (sy = 0; sy < FP_LCD_SCALE; sy++) {
                for (sx = 0; sx < FP_LCD_SCALE; sx++) {
                    frontpanel_put_pixel(s, surface,
                                         FP_LCD_X + x * FP_LCD_SCALE + sx,
                                         FP_LCD_Y + y * FP_LCD_SCALE + sy,
                                         pixel);
                }
            }
        }
    }
}

static bool frontpanel_indicator_active(STMP3770LCDIFState *s, int panel_x)
{
    int y;

    if (panel_x < 0 || panel_x >= STMP3770_LCDIF_PANEL_WIDTH) {
        return false;
    }

    for (y = 0; y < 24 && y < STMP3770_LCDIF_PANEL_HEIGHT; y++) {
        if (s->panel_vram[y * STMP3770_LCDIF_PANEL_WIDTH + panel_x] < 128) {
            return true;
        }
    }
    return false;
}

static void frontpanel_draw_status(STMP3770LCDIFState *s,
                                   DisplaySurface *surface)
{
    static const struct {
        int sample_x;
        const char *label;
    } indicators[] = {
        { 10, "A-Z" },
        { 28, "TX" },
        { 37, "L" },
        { 44, "a-z" },
        { 50, "RX" },
        { 64, "R" },
        { 82, "BUSY" },
        { 76, "BAT" },
    };
    uint32_t white = lcdif_rgb_to_surface_pixel(s, 248, 248, 248);
    uint32_t border = lcdif_rgb_to_surface_pixel(s, 120, 120, 120);
    uint32_t inactive = lcdif_rgb_to_surface_pixel(s, 226, 226, 226);
    uint32_t active = lcdif_rgb_to_surface_pixel(s, 35, 93, 132);
    uint32_t text = lcdif_rgb_to_surface_pixel(s, 20, 20, 20);
    uint32_t active_text = lcdif_rgb_to_surface_pixel(s, 255, 255, 255);
    int i;
    int x = FP_STATUS_X + 12;

    frontpanel_fill_rect(s, surface, FP_STATUS_X, FP_STATUS_Y,
                         FP_STATUS_W, FP_STATUS_H, white);
    frontpanel_draw_rect(s, surface, FP_STATUS_X, FP_STATUS_Y,
                         FP_STATUS_W, FP_STATUS_H, border);

    for (i = 0; i < ARRAY_SIZE(indicators); i++) {
        bool on = frontpanel_indicator_active(s, indicators[i].sample_x);
        uint32_t fill = on ? active : inactive;
        uint32_t fg = on ? active_text : text;

        frontpanel_fill_rect(s, surface, x, FP_STATUS_Y + 16, 45, 24, fill);
        frontpanel_draw_rect(s, surface, x, FP_STATUS_Y + 16, 45, 24, border);
        frontpanel_draw_text(s, surface, x + 5, FP_STATUS_Y + 20,
                             indicators[i].label, 1, fg);
        x += 50;
    }
}

static void frontpanel_draw_nav(STMP3770LCDIFState *s, DisplaySurface *surface)
{
    uint32_t dark = lcdif_rgb_to_surface_pixel(s, 58, 58, 58);
    uint32_t mid = lcdif_rgb_to_surface_pixel(s, 170, 170, 170);
    uint32_t light = lcdif_rgb_to_surface_pixel(s, 225, 225, 225);
    uint32_t blue = lcdif_rgb_to_surface_pixel(s, 0, 72, 160);
    int cx = FP_NAV_X + FP_NAV_SIZE / 2;
    int cy = FP_NAV_Y + FP_NAV_SIZE / 2;
    int r = FP_NAV_SIZE / 2;
    int x;
    int y;

    for (y = -r; y <= r; y++) {
        for (x = -r; x <= r; x++) {
            int d2 = x * x + y * y;
            if (d2 <= r * r) {
                uint32_t pixel = (d2 < (r - 18) * (r - 18)) ? mid : dark;
                if (x < -20 || y < -20) {
                    pixel = light;
                }
                frontpanel_put_pixel(s, surface, cx + x, cy + y, pixel);
            }
        }
    }

    frontpanel_draw_text(s, surface, cx - 4, FP_NAV_Y + 12, "^", 1, blue);
    frontpanel_draw_text(s, surface, cx - 4, FP_NAV_Y + FP_NAV_SIZE - 28,
                         "v", 1, blue);
    frontpanel_draw_text(s, surface, FP_NAV_X + 16, cy - 8, "<", 1, blue);
    frontpanel_draw_text(s, surface, FP_NAV_X + FP_NAV_SIZE - 24, cy - 8,
                         ">", 1, blue);
}

static void frontpanel_draw_background(STMP3770LCDIFState *s,
                                       DisplaySurface *surface)
{
    uint32_t bg = lcdif_rgb_to_surface_pixel(s, 244, 244, 244);
    uint32_t dot = lcdif_rgb_to_surface_pixel(s, 185, 185, 185);
    uint32_t border = lcdif_rgb_to_surface_pixel(s, 16, 115, 155);
    uint32_t text = lcdif_rgb_to_surface_pixel(s, 0, 0, 0);
    int x;
    int y;

    frontpanel_fill_rect(s, surface, 0, 0,
                         STMP3770_LCDIF_FRONTPANEL_WIDTH,
                         STMP3770_LCDIF_FRONTPANEL_HEIGHT, bg);

    for (y = 22; y < STMP3770_LCDIF_FRONTPANEL_HEIGHT - 18; y += 8) {
        for (x = 8; x < STMP3770_LCDIF_FRONTPANEL_WIDTH - 8; x += 8) {
            frontpanel_put_pixel(s, surface, x, y, dot);
        }
    }

    frontpanel_draw_rect(s, surface, 0, 0,
                         STMP3770_LCDIF_FRONTPANEL_WIDTH,
                         STMP3770_LCDIF_FRONTPANEL_HEIGHT, border);
    frontpanel_draw_text(s, surface, 8, 8, "Exist OS Emulator (ARM)", 1, text);
}

static int frontpanel_find_key_at(int x, int y)
{
    int i;
    int nx = x - FP_NAV_X;
    int ny = y - FP_NAV_Y;

    for (i = 0; i < ARRAY_SIZE(frontpanel_buttons); i++) {
        const STMP3770FrontpanelButton *button = &frontpanel_buttons[i];

        if (x >= button->x && x < button->x + button->w &&
            y >= button->y && y < button->y + button->h) {
            return button->key;
        }
    }

    if (nx >= 0 && ny >= 0 && nx < FP_NAV_SIZE && ny < FP_NAV_SIZE) {
        if (ny < 50 && nx >= 45 && nx < 105) {
            return HP39GII_KEY(0, 4);
        }
        if (ny >= 100 && nx >= 45 && nx < 105) {
            return HP39GII_KEY(3, 4);
        }
        if (nx < 50 && ny >= 45 && ny < 105) {
            return HP39GII_KEY(2, 4);
        }
        if (nx >= 100 && ny >= 45 && ny < 105) {
            return HP39GII_KEY(1, 4);
        }
    }

    return HP39GII_KEY_NONE;
}

static int frontpanel_key_for_qcode(QKeyCode qcode)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(frontpanel_buttons); i++) {
        if (frontpanel_buttons[i].qcode == qcode) {
            return frontpanel_buttons[i].key;
        }
    }

    switch (qcode) {
    case Q_KEY_CODE_UP:
        return HP39GII_KEY(0, 4);
    case Q_KEY_CODE_RIGHT:
        return HP39GII_KEY(1, 4);
    case Q_KEY_CODE_LEFT:
        return HP39GII_KEY(2, 4);
    case Q_KEY_CODE_DOWN:
        return HP39GII_KEY(3, 4);
    case Q_KEY_CODE_KP_ENTER:
        return HP39GII_KEY(9, 4);
    case Q_KEY_CODE_KP_0:
        return HP39GII_KEY(9, 0);
    case Q_KEY_CODE_KP_1:
        return HP39GII_KEY(9, 1);
    case Q_KEY_CODE_KP_2:
        return HP39GII_KEY(8, 2);
    case Q_KEY_CODE_KP_3:
        return HP39GII_KEY(8, 3);
    case Q_KEY_CODE_KP_4:
        return HP39GII_KEY(8, 1);
    case Q_KEY_CODE_KP_5:
        return HP39GII_KEY(7, 2);
    case Q_KEY_CODE_KP_6:
        return HP39GII_KEY(7, 3);
    case Q_KEY_CODE_KP_7:
        return HP39GII_KEY(7, 1);
    case Q_KEY_CODE_KP_8:
        return HP39GII_KEY(6, 2);
    case Q_KEY_CODE_KP_9:
        return HP39GII_KEY(6, 3);
    case Q_KEY_CODE_EQUAL:
        return HP39GII_KEY(8, 4);
    case Q_KEY_CODE_MINUS:
        return HP39GII_KEY(7, 4);
    case Q_KEY_CODE_SLASH:
        return HP39GII_KEY(5, 4);
    case Q_KEY_CODE_ASTERISK:
        return HP39GII_KEY(6, 4);
    case Q_KEY_CODE_ESC:
        return HP39GII_KEY(10, 0);
    case Q_KEY_CODE_DELETE:
        return HP39GII_KEY(3, 3);
    default:
        return HP39GII_KEY_NONE;
    }
}

static void frontpanel_draw(STMP3770LCDIFState *s, DisplaySurface *surface)
{
    int i;

    frontpanel_draw_background(s, surface);
    frontpanel_draw_status(s, surface);
    frontpanel_draw_lcd(s, surface);
    frontpanel_draw_nav(s, surface);

    for (i = 0; i < ARRAY_SIZE(frontpanel_buttons); i++) {
        frontpanel_draw_button(s, surface, &frontpanel_buttons[i]);
    }
}

static void frontpanel_release_mouse_key(STMP3770LCDIFState *s)
{
    if (s->frontpanel_mouse_key != HP39GII_KEY_NONE) {
        frontpanel_set_key(s, s->frontpanel_mouse_key, false);
        s->frontpanel_mouse_key = HP39GII_KEY_NONE;
    }
}

static void frontpanel_pointer_abs(STMP3770LCDIFState *s, InputMoveEvent *move)
{
    if (move->axis == INPUT_AXIS_X) {
        s->frontpanel_mouse_x =
            qemu_input_scale_axis(move->value,
                                  INPUT_EVENT_ABS_MIN,
                                  INPUT_EVENT_ABS_MAX,
                                  0,
                                  STMP3770_LCDIF_FRONTPANEL_WIDTH - 1);
    } else if (move->axis == INPUT_AXIS_Y) {
        s->frontpanel_mouse_y =
            qemu_input_scale_axis(move->value,
                                  INPUT_EVENT_ABS_MIN,
                                  INPUT_EVENT_ABS_MAX,
                                  0,
                                  STMP3770_LCDIF_FRONTPANEL_HEIGHT - 1);
    }
}

static void frontpanel_pointer_button(STMP3770LCDIFState *s,
                                      InputBtnEvent *btn)
{
    int key;

    if (btn->button != INPUT_BUTTON_LEFT) {
        return;
    }

    if (!btn->down) {
        frontpanel_release_mouse_key(s);
        return;
    }

    key = frontpanel_find_key_at(s->frontpanel_mouse_x,
                                 s->frontpanel_mouse_y);
    if (key == HP39GII_KEY_NONE) {
        frontpanel_release_mouse_key(s);
        return;
    }

    if (s->frontpanel_mouse_key != key) {
        frontpanel_release_mouse_key(s);
        frontpanel_set_key(s, key, true);
        s->frontpanel_mouse_key = key;
    }
}

static void lcdif_frontpanel_input_event(DeviceState *dev, QemuConsole *src,
                                         InputEvent *evt)
{
    STMP3770LCDIFState *s = STMP3770_LCDIF(dev);
    int key;

    switch (evt->type) {
    case INPUT_EVENT_KIND_KEY:
        key = frontpanel_key_for_qcode(
            qemu_input_key_value_to_qcode(evt->u.key.data->key));
        if (key != HP39GII_KEY_NONE) {
            frontpanel_set_key(s, key, evt->u.key.data->down);
        }
        break;
    case INPUT_EVENT_KIND_ABS:
        frontpanel_pointer_abs(s, evt->u.abs.data);
        break;
    case INPUT_EVENT_KIND_BTN:
        frontpanel_pointer_button(s, evt->u.btn.data);
        break;
    default:
        break;
    }
}

static const QemuInputHandler lcdif_frontpanel_input_handler = {
    .name = "stmp3770-frontpanel",
    .mask = INPUT_EVENT_MASK_KEY | INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = lcdif_frontpanel_input_event,
};

static void lcdif_draw_line(void *opaque, uint8_t *dest,
                            const uint8_t *src, int width, int deststep)
{
    STMP3770LCDIFState *s = opaque;
    int i;

    for (i = 0; i < width; i++) {
        uint16_t pixel = ((const uint16_t *)src)[i];
        unsigned int r = ((pixel >> 11) & 0x1F) << 3;
        unsigned int g = ((pixel >> 5) & 0x3F) << 2;
        unsigned int b = (pixel & 0x1F) << 3;
        uint32_t val = lcdif_rgb_to_surface_pixel(s, r, g, b);

        lcdif_store_surface_pixel(dest + i * deststep, deststep, val);
    }
}

static DisplaySurface *lcdif_prepare_surface(STMP3770LCDIFState *s,
                                             int width, int height)
{
    DisplaySurface *surface = qemu_console_surface(s->con);

    if (!surface ||
        surface_width(surface) != width ||
        surface_height(surface) != height) {
        qemu_console_resize(s->con, width, height);
        surface = qemu_console_surface(s->con);
    }

    if (!surface || surface_bits_per_pixel(surface) == 0) {
        return NULL;
    }

    s->surface_format = surface_format(surface);
    return surface;
}

static bool lcdif_update_panel_display(STMP3770LCDIFState *s)
{
    DisplaySurface *surface;

    if (!s->panel_dirty) {
        return false;
    }

    surface = lcdif_prepare_surface(s, STMP3770_LCDIF_FRONTPANEL_WIDTH,
                                    STMP3770_LCDIF_FRONTPANEL_HEIGHT);
    if (!surface) {
        return false;
    }

    frontpanel_draw(s, surface);
    s->panel_dirty = false;
    dpy_gfx_update(s->con, 0, 0,
                   STMP3770_LCDIF_FRONTPANEL_WIDTH,
                   STMP3770_LCDIF_FRONTPANEL_HEIGHT);
    return true;
}

static bool lcdif_update_rgb565_display(STMP3770LCDIFState *s)
{
    DisplaySurface *surface;
    int src_width;
    int first = -1, last = -1;

    if (!lcdif_enabled(s)) {
        return false;
    }

    if (s->width <= 0 || s->height <= 0 || s->cur_buf == 0) {
        return false;
    }

    surface = lcdif_prepare_surface(s, s->width, s->height);
    if (!surface) {
        return false;
    }

    src_width = s->width * 2; /* 16-bit RGB565 framebuffer */

    framebuffer_update_memory_section(&s->fbsection, s->system_memory,
                                      s->cur_buf, s->height, src_width);

    framebuffer_update_display(surface, &s->fbsection,
                               s->width, s->height,
                               src_width,
                               surface_stride(surface),
                               surface_bytes_per_pixel(surface),
                               0, lcdif_draw_line, s,
                               &first, &last);

    if (first >= 0) {
        dpy_gfx_update(s->con, 0, first, s->width, last - first + 1);
    }

    return first >= 0;
}

static void lcdif_update_display(void *opaque)
{
    STMP3770LCDIFState *s = opaque;

    if (lcdif_update_panel_display(s)) {
        return;
    }

    lcdif_update_rgb565_display(s);
}

static void lcdif_refresh(void *opaque)
{
    STMP3770LCDIFState *s = opaque;

    lcdif_update_display(s);

    if (lcdif_enabled(s)) {
        s->irq |= IRQ_VSYNC;
        lcdif_update_irq(s);
    }

    timer_mod(s->refresh_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NS_PER_SEC / REFRESH_RATE_HZ);
}

static void lcdif_apply_sct(uint32_t *reg, uint32_t value, int sct)
{
    switch (sct) {
    case 0:
        *reg = value;
        break;
    case 1:
        *reg |= value;
        break;
    case 2:
        *reg &= ~value;
        break;
    case 3:
        *reg ^= value;
        break;
    }
}

static void lcdif_consume_data_words(STMP3770LCDIFState *s, size_t bytes)
{
    unsigned int words;
    uint32_t count;

    if (!(s->ctrl0 & CTRL0_RUN) || (s->ctrl0 & CTRL0_BYPASS_COUNT)) {
        return;
    }

    words = (s->ctrl0 & CTRL0_WORD_LENGTH) ? bytes : DIV_ROUND_UP(bytes, 2);
    count = s->ctrl0 & CTRL0_COUNT_MASK;
    if (words >= count) {
        s->ctrl0 &= ~(CTRL0_RUN | CTRL0_COUNT_MASK);
    } else {
        s->ctrl0 = (s->ctrl0 & ~CTRL0_COUNT_MASK) | (count - words);
    }
}

static uint64_t lcdif_read(void *opaque, hwaddr offset, unsigned size)
{
    STMP3770LCDIFState *s = opaque;

    if (offset != REG_DATA && size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-lcdif: unsupported read size %u at offset "
                      HWADDR_FMT_plx "\n", size, offset);
        return 0;
    }
    if (offset == REG_DATA && size != 1 && size != 2 && size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-lcdif: unsupported data read size %u\n",
                      size);
        return 0;
    }

    switch (offset) {
    case REG_CTRL0:
        return s->ctrl0;
    case REG_CTRL1:
        return (s->ctrl1 & ~CTRL1_IRQ_MASK) | lcdif_irq_to_ctrl1(s->irq);
    case REG_TIMING:
        return s->hw_timing;
    case REG_VDCTRL0:
        return s->vdctrl[0];
    case REG_VDCTRL1:
        return s->vdctrl[1];
    case REG_VDCTRL2:
        return s->vdctrl[2];
    case REG_VDCTRL3:
        return s->vdctrl[3];
    case REG_DVICTRL0:
        return s->dvctrl[0];
    case REG_DVICTRL1:
        return s->dvctrl[1];
    case REG_DVICTRL2:
        return s->dvctrl[2];
    case REG_DVICTRL3:
        return s->dvctrl[3];
    case REG_DATA:
    {
        uint8_t data[4];
        uint32_t value = 0;
        unsigned int i;

        lcdif_panel_read(s, data, size);
        lcdif_consume_data_words(s, size);
        for (i = 0; i < size; i++) {
            value |= (uint32_t)data[i] << (i * 8);
        }
        return value;
    }
    case REG_STAT:
        return STAT_RESET_VALUE;
    case REG_VERSION:
        return LCDIF_VERSION;
    case REG_DEBUG0:
        return DEBUG0_RESET_VALUE;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-lcdif: read from unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        return 0;
    }
}

static void lcdif_write(void *opaque, hwaddr offset,
                        uint64_t value, unsigned size)
{
    STMP3770LCDIFState *s = opaque;
    int sct = (offset >> 2) & 3;
    hwaddr base = offset & ~0xC;

    if (offset != REG_DATA && size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-lcdif: unsupported write size %u at offset "
                      HWADDR_FMT_plx "\n", size, offset);
        return;
    }
    if (offset == REG_DATA && size != 1 && size != 2 && size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-lcdif: unsupported data write size %u\n",
                      size);
        return;
    }

    if (base == REG_CTRL0) {
        lcdif_apply_sct(&s->ctrl0, (uint32_t)value & CTRL0_WRITABLE_MASK,
                        sct);
        /*
         * Hardware ties CLKGATE to SFTRST: asserting reset automatically
         * gates the clock, so firmware polls CLKGATE after setting SFTRST.
         */
        if (s->ctrl0 & CTRL0_SFTRST) {
            s->ctrl0 |= CTRL0_CLKGATE;
        }
        return;
    }

    if (base == REG_CTRL1) {
        lcdif_apply_sct(&s->ctrl1, (uint32_t)value & CTRL1_WRITABLE_MASK,
                        sct);
        if (sct == 0) {
            s->irq = lcdif_ctrl1_status_to_irq(s->ctrl1);
        } else if (sct == 1) {
            s->irq |= lcdif_ctrl1_status_to_irq((uint32_t)value);
        } else if (sct == 2) {
            s->irq &= ~lcdif_ctrl1_status_to_irq((uint32_t)value);
        } else {
            s->irq ^= lcdif_ctrl1_status_to_irq((uint32_t)value);
        }
        s->ctrl1 = (s->ctrl1 & ~CTRL1_IRQ_MASK) | lcdif_irq_to_ctrl1(s->irq);
        s->irq_en = lcdif_ctrl1_to_irq_en(s->ctrl1);
        lcdif_update_irq(s);
        return;
    }

    if (base == REG_VDCTRL0) {
        lcdif_apply_sct(&s->vdctrl[0],
                        (uint32_t)value & VDCTRL0_WRITABLE_MASK, sct);
        return;
    }

    if (sct != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-lcdif: write to undocumented alias "
                      HWADDR_FMT_plx "\n", offset);
        return;
    }

    switch (offset) {
    case REG_TIMING:
        s->hw_timing = (uint32_t)value;
        break;
    case REG_VDCTRL1:
        s->vdctrl[1] = (uint32_t)value;
        break;
    case REG_VDCTRL2:
        s->vdctrl[2] = (uint32_t)value;
        break;
    case REG_VDCTRL3:
        s->vdctrl[3] = (uint32_t)value & VDCTRL3_WRITABLE_MASK;
        break;
    case REG_DVICTRL0:
        s->dvctrl[0] = (uint32_t)value;
        break;
    case REG_DVICTRL1:
        s->dvctrl[1] = (uint32_t)value & DVICTRL1_WRITABLE_MASK;
        break;
    case REG_DVICTRL2:
        s->dvctrl[2] = (uint32_t)value & DVICTRL1_WRITABLE_MASK;
        break;
    case REG_DVICTRL3:
        s->dvctrl[3] = (uint32_t)value & DVICTRL3_WRITABLE_MASK;
        break;
    case REG_DATA:
    {
        uint8_t data[4];
        bool data_select = (s->ctrl0 & CTRL0_DATA_SELECT) != 0;
        unsigned int i;

        for (i = 0; i < size; i++) {
            data[i] = value >> (i * 8);
        }
        lcdif_panel_write(s, data, size, data_select);
        lcdif_consume_data_words(s, size);
        break;
    }
    case REG_STAT:
    case REG_VERSION:
    case REG_DEBUG0:
        /* Read-only. */
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-lcdif: write to unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        break;
    }
}

static int stmp3770_lcdif_dma_handler(STMP3770DMAState *dma,
                                      int channel, STMP3770DMAEvent event,
                                      void *buf, size_t len, void *opaque)
{
    STMP3770LCDIFState *s = STMP3770_LCDIF(opaque);
    STMP3770DMAChannel *ch = &dma->ch[channel];
    bool data_select;

    if (event == STMP3770_DMA_EVENT_PIO) {
        if (ch->num_pio_words > 0) {
            s->dma_pio_ctrl = ch->pio_words[0];
            s->ctrl0 = (s->ctrl0 & (CTRL0_SFTRST | CTRL0_CLKGATE)) |
                       (s->dma_pio_ctrl &
                        ~(CTRL0_SFTRST | CTRL0_CLKGATE));
        }
        return ch->num_pio_words * sizeof(uint32_t);
    }

    data_select = (s->dma_pio_ctrl & CTRL0_DATA_SELECT) != 0;

    if (event == STMP3770_DMA_EVENT_DATA_WRITE) {
        lcdif_panel_write(s, buf, len, data_select);
        lcdif_consume_data_words(s, len);
        return len;
    }

    if (event == STMP3770_DMA_EVENT_DATA_READ) {
        lcdif_panel_read(s, buf, len);
        lcdif_consume_data_words(s, len);
        return len;
    }

    return 0;
}

void stmp3770_lcdif_set_dma(STMP3770LCDIFState *s, STMP3770DMAState *dma,
                            int channel)
{
    s->dma = dma;
    s->dma_channel = channel;

    if (!dma) {
        return;
    }

    stmp3770_dma_set_channel_handler(dma, channel,
                                     stmp3770_lcdif_dma_handler, s);
}

void stmp3770_lcdif_set_pinctrl(STMP3770LCDIFState *s,
                                STMP3770PINCTRLState *pinctrl)
{
    int key;

    s->pinctrl = pinctrl;
    if (!pinctrl) {
        return;
    }

    for (key = 0; key < 128; key++) {
        stmp3770_pinctrl_set_key(pinctrl, key,
                                 frontpanel_key_is_down(s, key));
    }
}

static const MemoryRegionOps lcdif_ops = {
    .read = lcdif_read,
    .write = lcdif_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void lcdif_reset(DeviceState *dev)
{
    STMP3770LCDIFState *s = STMP3770_LCDIF(dev);

    s->ctrl0 = CTRL0_SFTRST | CTRL0_CLKGATE;
    s->ctrl1 = CTRL1_RESET_VALUE;
    s->cur_buf = 0;
    s->next_buf = 0;
    memset(s->timing, 0, sizeof(s->timing));
    s->vdctrl0 = 0;
    s->vdctrl1 = 0;
    s->vdctrl2 = 0;
    s->vdctrl3 = 0;
    s->hw_timing = 0;
    memset(s->vdctrl, 0, sizeof(s->vdctrl));
    memset(s->dvctrl, 0, sizeof(s->dvctrl));
    s->irq = 0;
    s->irq_en = 0;
    s->width = 0;
    s->height = 0;
    s->dma_pio_ctrl = 0;
    s->panel_cmd = 0;
    s->panel_param_cmd = 0;
    s->panel_param_len = 0;
    memset(s->panel_param, 0, sizeof(s->panel_param));
    memset(s->panel_vram, 0, sizeof(s->panel_vram));
    memset(s->frontpanel_key_state, 0, sizeof(s->frontpanel_key_state));
    s->frontpanel_mouse_x = 0;
    s->frontpanel_mouse_y = 0;
    s->frontpanel_mouse_key = HP39GII_KEY_NONE;
    s->panel_dirty = true;
    lcdif_panel_reset_window(s);
}

static void lcdif_invalidate_display(void *opaque)
{
    STMP3770LCDIFState *s = opaque;

    s->panel_dirty = true;
    lcdif_update_display(s);
}

static const GraphicHwOps lcdif_gfx_ops = {
    .invalidate = lcdif_invalidate_display,
    .gfx_update = lcdif_update_display,
};

static void lcdif_realize(DeviceState *dev, Error **errp)
{
    STMP3770LCDIFState *s = STMP3770_LCDIF(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &lcdif_ops, s,
                          TYPE_STMP3770_LCDIF, 0x2000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq_out);

    s->system_memory = get_system_memory();
    s->con = graphic_console_init(dev, 0, &lcdif_gfx_ops, s);
    if (!s->con) {
        error_setg(errp, "stmp3770-lcdif: failed to initialize graphic console");
        return;
    }
    qemu_console_resize(s->con, STMP3770_LCDIF_FRONTPANEL_WIDTH,
                        STMP3770_LCDIF_FRONTPANEL_HEIGHT);

    s->input_handler =
        qemu_input_handler_register(dev, &lcdif_frontpanel_input_handler);
    qemu_input_handler_activate(s->input_handler);

    s->refresh_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, lcdif_refresh, s);
    timer_mod(s->refresh_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NS_PER_SEC / REFRESH_RATE_HZ);
}

static const VMStateDescription vmstate_lcdif = {
    .name = "stmp3770-lcdif",
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl0, STMP3770LCDIFState),
        VMSTATE_UINT32(ctrl1, STMP3770LCDIFState),
        VMSTATE_UINT32(cur_buf, STMP3770LCDIFState),
        VMSTATE_UINT32(next_buf, STMP3770LCDIFState),
        VMSTATE_UINT32_ARRAY(timing, STMP3770LCDIFState, 4),
        VMSTATE_UINT32(vdctrl0, STMP3770LCDIFState),
        VMSTATE_UINT32(vdctrl1, STMP3770LCDIFState),
        VMSTATE_UINT32(vdctrl2, STMP3770LCDIFState),
        VMSTATE_UINT32(vdctrl3, STMP3770LCDIFState),
        VMSTATE_UINT32_V(hw_timing, STMP3770LCDIFState, 2),
        VMSTATE_UINT32_ARRAY_V(vdctrl, STMP3770LCDIFState, 4, 2),
        VMSTATE_UINT32_ARRAY_V(dvctrl, STMP3770LCDIFState, 4, 2),
        VMSTATE_UINT32(irq, STMP3770LCDIFState),
        VMSTATE_UINT32(irq_en, STMP3770LCDIFState),
        VMSTATE_UINT32(dma_pio_ctrl, STMP3770LCDIFState),
        VMSTATE_UINT8(panel_cmd, STMP3770LCDIFState),
        VMSTATE_UINT8(panel_param_cmd, STMP3770LCDIFState),
        VMSTATE_UINT8(panel_param_len, STMP3770LCDIFState),
        VMSTATE_UINT8_ARRAY(panel_param, STMP3770LCDIFState, 4),
        VMSTATE_UINT16(panel_x_start, STMP3770LCDIFState),
        VMSTATE_UINT16(panel_x_end, STMP3770LCDIFState),
        VMSTATE_UINT16(panel_y_start, STMP3770LCDIFState),
        VMSTATE_UINT16(panel_y_end, STMP3770LCDIFState),
        VMSTATE_UINT16(panel_x, STMP3770LCDIFState),
        VMSTATE_UINT16(panel_y, STMP3770LCDIFState),
        VMSTATE_UINT8_ARRAY(panel_vram, STMP3770LCDIFState,
                            STMP3770_LCDIF_PANEL_SIZE),
        VMSTATE_BOOL(panel_dirty, STMP3770LCDIFState),
        VMSTATE_UINT64_ARRAY(frontpanel_key_state, STMP3770LCDIFState, 2),
        VMSTATE_INT32(frontpanel_mouse_x, STMP3770LCDIFState),
        VMSTATE_INT32(frontpanel_mouse_y, STMP3770LCDIFState),
        VMSTATE_INT32(frontpanel_mouse_key, STMP3770LCDIFState),
        VMSTATE_INT32(width, STMP3770LCDIFState),
        VMSTATE_INT32(height, STMP3770LCDIFState),
        VMSTATE_END_OF_LIST()
    }
};

static void lcdif_init(Object *obj)
{
    STMP3770LCDIFState *s = STMP3770_LCDIF(obj);

    s->width = 0;
    s->height = 0;
    s->surface_format = PIXMAN_x8r8g8b8;
    s->panel_dirty = true;
    s->dma_channel = -1;
    s->frontpanel_mouse_key = HP39GII_KEY_NONE;
    lcdif_panel_reset_window(s);
}

static void lcdif_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = lcdif_realize;
    device_class_set_legacy_reset(dc, lcdif_reset);
    dc->vmsd = &vmstate_lcdif;
}

static const TypeInfo lcdif_type_info = {
    .name          = TYPE_STMP3770_LCDIF,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STMP3770LCDIFState),
    .instance_init = lcdif_init,
    .class_init    = lcdif_class_init,
};

static void lcdif_register_types(void)
{
    type_register_static(&lcdif_type_info);
}

type_init(lcdif_register_types)
