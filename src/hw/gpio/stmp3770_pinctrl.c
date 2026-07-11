/*
 * STMP3770 Pin Control and GPIO
 *
 * Based on STMP3770 Reference Manual Chapter 33
 *
 * Features:
 * - Pin multiplexing (2 bits per pin)
 * - Drive strength / voltage selection
 * - Pull-up / pad keeper control
 * - GPIO data output, output enable, data input
 * - GPIO interrupt control (level/edge, polarity, enable, status)
 * - SET/CLR/TOG register variants
 *
 * Copyright (C) 2024
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/gpio/stmp3770_pinctrl.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

/* Register offsets */
#define REG_CTRL            0x000

#define REG_MUXSEL0         0x100
#define REG_MUXSEL1         0x110
#define REG_MUXSEL2         0x120
#define REG_MUXSEL3         0x130
#define REG_MUXSEL4         0x140
#define REG_MUXSEL5         0x150
#define REG_MUXSEL6         0x160
#define REG_MUXSEL7         0x170

#define REG_DRIVE0          0x200
#define REG_DRIVE1          0x210
#define REG_DRIVE2          0x220
#define REG_DRIVE3          0x230
#define REG_DRIVE4          0x240
#define REG_DRIVE5          0x250
#define REG_DRIVE6          0x260
#define REG_DRIVE7          0x270
#define REG_DRIVE8          0x280
#define REG_DRIVE9          0x290
#define REG_DRIVE10         0x2A0
#define REG_DRIVE11         0x2B0
#define REG_DRIVE12         0x2C0
#define REG_DRIVE13         0x2D0
#define REG_DRIVE14         0x2E0

#define REG_PULL0           0x300
#define REG_PULL1           0x310
#define REG_PULL2           0x320
#define REG_PULL3           0x330

#define REG_DOUT0           0x400
#define REG_DOUT1           0x410
#define REG_DOUT2           0x420
#define REG_DOUT3           0x430

#define REG_DIN0            0x500
#define REG_DIN1            0x510
#define REG_DIN2            0x520
#define REG_DIN3            0x530

#define REG_DOE0            0x600
#define REG_DOE1            0x610
#define REG_DOE2            0x620
#define REG_DOE3            0x630

#define REG_PIN2IRQ0        0x700
#define REG_PIN2IRQ1        0x710
#define REG_PIN2IRQ2        0x720
#define REG_PIN2IRQ3        0x730

#define REG_IRQEN0          0x800
#define REG_IRQEN1          0x810
#define REG_IRQEN2          0x820
#define REG_IRQEN3          0x830

#define REG_IRQLEVEL0       0x900
#define REG_IRQLEVEL1       0x910
#define REG_IRQLEVEL2       0x920
#define REG_IRQLEVEL3       0x930

#define REG_IRQPOL0         0xA00
#define REG_IRQPOL1         0xA10
#define REG_IRQPOL2         0xA20
#define REG_IRQPOL3         0xA30

#define REG_IRQSTAT0        0xB00
#define REG_IRQSTAT1        0xB10
#define REG_IRQSTAT2        0xB20
#define REG_IRQSTAT3        0xB30

/* SET/CLR/TOG offsets (within 16-byte aligned block) */
#define REG_SET             0x4
#define REG_CLR             0x8
#define REG_TOG             0xC

/* CTRL register bits */
#define CTRL_SFTRST         (1U << 31)
#define CTRL_CLKGATE        (1U << 30)
#define CTRL_PRESENT3       (1U << 29)
#define CTRL_PRESENT2       (1U << 28)
#define CTRL_PRESENT1       (1U << 27)
#define CTRL_PRESENT0       (1U << 26)
#define CTRL_IRQOUT3        (1U << 3)
#define CTRL_IRQOUT2        (1U << 2)
#define CTRL_IRQOUT1        (1U << 1)
#define CTRL_IRQOUT0        (1U << 0)

/* Mask of valid pins per bank */
static const uint32_t bank_pin_mask[STMP3770_PINCTRL_NUM_BANKS] = {
    0x3FFFFFFF,  /* Bank 0: pins 0-29 */
    0x1FFFFFFF,  /* Bank 1: pins 0-28 */
    0xFFFFFFFF,  /* Bank 2: pins 0-31 */
    0x00000000,  /* Bank 3: no GPIO functionality on STMP3770 */
};

