/*
 * STMP3770 Clock Generation and Control (CLKCTRL)
 *
 * Based on STMP3770 Reference Manual Chapter 4
 *
 * Features:
 * - 480 MHz PLL with Phase Fractional Dividers (PFD)
 * - Multiple clock domains (CPU, HBUS, XBUS, SSP, GPMI, PIX, etc.)
 * - Clock gating and power management
 * - SET/CLR/TOG register support
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
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/misc/stmp3770_clkctrl.h"
#include "system/runstate.h"

/* Register offsets */
#define REG_PLLCTRL0        0x000
#define REG_PLLCTRL1        0x010
#define REG_CPU             0x020
#define REG_HBUS            0x030
#define REG_XBUS            0x040
#define REG_XTAL            0x050
#define REG_PIX             0x060
#define REG_SSP             0x070
#define REG_GPMI            0x080
#define REG_SPDIF           0x090
#define REG_FRAC            0x0D0
#define REG_CLKSEQ          0x0E0
#define REG_RESET           0x0F0
#define REG_VERSION         0x100

/* SET/CLR/TOG offsets */
#define REG_SET             0x4
#define REG_CLR             0x8
#define REG_TOG             0xC

/* PLLCTRL0 bits */
#define PLLCTRL0_POWER          (1 << 16)
#define PLLCTRL0_EN_USB_CLKS    (1 << 18)
#define PLLCTRL0_RW_MASK        0x33350000U

/* CPU register bits */
#define CPU_DIV_CPU_MASK        0x3FF
#define CPU_DIV_XTAL_MASK       (0x3FF << 16)
#define CPU_DIV_CPU_FRAC_EN     (1 << 10)
#define CPU_DIV_XTAL_FRAC_EN    (1 << 26)
#define CPU_BUSY_REF_CPU        (1 << 28)
#define CPU_BUSY_REF_XTAL       (1 << 29)
#define CPU_RW_MASK             0x07FF17FFU

/* HBUS register bits */
#define HBUS_DIV_MASK           0x1F
#define HBUS_BUSY               (1 << 29)
#define HBUS_DIV_FRAC_EN        (1 << 5)
#define HBUS_SLOW_DIV_MASK      (0x7 << 16)
#define HBUS_AUTO_SLOW_MODE     (1 << 20)
#define HBUS_RW_MASK            0x07F7003FU

/* FRAC register - Phase Fractional Dividers */
#define FRAC_CLKGATECPU         (1 << 7)
#define FRAC_CPU_STABLE         (1 << 6)
#define FRAC_CPUFRAC_MASK       0x3F
#define FRAC_CLKGATEIO          (1U << 31)
#define FRAC_IO_STABLE          (1 << 30)
#define FRAC_IOFRAC_MASK        (0x3F << 24)
#define FRAC_CLKGATEPIX         (1 << 23)
#define FRAC_PIX_STABLE         (1 << 22)
#define FRAC_PIXFRAC_MASK       (0x3F << 16)
#define FRAC_RW_MASK            0xBFBF00BFU
#define FRAC_MIN_DIV            0x12
#define FRAC_MAX_DIV            0x23

/* CLKSEQ register bits */
#define CLKSEQ_BYPASS_CPU       (1 << 7)
#define CLKSEQ_BYPASS_SSP       (1 << 5)
#define CLKSEQ_BYPASS_GPMI      (1 << 4)
#define CLKSEQ_BYPASS_IR        (1 << 3)
#define CLKSEQ_BYPASS_PIX       (1 << 1)
#define CLKSEQ_BYPASS_SAIF      (1 << 0)
#define CLKSEQ_RW_MASK          0x000000BBU

/* Peripheral divider register bits */
#define PERCLK_CLKGATE          (1U << 31)
#define PERCLK_BUSY             (1 << 29)
#define PERCLK_DIV_MASK_15      0x00007FFFU
#define PERCLK_DIV_MASK_10      0x000003FFU
#define PERCLK_DIV_MASK_9       0x000001FFU
#define PIX_RW_MASK             0x8000FFFFU
#define SSP_RW_MASK             0x800003FFU
#define GPMI_RW_MASK            0x800007FFU
#define XBUS_BUSY               (1U << 31)
#define XBUS_RW_MASK            0x000007FFU
#define XTAL_RW_MASK            0xFC000000U
#define XTAL_DIV_UART_FIXED     0x00000001U
#define SPDIF_RW_MASK           0x80000000U
#define PLLCTRL1_RW_MASK        0x40000000U
#define RESET_RW_MASK           0x00000003U

