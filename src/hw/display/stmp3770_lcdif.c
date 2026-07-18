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
#include "hw/display/framebuffer.h"
#include "hw/display/stmp3770_lcdif.h"
#include "hw/display/hp39gii_frontpanel.h"

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

static inline bool lcdif_enabled(STMP3770LCDIFState *s)
{
    return (s->ctrl0 & (CTRL0_SFTRST | CTRL0_CLKGATE)) == 0 &&
           (s->ctrl0 & CTRL0_RUN) != 0;
}

static uint32_t lcdif_status(STMP3770LCDIFState *s)
{
    uint32_t status = STAT_RESET_VALUE;

    if (lcdif_enabled(s) && !(s->ctrl0 & CTRL0_READ_WRITEB)) {
        status |= STAT_DMA_REQ | STAT_TXFIFO_EMPTY;
    }
    return status;
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
    case 0x28: /* Display off: the glass goes blank. */
        s->display_on = false;
        s->panel_dirty = true;
        s->panel_param_cmd = 0;
        s->panel_param_len = 0;
        break;
    case 0x29: /* Display on. */
        s->display_on = true;
        s->panel_dirty = true;
        s->panel_param_cmd = 0;
        s->panel_param_len = 0;
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

static void frontpanel_release_mouse_key(STMP3770LCDIFState *s)
{
    if (s->frontpanel_mouse_key != HP39GII_FP_KEY_NONE) {
        frontpanel_set_key(s, s->frontpanel_mouse_key, false);
        s->frontpanel_mouse_key = HP39GII_FP_KEY_NONE;
    }
}

static void lcdif_frontpanel_input_event(DeviceState *dev, QemuConsole *src,
                                         InputEvent *evt)
{
    STMP3770LCDIFState *s = STMP3770_LCDIF(dev);
    int key;

    switch (evt->type) {
    case INPUT_EVENT_KIND_KEY:
        key = hp39gii_fp_key_for_qcode(
            qemu_input_key_value_to_qcode(evt->u.key.data->key));
        if (key != HP39GII_FP_KEY_NONE) {
            frontpanel_set_key(s, key, evt->u.key.data->down);
        }
        break;
    case INPUT_EVENT_KIND_ABS:
        if (evt->u.abs.data->axis == INPUT_AXIS_X) {
            s->frontpanel_mouse_x =
                qemu_input_scale_axis(evt->u.abs.data->value,
                                      INPUT_EVENT_ABS_MIN,
                                      INPUT_EVENT_ABS_MAX,
                                      0, HP39GII_FP_WIDTH - 1);
        } else if (evt->u.abs.data->axis == INPUT_AXIS_Y) {
            s->frontpanel_mouse_y =
                qemu_input_scale_axis(evt->u.abs.data->value,
                                      INPUT_EVENT_ABS_MIN,
                                      INPUT_EVENT_ABS_MAX,
                                      0, HP39GII_FP_HEIGHT - 1);
        }
        break;
    case INPUT_EVENT_KIND_BTN:
        if (evt->u.btn.data->button != INPUT_BUTTON_LEFT) {
            break;
        }
        if (!evt->u.btn.data->down) {
            frontpanel_release_mouse_key(s);
            break;
        }
        key = hp39gii_fp_key_at(s->frontpanel_mouse_x, s->frontpanel_mouse_y);
        if (key == HP39GII_FP_KEY_NONE) {
            frontpanel_release_mouse_key(s);
            break;
        }
        if (s->frontpanel_mouse_key != key) {
            frontpanel_release_mouse_key(s);
            frontpanel_set_key(s, key, true);
            s->frontpanel_mouse_key = key;
        }
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
    if (!s->panel_dirty) {
        return false;
    }

    hp39gii_fp_render(s->con, s->panel_vram, s->frontpanel_key_state,
                      s->display_on);
    s->panel_dirty = false;
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

static void lcdif_soft_reset(STMP3770LCDIFState *s)
{
    uint32_t panel_reset = s->ctrl1 & 1;

    s->ctrl0 = CTRL0_SFTRST | CTRL0_CLKGATE;
    s->ctrl1 = CTRL1_RESET_VALUE | panel_reset;
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
    lcdif_update_irq(s);
}

static unsigned int lcdif_data_words(STMP3770LCDIFState *s,
                                     uint8_t valid_bytes)
{
    unsigned int words = 0;

    if (s->ctrl0 & CTRL0_WORD_LENGTH) {
        while (valid_bytes) {
            words += valid_bytes & 1;
            valid_bytes >>= 1;
        }
        return words;
    }

    if ((valid_bytes & 0x3) == 0x3) {
        words++;
    }
    if ((valid_bytes & 0xc) == 0xc) {
        words++;
    }
    return words;
}

static void lcdif_data_swizzle(STMP3770LCDIFState *s, uint8_t data[4],
                                uint8_t *valid_bytes)
{
    uint8_t tmp;
    uint8_t valid = *valid_bytes;

    switch ((s->ctrl0 & CTRL0_DATA_SWIZZLE_MASK) >> 21) {
    case 1:
        tmp = data[0];
        data[0] = data[3];
        data[3] = tmp;
        tmp = data[1];
        data[1] = data[2];
        data[2] = tmp;
        valid = ((valid & 0x1) << 3) | ((valid & 0x2) << 1) |
                ((valid & 0x4) >> 1) | ((valid & 0x8) >> 3);
        break;
    case 2:
        tmp = data[0];
        data[0] = data[2];
        data[2] = tmp;
        tmp = data[1];
        data[1] = data[3];
        data[3] = tmp;
        valid = ((valid & 0x3) << 2) | ((valid & 0xc) >> 2);
        break;
    case 3:
        tmp = data[0];
        data[0] = data[1];
        data[1] = tmp;
        tmp = data[2];
        data[2] = data[3];
        data[3] = tmp;
        valid = ((valid & 0x5) << 1) | ((valid & 0xa) >> 1);
        break;
    default:
        break;
    }

    *valid_bytes = valid;
}

static void lcdif_data_shift(STMP3770LCDIFState *s, uint8_t data[4])
{
    unsigned int shift = (s->ctrl0 & CTRL0_SHIFT_NUM_BITS_MASK) >> 25;
    unsigned int i;

    if (!shift) {
        return;
    }

    if (s->ctrl0 & CTRL0_WORD_LENGTH) {
        for (i = 0; i < 4; i++) {
            data[i] = (s->ctrl0 & CTRL0_DATA_SHIFT_DIR) ?
                      data[i] >> shift : data[i] << shift;
        }
        return;
    }

    for (i = 0; i < 4; i += 2) {
        uint16_t word = data[i] | ((uint16_t)data[i + 1] << 8);

        word = (s->ctrl0 & CTRL0_DATA_SHIFT_DIR) ?
               word >> shift : word << shift;
        data[i] = word;
        data[i + 1] = word >> 8;
    }
}

static unsigned int lcdif_panel_write_packed(STMP3770LCDIFState *s,
                                             uint8_t data[4],
                                             uint8_t valid_bytes,
                                             bool data_select)
{
    uint8_t packed[4];
    uint8_t byte_packing = (s->ctrl1 >> 16) & 0xf;
    unsigned int i;
    unsigned int packed_len = 0;

    valid_bytes &= byte_packing;
    lcdif_data_swizzle(s, data, &valid_bytes);
    lcdif_data_shift(s, data);
    if (s->ctrl0 & CTRL0_WORD_LENGTH) {
        for (i = 0; i < 4; i++) {
            if (valid_bytes & (1U << i)) {
                packed[packed_len++] = data[i];
            }
        }
    } else {
        for (i = 0; i < 4; i += 2) {
            if ((valid_bytes & (3U << i)) == (3U << i)) {
                packed[packed_len++] = data[i];
                packed[packed_len++] = data[i + 1];
            }
        }
    }

    lcdif_panel_write(s, packed, packed_len, data_select);
    return lcdif_data_words(s, valid_bytes);
}

static unsigned int lcdif_panel_write_buffer(STMP3770LCDIFState *s,
                                              const uint8_t *buf, size_t len,
                                              bool data_select)
{
    unsigned int words = 0;

    while (len) {
        uint8_t data[4] = { 0 };
        size_t chunk = MIN(len, sizeof(data));
        uint8_t valid_bytes = (1U << chunk) - 1;

        memcpy(data, buf, chunk);
        words += lcdif_panel_write_packed(s, data, valid_bytes, data_select);
        buf += chunk;
        len -= chunk;
    }

    return words;
}

static void lcdif_consume_data_words(STMP3770LCDIFState *s,
                                     unsigned int words)
{
    uint32_t count;

    if (!words || !(s->ctrl0 & CTRL0_RUN) ||
        (s->ctrl0 & CTRL0_BYPASS_COUNT)) {
        return;
    }

    count = s->ctrl0 & CTRL0_COUNT_MASK;
    if (words >= count) {
        s->ctrl0 &= ~(CTRL0_RUN | CTRL0_COUNT_MASK);
    } else {
        s->ctrl0 = (s->ctrl0 & ~CTRL0_COUNT_MASK) | (count - words);
    }
}

static void lcdif_panel_read_for_fifo(STMP3770LCDIFState *s, uint8_t *buf,
                                      size_t len)
{
    if (s->first_read_dummy_pending) {
        uint8_t dummy;

        lcdif_panel_read(s, &dummy, sizeof(dummy));
        s->first_read_dummy_pending = false;
    }
    lcdif_panel_read(s, buf, len);
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

        lcdif_panel_read_for_fifo(s, data, size);
        lcdif_consume_data_words(s, (s->ctrl0 & CTRL0_WORD_LENGTH) ?
                                 size : DIV_ROUND_UP(size, 2));
        for (i = 0; i < size; i++) {
            value |= (uint32_t)data[i] << (i * 8);
        }
        return value;
    }
    case REG_STAT:
        return lcdif_status(s);
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
        uint32_t old_ctrl0 = s->ctrl0;
        uint32_t streaming_modes = CTRL0_VSYNC_MODE | CTRL0_DOTCLK_MODE |
                                   CTRL0_DVI_MODE;

        lcdif_apply_sct(&s->ctrl0, (uint32_t)value & CTRL0_WRITABLE_MASK,
                        sct);
        if (old_ctrl0 & CTRL0_RUN) {
            s->ctrl0 = (s->ctrl0 & ~CTRL0_DATA_SELECT) |
                       (old_ctrl0 & CTRL0_DATA_SELECT);
        }
        if (!(old_ctrl0 & CTRL0_RUN) && (s->ctrl0 & CTRL0_RUN) &&
            (s->ctrl0 & CTRL0_READ_WRITEB) && (s->ctrl1 & (1U << 4))) {
            s->first_read_dummy_pending = true;
        }
        if ((old_ctrl0 & (CTRL0_RUN | CTRL0_BYPASS_COUNT)) ==
            (CTRL0_RUN | CTRL0_BYPASS_COUNT) &&
            (old_ctrl0 & streaming_modes &
             ~(s->ctrl0 & streaming_modes))) {
            s->ctrl0 &= ~CTRL0_RUN;
        }
        /*
         * Hardware ties CLKGATE to SFTRST: asserting reset automatically
         * gates the clock, so firmware polls CLKGATE after setting SFTRST.
         */
        if (s->ctrl0 & CTRL0_SFTRST) {
            lcdif_soft_reset(s);
        }
        return;
    }

    if (base == REG_CTRL1) {
        uint32_t old_ctrl1 = s->ctrl1;

        lcdif_apply_sct(&s->ctrl1, (uint32_t)value & CTRL1_WRITABLE_MASK,
                        sct);
        if (s->ctrl0 & CTRL0_RUN) {
            s->ctrl1 = (s->ctrl1 & ~2U) | (old_ctrl1 & 2U);
        }
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
        uint8_t data[4] = { 0 };
        bool data_select = (s->ctrl0 & CTRL0_DATA_SELECT) != 0;
        unsigned int i;

        for (i = 0; i < size; i++) {
            data[i] = value >> (i * 8);
        }
        lcdif_consume_data_words(s,
                                 lcdif_panel_write_packed(s, data,
                                                          (1U << size) - 1,
                                                          data_select));
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
        lcdif_consume_data_words(s,
                                 lcdif_panel_write_buffer(s, buf, len,
                                                          data_select));
        return len;
    }

    if (event == STMP3770_DMA_EVENT_DATA_READ) {
        lcdif_panel_read_for_fifo(s, buf, len);
        lcdif_consume_data_words(s, (s->ctrl0 & CTRL0_WORD_LENGTH) ?
                                 len : DIV_ROUND_UP(len, 2));
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
    s->first_read_dummy_pending = false;
    memset(s->panel_param, 0, sizeof(s->panel_param));
    memset(s->panel_vram, 0, sizeof(s->panel_vram));
    memset(s->frontpanel_key_state, 0, sizeof(s->frontpanel_key_state));
    s->frontpanel_mouse_x = 0;
    s->frontpanel_mouse_y = 0;
    s->frontpanel_mouse_key = HP39GII_FP_KEY_NONE;
    s->display_on = false;
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
    qemu_console_resize(s->con, HP39GII_FP_WIDTH,
                        HP39GII_FP_HEIGHT);

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
    .version_id = 4,
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
        VMSTATE_BOOL_V(first_read_dummy_pending, STMP3770LCDIFState, 3),
        VMSTATE_UINT8_ARRAY(panel_vram, STMP3770LCDIFState,
                            STMP3770_LCDIF_PANEL_SIZE),
        VMSTATE_BOOL(panel_dirty, STMP3770LCDIFState),
        VMSTATE_BOOL_V(display_on, STMP3770LCDIFState, 4),
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
    s->frontpanel_mouse_key = HP39GII_FP_KEY_NONE;
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