/* Each pad drive field has three writable bits; its fourth bit is reserved. */
static const uint32_t drive_mask[15] = {
    0x77777777, 0x77777777, 0x77777777, 0x77777777,
    0x77777777, 0x77777777, 0x77777777, 0x00077777,
    0x77777777, 0x77777777, 0x77777777, 0x77777777,
    0x77777777, 0x77777777, 0x00777777,
};

/* PULL0..2 implement only the pads documented in Tables 1206, 1208 and 1210. */
static const uint32_t pull_mask[STMP3770_PINCTRL_NUM_BANKS] = {
    0x3c1000fe, 0x0f400000, 0x00004000, 0x0003ffff,
};

#define HP39GII_KEY_COL_BANK 1
#define HP39GII_KEY_COL_MASK ((1U << 22) | (1U << 23) | (1U << 25) | \
                              (1U << 26) | (1U << 27))

typedef struct HP39GIIKeyLine {
    uint8_t bank;
    uint8_t pin;
} HP39GIIKeyLine;

static const HP39GIIKeyLine hp39gii_key_rows[10] = {
    { 2, 6 },  /* row 0 */
    { 2, 5 },  /* row 1 */
    { 2, 4 },  /* row 2 */
    { 2, 2 },  /* row 3 */
    { 2, 3 },  /* row 4 */
    { 1, 24 }, /* row 5 */
    { 2, 8 },  /* row 6 */
    { 2, 7 },  /* row 7 */
    { 0, 20 }, /* row 8 */
    { 2, 14 }, /* row 9 */
};

static const uint8_t hp39gii_key_col_pins[5] = {
    23, 25, 27, 26, 22,
};

static bool hp39gii_key_down(STMP3770PINCTRLState *s, unsigned int key)
{
    if (key >= 128) {
        return false;
    }
    return (s->key_state[key >> 6] & (1ULL << (key & 63))) != 0;
}

static bool hp39gii_row_is_driven_low(STMP3770PINCTRLState *s,
                                      unsigned int row)
{
    const HP39GIIKeyLine *line = &hp39gii_key_rows[row];
    uint32_t mask = 1U << line->pin;

    return (s->doe[line->bank] & mask) != 0 &&
           (s->dout[line->bank] & mask) == 0;
}

static void hp39gii_apply_keyboard_columns(STMP3770PINCTRLState *s,
                                           uint32_t *val)
{
    unsigned int row;
    unsigned int col;

    for (row = 0; row < ARRAY_SIZE(hp39gii_key_rows); row++) {
        if (!hp39gii_row_is_driven_low(s, row)) {
            continue;
        }

        for (col = 0; col < ARRAY_SIZE(hp39gii_key_col_pins); col++) {
            unsigned int key = (row << 3) + col;
            uint32_t col_mask = 1U << hp39gii_key_col_pins[col];

            if (hp39gii_key_down(s, key) && (HP39GII_KEY_COL_MASK & col_mask) &&
                (s->doe[HP39GII_KEY_COL_BANK] & col_mask) == 0) {
                *val &= ~col_mask;
            }
        }
    }
}

static void stmp3770_pinctrl_sample_din(STMP3770PINCTRLState *s);

void stmp3770_pinctrl_set_key(STMP3770PINCTRLState *s,
                              unsigned int key, bool down)
{
    if (key >= 128) {
        return;
    }

    if (down) {
        s->key_state[key >> 6] |= 1ULL << (key & 63);
    } else {
        s->key_state[key >> 6] &= ~(1ULL << (key & 63));
    }

    stmp3770_pinctrl_sample_din(s);
}

void stmp3770_pinctrl_set_pwm_output(STMP3770PINCTRLState *s,
                                     unsigned int channel, uint8_t level)
{
    if (channel >= ARRAY_SIZE(s->pwm_output)) {
        return;
    }
    s->pwm_output[channel] = level;
    stmp3770_pinctrl_sample_din(s);
}