/* Version register */
#define VERSION_MAJOR           0x02
#define VERSION_MINOR           0x01
#define VERSION_STEP            0x00

#define TYPE_STMP3770_CLKCTRL "stmp3770-clkctrl"

struct STMP3770CLKCTRLState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    /* Registers */
    uint32_t pllctrl0;
    uint32_t pllctrl1;
    uint32_t cpu;
    uint32_t hbus;
    uint32_t xbus;
    uint32_t xtal;
    uint32_t pix;
    uint32_t ssp;
    uint32_t gpmi;
    uint32_t spdif;
    uint32_t frac;
    uint32_t clkseq;
    uint32_t reset;

    /* Clock state */
    bool pll_powered;

    STMP3770CLKCTRLDigResetFn dig_reset_cb;
    void *dig_reset_opaque;
    STMP3770CLKCTRLHclkRateFn hclk_rate_cb[2];
    void *hclk_rate_opaque[2];
    size_t hclk_rate_cb_count;
    STMP3770CLKCTRLGpmiRateFn gpmi_rate_cb;
    void *gpmi_rate_opaque;
};

void stmp3770_clkctrl_set_dig_reset_callback(STMP3770CLKCTRLState *s,
                                             STMP3770CLKCTRLDigResetFn cb,
                                             void *opaque)
{
    s->dig_reset_cb = cb;
    s->dig_reset_opaque = opaque;
}

static uint32_t stmp3770_clkctrl_scale_fractional(uint32_t input_hz,
                                                   uint32_t divider,
                                                   uint32_t width)
{
    return (uint64_t)input_hz * divider / (1U << width);
}

uint32_t stmp3770_clkctrl_get_hclk_rate(STMP3770CLKCTRLState *s)
{
    uint32_t cpu_div;
    uint32_t hbus_div = s->hbus & HBUS_DIV_MASK;
    uint32_t cpu_hz;
    uint32_t hclk_hz;

    if (s->clkseq & CLKSEQ_BYPASS_CPU) {
        cpu_div = (s->cpu & CPU_DIV_XTAL_MASK) >> 16;
        cpu_hz = 24000000;
        if (s->cpu & CPU_DIV_XTAL_FRAC_EN) {
            cpu_hz = stmp3770_clkctrl_scale_fractional(cpu_hz, cpu_div, 10);
        } else {
            cpu_hz /= cpu_div;
        }
    } else {
        uint32_t cpu_frac = s->frac & FRAC_CPUFRAC_MASK;

        if (!s->pll_powered || (s->frac & FRAC_CLKGATECPU)) {
            return 0;
        }

        cpu_div = s->cpu & CPU_DIV_CPU_MASK;
        cpu_hz = 480000000ULL * 18 / cpu_frac;
        if (s->cpu & CPU_DIV_CPU_FRAC_EN) {
            cpu_hz = stmp3770_clkctrl_scale_fractional(cpu_hz, cpu_div, 10);
        } else {
            cpu_hz /= cpu_div;
        }
    }

    if (s->hbus & HBUS_DIV_FRAC_EN) {
        hclk_hz = stmp3770_clkctrl_scale_fractional(cpu_hz, hbus_div, 5);
    } else {
        hclk_hz = cpu_hz / hbus_div;
    }

    if (s->hbus & HBUS_AUTO_SLOW_MODE) {
        hclk_hz >>= (s->hbus & HBUS_SLOW_DIV_MASK) >> 16;
    }

    return hclk_hz;
}

uint32_t stmp3770_clkctrl_get_gpmi_rate(STMP3770CLKCTRLState *s)
{
    uint32_t io_frac;
    uint32_t ref_hz;
    uint32_t div;

    if ((s->gpmi & PERCLK_CLKGATE) != 0) {
        return 0;
    }

    div = s->gpmi & PERCLK_DIV_MASK_10;
    if (div == 0) {
        return 0;
    }

    if ((s->clkseq & CLKSEQ_BYPASS_GPMI) != 0) {
        ref_hz = 24000000;
    } else {
        if (!s->pll_powered || (s->frac & FRAC_CLKGATEIO) != 0) {
            return 0;
        }

        io_frac = (s->frac & FRAC_IOFRAC_MASK) >> 24;
        if (io_frac == 0) {
            return 0;
        }
        ref_hz = 480000000ULL * 18 / io_frac;
    }

    return ref_hz / div;
}

