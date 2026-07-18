/*
 * HP 39gII front panel (case, LCD glass, annunciators, keypad) rendering
 *
 * All layout, colors and glyphs are modelled after photographs of the
 * real HP 39gII calculator. The annunciator strip at the top of the LCD
 * glass is made of independent LCD segments outside the 256x128 pixel
 * matrix: the panel controller maps them to byte columns of memory
 * rows 0..7 (each segment replicated at column c, c+86 and c+172
 * because the row stride of 258 equals 3 * 86). A segment is lit when
 * its byte is dark (< 128), matching the panel's 0x00 = on /
 * 0xFF = off convention used by firmware.
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
#include "ui/console.h"
#include "ui/surface.h"
#include "ui/pixel_ops.h"
#include "ui/vgafont.h"
#include "hw/display/hp39gii_frontpanel.h"

/* ------------------------------------------------------------------ */
/* Layout (canvas is HP39GII_FP_WIDTH x HP39GII_FP_HEIGHT)            */

#define BEZEL_X      24
#define BEZEL_Y      96
#define BEZEL_W      528
#define BEZEL_H      324

#define GLASS_X      32
#define GLASS_Y      104
#define GLASS_W      512

#define STRIP_Y      104   /* annunciator strip inside the glass */
#define STRIP_H      44

#define PIX_X        32
#define PIX_Y        156
#define PIX_SCALE    2     /* 256x128 pixels shown at 512x256 */

#define FKEY_X0      30
#define FKEY_Y       440
#define FKEY_W       76
#define FKEY_H       36
#define FKEY_GAP     12

#define APP_KEY_X0   44
#define APP_KEY_Y0   500
#define APP_KEY_Y1   562
#define APP_KEY_W    88
#define APP_KEY_H    50
#define APP_KEY_GAP  12

#define DPAD_CX      424
#define DPAD_CY      558
#define DPAD_R       78

#define GRID_X0      44
#define GRID_Y0      668
#define GRID_KEY_W   88
#define GRID_KEY_H   56
#define GRID_GAP_X   12
#define GRID_GAP_Y   12

/* ------------------------------------------------------------------ */
/* Colors (RGB triples)                                               */

#define COL_BODY_TOP    217, 217, 217
#define COL_BODY_BOT    193, 193, 193
#define COL_BODY_EDGE   166, 166, 166
#define COL_HEADER_TEXT 74, 74, 74
#define COL_LOGO_BG     59, 59, 64
#define COL_LOGO_FG     240, 240, 240
#define COL_BEZEL       29, 32, 37
#define COL_BEZEL_EDGE  11, 12, 14
#define COL_GLASS       195, 201, 179
#define COL_INK         35, 40, 46
#define COL_HAIRLINE    154, 160, 140
#define COL_FKEY_TOP    214, 226, 231
#define COL_FKEY_BOT    175, 195, 202
#define COL_FKEY_EDGE   90, 106, 112
#define COL_KEY_TOP     246, 246, 246
#define COL_KEY_BOT     201, 201, 201
#define COL_KEY_EDGE    79, 79, 79
#define COL_KEY_SHADOW  124, 124, 124
#define COL_KEY_DN_TOP  184, 184, 184
#define COL_KEY_DN_BOT  148, 148, 148
#define COL_KEY_TEXT    17, 17, 17
#define COL_LEGEND_BLUE 14, 90, 167
#define COL_LEGEND_RED  176, 58, 46
#define COL_ALPHA_BLOCK 192, 57, 43
#define COL_SHIFT_BLOCK 31, 97, 141
#define COL_BLOCK_TEXT  245, 245, 245
#define COL_DPAD_RING   119, 119, 125
#define COL_DPAD_DISC   213, 213, 218
#define COL_DPAD_SECTOR 198, 198, 203
#define COL_DPAD_DOWN   150, 150, 156
#define COL_DPAD_DOT    106, 106, 112

/* ------------------------------------------------------------------ */
/* Drawing context and pixel helpers                                  */

typedef struct FPCtx {
    DisplaySurface *surface;
    pixman_format_code_t format;
    const uint64_t *key_state;
} FPCtx;