static inline int bank_from_offset(unsigned offset)
{
    if (offset >= REG_DOUT0 && offset < REG_DOUT0 + 0x100) {
        return (offset - REG_DOUT0) >> 4;
    }
    if (offset >= REG_DIN0 && offset < REG_DIN0 + 0x100) {
        return (offset - REG_DIN0) >> 4;
    }
    if (offset >= REG_DOE0 && offset < REG_DOE0 + 0x100) {
        return (offset - REG_DOE0) >> 4;
    }
    if (offset >= REG_PIN2IRQ0 && offset < REG_PIN2IRQ0 + 0x100) {
        return (offset - REG_PIN2IRQ0) >> 4;
    }
    if (offset >= REG_IRQEN0 && offset < REG_IRQEN0 + 0x100) {
        return (offset - REG_IRQEN0) >> 4;
    }
    if (offset >= REG_IRQLEVEL0 && offset < REG_IRQLEVEL0 + 0x100) {
        return (offset - REG_IRQLEVEL0) >> 4;
    }
    if (offset >= REG_IRQPOL0 && offset < REG_IRQPOL0 + 0x100) {
        return (offset - REG_IRQPOL0) >> 4;
    }
    if (offset >= REG_IRQSTAT0 && offset < REG_IRQSTAT0 + 0x100) {
        return (offset - REG_IRQSTAT0) >> 4;
    }
    return -1;
}

static void stmp3770_pinctrl_update_irq(STMP3770PINCTRLState *s)
{
    uint32_t irqout = 0;
    int i;

    for (i = 0; i < STMP3770_PINCTRL_NUM_BANKS; i++) {
        uint32_t pending = s->irqstat[i] & s->irqen[i] & s->pin2irq[i];
        qemu_set_irq(s->irq[i], pending ? 1 : 0);
        if (pending) {
            irqout |= (1U << i);
        }
    }

    /* Update IRQOUT bits in CTRL (read-only view) */
    s->ctrl &= ~(CTRL_IRQOUT0 | CTRL_IRQOUT1 | CTRL_IRQOUT2 | CTRL_IRQOUT3);
    s->ctrl |= irqout & 0xF;
}

static uint32_t stmp3770_pinctrl_read_din(STMP3770PINCTRLState *s, int bank);

static void stmp3770_pinctrl_sample_din(STMP3770PINCTRLState *s)
{
    int i;

    for (i = 0; i < STMP3770_PINCTRL_NUM_BANKS; i++) {
        uint32_t mask = bank_pin_mask[i];
        uint32_t now = stmp3770_pinctrl_read_din(s, i) & mask;
        uint32_t prev = s->prev_din[i] & mask;
        uint32_t changed = now ^ prev;
        uint32_t active_now, active_prev;
        uint32_t level_mask, edge_mask;
        uint32_t edge_set;

        active_now = (now & s->irqpol[i]) | (~now & ~s->irqpol[i]);
        active_prev = (prev & s->irqpol[i]) | (~prev & ~s->irqpol[i]);
        active_now &= mask;
        active_prev &= mask;

        level_mask = mask & s->irqlevel[i] & s->pin2irq[i];
        edge_mask = mask & ~s->irqlevel[i] & s->pin2irq[i];

        /* Level-sensitive IRQSTAT tracks the live input state */
        s->irqstat[i] = (s->irqstat[i] & ~level_mask) | (level_mask & active_now);

        /* Edge-sensitive IRQSTAT latches on the active transition */
        edge_set = edge_mask & active_now & ~active_prev & changed;
        s->irqstat[i] |= edge_set;

        s->prev_din[i] = now;
    }

    stmp3770_pinctrl_update_irq(s);
}

static uint32_t stmp3770_pinctrl_read_din(STMP3770PINCTRLState *s, int bank)
{
    uint32_t val = s->din[bank] | (s->dout[bank] & s->doe[bank]);
    unsigned int ch;

    if (bank == 2) {
        for (ch = 0; ch < ARRAY_SIZE(s->pwm_output); ch++) {
            uint32_t mux = (s->muxsel[4] >> (ch * 2)) & 0x3;
            uint32_t mask = 1U << ch;

            if (mux != 0 || s->pwm_output[ch] == STMP3770_PINCTRL_PWM_HI_Z) {
                continue;
            }
            if (s->pwm_output[ch]) {
                val |= mask;
            } else {
                val &= ~mask;
            }
        }
    }

    if (bank == HP39GII_KEY_COL_BANK) {
        uint32_t input_cols = HP39GII_KEY_COL_MASK & ~s->doe[bank];

        val |= input_cols;
        hp39gii_apply_keyboard_columns(s, &val);
    }

    if (bank == 0 && hp39gii_key_down(s, (10U << 3)) &&
        (s->doe[0] & (1U << 14)) == 0) {
        val |= 1U << 14;
    }

    return val;
}