static void stmp3770_clkctrl_notify_hclk_rate(STMP3770CLKCTRLState *s)
{
    size_t i;
    uint32_t hclk_hz = stmp3770_clkctrl_get_hclk_rate(s);

    for (i = 0; i < s->hclk_rate_cb_count; i++) {
        s->hclk_rate_cb[i](s->hclk_rate_opaque[i], hclk_hz);
    }
}

static void stmp3770_clkctrl_notify_gpmi_rate(STMP3770CLKCTRLState *s)
{
    if (s->gpmi_rate_cb) {
        s->gpmi_rate_cb(s->gpmi_rate_opaque,
                        stmp3770_clkctrl_get_gpmi_rate(s));
    }
}

void stmp3770_clkctrl_set_hclk_rate_callback(STMP3770CLKCTRLState *s,
                                              STMP3770CLKCTRLHclkRateFn cb,
                                              void *opaque)
{
    if (s->hclk_rate_cb_count < ARRAY_SIZE(s->hclk_rate_cb)) {
        s->hclk_rate_cb[s->hclk_rate_cb_count] = cb;
        s->hclk_rate_opaque[s->hclk_rate_cb_count] = opaque;
        s->hclk_rate_cb_count++;
    }
    stmp3770_clkctrl_notify_hclk_rate(s);
}

void stmp3770_clkctrl_set_gpmi_rate_callback(STMP3770CLKCTRLState *s,
                                             STMP3770CLKCTRLGpmiRateFn cb,
                                             void *opaque)
{
    s->gpmi_rate_cb = cb;
    s->gpmi_rate_opaque = opaque;
    stmp3770_clkctrl_notify_gpmi_rate(s);
}

static uint32_t stmp3770_clkctrl_apply_sct(uint32_t current, uint32_t val,
                                           bool is_set, bool is_clr, bool is_tog)
{
    if (is_set) {
        return current | val;
    }
    if (is_clr) {
        return current & ~val;
    }
    if (is_tog) {
        return current ^ val;
    }
    return val;
}

static void stmp3770_clkctrl_write_masked(uint32_t *target, uint32_t val,
                                          uint32_t writable_mask,
                                          bool is_set, bool is_clr, bool is_tog)
{
    uint32_t current = *target & writable_mask;
    uint32_t next = stmp3770_clkctrl_apply_sct(current, val & writable_mask,
                                               is_set, is_clr, is_tog) &
                    writable_mask;

    *target = (*target & ~writable_mask) | next;
}

static void stmp3770_clkctrl_restore_invalid_divider(uint32_t *target,
                                                     uint32_t old,
                                                     uint32_t div_mask,
                                                     unsigned shift,
                                                     uint32_t max_div)
{
    uint32_t div = (*target & div_mask) >> shift;

    if (div == 0 || div > max_div) {
        *target = (*target & ~div_mask) | (old & div_mask);
    }
}

static void stmp3770_clkctrl_write_cpu(uint32_t *target, uint32_t val,
                                       bool is_set, bool is_clr, bool is_tog)
{
    uint32_t old = *target;

    stmp3770_clkctrl_write_masked(target, val, CPU_RW_MASK,
                                  is_set, is_clr, is_tog);
    stmp3770_clkctrl_restore_invalid_divider(target, old, CPU_DIV_XTAL_MASK,
                                             16, 0x3FF);
    stmp3770_clkctrl_restore_invalid_divider(target, old, CPU_DIV_CPU_MASK,
                                             0, 0x3FF);

    if ((old & CPU_DIV_XTAL_MASK) != (*target & CPU_DIV_XTAL_MASK)) {
        *target |= CPU_BUSY_REF_XTAL;
    }
    if ((old & CPU_DIV_CPU_MASK) != (*target & CPU_DIV_CPU_MASK)) {
        *target |= CPU_BUSY_REF_CPU;
    }
}