static uint32_t fp_rgb(const FPCtx *ctx, unsigned int r, unsigned int g,
                       unsigned int b)
{
    switch (ctx->format) {
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

static void fp_put_pixel(const FPCtx *ctx, int x, int y, uint32_t pix)
{
    int deststep;
    int stride;
    uint8_t *dest;

    if (x < 0 || y < 0 ||
        x >= surface_width(ctx->surface) || y >= surface_height(ctx->surface)) {
        return;
    }

    deststep = surface_bytes_per_pixel(ctx->surface);
    stride = surface_stride(ctx->surface);
    if (deststep <= 0 || stride <= 0) {
        return;
    }

    dest = surface_data(ctx->surface) + y * stride + x * deststep;
    switch (deststep) {
    case 1:
        dest[0] = (uint8_t)pix;
        break;
    case 2:
        ((uint16_t *)dest)[0] = (uint16_t)pix;
        break;
    case 3:
        dest[0] = pix & 0xFF;
        dest[1] = (pix >> 8) & 0xFF;
        dest[2] = (pix >> 16) & 0xFF;
        break;
    case 4:
        ((uint32_t *)dest)[0] = pix;
        break;
    default:
        memset(dest, 0, deststep);
        memcpy(dest, &pix, MIN(deststep, (int)sizeof(pix)));
        break;
    }
}

static void fp_fill_rect(const FPCtx *ctx, int x, int y, int w, int h,
                         uint32_t pix)
{
    int xx;
    int yy;
    int x0 = MAX(x, 0);
    int y0 = MAX(y, 0);
    int x1 = MIN(x + w, surface_width(ctx->surface));
    int y1 = MIN(y + h, surface_height(ctx->surface));
    int bpp = surface_bytes_per_pixel(ctx->surface);
    int stride = surface_stride(ctx->surface);
    uint8_t *base;

    if (x0 >= x1 || y0 >= y1 || bpp <= 0 || stride <= 0) {
        return;
    }

    base = surface_data(ctx->surface);
    if (bpp == 4) {
        for (yy = y0; yy < y1; yy++) {
            uint32_t *row = (uint32_t *)(base + yy * stride);

            for (xx = x0; xx < x1; xx++) {
                row[xx] = pix;
            }
        }
        return;
    }
    for (yy = y0; yy < y1; yy++) {
        for (xx = x0; xx < x1; xx++) {
            fp_put_pixel(ctx, xx, yy, pix);
        }
    }
}

/* Solid rounded rectangle (corner radius r, axis-aligned). */
static void fp_fill_rounded(const FPCtx *ctx, int x, int y, int w, int h,
                            int r, uint32_t pix)
{
    int xx;
    int yy;

    fp_fill_rect(ctx, x + r, y, w - 2 * r, h, pix);
    fp_fill_rect(ctx, x, y + r, w, h - 2 * r, pix);
    for (yy = 0; yy < r; yy++) {
        for (xx = 0; xx < r; xx++) {
            int d2 = (r - xx) * (r - xx) + (r - yy) * (r - yy);

            if (d2 > r * r) {
                continue;
            }
            fp_put_pixel(ctx, x + xx, y + yy, pix);
            fp_put_pixel(ctx, x + w - 1 - xx, y + yy, pix);
            fp_put_pixel(ctx, x + xx, y + h - 1 - yy, pix);
            fp_put_pixel(ctx, x + w - 1 - xx, y + h - 1 - yy, pix);
        }
    }
}

static void fp_fill_circle(const FPCtx *ctx, int cx, int cy, int r,
                           uint32_t pix)
{
    int xx;
    int yy;

    for (yy = -r; yy <= r; yy++) {
        for (xx = -r; xx <= r; xx++) {
            if (xx * xx + yy * yy <= r * r) {
                fp_put_pixel(ctx, cx + xx, cy + yy, pix);
            }
        }
    }
}

static void fp_vgradient(const FPCtx *ctx, int x, int y, int w, int h,
                         unsigned int r0, unsigned int g0, unsigned int b0,
                         unsigned int r1, unsigned int g1, unsigned int b1)
{
    int yy;

    for (yy = 0; yy < h; yy++) {
        unsigned int t = h > 1 ? yy * 255 / (h - 1) : 0;
        unsigned int r = r0 + (r1 - r0) * t / 255;
        unsigned int g = g0 + (g1 - g0) * t / 255;
        unsigned int b = b0 + (b1 - b0) * t / 255;

        fp_fill_rect(ctx, x, y + yy, w, 1, fp_rgb(ctx, r, g, b));
    }
}

/* ------------------------------------------------------------------ */
/* Text (VGA 8x16 font, integer scales, plus half-scale superscripts) */

/* Custom 8x16 glyphs for legend characters missing from the font. */
#define FP_CH_ANGLE 0x01
#define FP_CH_LEFT  0x02
#define FP_CH_TIMES 0x03
#define FP_CH_MINUS 0x04

static const char **fp_custom_glyphs[] = {
    [FP_CH_ANGLE] = (const char *[]) {
        "........",
        "........",
        "....#...",
        "...#....",
        "..#.....",
        ".#......",
        "#.......",
        "#.......",
        "#.......",
        "#.......",
        "#.......",
        "#.......",
        "########",
        "........",
        "........",
        "........",
    },
    [FP_CH_LEFT] = (const char *[]) {
        "........",
        "........",
        "...#....",
        "..##....",
        ".###....",
        "########",
        "########",
        "########",
        "########",
        ".###....",
        "..##....",
        "...#....",
        "........",
        "........",
        "........",
        "........",
    },
    [FP_CH_TIMES] = (const char *[]) {
        "........",
        "........",
        "........",
        "#......#",
        ".#....#.",
        "..#..#..",
        "...##...",
        "...##...",
        "..#..#..",
        ".#....#.",
        "#......#",
        "........",
        "........",
        "........",
        "........",
        "........",
    },
    [FP_CH_MINUS] = (const char *[]) {
        "........",
        "........",
        "........",
        "........",
        "........",
        "........",
        "########",
        "########",
        "........",
        "........",
        "........",
        "........",
        "........",
        "........",
        "........",
        "........",
    },
};

static void fp_glyph_row(uint8_t ch, int row, uint8_t *bits)
{
    if (ch >= FP_CH_ANGLE && ch <= FP_CH_MINUS) {
        const char *line = fp_custom_glyphs[ch][row];

        *bits = 0;
        for (int i = 0; i < 8; i++) {
            if (line[i] == '#') {
                *bits |= 0x80 >> i;
            }
        }
        return;
    }
    *bits = vgafont16[ch * 16 + row];
}

static void fp_draw_char(const FPCtx *ctx, int x, int y, uint8_t ch,
                         int scale, uint32_t pix)
{
    int row;
    int bit;
    int sx;
    int sy;

    for (row = 0; row < 16; row++) {
        uint8_t bits;

        fp_glyph_row(ch, row, &bits);
        for (bit = 0; bit < 8; bit++) {
            if ((bits & (0x80 >> bit)) == 0) {
                continue;
            }
            for (sy = 0; sy < scale; sy++) {
                for (sx = 0; sx < scale; sx++) {
                    fp_put_pixel(ctx, x + bit * scale + sx,
                                 y + row * scale + sy, pix);
                }
            }
        }
    }
}

static int fp_text_width(const char *text, int scale)
{
    return (int)strlen(text) * 8 * scale;
}

static void fp_draw_text(const FPCtx *ctx, int x, int y, const char *text,
                         int scale, uint32_t pix)
{
    const uint8_t *p = (const uint8_t *)text;

    while (*p) {
        fp_draw_char(ctx, x, y, *p, scale, pix);
        x += 8 * scale;
        p++;
    }
}

/* Half-scale (4x8) text for superscript legends such as e^x, 10^x, x^-1. */
static void fp_draw_text_half(const FPCtx *ctx, int x, int y,
                              const char *text, uint32_t pix)
{
    const uint8_t *p = (const uint8_t *)text;

    while (*p) {
        int row;
        int bit;

        for (row = 0; row < 8; row++) {
            uint8_t bits;

            fp_glyph_row(*p, row * 2, &bits);
            for (bit = 0; bit < 4; bit++) {
                if (bits & (0x80 >> (bit * 2))) {
                    fp_put_pixel(ctx, x + bit, y + row, pix);
                }
            }
        }
        x += 5;
        p++;
    }
}

/* ------------------------------------------------------------------ */
/* Annunciator strip                                                  */

/* Segment byte columns within panel memory rows 0..7 (replicated at
 * c, c + 86 and c + 172 by the panel controller's 258-byte row stride). */
#define SEG_ARROW_L  37
#define SEG_ARROW_R  64
#define SEG_ALPHA_UP 10
#define SEG_ALPHA_LO 44
#define SEG_BUSY     82
#define SEG_TX       28
#define SEG_RX       50
#define SEG_BAT_BOX  84
#define SEG_BAT_BAR0 76
#define SEG_BAT_BAR1 77
#define SEG_BAT_BAR2 75
#define SEG_BAT_BAR3 78

/* 8x8 annunciator glyphs (drawn at scale 3). */
static const char *const fp_glyph_arrow_l[8] = {
    "..##....",
    ".###....",
    "#####...",
    "##.#....",
    "..#.....",
    "..#...#.",
    "..#...#.",
    "..####..",
};

static const char *const fp_glyph_arrow_r[8] = {
    "....##..",
    "....###.",
    "...#####",
    "....#.##",
    ".....#..",
    ".#...#..",
    ".#...#..",
    "..####..",
};

static const char *const fp_glyph_hourglass[8] = {
    "########",
    ".#....#.",
    "..#..#..",
    "...##...",
    "...##...",
    "..#..#..",
    ".#....#.",
    "########",
};

static const char *const fp_glyph_tx[8] = {
    "#.......",
    "#..#....",
    "#...#...",
    "########",
    "########",
    "#...#...",
    "#..#....",
    "#.......",
};

static const char *const fp_glyph_rx[8] = {
    ".......#",
    "....#..#",
    "...#...#",
    "########",
    "########",
    "...#...#",
    "....#..#",
    ".......#",
};

/* A segment is lit when any of its replicated memory bytes is dark. */
static bool fp_segment_on(const uint8_t *vram, int col)
{
    int rep;
    int row;

    for (rep = 0; rep < 3; rep++) {
        int c = col + rep * 86;

        for (row = 0; row < HP39GII_PANEL_VIEW_Y; row++) {
            if (vram[row * HP39GII_PANEL_WIDTH + c] < 128) {
                return true;
            }
        }
    }
    return false;
}

static void fp_draw_strip_glyph(const FPCtx *ctx, int x, int y,
                                const char *const glyph[8], int scale,
                                uint32_t pix)
{
    int row;
    int col;
    int sx;
    int sy;

    for (row = 0; row < 8; row++) {
        for (col = 0; col < 8; col++) {
            if (glyph[row][col] != '#') {
                continue;
            }
            for (sy = 0; sy < scale; sy++) {
                for (sx = 0; sx < scale; sx++) {
                    fp_put_pixel(ctx, x + col * scale + sx,
                                 y + row * scale + sy, pix);
                }
            }
        }
    }
}

static void fp_draw_annunciators(const FPCtx *ctx, const uint8_t *vram)
{
    uint32_t ink = fp_rgb(ctx, COL_INK);
    int y = STRIP_Y + 6;
    int i;

    if (fp_segment_on(vram, SEG_ARROW_L)) {
        fp_draw_strip_glyph(ctx, GLASS_X + 16, y, fp_glyph_arrow_l, 3, ink);
    }
    if (fp_segment_on(vram, SEG_ARROW_R)) {
        fp_draw_strip_glyph(ctx, GLASS_X + 64, y, fp_glyph_arrow_r, 3, ink);
    }
    if (fp_segment_on(vram, SEG_ALPHA_UP)) {
        fp_draw_text(ctx, GLASS_X + 116, y, "A..Z", 2, ink);
    }
    if (fp_segment_on(vram, SEG_ALPHA_LO)) {
        fp_draw_text(ctx, GLASS_X + 208, y, "a..z", 2, ink);
    }
    if (fp_segment_on(vram, SEG_BUSY)) {
        fp_draw_strip_glyph(ctx, GLASS_X + 292, y, fp_glyph_hourglass, 3, ink);
    }
    if (fp_segment_on(vram, SEG_TX)) {
        fp_draw_strip_glyph(ctx, GLASS_X + 344, y, fp_glyph_tx, 3, ink);
    }
    if (fp_segment_on(vram, SEG_RX)) {
        fp_draw_strip_glyph(ctx, GLASS_X + 392, y, fp_glyph_rx, 3, ink);
    }

    /* Battery: outline box with positive nub, four charge bars inside. */
    if (fp_segment_on(vram, SEG_BAT_BOX)) {
        static const int bar_segs[4] = {
            SEG_BAT_BAR0, SEG_BAT_BAR1, SEG_BAT_BAR2, SEG_BAT_BAR3,
        };
        int bx = GLASS_X + 448;
        int by = STRIP_Y + 9;
        int bw = 52;
        int bh = 26;

        fp_fill_rect(ctx, bx, by, bw, 3, ink);
        fp_fill_rect(ctx, bx, by + bh - 3, bw, 3, ink);
        fp_fill_rect(ctx, bx, by, 3, bh, ink);
        fp_fill_rect(ctx, bx + bw - 3, by, 3, bh, ink);
        fp_fill_rect(ctx, bx + bw, by + 8, 5, bh - 16, ink);

        for (i = 0; i < 4; i++) {
            if (fp_segment_on(vram, bar_segs[i])) {
                fp_fill_rect(ctx, bx + 6 + i * 11, by + 6, 8, bh - 12, ink);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Keypad                                                             */

enum {
    FP_STYLE_STD,
    FP_STYLE_FKEY,
    FP_STYLE_ALPHA,
    FP_STYLE_SHIFT,
};

typedef struct FPButton {
    int16_t x;
    int16_t y;
    uint8_t w;
    uint8_t h;
    uint8_t key;         /* keyboard matrix code */
    uint8_t style;
    int qcode;           /* primary PC-key binding, or Q_KEY_CODE_UNMAPPED */
    const char *primary;
    const char *primary_sup;
    const char *shift;
    const char *shift_sup;
    const char *alpha;
} FPButton;

#define FP_GRID_X(col) (GRID_X0 + (col) * (GRID_KEY_W + GRID_GAP_X))
#define FP_GRID_Y(row) (GRID_Y0 + (row) * (GRID_KEY_H + GRID_GAP_Y))
#define FP_FKEY_X(idx) (FKEY_X0 + (idx) * (FKEY_W + FKEY_GAP))

static const FPButton fp_buttons[] = {
    /* F1..F6 soft keys */
    { FP_FKEY_X(0), FKEY_Y, FKEY_W, FKEY_H, HP39GII_FP_KEY(0, 0), FP_STYLE_FKEY, Q_KEY_CODE_F1, "F1", NULL, NULL, NULL, NULL },
    { FP_FKEY_X(1), FKEY_Y, FKEY_W, FKEY_H, HP39GII_FP_KEY(1, 1), FP_STYLE_FKEY, Q_KEY_CODE_F2, "F2", NULL, NULL, NULL, NULL },
    { FP_FKEY_X(2), FKEY_Y, FKEY_W, FKEY_H, HP39GII_FP_KEY(0, 1), FP_STYLE_FKEY, Q_KEY_CODE_F3, "F3", NULL, NULL, NULL, NULL },
    { FP_FKEY_X(3), FKEY_Y, FKEY_W, FKEY_H, HP39GII_FP_KEY(0, 2), FP_STYLE_FKEY, Q_KEY_CODE_F4, "F4", NULL, NULL, NULL, NULL },
    { FP_FKEY_X(4), FKEY_Y, FKEY_W, FKEY_H, HP39GII_FP_KEY(0, 3), FP_STYLE_FKEY, Q_KEY_CODE_F5, "F5", NULL, NULL, NULL, NULL },
    { FP_FKEY_X(5), FKEY_Y, FKEY_W, FKEY_H, HP39GII_FP_KEY(1, 3), FP_STYLE_FKEY, Q_KEY_CODE_F6, "F6", NULL, NULL, NULL, NULL },

    /* Application keys (left of the directional pad) */
    { APP_KEY_X0, APP_KEY_Y0, APP_KEY_W, APP_KEY_H, HP39GII_FP_KEY(1, 0), FP_STYLE_STD, Q_KEY_CODE_UNMAPPED, "Symb", NULL, "Setup", NULL, NULL },
    { APP_KEY_X0 + 100, APP_KEY_Y0, APP_KEY_W, APP_KEY_H, HP39GII_FP_KEY(2, 1), FP_STYLE_STD, Q_KEY_CODE_UNMAPPED, "Plot", NULL, "Setup", NULL, NULL },
    { APP_KEY_X0 + 200, APP_KEY_Y0, APP_KEY_W, APP_KEY_H, HP39GII_FP_KEY(1, 2), FP_STYLE_STD, Q_KEY_CODE_UNMAPPED, "Num", NULL, "Setup", NULL, NULL },
    { APP_KEY_X0, APP_KEY_Y1, APP_KEY_W, APP_KEY_H, HP39GII_FP_KEY(2, 0), FP_STYLE_STD, Q_KEY_CODE_HOME, "Home", NULL, "Modes", NULL, NULL },
    { APP_KEY_X0 + 100, APP_KEY_Y1, APP_KEY_W, APP_KEY_H, HP39GII_FP_KEY(3, 1), FP_STYLE_STD, Q_KEY_CODE_UNMAPPED, "Apps", NULL, "Info", NULL, NULL },
    { APP_KEY_X0 + 200, APP_KEY_Y1, APP_KEY_W, APP_KEY_H, HP39GII_FP_KEY(2, 2), FP_STYLE_STD, Q_KEY_CODE_UNMAPPED, "Views", NULL, "Help", NULL, NULL },

    /* Main grid, row 0 */
    { FP_GRID_X(0), FP_GRID_Y(0), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(3, 0), FP_STYLE_STD, Q_KEY_CODE_UNMAPPED, "Vars", NULL, "Chars", NULL, "A" },
    { FP_GRID_X(1), FP_GRID_Y(0), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(4, 1), FP_STYLE_STD, Q_KEY_CODE_UNMAPPED, "Math", NULL, "Cmds", NULL, "B" },
    { FP_GRID_X(2), FP_GRID_Y(0), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(3, 2), FP_STYLE_STD, Q_KEY_CODE_UNMAPPED, "a b/c", NULL, NULL, NULL, "C" },
    { FP_GRID_X(3), FP_GRID_Y(0), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(2, 3), FP_STYLE_STD, Q_KEY_CODE_UNMAPPED, "x,T,\xE9,N", NULL, "EEX", NULL, "D" },
    { FP_GRID_X(4), FP_GRID_Y(0), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(3, 3), FP_STYLE_STD, Q_KEY_CODE_BACKSPACE, "\x02", NULL, "Clear", NULL, NULL },

    /* Row 1 */
    { FP_GRID_X(0), FP_GRID_Y(1), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(4, 0), FP_STYLE_STD, Q_KEY_CODE_UNMAPPED, "SIN", NULL, "ASIN", NULL, "E" },
    { FP_GRID_X(1), FP_GRID_Y(1), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(5, 1), FP_STYLE_STD, Q_KEY_CODE_UNMAPPED, "COS", NULL, "ACOS", NULL, "F" },
    { FP_GRID_X(2), FP_GRID_Y(1), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(4, 2), FP_STYLE_STD, Q_KEY_CODE_UNMAPPED, "TAN", NULL, "ATAN", NULL, "G" },
    { FP_GRID_X(3), FP_GRID_Y(1), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(4, 3), FP_STYLE_STD, Q_KEY_CODE_UNMAPPED, "LN", NULL, "e", "x", "H" },
    { FP_GRID_X(4), FP_GRID_Y(1), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(4, 4), FP_STYLE_STD, Q_KEY_CODE_UNMAPPED, "LOG", NULL, "10", "x", "I" },

    /* Row 2 */
    { FP_GRID_X(0), FP_GRID_Y(2), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(5, 0), FP_STYLE_STD, Q_KEY_CODE_UNMAPPED, "x\xFD", NULL, "\xFB", NULL, "J" },
    { FP_GRID_X(1), FP_GRID_Y(2), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(6, 1), FP_STYLE_STD, Q_KEY_CODE_UNMAPPED, "x", "y", "\xFC\xFB", NULL, "K" },
    { FP_GRID_X(2), FP_GRID_Y(2), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(5, 2), FP_STYLE_STD, Q_KEY_CODE_BRACKET_LEFT, "(", NULL, "Copy", NULL, "L" },
    { FP_GRID_X(3), FP_GRID_Y(2), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(5, 3), FP_STYLE_STD, Q_KEY_CODE_BRACKET_RIGHT, ")", NULL, "Paste", NULL, "M" },
    { FP_GRID_X(4), FP_GRID_Y(2), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(5, 4), FP_STYLE_STD, Q_KEY_CODE_SLASH, "\xF6", NULL, "x", "-1", "N" },

    /* Row 3 */
    { FP_GRID_X(0), FP_GRID_Y(3), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(6, 0), FP_STYLE_STD, Q_KEY_CODE_COMMA, ",", NULL, "Mem", NULL, "O" },
    { FP_GRID_X(1), FP_GRID_Y(3), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(7, 1), FP_STYLE_STD, Q_KEY_CODE_7, "7", NULL, "List", NULL, "P" },
    { FP_GRID_X(2), FP_GRID_Y(3), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(6, 2), FP_STYLE_STD, Q_KEY_CODE_8, "8", NULL, "{", NULL, "Q" },
    { FP_GRID_X(3), FP_GRID_Y(3), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(6, 3), FP_STYLE_STD, Q_KEY_CODE_9, "9", NULL, "}", NULL, "R" },
    { FP_GRID_X(4), FP_GRID_Y(3), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(6, 4), FP_STYLE_STD, Q_KEY_CODE_ASTERISK, "\x03", NULL, "!", NULL, "S" },

    /* Row 4 */
    { FP_GRID_X(0), FP_GRID_Y(4), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(7, 0), FP_STYLE_ALPHA, Q_KEY_CODE_A, "ALPHA", NULL, "alpha", NULL, NULL },
    { FP_GRID_X(1), FP_GRID_Y(4), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(8, 1), FP_STYLE_STD, Q_KEY_CODE_4, "4", NULL, "Matrix", NULL, "T" },
    { FP_GRID_X(2), FP_GRID_Y(4), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(7, 2), FP_STYLE_STD, Q_KEY_CODE_5, "5", NULL, "[", NULL, "U" },
    { FP_GRID_X(3), FP_GRID_Y(4), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(7, 3), FP_STYLE_STD, Q_KEY_CODE_6, "6", NULL, "]", NULL, "V" },
    { FP_GRID_X(4), FP_GRID_Y(4), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(7, 4), FP_STYLE_STD, Q_KEY_CODE_MINUS, "\x04", NULL, "\x01", NULL, "W" },

    /* Row 5 */
    { FP_GRID_X(0), FP_GRID_Y(5), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(8, 0), FP_STYLE_SHIFT, Q_KEY_CODE_SHIFT, "SHIFT", NULL, NULL, NULL, NULL },
    { FP_GRID_X(1), FP_GRID_Y(5), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(9, 1), FP_STYLE_STD, Q_KEY_CODE_1, "1", NULL, "Prgm", NULL, "X" },
    { FP_GRID_X(2), FP_GRID_Y(5), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(8, 2), FP_STYLE_STD, Q_KEY_CODE_2, "2", NULL, "i", NULL, "Y" },
    { FP_GRID_X(3), FP_GRID_Y(5), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(8, 3), FP_STYLE_STD, Q_KEY_CODE_3, "3", NULL, "\xE3", NULL, "Z" },
    { FP_GRID_X(4), FP_GRID_Y(5), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(8, 4), FP_STYLE_STD, Q_KEY_CODE_KP_ADD, "+", NULL, "\xE4", NULL, "_" },

    /* Row 6 */
    { FP_GRID_X(0), FP_GRID_Y(6), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(10, 0), FP_STYLE_STD, Q_KEY_CODE_ESC, "ON/C", NULL, "OFF", NULL, NULL },
    { FP_GRID_X(1), FP_GRID_Y(6), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(9, 0), FP_STYLE_STD, Q_KEY_CODE_0, "0", NULL, "Notes", NULL, "\"" },
    { FP_GRID_X(2), FP_GRID_Y(6), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(9, 2), FP_STYLE_STD, Q_KEY_CODE_DOT, ".", NULL, "=", NULL, ":" },
    { FP_GRID_X(3), FP_GRID_Y(6), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(9, 3), FP_STYLE_STD, Q_KEY_CODE_UNMAPPED, "(\x04)", NULL, "ABS", NULL, ";" },
    { FP_GRID_X(4), FP_GRID_Y(6), GRID_KEY_W, GRID_KEY_H, HP39GII_FP_KEY(9, 4), FP_STYLE_STD, Q_KEY_CODE_RET, "ENTER", NULL, "ANS", NULL, NULL },
};

/* Directional-pad matrix codes, indexed by wedge (0 = up, clockwise). */
static const int fp_dpad_keys[4] = {
    HP39GII_FP_KEY(0, 4), /* up */
    HP39GII_FP_KEY(1, 4), /* right */
    HP39GII_FP_KEY(3, 4), /* down */
    HP39GII_FP_KEY(2, 4), /* left */
};

static bool fp_key_down(const uint64_t *key_state, int key)
{
    if (key < 0 || key >= 128) {
        return false;
    }
    return (key_state[key >> 6] & (1ULL << (key & 63))) != 0;
}

/* ------------------------------------------------------------------ */
/* Key and case rendering                                             */

static void fp_draw_key(const FPCtx *ctx, const FPButton *b)
{
    bool down = fp_key_down(ctx->key_state, b->key);
    uint32_t edge;
    uint32_t shadow;
    uint32_t top;
    uint32_t bot;
    uint32_t text = fp_rgb(ctx, COL_KEY_TEXT);
    uint32_t blue = fp_rgb(ctx, COL_LEGEND_BLUE);
    uint32_t red = fp_rgb(ctx, COL_LEGEND_RED);
    int primary_scale;
    int tx;
    int ty;

    if (b->style == FP_STYLE_FKEY) {
        edge = fp_rgb(ctx, COL_FKEY_EDGE);
        shadow = edge;
        top = fp_rgb(ctx, COL_FKEY_TOP);
        bot = fp_rgb(ctx, COL_FKEY_BOT);
    } else {
        edge = fp_rgb(ctx, COL_KEY_EDGE);
        shadow = fp_rgb(ctx, COL_KEY_SHADOW);
        /* NB: the COL_* macros are comma triples; they cannot be used
         * inside a ?: expression. */
        if (down) {
            top = fp_rgb(ctx, COL_KEY_DN_TOP);
            bot = fp_rgb(ctx, COL_KEY_DN_BOT);
        } else {
            top = fp_rgb(ctx, COL_KEY_TOP);
            bot = fp_rgb(ctx, COL_KEY_BOT);
        }
    }
    if (down) {
        uint32_t tmp = top;

        top = bot;
        bot = tmp;
    }

    /* Drop shadow, edge, then two-tone body. */
    if (!down) {
        fp_fill_rounded(ctx, b->x + 2, b->y + 3, b->w, b->h, 6, shadow);
    }
    fp_fill_rounded(ctx, b->x, b->y, b->w, b->h, 6, edge);
    fp_fill_rounded(ctx, b->x + 2, b->y + 2, b->w - 4, b->h - 4, 4, bot);
    fp_fill_rect(ctx, b->x + 4, b->y + 2, b->w - 8, (b->h - 4) / 2, top);

    if (b->style == FP_STYLE_ALPHA || b->style == FP_STYLE_SHIFT) {
        /* Colored legend block with white text, like the real keys. */
        uint32_t block;
        uint32_t block_text = fp_rgb(ctx, COL_BLOCK_TEXT);
        int bw = b->w - 12;
        int bh = 20;

        if (b->style == FP_STYLE_ALPHA) {
            block = fp_rgb(ctx, COL_ALPHA_BLOCK);
        } else {
            block = fp_rgb(ctx, COL_SHIFT_BLOCK);
        }

        fp_fill_rounded(ctx, b->x + 6, b->y + 5, bw, bh, 3, block);
        tx = b->x + 6 + (bw - fp_text_width(b->primary, 1)) / 2;
        fp_draw_text(ctx, tx, b->y + 7, b->primary, 1, block_text);
    } else if (b->style == FP_STYLE_FKEY) {
        tx = b->x + (b->w - fp_text_width(b->primary, 1)) / 2;
        fp_draw_text(ctx, tx, b->y + (b->h - 16) / 2, b->primary, 1, text);
    } else {
        primary_scale = fp_text_width(b->primary, 2) <= b->w - 8 ? 2 : 1;
        tx = b->x + (b->w - fp_text_width(b->primary, primary_scale)) / 2;
        ty = b->y + (primary_scale == 2 ? 4 : 8);
        fp_draw_text(ctx, tx, ty, b->primary, primary_scale, text);
        if (b->primary_sup) {
            fp_draw_text_half(ctx, tx + fp_text_width(b->primary,
                              primary_scale) + 1, ty, b->primary_sup, text);
        }
    }

    /* Blue shifted-function legend, bottom left. */
    if (b->shift) {
        int sx = b->x + 5;
        int sy = b->y + b->h - 19;

        fp_draw_text(ctx, sx, sy, b->shift, 1, blue);
        if (b->shift_sup) {
            fp_draw_text_half(ctx, sx + fp_text_width(b->shift, 1) + 1,
                              sy, b->shift_sup, blue);
        }
    }

    /* Red alpha letter, bottom right corner. */
    if (b->alpha) {
        fp_draw_text(ctx, b->x + b->w - 5 - fp_text_width(b->alpha, 1),
                     b->y + b->h - 19, b->alpha, 1, red);
    }
}

/* Wedge index for a directional-pad offset: 0 up, 1 right, 2 down,
 * 3 left, -1 for the diagonal gaps between wedges. */
static int fp_dpad_wedge(int dx, int dy)
{
    int ax = dx < 0 ? -dx : dx;
    int ay = dy < 0 ? -dy : dy;

    if (ay > ax * 3 / 2 && ay > ax) {
        return dy < 0 ? 0 : 2;
    }
    if (ax > ay * 3 / 2 && ax > ay) {
        return dx < 0 ? 3 : 1;
    }
    return -1;
}

static void fp_draw_dpad(const FPCtx *ctx)
{
    uint32_t ring = fp_rgb(ctx, COL_DPAD_RING);
    uint32_t disc = fp_rgb(ctx, COL_DPAD_DISC);
    uint32_t sector = fp_rgb(ctx, COL_DPAD_SECTOR);
    uint32_t pressed = fp_rgb(ctx, COL_DPAD_DOWN);
    int dx;
    int dy;

    fp_fill_circle(ctx, DPAD_CX, DPAD_CY, DPAD_R + 3, ring);
    fp_fill_circle(ctx, DPAD_CX, DPAD_CY, DPAD_R, disc);

    for (dy = -DPAD_R; dy <= DPAD_R; dy++) {
        for (dx = -DPAD_R; dx <= DPAD_R; dx++) {
            int d2 = dx * dx + dy * dy;
            int wedge;

            if (d2 > (DPAD_R - 2) * (DPAD_R - 2)) {
                continue;
            }
            wedge = fp_dpad_wedge(dx, dy);
            if (wedge < 0) {
                continue;
            }
            fp_put_pixel(ctx, DPAD_CX + dx, DPAD_CY + dy,
                         fp_key_down(ctx->key_state, fp_dpad_keys[wedge]) ?
                         pressed : sector);
        }
    }

    fp_fill_circle(ctx, DPAD_CX, DPAD_CY, 3, fp_rgb(ctx, COL_DPAD_DOT));
}

static void fp_draw_body(const FPCtx *ctx)
{
    uint32_t text = fp_rgb(ctx, COL_HEADER_TEXT);

    fp_vgradient(ctx, 0, 0, HP39GII_FP_WIDTH, HP39GII_FP_HEIGHT,
                 COL_BODY_TOP, COL_BODY_BOT);
    fp_fill_rect(ctx, 0, 0, HP39GII_FP_WIDTH, 2, fp_rgb(ctx, COL_BODY_EDGE));
    fp_fill_rect(ctx, 0, HP39GII_FP_HEIGHT - 2, HP39GII_FP_WIDTH, 2,
                 fp_rgb(ctx, COL_BODY_EDGE));
    fp_fill_rect(ctx, 0, 0, 2, HP39GII_FP_HEIGHT, fp_rgb(ctx, COL_BODY_EDGE));
    fp_fill_rect(ctx, HP39GII_FP_WIDTH - 2, 0, 2, HP39GII_FP_HEIGHT,
                 fp_rgb(ctx, COL_BODY_EDGE));

    fp_draw_text(ctx, 30, 22, "HP 39gII", 2, text);
    fp_draw_text(ctx, 30, 58, "Graphing Calculator", 1, text);

    fp_fill_circle(ctx, 506, 46, 26, fp_rgb(ctx, COL_LOGO_BG));
    fp_draw_text(ctx, 506 - fp_text_width("hp", 2) / 2, 46 - 16, "hp", 2,
                 fp_rgb(ctx, COL_LOGO_FG));

    /* LCD bezel; the glass itself is dynamic and drawn per frame. */
    fp_fill_rounded(ctx, BEZEL_X, BEZEL_Y, BEZEL_W, BEZEL_H, 10,
                    fp_rgb(ctx, COL_BEZEL_EDGE));
    fp_fill_rounded(ctx, BEZEL_X + 2, BEZEL_Y + 2, BEZEL_W - 4, BEZEL_H - 4,
                    8, fp_rgb(ctx, COL_BEZEL));
}

/* ------------------------------------------------------------------ */
/* LCD glass                                                          */

/* Gray-level -> surface pixel LUT, rebuilt when the format changes. */
static uint32_t fp_glass_lut[256];
static pixman_format_code_t fp_glass_lut_fmt;

static void fp_build_glass_lut(const FPCtx *ctx)
{
    int g;

    for (g = 0; g < 256; g++) {
        unsigned int t = 255 - g; /* 0 = light, 255 = dark */
        unsigned int r = 195 + (35 - 195) * t / 255;
        unsigned int gg = 201 + (40 - 201) * t / 255;
        unsigned int b = 179 + (46 - 179) * t / 255;

        fp_glass_lut[g] = fp_rgb(ctx, r, gg, b);
    }
    fp_glass_lut_fmt = ctx->format;
}

static void fp_draw_pixels(const FPCtx *ctx, const uint8_t *vram)
{
    int bpp = surface_bytes_per_pixel(ctx->surface);
    int stride = surface_stride(ctx->surface);
    uint8_t *base = surface_data(ctx->surface);
    int vx;
    int vy;

    if (fp_glass_lut_fmt != ctx->format) {
        fp_build_glass_lut(ctx);
    }

    if (bpp == 4) {
        for (vy = 0; vy < HP39GII_PANEL_VIEW_H; vy++) {
            const uint8_t *src =
                &vram[(vy + HP39GII_PANEL_VIEW_Y) * HP39GII_PANEL_WIDTH];
            uint32_t *row0 =
                (uint32_t *)(base + (PIX_Y + vy * PIX_SCALE) * stride) + PIX_X;
            uint32_t *row1 =
                (uint32_t *)(base + (PIX_Y + vy * PIX_SCALE + 1) * stride) +
                PIX_X;

            for (vx = 0; vx < HP39GII_PANEL_VIEW_W; vx++) {
                uint32_t pix = fp_glass_lut[src[vx]];

                row0[vx * 2] = pix;
                row0[vx * 2 + 1] = pix;
                row1[vx * 2] = pix;
                row1[vx * 2 + 1] = pix;
            }
        }
        return;
    }

    for (vy = 0; vy < HP39GII_PANEL_VIEW_H; vy++) {
        const uint8_t *src =
            &vram[(vy + HP39GII_PANEL_VIEW_Y) * HP39GII_PANEL_WIDTH];

        for (vx = 0; vx < HP39GII_PANEL_VIEW_W; vx++) {
            uint32_t pix = fp_glass_lut[src[vx]];
            int sx;
            int sy;

            for (sy = 0; sy < PIX_SCALE; sy++) {
                for (sx = 0; sx < PIX_SCALE; sx++) {
                    fp_put_pixel(ctx, PIX_X + vx * PIX_SCALE + sx,
                                 PIX_Y + vy * PIX_SCALE + sy, pix);
                }
            }
        }
    }
}

static void fp_draw_glass(const FPCtx *ctx, const uint8_t *vram,
                          bool display_on)
{
    /* The whole glass area is repainted on every call; the bezel around
     * it belongs to the static backdrop (fp_draw_body). */
    fp_fill_rect(ctx, GLASS_X, GLASS_Y, GLASS_W, 308,
                 fp_rgb(ctx, COL_GLASS));

    if (!display_on) {
        return;
    }

    /* Hairline between the annunciator strip and the pixel matrix. */
    fp_fill_rect(ctx, GLASS_X, STRIP_Y + STRIP_H, GLASS_W, 2,
                 fp_rgb(ctx, COL_HAIRLINE));

    fp_draw_annunciators(ctx, vram);
    fp_draw_pixels(ctx, vram);
}

/* ------------------------------------------------------------------ */
/* Public interface                                                   */

static DisplaySurface *fp_prepare_surface(QemuConsole *con)
{
    DisplaySurface *surface = qemu_console_surface(con);

    if (!surface ||
        surface_width(surface) != HP39GII_FP_WIDTH ||
        surface_height(surface) != HP39GII_FP_HEIGHT) {
        qemu_console_resize(con, HP39GII_FP_WIDTH, HP39GII_FP_HEIGHT);
        surface = qemu_console_surface(con);
    }

    if (!surface || surface_bits_per_pixel(surface) == 0) {
        return NULL;
    }
    return surface;
}

void hp39gii_fp_render(QemuConsole *con, const uint8_t *vram,
                       const uint64_t *key_state, bool display_on)
{
    /* The case, bezel, keypad and directional pad only change with key
     * presses, so they are repainted only when the key state (or the
     * surface) changes; the glass is repainted on every call. */
    static DisplaySurface *backdrop_surface;
    static pixman_format_code_t backdrop_fmt;
    static uint64_t backdrop_keys[2];
    static bool backdrop_valid;
    FPCtx ctx;
    bool backdrop_stale;
    size_t i;

    ctx.surface = fp_prepare_surface(con);
    if (!ctx.surface) {
        return;
    }
    ctx.format = surface_format(ctx.surface);
    ctx.key_state = key_state;

    backdrop_stale = !backdrop_valid ||
                     backdrop_surface != ctx.surface ||
                     backdrop_fmt != ctx.format ||
                     backdrop_keys[0] != key_state[0] ||
                     backdrop_keys[1] != key_state[1];

    if (backdrop_stale) {
        fp_draw_body(&ctx);
        fp_draw_dpad(&ctx);
        for (i = 0; i < ARRAY_SIZE(fp_buttons); i++) {
            fp_draw_key(&ctx, &fp_buttons[i]);
        }
        backdrop_surface = ctx.surface;
        backdrop_fmt = ctx.format;
        backdrop_keys[0] = key_state[0];
        backdrop_keys[1] = key_state[1];
        backdrop_valid = true;
    }

    fp_draw_glass(&ctx, vram, display_on);

    dpy_gfx_update(con, 0, 0, HP39GII_FP_WIDTH, HP39GII_FP_HEIGHT);
}

int hp39gii_fp_key_at(int x, int y)
{
    size_t i;
    int dx = x - DPAD_CX;
    int dy = y - DPAD_CY;

    for (i = 0; i < ARRAY_SIZE(fp_buttons); i++) {
        const FPButton *b = &fp_buttons[i];

        if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) {
            return b->key;
        }
    }

    if (dx * dx + dy * dy <= DPAD_R * DPAD_R) {
        int wedge = fp_dpad_wedge(dx, dy);

        if (wedge >= 0) {
            return fp_dpad_keys[wedge];
        }
    }

    return HP39GII_FP_KEY_NONE;
}

int hp39gii_fp_key_for_qcode(QKeyCode qcode)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(fp_buttons); i++) {
        if (fp_buttons[i].qcode == (int)qcode) {
            return fp_buttons[i].key;
        }
    }

    switch (qcode) {
    case Q_KEY_CODE_UP:
        return HP39GII_FP_KEY(0, 4);
    case Q_KEY_CODE_RIGHT:
        return HP39GII_FP_KEY(1, 4);
    case Q_KEY_CODE_DOWN:
        return HP39GII_FP_KEY(3, 4);
    case Q_KEY_CODE_LEFT:
        return HP39GII_FP_KEY(2, 4);
    case Q_KEY_CODE_KP_ENTER:
        return HP39GII_FP_KEY(9, 4);
    case Q_KEY_CODE_KP_0:
        return HP39GII_FP_KEY(9, 0);
    case Q_KEY_CODE_KP_1:
        return HP39GII_FP_KEY(9, 1);
    case Q_KEY_CODE_KP_2:
        return HP39GII_FP_KEY(8, 2);
    case Q_KEY_CODE_KP_3:
        return HP39GII_FP_KEY(8, 3);
    case Q_KEY_CODE_KP_4:
        return HP39GII_FP_KEY(8, 1);
    case Q_KEY_CODE_KP_5:
        return HP39GII_FP_KEY(7, 2);
    case Q_KEY_CODE_KP_6:
        return HP39GII_FP_KEY(7, 3);
    case Q_KEY_CODE_KP_7:
        return HP39GII_FP_KEY(7, 1);
    case Q_KEY_CODE_KP_8:
        return HP39GII_FP_KEY(6, 2);
    case Q_KEY_CODE_KP_9:
        return HP39GII_FP_KEY(6, 3);
    case Q_KEY_CODE_KP_DECIMAL:
        return HP39GII_FP_KEY(9, 2);
    case Q_KEY_CODE_KP_MULTIPLY:
        return HP39GII_FP_KEY(6, 4);
    case Q_KEY_CODE_KP_DIVIDE:
        return HP39GII_FP_KEY(5, 4);
    case Q_KEY_CODE_KP_SUBTRACT:
        return HP39GII_FP_KEY(7, 4);
    case Q_KEY_CODE_EQUAL:
        return HP39GII_FP_KEY(8, 4);
    case Q_KEY_CODE_DELETE:
        return HP39GII_FP_KEY(3, 3);
    default:
        return HP39GII_FP_KEY_NONE;
    }
}