static void stmp3770_pinctrl_reset(DeviceState *dev)
{
    STMP3770PINCTRLState *s = STMP3770_PINCTRL(dev);
    int i;

    s->ctrl = CTRL_SFTRST | CTRL_CLKGATE |
              CTRL_PRESENT0 | CTRL_PRESENT1 | CTRL_PRESENT2;

    for (i = 0; i < ARRAY_SIZE(s->muxsel); i++) {
        s->muxsel[i] = 0;
    }

    /* PDF Tables 1176-1204 reset each documented pad to 3.3 V / 4 mA. */
    for (i = 0; i < ARRAY_SIZE(s->drive); i++) {
        s->drive[i] = drive_mask[i] & 0x44444444;
    }

    for (i = 0; i < ARRAY_SIZE(s->pull); i++) {
        s->pull[i] = 0;
    }
    for (i = 0; i < STMP3770_PINCTRL_NUM_BANKS; i++) {
        s->dout[i] = 0;
        s->din[i] = 0;
        s->doe[i] = 0;
        s->prev_din[i] = 0;
        s->pin2irq[i] = 0;
        s->irqen[i] = 0;
        s->irqlevel[i] = 0;
        s->irqpol[i] = 0;
        s->irqstat[i] = 0;
    }
    memset(s->key_state, 0, sizeof(s->key_state));
    memset(s->pwm_output, STMP3770_PINCTRL_PWM_HI_Z,
           sizeof(s->pwm_output));

    stmp3770_pinctrl_update_irq(s);
}

static uint64_t stmp3770_pinctrl_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    STMP3770PINCTRLState *s = STMP3770_PINCTRL(opaque);
    uint32_t val = 0;
    int bank;

    /* Only 32-bit accesses are meaningful for most registers */
    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-pinctrl: unsupported read size %u at "
                      HWADDR_FMT_plx "\n", size, offset);
        return 0;
    }

    /* Chapter 34: hardware SET/CLR/TOG aliases are write-only and read zero. */
    if (offset & 0xF) {
        return 0;
    }

    offset &= ~0xF;

    switch (offset) {
    case REG_CTRL:
        val = s->ctrl;
        break;

    case REG_MUXSEL0 ... REG_MUXSEL7:
        val = s->muxsel[(offset - REG_MUXSEL0) >> 4];
        break;

    case REG_DRIVE0 ... REG_DRIVE14:
        val = s->drive[(offset - REG_DRIVE0) >> 4];
        break;

    case REG_PULL0 ... REG_PULL3:
        val = s->pull[(offset - REG_PULL0) >> 4];
        break;

    case REG_DOUT0 ... REG_DOUT3:
        bank = (offset - REG_DOUT0) >> 4;
        val = s->dout[bank];
        break;

    case REG_DIN0 ... REG_DIN3:
        bank = (offset - REG_DIN0) >> 4;
        val = stmp3770_pinctrl_read_din(s, bank);
        break;

    case REG_DOE0 ... REG_DOE3:
        bank = (offset - REG_DOE0) >> 4;
        val = s->doe[bank];
        break;

    case REG_PIN2IRQ0 ... REG_PIN2IRQ3:
        bank = (offset - REG_PIN2IRQ0) >> 4;
        val = s->pin2irq[bank];
        break;

    case REG_IRQEN0 ... REG_IRQEN3:
        bank = (offset - REG_IRQEN0) >> 4;
        val = s->irqen[bank];
        break;

    case REG_IRQLEVEL0 ... REG_IRQLEVEL3:
        bank = (offset - REG_IRQLEVEL0) >> 4;
        val = s->irqlevel[bank];
        break;

    case REG_IRQPOL0 ... REG_IRQPOL3:
        bank = (offset - REG_IRQPOL0) >> 4;
        val = s->irqpol[bank];
        break;

    case REG_IRQSTAT0 ... REG_IRQSTAT3:
        bank = (offset - REG_IRQSTAT0) >> 4;
        val = s->irqstat[bank];
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-pinctrl: read from unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        val = 0;
        break;
    }

    return val;
}