static void stmp3770_clkctrl_write_hbus(uint32_t *target, uint32_t val,
                                        bool is_set, bool is_clr, bool is_tog)
{
    uint32_t old = *target;

    stmp3770_clkctrl_write_masked(target, val, HBUS_RW_MASK,
                                  is_set, is_clr, is_tog);
    stmp3770_clkctrl_restore_invalid_divider(target, old, HBUS_DIV_MASK,
                                             0, HBUS_DIV_MASK);

    if ((old & HBUS_DIV_MASK) != (*target & HBUS_DIV_MASK)) {
        *target |= HBUS_BUSY;
    }
}

static void stmp3770_clkctrl_write_xbus(uint32_t *target, uint32_t val,
                                        bool is_set, bool is_clr, bool is_tog)
{
    uint32_t old = *target;

    stmp3770_clkctrl_write_masked(target, val, XBUS_RW_MASK,
                                  is_set, is_clr, is_tog);
    stmp3770_clkctrl_restore_invalid_divider(target, old, PERCLK_DIV_MASK_10,
                                             0, PERCLK_DIV_MASK_10);

    if ((old & PERCLK_DIV_MASK_10) != (*target & PERCLK_DIV_MASK_10)) {
        *target |= XBUS_BUSY;
    }
}

static void stmp3770_clkctrl_write_perclk(uint32_t *target, uint32_t val,
                                          uint32_t writable_mask,
                                          uint32_t div_mask,
                                          uint32_t max_div,
                                          bool is_set, bool is_clr, bool is_tog)
{
    uint32_t current = *target;
    uint32_t current_visible = current & writable_mask;
    bool gate_changes = false;

    if (is_set || is_clr || is_tog) {
        gate_changes = val & PERCLK_CLKGATE;
    } else {
        gate_changes = ((current ^ val) & PERCLK_CLKGATE) != 0;
    }

    if (gate_changes || (current & PERCLK_CLKGATE)) {
        val = (val & ~div_mask) | (current_visible & div_mask);
    }

    stmp3770_clkctrl_write_masked(target, val, writable_mask,
                                  is_set, is_clr, is_tog);
    stmp3770_clkctrl_restore_invalid_divider(target, current, div_mask,
                                             0, max_div);

    if ((current & div_mask) != (*target & div_mask)) {
        *target |= PERCLK_BUSY;
    }
}

static void stmp3770_clkctrl_write_frac(uint32_t *target, uint32_t val,
                                        bool is_set, bool is_clr, bool is_tog)
{
    uint32_t old = *target;

    stmp3770_clkctrl_write_masked(target, val, FRAC_RW_MASK,
                                  is_set, is_clr, is_tog);
    stmp3770_clkctrl_restore_invalid_divider(target, old, FRAC_CPUFRAC_MASK,
                                             0, FRAC_MAX_DIV);
    stmp3770_clkctrl_restore_invalid_divider(target, old, FRAC_PIXFRAC_MASK,
                                             16, FRAC_MAX_DIV);
    stmp3770_clkctrl_restore_invalid_divider(target, old, FRAC_IOFRAC_MASK,
                                             24, FRAC_MAX_DIV);

    if ((*target & FRAC_CPUFRAC_MASK) < FRAC_MIN_DIV) {
        *target = (*target & ~FRAC_CPUFRAC_MASK) | (old & FRAC_CPUFRAC_MASK);
    }
    if (((*target & FRAC_PIXFRAC_MASK) >> 16) < FRAC_MIN_DIV) {
        *target = (*target & ~FRAC_PIXFRAC_MASK) | (old & FRAC_PIXFRAC_MASK);
    }
    if (((*target & FRAC_IOFRAC_MASK) >> 24) < FRAC_MIN_DIV) {
        *target = (*target & ~FRAC_IOFRAC_MASK) | (old & FRAC_IOFRAC_MASK);
    }

    if ((old & FRAC_CPUFRAC_MASK) != (*target & FRAC_CPUFRAC_MASK)) {
        *target ^= FRAC_CPU_STABLE;
    }
    if ((old & FRAC_PIXFRAC_MASK) != (*target & FRAC_PIXFRAC_MASK)) {
        *target ^= FRAC_PIX_STABLE;
    }
    if ((old & FRAC_IOFRAC_MASK) != (*target & FRAC_IOFRAC_MASK)) {
        *target ^= FRAC_IO_STABLE;
    }
}

