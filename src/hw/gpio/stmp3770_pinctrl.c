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
    0x003FFFFF,  /* Bank 3: pins 0-21 */
};

#define HP39GII_KEY_COL_BANK 1
#define HP39GII_KEY_COL_MASK ((1U << 22) | (1U << 23) | (1U << 25) | \
                              (1U << 26) | (1U << 27))

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

static uint32_t stmp3770_pinctrl_read_din(STMP3770PINCTRLState *s, int bank)
{
    uint32_t val = s->din[bank] | (s->dout[bank] & s->doe[bank]);

    if (bank == HP39GII_KEY_COL_BANK) {
        uint32_t input_cols = HP39GII_KEY_COL_MASK & ~s->doe[bank];

        val |= input_cols;
    }

    return val;
}

static void stmp3770_pinctrl_reset(DeviceState *dev)
{
    STMP3770PINCTRLState *s = STMP3770_PINCTRL(dev);
    int i;

    s->ctrl = CTRL_SFTRST | CTRL_CLKGATE |
              CTRL_PRESENT0 | CTRL_PRESENT1 | CTRL_PRESENT2 | CTRL_PRESENT3;

    for (i = 0; i < ARRAY_SIZE(s->muxsel); i++) {
        s->muxsel[i] = 0;
    }

    /*
     * Drive strength configuration based on ExistOS BSP analysis.
     * Each register controls 16 pins (2 bits per pin):
     *   00 = 4mA, 01 = 8mA, 10 = 12mA, 11 = 16mA
     *
     * Default: 4mA for all pins (0x00000000)
     * GPMI NAND pins (Bank0 Pin 0-7): need 8mA (ExistOS stmp_board.cpp:120-196)
     */
    for (i = 0; i < ARRAY_SIZE(s->drive); i++) {
        s->drive[i] = 0x00000000;       /* Default 4mA for all */
    }

    /* DRIVE0: Bank0 Pin 0-7 (GPMI D0-D7)
     * Set MA=1 (8mA) for GPMI data lines
     * Bits [15:0] control Pin 0-7 (2 bits each)
     */
    s->drive[0] = 0x00005555;           /* Pin 0-7: 8mA, Pin 8-15: 4mA */

    /* DRIVE1: Bank0 Pin 16-23 (includes GPMI control signals)
     * Pin 16-19: GPMI CLE/ALE/WRN/RDN also need 8mA
     * Pin 22-25: GPMI RDY/CS/WP/RST also 8mA
     */
    s->drive[1] = 0x00000055;           /* Pin 16-19: 8mA */
    s->drive[2] = 0x00005555;           /* Pin 22-25 (in next reg): 8mA */

    for (i = 0; i < ARRAY_SIZE(s->pull); i++) {
        s->pull[i] = 0;
    }
    for (i = 0; i < STMP3770_PINCTRL_NUM_BANKS; i++) {
        s->dout[i] = 0;
        s->din[i] = 0;
        s->doe[i] = 0;
        s->pin2irq[i] = 0;
        s->irqen[i] = 0;
        s->irqlevel[i] = 0;
        s->irqpol[i] = 0;
        s->irqstat[i] = 0;
    }

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

    /* Ignore SET/CLR/TOG bits on read (offset should not have them) */
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
        break;

    case REG_DRIVE0 ... REG_DRIVE14:
        bank = (offset - REG_DRIVE0) >> 4;
        stmp3770_pinctrl_apply_write(&s->drive[bank], 0xFFFFFFFF,
                                     value, is_set, is_clr, is_tog);
        break;

    case REG_PULL0 ... REG_PULL3:
        bank = (offset - REG_PULL0) >> 4;
        mask = bank_pin_mask[bank];
        stmp3770_pinctrl_apply_write(&s->pull[bank], mask,
                                     value, is_set, is_clr, is_tog);
        break;

    case REG_DOUT0 ... REG_DOUT3:
        bank = (offset - REG_DOUT0) >> 4;
        mask = bank_pin_mask[bank];
        stmp3770_pinctrl_apply_write(&s->dout[bank], mask,
                                     value, is_set, is_clr, is_tog);
        break;

    case REG_DIN0 ... REG_DIN3:
        /* DIN is read-only; writes are silently ignored */
        break;

    case REG_DOE0 ... REG_DOE3:
        bank = (offset - REG_DOE0) >> 4;
        mask = bank_pin_mask[bank];
        stmp3770_pinctrl_apply_write(&s->doe[bank], mask,
                                     value, is_set, is_clr, is_tog);
        break;

    case REG_PIN2IRQ0 ... REG_PIN2IRQ3:
        bank = (offset - REG_PIN2IRQ0) >> 4;
        mask = bank_pin_mask[bank];
        stmp3770_pinctrl_apply_write(&s->pin2irq[bank], mask,
                                     value, is_set, is_clr, is_tog);
        stmp3770_pinctrl_update_irq(s);
        break;

    case REG_IRQEN0 ... REG_IRQEN3:
        bank = (offset - REG_IRQEN0) >> 4;
        mask = bank_pin_mask[bank];
        stmp3770_pinctrl_apply_write(&s->irqen[bank], mask,
                                     value, is_set, is_clr, is_tog);
        stmp3770_pinctrl_update_irq(s);
        break;

    case REG_IRQLEVEL0 ... REG_IRQLEVEL3:
        bank = (offset - REG_IRQLEVEL0) >> 4;
        mask = bank_pin_mask[bank];
        stmp3770_pinctrl_apply_write(&s->irqlevel[bank], mask,
                                     value, is_set, is_clr, is_tog);
        break;

    case REG_IRQPOL0 ... REG_IRQPOL3:
        bank = (offset - REG_IRQPOL0) >> 4;
        mask = bank_pin_mask[bank];
        stmp3770_pinctrl_apply_write(&s->irqpol[bank], mask,
                                     value, is_set, is_clr, is_tog);
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
        stmp3770_pinctrl_update_irq(s);
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

static const VMStateDescription vmstate_stmp3770_pinctrl = {
    .name = TYPE_STMP3770_PINCTRL,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, STMP3770PINCTRLState),
        VMSTATE_UINT32_ARRAY(muxsel, STMP3770PINCTRLState, 8),
        VMSTATE_UINT32_ARRAY(drive, STMP3770PINCTRLState, 15),
        VMSTATE_UINT32_ARRAY(pull, STMP3770PINCTRLState, 4),
        VMSTATE_UINT32_ARRAY(dout, STMP3770PINCTRLState, 4),
        VMSTATE_UINT32_ARRAY(din, STMP3770PINCTRLState, 4),
        VMSTATE_UINT32_ARRAY(doe, STMP3770PINCTRLState, 4),
        VMSTATE_UINT32_ARRAY(pin2irq, STMP3770PINCTRLState, 4),
        VMSTATE_UINT32_ARRAY(irqen, STMP3770PINCTRLState, 4),
        VMSTATE_UINT32_ARRAY(irqlevel, STMP3770PINCTRLState, 4),
        VMSTATE_UINT32_ARRAY(irqpol, STMP3770PINCTRLState, 4),
        VMSTATE_UINT32_ARRAY(irqstat, STMP3770PINCTRLState, 4),
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