static void stmp3770_pinctrl_apply_write(uint32_t *reg, uint32_t mask,
                                         uint32_t value, bool is_set,
                                         bool is_clr, bool is_tog)
{
    if (is_set) {
        *reg |= (value & mask);
    } else if (is_clr) {
        *reg &= ~(value & mask);
    } else if (is_tog) {
        *reg ^= (value & mask);
    } else {
        *reg = (value & mask) | (*reg & ~mask);
    }
}

static void stmp3770_pinctrl_write(void *opaque, hwaddr offset,
                                   uint64_t value, unsigned size)
{
    STMP3770PINCTRLState *s = STMP3770_PINCTRL(opaque);
    bool is_set = (offset & 0xF) == REG_SET;
    bool is_clr = (offset & 0xF) == REG_CLR;
    bool is_tog = (offset & 0xF) == REG_TOG;
    uint32_t mask;
    int bank;

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-pinctrl: unsupported write size %u at "
                      HWADDR_FMT_plx "\n", size, offset);
        return;
    }

    offset &= ~0xF;

    switch (offset) {
    case REG_CTRL:
        /* PRESENT and IRQOUT bits are read-only */
        mask = CTRL_SFTRST | CTRL_CLKGATE;
        stmp3770_pinctrl_apply_write(&s->ctrl, mask, value,
                                     is_set, is_clr, is_tog);
        if (s->ctrl & CTRL_SFTRST) {
            stmp3770_pinctrl_reset(DEVICE(s));
        }
        break;

    case REG_MUXSEL0 ... REG_MUXSEL7:
        bank = (offset - REG_MUXSEL0) >> 4;
        stmp3770_pinctrl_apply_write(&s->muxsel[bank], 0xFFFFFFFF,
                                     value, is_set, is_clr, is_tog);
        stmp3770_pinctrl_sample_din(s);
        break;

    case REG_DRIVE0 ... REG_DRIVE14:
        bank = (offset - REG_DRIVE0) >> 4;
        stmp3770_pinctrl_apply_write(&s->drive[bank], drive_mask[bank],
                                     value, is_set, is_clr, is_tog);
        break;

    case REG_PULL0 ... REG_PULL3:
        bank = (offset - REG_PULL0) >> 4;
        mask = pull_mask[bank];
        stmp3770_pinctrl_apply_write(&s->pull[bank], mask,
                                     value, is_set, is_clr, is_tog);
        break;

    case REG_DOUT0 ... REG_DOUT3:
        bank = (offset - REG_DOUT0) >> 4;
        mask = bank_pin_mask[bank];
        stmp3770_pinctrl_apply_write(&s->dout[bank], mask,
                                     value, is_set, is_clr, is_tog);
        stmp3770_pinctrl_sample_din(s);
        break;

    case REG_DIN0 ... REG_DIN3:
        /* DIN is read-only; writes are silently ignored */
        break;

    case REG_DOE0 ... REG_DOE3:
        bank = (offset - REG_DOE0) >> 4;
        mask = bank_pin_mask[bank];
        stmp3770_pinctrl_apply_write(&s->doe[bank], mask,
                                     value, is_set, is_clr, is_tog);
        stmp3770_pinctrl_sample_din(s);
        break;

    case REG_PIN2IRQ0 ... REG_PIN2IRQ3:
        bank = (offset - REG_PIN2IRQ0) >> 4;
        mask = bank_pin_mask[bank];
        stmp3770_pinctrl_apply_write(&s->pin2irq[bank], mask,
                                     value, is_set, is_clr, is_tog);
        stmp3770_pinctrl_sample_din(s);
        break;

    case REG_IRQEN0 ... REG_IRQEN3:
        bank = (offset - REG_IRQEN0) >> 4;
        mask = bank_pin_mask[bank];
        stmp3770_pinctrl_apply_write(&s->irqen[bank], mask,
                                     value, is_set, is_clr, is_tog);
        stmp3770_pinctrl_sample_din(s);
        break;

    case REG_IRQLEVEL0 ... REG_IRQLEVEL3:
        bank = (offset - REG_IRQLEVEL0) >> 4;
        mask = bank_pin_mask[bank];
        stmp3770_pinctrl_apply_write(&s->irqlevel[bank], mask,
                                     value, is_set, is_clr, is_tog);
        stmp3770_pinctrl_sample_din(s);
        break;

    case REG_IRQPOL0 ... REG_IRQPOL3:
        bank = (offset - REG_IRQPOL0) >> 4;
        mask = bank_pin_mask[bank];
        stmp3770_pinctrl_apply_write(&s->irqpol[bank], mask,
                                     value, is_set, is_clr, is_tog);
        stmp3770_pinctrl_sample_din(s);
        break;

    case REG_IRQSTAT0 ... REG_IRQSTAT3:
        bank = (offset - REG_IRQSTAT0) >> 4;
        mask = bank_pin_mask[bank];
        if (is_set) {
            s->irqstat[bank] |= (value & mask);
        } else if (is_clr) {
            s->irqstat[bank] &= ~(value & mask);
        } else if (is_tog) {
            s->irqstat[bank] ^= (value & mask);
        } else {
            /* Regular write: 1s set, 0s ignored */
            s->irqstat[bank] |= (value & mask);
        }
        stmp3770_pinctrl_sample_din(s);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-pinctrl: write to unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        break;
    }
}