static bool stmp3770_clkctrl_clkseq_switch_blocked(STMP3770CLKCTRLState *s,
                                                   uint32_t bit)
{
    switch (bit) {
    case CLKSEQ_BYPASS_CPU:
        return (s->frac & FRAC_CLKGATECPU) != 0;
    case CLKSEQ_BYPASS_SSP:
        return (s->frac & FRAC_CLKGATEIO) != 0 || (s->ssp & PERCLK_CLKGATE) != 0;
    case CLKSEQ_BYPASS_GPMI:
        return (s->frac & FRAC_CLKGATEIO) != 0 || (s->gpmi & PERCLK_CLKGATE) != 0;
    case CLKSEQ_BYPASS_IR:
        return (s->frac & FRAC_CLKGATEIO) != 0;
    case CLKSEQ_BYPASS_PIX:
        return (s->frac & FRAC_CLKGATEPIX) != 0 || (s->pix & PERCLK_CLKGATE) != 0;
    default:
        return false;
    }
}

static void stmp3770_clkctrl_write_clkseq(STMP3770CLKCTRLState *s, uint32_t val,
                                          bool is_set, bool is_clr, bool is_tog)
{
    uint32_t old = s->clkseq;
    uint32_t next;
    uint32_t changed;
    uint32_t blocked = 0;
    const uint32_t bypass_bits[] = {
        CLKSEQ_BYPASS_CPU,
        CLKSEQ_BYPASS_SSP,
        CLKSEQ_BYPASS_GPMI,
        CLKSEQ_BYPASS_IR,
        CLKSEQ_BYPASS_PIX,
    };
    size_t i;

    stmp3770_clkctrl_write_masked(&s->clkseq, val, CLKSEQ_RW_MASK,
                                  is_set, is_clr, is_tog);
    s->clkseq &= ~CLKSEQ_BYPASS_SAIF;

    next = s->clkseq;
    changed = (old ^ next) & CLKSEQ_RW_MASK;

    for (i = 0; i < ARRAY_SIZE(bypass_bits); i++) {
        uint32_t bit = bypass_bits[i];

        if ((changed & bit) && stmp3770_clkctrl_clkseq_switch_blocked(s, bit)) {
            blocked |= bit;
        }
    }

    s->clkseq = (next & ~blocked) | (old & blocked);
    s->clkseq &= ~CLKSEQ_BYPASS_SAIF;
}

static uint64_t stmp3770_clkctrl_read(void *opaque, hwaddr offset, unsigned size)
{
    STMP3770CLKCTRLState *s = STMP3770_CLKCTRL(opaque);
    uint32_t value = 0;

    /* Handle SET/CLR/TOG - they read the same as base register */
    offset &= ~0xF;

    switch (offset) {
    case REG_PLLCTRL0:
        value = s->pllctrl0;
        break;

    case REG_PLLCTRL1:
        value = s->pllctrl1;
        break;

    case REG_CPU:
        value = s->cpu;
        s->cpu &= ~(CPU_BUSY_REF_XTAL | CPU_BUSY_REF_CPU);
        break;

    case REG_HBUS:
        value = s->hbus;
        s->hbus &= ~HBUS_BUSY;
        break;

    case REG_XBUS:
        value = s->xbus;
        s->xbus &= ~XBUS_BUSY;
        break;

    case REG_XTAL:
        value = s->xtal;
        break;

    case REG_PIX:
        value = s->pix;
        s->pix &= ~PERCLK_BUSY;
        break;

    case REG_SSP:
        value = s->ssp;
        s->ssp &= ~PERCLK_BUSY;
        break;

    case REG_GPMI:
        value = s->gpmi;
        s->gpmi &= ~PERCLK_BUSY;
        break;

    case REG_SPDIF:
        value = s->spdif;
        break;

    case REG_FRAC:
        value = s->frac;
        break;

    case REG_CLKSEQ:
        value = s->clkseq;
        break;

    case REG_RESET:
        value = s->reset;
        break;

    case REG_VERSION:
        value = (VERSION_MAJOR << 24) | (VERSION_MINOR << 16) | VERSION_STEP;
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                     "%s: bad offset 0x%" HWADDR_PRIx "\n", __func__, offset);
        break;
    }

    return value;
}

static void stmp3770_clkctrl_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    STMP3770CLKCTRLState *s = STMP3770_CLKCTRL(opaque);
    uint32_t val = value;
    bool is_set = (offset & 0xF) == REG_SET;
    bool is_clr = (offset & 0xF) == REG_CLR;
    bool is_tog = (offset & 0xF) == REG_TOG;
    uint32_t *target = NULL;

    offset &= ~0xF;

    switch (offset) {
    case REG_PLLCTRL0:
        target = &s->pllctrl0;
        stmp3770_clkctrl_write_masked(target, val, PLLCTRL0_RW_MASK,
                                      is_set, is_clr, is_tog);
        s->pll_powered = (s->pllctrl0 & PLLCTRL0_POWER) != 0;
        stmp3770_clkctrl_notify_hclk_rate(s);
        stmp3770_clkctrl_notify_gpmi_rate(s);
        return;

    case REG_PLLCTRL1:
        target = &s->pllctrl1;
        stmp3770_clkctrl_write_masked(target, val, PLLCTRL1_RW_MASK,
                                      is_set, is_clr, is_tog);
        return;

    case REG_CPU:
        target = &s->cpu;
        stmp3770_clkctrl_write_cpu(target, val, is_set, is_clr, is_tog);
        stmp3770_clkctrl_notify_hclk_rate(s);
        return;

    case REG_HBUS:
        target = &s->hbus;
        stmp3770_clkctrl_write_hbus(target, val, is_set, is_clr, is_tog);
        stmp3770_clkctrl_notify_hclk_rate(s);
        return;

    case REG_XBUS:
        target = &s->xbus;
        stmp3770_clkctrl_write_xbus(target, val, is_set, is_clr, is_tog);
        return;

    case REG_XTAL:
        target = &s->xtal;
        stmp3770_clkctrl_write_masked(target, val, XTAL_RW_MASK,
                                      is_set, is_clr, is_tog);
        s->xtal = (s->xtal & XTAL_RW_MASK) | XTAL_DIV_UART_FIXED;
        return;

    case REG_PIX:
        target = &s->pix;
        stmp3770_clkctrl_write_perclk(target, val, PIX_RW_MASK,
                                      PERCLK_DIV_MASK_15,
                                      0xFF,
                                      is_set, is_clr, is_tog);
        return;

    case REG_SSP:
        target = &s->ssp;
        stmp3770_clkctrl_write_perclk(target, val, SSP_RW_MASK,
                                      PERCLK_DIV_MASK_9,
                                      PERCLK_DIV_MASK_9,
                                      is_set, is_clr, is_tog);
        return;

    case REG_GPMI:
        target = &s->gpmi;
        stmp3770_clkctrl_write_perclk(target, val, GPMI_RW_MASK,
                                      PERCLK_DIV_MASK_10,
                                      PERCLK_DIV_MASK_10,
                                      is_set, is_clr, is_tog);
        stmp3770_clkctrl_notify_gpmi_rate(s);
        return;

    case REG_SPDIF:
        target = &s->spdif;
        stmp3770_clkctrl_write_masked(target, val, SPDIF_RW_MASK,
                                      is_set, is_clr, is_tog);
        return;

    case REG_FRAC:
        target = &s->frac;
        stmp3770_clkctrl_write_frac(target, val, is_set, is_clr, is_tog);
        stmp3770_clkctrl_notify_hclk_rate(s);
        stmp3770_clkctrl_notify_gpmi_rate(s);
        return;

    case REG_CLKSEQ:
        stmp3770_clkctrl_write_clkseq(s, val, is_set, is_clr, is_tog);
        stmp3770_clkctrl_notify_hclk_rate(s);
        stmp3770_clkctrl_notify_gpmi_rate(s);
        return;

    case REG_RESET:
        target = &s->reset;
        stmp3770_clkctrl_write_masked(target, val, RESET_RW_MASK,
                                      is_set, is_clr, is_tog);
        if (s->reset & 0x1) {
            if (s->dig_reset_cb) {
                s->dig_reset_cb(s->dig_reset_opaque);
            } else {
                qemu_log_mask(LOG_UNIMP, "%s: digital reset requested without callback\n",
                              __func__);
            }
        }
        if (s->reset & 0x2) {
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        }
        s->reset = 0;
        return;

    case REG_VERSION:
        /* Read-only, ignore writes */
        return;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                     "%s: bad offset 0x%" HWADDR_PRIx "\n", __func__, offset);
        return;
    }

}