static const MemoryRegionOps stmp3770_pinctrl_ops = {
    .read = stmp3770_pinctrl_read,
    .write = stmp3770_pinctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void stmp3770_pinctrl_init(Object *obj)
{
    STMP3770PINCTRLState *s = STMP3770_PINCTRL(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    int i;

    memory_region_init_io(&s->iomem, obj, &stmp3770_pinctrl_ops, s,
                          TYPE_STMP3770_PINCTRL, 0x2000);
    sysbus_init_mmio(sbd, &s->iomem);

    for (i = 0; i < STMP3770_PINCTRL_NUM_BANKS; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
}

static int stmp3770_pinctrl_post_load(void *opaque, int version_id)
{
    STMP3770PINCTRLState *s = STMP3770_PINCTRL(opaque);

    if (version_id < 2) {
        memset(s->pwm_output, STMP3770_PINCTRL_PWM_HI_Z,
               sizeof(s->pwm_output));
    }
    return 0;
}

static const VMStateDescription vmstate_stmp3770_pinctrl = {
    .name = TYPE_STMP3770_PINCTRL,
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = stmp3770_pinctrl_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, STMP3770PINCTRLState),
        VMSTATE_UINT32_ARRAY(muxsel, STMP3770PINCTRLState, 8),
        VMSTATE_UINT32_ARRAY(drive, STMP3770PINCTRLState, 15),
        VMSTATE_UINT32_ARRAY(pull, STMP3770PINCTRLState, 4),
        VMSTATE_UINT32_ARRAY(dout, STMP3770PINCTRLState, 4),
        VMSTATE_UINT32_ARRAY(din, STMP3770PINCTRLState, 4),
        VMSTATE_UINT32_ARRAY(doe, STMP3770PINCTRLState, 4),
        VMSTATE_UINT32_ARRAY(prev_din, STMP3770PINCTRLState, 4),
        VMSTATE_UINT8_ARRAY_V(pwm_output, STMP3770PINCTRLState, 5, 2),
        VMSTATE_UINT32_ARRAY(pin2irq, STMP3770PINCTRLState, 4),
        VMSTATE_UINT32_ARRAY(irqen, STMP3770PINCTRLState, 4),
        VMSTATE_UINT32_ARRAY(irqlevel, STMP3770PINCTRLState, 4),
        VMSTATE_UINT32_ARRAY(irqpol, STMP3770PINCTRLState, 4),
        VMSTATE_UINT32_ARRAY(irqstat, STMP3770PINCTRLState, 4),
        VMSTATE_UINT64_ARRAY(key_state, STMP3770PINCTRLState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void stmp3770_pinctrl_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, stmp3770_pinctrl_reset);
    dc->vmsd = &vmstate_stmp3770_pinctrl;
}

static const TypeInfo stmp3770_pinctrl_type_info = {
    .name          = TYPE_STMP3770_PINCTRL,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STMP3770PINCTRLState),
    .instance_init = stmp3770_pinctrl_init,
    .class_init    = stmp3770_pinctrl_class_init,
};

static void stmp3770_pinctrl_register_types(void)
{
    type_register_static(&stmp3770_pinctrl_type_info);
}

type_init(stmp3770_pinctrl_register_types)