static const MemoryRegionOps stmp3770_clkctrl_ops = {
    .read = stmp3770_clkctrl_read,
    .write = stmp3770_clkctrl_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void stmp3770_clkctrl_reset(DeviceState *dev)
{
    STMP3770CLKCTRLState *s = STMP3770_CLKCTRL(dev);

    /* PLL disabled at reset */
    s->pllctrl0 = 0;                    /* POWER=0, PLL off */
    s->pllctrl1 = 0;

    /* CPU/HBUS/XBUS dividers reset to divide-by-1 with CPU on XTAL bypass */
    s->cpu = 0x00010001;                /* DIV_CPU=1, DIV_CPU_FRAC=1 */
    s->hbus = 0x00000001;               /* DIV=1 */
    s->xbus = 0x00000001;               /* DIV=1 */
    s->xtal = 0x70000001;               /* FILT/PWM/DRI gated, DIV_UART=1 */

    /* Peripheral dividers reset gated with divide-by-1 */
    s->pix = 0x80000001;
    s->ssp = 0x80000001;
    s->gpmi = 0x80000001;
    s->spdif = 0x80000000;

    /* Fractional dividers reset gated with FRAC=0x12 and stable bits cleared */
    s->frac = 0x92920092;

    /* Clock sequencer reset keeps all supported domains on XTAL bypass */
    s->clkseq = 0x000000BB;
    s->reset = 0;

    /* Clock state */
    s->pll_powered = false;
    stmp3770_clkctrl_notify_hclk_rate(s);
    stmp3770_clkctrl_notify_gpmi_rate(s);
}

static void stmp3770_clkctrl_init(Object *obj)
{
    STMP3770CLKCTRLState *s = STMP3770_CLKCTRL(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &stmp3770_clkctrl_ops, s,
                         TYPE_STMP3770_CLKCTRL, 0x200);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription vmstate_stmp3770_clkctrl = {
    .name = TYPE_STMP3770_CLKCTRL,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(pllctrl0, STMP3770CLKCTRLState),
        VMSTATE_UINT32(pllctrl1, STMP3770CLKCTRLState),
        VMSTATE_UINT32(cpu, STMP3770CLKCTRLState),
        VMSTATE_UINT32(hbus, STMP3770CLKCTRLState),
        VMSTATE_UINT32(xbus, STMP3770CLKCTRLState),
        VMSTATE_UINT32(xtal, STMP3770CLKCTRLState),
        VMSTATE_UINT32(pix, STMP3770CLKCTRLState),
        VMSTATE_UINT32(ssp, STMP3770CLKCTRLState),
        VMSTATE_UINT32(gpmi, STMP3770CLKCTRLState),
        VMSTATE_UINT32(spdif, STMP3770CLKCTRLState),
        VMSTATE_UINT32(frac, STMP3770CLKCTRLState),
        VMSTATE_UINT32(clkseq, STMP3770CLKCTRLState),
        VMSTATE_UINT32(reset, STMP3770CLKCTRLState),
        VMSTATE_BOOL(pll_powered, STMP3770CLKCTRLState),
        VMSTATE_END_OF_LIST()
    }
};

static void stmp3770_clkctrl_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, stmp3770_clkctrl_reset);
    dc->vmsd = &vmstate_stmp3770_clkctrl;
}

static const TypeInfo stmp3770_clkctrl_info = {
    .name          = TYPE_STMP3770_CLKCTRL,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STMP3770CLKCTRLState),
    .instance_init = stmp3770_clkctrl_init,
    .class_init    = stmp3770_clkctrl_class_init,
};

static void stmp3770_clkctrl_register_types(void)
{
    type_register_static(&stmp3770_clkctrl_info);
}

type_init(stmp3770_clkctrl_register_types)
