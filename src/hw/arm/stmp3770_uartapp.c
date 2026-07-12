/*
 * STMP3770 Application UART
 *
 * Copyright (C) 2024
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/arm/stmp3770_uartapp.h"
#include "hw/dma/stmp3770_dma.h"
#include "hw/irq.h"
#include "hw/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define SCT_SET 0x4
#define SCT_CLR 0x8
#define SCT_TOG 0xc

#define UARTAPP_CTRL0     0x000
#define UARTAPP_CTRL1     0x010
#define UARTAPP_CTRL2     0x020
#define UARTAPP_LINECTRL  0x030
#define UARTAPP_LINECTRL2 0x040
#define UARTAPP_INTR      0x050
#define UARTAPP_DATA      0x060
#define UARTAPP_STAT      0x070
#define UARTAPP_DEBUG     0x080
#define UARTAPP_VERSION   0x090

#define UARTAPP_CTRL0_SFTRST     (1U << 31)
#define UARTAPP_CTRL0_CLKGATE    (1U << 30)
#define UARTAPP_CTRL0_RUN        (1U << 29)
#define UARTAPP_CTRL0_RX_SOURCE  (1U << 28)
#define UARTAPP_CTRL0_RXTO_ENABLE (1U << 27)
#define UARTAPP_CTRL0_RXTIMEOUT_MASK 0x7ff0000U
#define UARTAPP_CTRL0_XFER_COUNT_MASK 0xffffU

#define UARTAPP_CTRL1_RUN        (1U << 28)
#define UARTAPP_CTRL1_XFER_COUNT_MASK 0xffffU
#define UARTAPP_CTRL1_RW_MASK 0x1000FFFFU

#define UARTAPP_CTRL2_UARTEN     (1U << 0)
#define UARTAPP_CTRL2_SIREN      (1U << 1)
#define UARTAPP_CTRL2_SIRLP      (1U << 2)
#define UARTAPP_CTRL2_USE_LCR2   (1U << 6)
#define UARTAPP_CTRL2_LBE        (1U << 7)
#define UARTAPP_CTRL2_TXE        (1U << 8)
#define UARTAPP_CTRL2_RXE        (1U << 9)
#define UARTAPP_CTRL2_RTS        (1U << 11)
#define UARTAPP_CTRL2_OUT1       (1U << 12)
#define UARTAPP_CTRL2_OUT2       (1U << 13)
#define UARTAPP_CTRL2_RTSEN      (1U << 14)
#define UARTAPP_CTRL2_CTSEN      (1U << 15)
#define UARTAPP_CTRL2_TXIFLSEL_SHIFT 16
#define UARTAPP_CTRL2_TXIFLSEL_MASK 0x7U
#define UARTAPP_CTRL2_RXIFLSEL_SHIFT 20
#define UARTAPP_CTRL2_RXIFLSEL_MASK 0x7U
#define UARTAPP_CTRL2_DMAONERR   (1U << 26)
#define UARTAPP_CTRL2_RXDMAE     (1U << 24)
#define UARTAPP_CTRL2_TXDMAE     (1U << 25)
#define UARTAPP_CTRL2_RW_MASK 0xFF77FFC7U

#define UARTAPP_LINECTRL_BRK     (1U << 0)
#define UARTAPP_LINECTRL_PEN     (1U << 1)
#define UARTAPP_LINECTRL_EPS     (1U << 2)
#define UARTAPP_LINECTRL_STP2    (1U << 3)
#define UARTAPP_LINECTRL_FEN     (1U << 4)
#define UARTAPP_LINECTRL_WLEN_SHIFT 5
#define UARTAPP_LINECTRL_WLEN_MASK 0x3U
#define UARTAPP_LINECTRL_SPS     (1U << 7)
#define UARTAPP_LINECTRL_BAUD_DIVFRAC_SHIFT 8
#define UARTAPP_LINECTRL_BAUD_DIVFRAC_MASK 0x3fU
#define UARTAPP_LINECTRL_BAUD_DIVINT_SHIFT 16
#define UARTAPP_LINECTRL_BAUD_DIVINT_MASK 0xffffU
#define UARTAPP_LINECTRL_RW_MASK 0xFFFF3FFFU

#define UARTAPP_LINECTRL2_RW_MASK 0xFFFF3FFEU

#define UARTAPP_INTR_STATUS_MASK 0x000007FFU
#define UARTAPP_INTR_ENABLE_MASK 0x07FF0000U
#define UARTAPP_INTR_RW_MASK 0x07FF07FFU

#define UARTAPP_INTR_RXIEN  (1U << 20)
#define UARTAPP_INTR_TXIEN  (1U << 21)
#define UARTAPP_INTR_RTIM   (1U << 22)
#define UARTAPP_INTR_RXIS   (1U << 4)
#define UARTAPP_INTR_TXIS   (1U << 5)
#define UARTAPP_INTR_RTIS   (1U << 6)

#define UARTAPP_STAT_WRITABLE_MASK 0x00F70000U

#define UARTAPP_UARTCLK_HZ 24000000ULL

static void uartapp_apply_sct(uint32_t *reg, uint32_t value, int sct,
                              uint32_t writable_mask)
{
    value &= writable_mask;

    switch (sct) {
    case SCT_SET:
        *reg |= value;
        break;
    case SCT_CLR:
        *reg &= ~value;
        break;
    case SCT_TOG:
        *reg ^= value;
        break;
    default:
        *reg = (*reg & ~writable_mask) | value;
        break;
    }
}

static unsigned uartapp_fifo_depth(STMP3770UARTAppState *s)
{
    return (s->linectrl & UARTAPP_LINECTRL_FEN) ? UARTAPP_FIFO_DEPTH : 1;
}

static unsigned uartapp_fifo_threshold(unsigned iflsel)
{
    if (iflsel == 0 || iflsel > 4) {
        return 1;
    }
    if (iflsel == 4) {
        return 14;
    }
    return iflsel * 4;
}

static void uartapp_update_intr_status(STMP3770UARTAppState *s)
{
    uint32_t status = 0;
    unsigned rx_iflsel, tx_iflsel;
    unsigned rx_threshold, tx_threshold;

    rx_iflsel = (s->ctrl2 >> UARTAPP_CTRL2_RXIFLSEL_SHIFT) &
                UARTAPP_CTRL2_RXIFLSEL_MASK;
    tx_iflsel = (s->ctrl2 >> UARTAPP_CTRL2_TXIFLSEL_SHIFT) &
                UARTAPP_CTRL2_TXIFLSEL_MASK;
    rx_threshold = uartapp_fifo_threshold(rx_iflsel);
    tx_threshold = uartapp_fifo_threshold(tx_iflsel);

    if ((s->ctrl2 & (UARTAPP_CTRL2_UARTEN | UARTAPP_CTRL2_RXE)) ==
            (UARTAPP_CTRL2_UARTEN | UARTAPP_CTRL2_RXE) &&
        s->rx_count >= rx_threshold) {
        status |= UARTAPP_INTR_RXIS;
    }
    if ((s->ctrl2 & (UARTAPP_CTRL2_UARTEN | UARTAPP_CTRL2_TXE)) ==
            (UARTAPP_CTRL2_UARTEN | UARTAPP_CTRL2_TXE) &&
        s->tx_count < tx_threshold) {
        status |= UARTAPP_INTR_TXIS;
    }

    s->intr_status = status;
}

static void uartapp_update_irq(STMP3770UARTAppState *s)
{
    uint32_t pending = ((s->intr & UARTAPP_INTR_STATUS_MASK) |
                        s->intr_status) &
                       ((s->intr & UARTAPP_INTR_ENABLE_MASK) >> 16);

    qemu_set_irq(s->irq, pending != 0);
}

static void uartapp_update_irq_full(STMP3770UARTAppState *s)
{
    uartapp_update_intr_status(s);
    uartapp_update_irq(s);
}

static void uartapp_reset_fifo(STMP3770UARTAppState *s)
{
    s->tx_count = 0;
    s->tx_rptr = 0;
    s->tx_wptr = 0;
    s->rx_count = 0;
    s->rx_rptr = 0;
    s->rx_wptr = 0;
    s->overrun = false;
}

static void uartapp_rx_dma_complete_check(STMP3770UARTAppState *s)
{
    if (s->rx_dma_active && s->rx_xfer_count == 0 && s->rx_dma_wait4endcmd) {
        s->rx_dma_wait4endcmd = false;
        s->rx_dma_active = false;
        s->ctrl0 &= ~UARTAPP_CTRL0_RUN;
        if (s->dma) {
            stmp3770_dma_complete_channel_command(s->dma, s->rx_dma_channel);
        }
    }
}

static bool uartapp_tx_push(STMP3770UARTAppState *s, uint8_t byte)
{
    unsigned depth = uartapp_fifo_depth(s);

    if (s->tx_count >= depth) {
        return false;
    }
    s->tx_fifo[s->tx_wptr] = byte;
    s->tx_wptr = (s->tx_wptr + 1) % UARTAPP_FIFO_DEPTH;
    s->tx_count++;
    return true;
}

static bool uartapp_tx_pop(STMP3770UARTAppState *s, uint8_t *byte)
{
    if (s->tx_count == 0) {
        return false;
    }
    *byte = s->tx_fifo[s->tx_rptr];
    s->tx_rptr = (s->tx_rptr + 1) % UARTAPP_FIFO_DEPTH;
    s->tx_count--;
    return true;
}

static bool uartapp_rx_push(STMP3770UARTAppState *s, uint8_t byte)
{
    unsigned depth = uartapp_fifo_depth(s);

    if (s->rx_count >= depth) {
        s->overrun = true;
        return false;
    }
    s->rx_fifo[s->rx_wptr] = byte;
    s->rx_wptr = (s->rx_wptr + 1) % UARTAPP_FIFO_DEPTH;
    s->rx_count++;
    if (s->rx_dma_active && s->rx_xfer_count > 0) {
        s->rx_xfer_count--;
        uartapp_rx_dma_complete_check(s);
    }
    return true;
}

static bool uartapp_rx_pop(STMP3770UARTAppState *s, uint8_t *byte)
{
    if (s->rx_count == 0) {
        return false;
    }
    *byte = s->rx_fifo[s->rx_rptr];
    s->rx_rptr = (s->rx_rptr + 1) % UARTAPP_FIFO_DEPTH;
    s->rx_count--;
    return true;
}

static uint32_t uartapp_tx_linectrl(const STMP3770UARTAppState *s)
{
    return (s->ctrl2 & UARTAPP_CTRL2_USE_LCR2) ? s->linectrl2 : s->linectrl;
}

static uint64_t uartapp_bit_time_ns(uint32_t linectrl)
{
    uint32_t divint = (linectrl >> UARTAPP_LINECTRL_BAUD_DIVINT_SHIFT) &
                      UARTAPP_LINECTRL_BAUD_DIVINT_MASK;
    uint32_t divfrac = (linectrl >> UARTAPP_LINECTRL_BAUD_DIVFRAC_SHIFT) &
                       UARTAPP_LINECTRL_BAUD_DIVFRAC_MASK;
    uint32_t divisor = (divint << 6) | divfrac;

    if (divisor == 0) {
        return 0;
    }
    return (uint64_t)divisor * NANOSECONDS_PER_SECOND /
           (UARTAPP_UARTCLK_HZ * 32);
}

static uint64_t uartapp_byte_time_ns(uint32_t linectrl)
{
    uint64_t bit_time = uartapp_bit_time_ns(linectrl);
    unsigned wlen_field;
    unsigned wlen;
    unsigned parity;
    unsigned stop_bits;
    unsigned frame_bits;

    if (bit_time == 0) {
        return 0;
    }

    wlen_field = (linectrl >> UARTAPP_LINECTRL_WLEN_SHIFT) &
                 UARTAPP_LINECTRL_WLEN_MASK;
    wlen = 5 + wlen_field;
    parity = (linectrl & UARTAPP_LINECTRL_PEN) ? 1 : 0;
    stop_bits = (linectrl & UARTAPP_LINECTRL_STP2) ? 2 : 1;
    frame_bits = 1 + wlen + parity + stop_bits;

    return bit_time * frame_bits;
}

static bool uartapp_tx_send_next(STMP3770UARTAppState *s)
{
    uint8_t byte;

    if (!s->tx_count) {
        return false;
    }

    if ((s->ctrl2 & (UARTAPP_CTRL2_UARTEN | UARTAPP_CTRL2_TXE)) !=
            (UARTAPP_CTRL2_UARTEN | UARTAPP_CTRL2_TXE)) {
        return false;
    }

    if (!uartapp_tx_pop(s, &byte)) {
        return false;
    }

    if (s->ctrl2 & UARTAPP_CTRL2_LBE) {
        uartapp_rx_push(s, byte);
    } else if (qemu_chr_fe_backend_connected(&s->chr)) {
        qemu_chr_fe_write_all(&s->chr, &byte, 1);
    }

    if (s->tx_dma_active && s->tx_xfer_count > 0) {
        s->tx_xfer_count--;
    }

    if (s->tx_dma_active && s->tx_xfer_count == 0 && s->tx_dma_wait4endcmd) {
        s->tx_dma_wait4endcmd = false;
        s->tx_dma_active = false;
        s->ctrl1 &= ~UARTAPP_CTRL1_RUN;
        if (s->dma) {
            stmp3770_dma_complete_channel_command(s->dma, s->tx_dma_channel);
        }
    }

    return true;
}

static void uartapp_tx_send_all(STMP3770UARTAppState *s)
{
    while (uartapp_tx_send_next(s)) {
    }
}

static void uartapp_tx_start_timer(STMP3770UARTAppState *s)
{
    uint32_t linectrl = uartapp_tx_linectrl(s);
    uint64_t byte_time = uartapp_byte_time_ns(linectrl);

    if (s->tx_baud_timer_active || s->tx_count == 0) {
        return;
    }

    if ((s->ctrl2 & (UARTAPP_CTRL2_UARTEN | UARTAPP_CTRL2_TXE)) !=
            (UARTAPP_CTRL2_UARTEN | UARTAPP_CTRL2_TXE)) {
        return;
    }

    if (byte_time == 0) {
        uartapp_tx_send_all(s);
        return;
    }

    s->tx_baud_timer_active = true;
    timer_mod(s->tx_baud_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + byte_time);
}

static void uartapp_tx_baud_cb(void *opaque)
{
    STMP3770UARTAppState *s = STMP3770_UARTAPP(opaque);

    s->tx_baud_timer_active = false;
    if (uartapp_tx_send_next(s)) {
        uartapp_tx_start_timer(s);
    }
    uartapp_update_irq_full(s);
}

static void uartapp_tx_process(STMP3770UARTAppState *s)
{
    if (!s->tx_count) {
        return;
    }

    if ((s->ctrl2 & (UARTAPP_CTRL2_UARTEN | UARTAPP_CTRL2_TXE)) !=
            (UARTAPP_CTRL2_UARTEN | UARTAPP_CTRL2_TXE)) {
        return;
    }

    uartapp_tx_start_timer(s);
    uartapp_update_irq_full(s);
}

static void uartapp_rx_process(STMP3770UARTAppState *s)
{
    unsigned depth = uartapp_fifo_depth(s);

    if (!s->rx_dma_active) {
        return;
    }

    while (s->rx_xfer_count > 0 && s->rx_count < depth) {
        if (s->ctrl2 & UARTAPP_CTRL2_LBE) {
            uint8_t byte;
            if (!uartapp_tx_pop(s, &byte)) {
                break;
            }
            uartapp_rx_push(s, byte);
        } else {
            uartapp_rx_push(s, 0xff);
        }
    }

    uartapp_rx_dma_complete_check(s);
    uartapp_update_irq_full(s);
}

static void uartapp_start_tx_dma(STMP3770UARTAppState *s)
{
    if (s->tx_dma_active) {
        return;
    }
    s->tx_xfer_count = s->ctrl1 & UARTAPP_CTRL1_XFER_COUNT_MASK;
    if (s->tx_xfer_count == 0) {
        s->tx_xfer_count = 1U << 16;
    }
    s->tx_dma_active = true;
    s->tx_dma_wait4endcmd = false;
}

static void uartapp_start_rx_dma(STMP3770UARTAppState *s)
{
    uint32_t xfer;

    if (s->rx_dma_active) {
        return;
    }
    xfer = s->ctrl0 & UARTAPP_CTRL0_XFER_COUNT_MASK;
    if (xfer == 0) {
        xfer = 1U << 16;
    }
    s->rx_xfer_count = (xfer > s->rx_count) ? (xfer - s->rx_count) : 0;
    s->rx_dma_active = true;
    s->rx_dma_wait4endcmd = false;
    uartapp_rx_process(s);
}

static void uartapp_dma_complete_tx(STMP3770UARTAppState *s)
{
    s->tx_dma_wait4endcmd = true;
    if (!s->tx_dma_active || s->tx_xfer_count == 0) {
        s->tx_dma_wait4endcmd = false;
        s->tx_dma_active = false;
        s->ctrl1 &= ~UARTAPP_CTRL1_RUN;
        if (s->dma) {
            stmp3770_dma_complete_channel_command(s->dma, s->tx_dma_channel);
        }
    }
}

static void uartapp_dma_complete_rx(STMP3770UARTAppState *s)
{
    s->rx_dma_wait4endcmd = true;
    if (!s->rx_dma_active || s->rx_xfer_count == 0) {
        s->rx_dma_wait4endcmd = false;
        s->rx_dma_active = false;
        s->ctrl0 &= ~UARTAPP_CTRL0_RUN;
        if (s->dma) {
            stmp3770_dma_complete_channel_command(s->dma, s->rx_dma_channel);
        }
    }
}

static uint32_t uartapp_read_stat(STMP3770UARTAppState *s)
{
    uint32_t val = 0xC0000000;
    unsigned depth = uartapp_fifo_depth(s);
    bool busy = s->tx_count > 0 || s->tx_baud_timer_active || s->tx_dma_active ||
                s->rx_dma_active ||
                (s->ctrl0 & UARTAPP_CTRL0_RUN) ||
                (s->ctrl1 & UARTAPP_CTRL1_RUN);

    if (busy) {
        val |= 1U << 29; /* BUSY */
    }
    if (s->tx_count == 0) {
        val |= 1U << 27; /* TXFE */
    }
    if (s->rx_count >= depth) {
        val |= 1U << 26; /* RXFF */
    }
    if (s->tx_count >= depth) {
        val |= 1U << 25; /* TXFF */
    }
    if (s->rx_count == 0) {
        val |= 1U << 24; /* RXFE */
    }
    val |= s->stat_writable & UARTAPP_STAT_WRITABLE_MASK;
    if (s->overrun) {
        val |= 1U << 19; /* OERR */
    }
    val |= s->rx_count & 0xffffU;
    return val;
}

static uint32_t uartapp_read_debug(STMP3770UARTAppState *s)
{
    uint32_t val = 0;
    unsigned rx_iflsel, tx_iflsel;
    unsigned rx_threshold, tx_threshold;

    rx_iflsel = (s->ctrl2 >> UARTAPP_CTRL2_RXIFLSEL_SHIFT) &
                UARTAPP_CTRL2_RXIFLSEL_MASK;
    tx_iflsel = (s->ctrl2 >> UARTAPP_CTRL2_TXIFLSEL_SHIFT) &
                UARTAPP_CTRL2_TXIFLSEL_MASK;
    rx_threshold = uartapp_fifo_threshold(rx_iflsel);
    tx_threshold = uartapp_fifo_threshold(tx_iflsel);

    if (s->tx_dma_active) {
        val |= 1U << 5; /* TXDMARUN */
    }
    if (s->rx_dma_active) {
        val |= 1U << 4; /* RXDMARUN */
    }
    if ((s->ctrl2 & (UARTAPP_CTRL2_UARTEN | UARTAPP_CTRL2_TXE)) ==
            (UARTAPP_CTRL2_UARTEN | UARTAPP_CTRL2_TXE) &&
        s->tx_count < tx_threshold) {
        val |= 1U << 1; /* TXDMARQ */
    }
    if ((s->ctrl2 & (UARTAPP_CTRL2_UARTEN | UARTAPP_CTRL2_RXE)) ==
            (UARTAPP_CTRL2_UARTEN | UARTAPP_CTRL2_RXE) &&
        s->rx_count >= rx_threshold) {
        val |= 1U << 0; /* RXDMARQ */
    }
    return val;
}

static void uartapp_reset(DeviceState *dev)
{
    STMP3770UARTAppState *s = STMP3770_UARTAPP(dev);

    s->ctrl0 = 0xC0030000;
    s->ctrl1 = 0;
    s->ctrl2 = 0x00220300;
    s->linectrl = 0;
    s->linectrl2 = 0;
    s->intr = 0;
    s->data = 0;
    s->stat_writable = 0x00F00000;
    s->intr_status = 0;

    uartapp_reset_fifo(s);

    s->tx_xfer_count = 0;
    s->rx_xfer_count = 0;
    s->tx_dma_active = false;
    s->rx_dma_active = false;
    s->tx_dma_wait4endcmd = false;
    s->rx_dma_wait4endcmd = false;
    s->tx_baud_timer_active = false;

    if (s->tx_baud_timer) {
        timer_del(s->tx_baud_timer);
    }

    uartapp_update_irq(s);
}

static uint64_t uartapp_read(void *opaque, hwaddr offset, unsigned size)
{
    STMP3770UARTAppState *s = STMP3770_UARTAPP(opaque);

    switch (offset & ~0xfULL) {
    case UARTAPP_CTRL0:
        return s->ctrl0;
    case UARTAPP_CTRL1:
        return s->ctrl1;
    case UARTAPP_CTRL2:
        return s->ctrl2;
    case UARTAPP_LINECTRL:
        return s->linectrl;
    case UARTAPP_LINECTRL2:
        return s->linectrl2;
    case UARTAPP_INTR:
        return s->intr | s->intr_status;
    case UARTAPP_DATA: {
        uint32_t val = 0;
        unsigned int i;

        if ((s->ctrl2 & (UARTAPP_CTRL2_UARTEN | UARTAPP_CTRL2_RXE)) ==
                (UARTAPP_CTRL2_UARTEN | UARTAPP_CTRL2_RXE)) {
            for (i = 0; i < size; i++) {
                uint8_t byte;
                if (!uartapp_rx_pop(s, &byte)) {
                    break;
                }
                val |= (uint32_t)byte << (i * 8);
            }
        }
        s->data = val;
        if (qemu_chr_fe_backend_connected(&s->chr)) {
            qemu_chr_fe_accept_input(&s->chr);
        }
        uartapp_update_irq_full(s);
        return val;
    }
    case UARTAPP_STAT:
        return uartapp_read_stat(s);
    case UARTAPP_DEBUG:
        return uartapp_read_debug(s);
    case UARTAPP_VERSION:
        return 0x02000000;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-uartapp: read from unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        return 0;
    }
}

static void uartapp_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    STMP3770UARTAppState *s = STMP3770_UARTAPP(opaque);
    int sct = offset & 0xf;
    unsigned int i;

    switch (offset & ~0xfULL) {
    case UARTAPP_CTRL0:
        uartapp_apply_sct(&s->ctrl0, value, sct, 0xFFFFFFFFU);
        if (s->ctrl0 & UARTAPP_CTRL0_SFTRST) {
            uartapp_reset(DEVICE(s));
        }
        break;
    case UARTAPP_CTRL1:
        uartapp_apply_sct(&s->ctrl1, value, sct, UARTAPP_CTRL1_RW_MASK);
        break;
    case UARTAPP_CTRL2:
        uartapp_apply_sct(&s->ctrl2, value, sct, UARTAPP_CTRL2_RW_MASK);
        uartapp_update_irq_full(s);
        break;
    case UARTAPP_LINECTRL:
        uartapp_apply_sct(&s->linectrl, value, sct, UARTAPP_LINECTRL_RW_MASK);
        break;
    case UARTAPP_LINECTRL2:
        uartapp_apply_sct(&s->linectrl2, value, sct,
                          UARTAPP_LINECTRL2_RW_MASK);
        break;
    case UARTAPP_INTR:
        uartapp_apply_sct(&s->intr, value, sct, UARTAPP_INTR_RW_MASK);
        uartapp_update_irq_full(s);
        break;
    case UARTAPP_DATA:
        if (sct == 0) {
            s->data = (uint32_t)value;
            if ((s->ctrl2 & (UARTAPP_CTRL2_UARTEN | UARTAPP_CTRL2_TXE)) ==
                    (UARTAPP_CTRL2_UARTEN | UARTAPP_CTRL2_TXE)) {
                for (i = 0; i < size; i++) {
                    if (!uartapp_tx_push(s, (value >> (i * 8)) & 0xFF)) {
                        break;
                    }
                }
                uartapp_tx_process(s);
            }
        }
        break;
    case UARTAPP_STAT:
        if (sct == 0) {
            s->stat_writable = value & UARTAPP_STAT_WRITABLE_MASK;
            s->overrun = false;
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-uartapp: write to unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        break;
    }
}

static const MemoryRegionOps uartapp_ops = {
    .read = uartapp_read,
    .write = uartapp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static int uartapp_can_receive(void *opaque)
{
    STMP3770UARTAppState *s = STMP3770_UARTAPP(opaque);

    return uartapp_fifo_depth(s) - s->rx_count;
}

static void uartapp_receive(void *opaque, const uint8_t *buf, int size)
{
    STMP3770UARTAppState *s = STMP3770_UARTAPP(opaque);
    int i;

    if ((s->ctrl2 & (UARTAPP_CTRL2_UARTEN | UARTAPP_CTRL2_RXE)) !=
            (UARTAPP_CTRL2_UARTEN | UARTAPP_CTRL2_RXE)) {
        return;
    }

    for (i = 0; i < size; i++) {
        uartapp_rx_push(s, buf[i]);
    }
    uartapp_update_irq_full(s);
}

static void uartapp_event(void *opaque, QEMUChrEvent event)
{
    STMP3770UARTAppState *s = STMP3770_UARTAPP(opaque);

    if (event == CHR_EVENT_BREAK) {
        uartapp_rx_push(s, 0);
    }
}

static void uartapp_init(Object *obj)
{
    STMP3770UARTAppState *s = STMP3770_UARTAPP(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &uartapp_ops, s,
                          TYPE_STMP3770_UARTAPP, 0x2000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    s->tx_baud_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, uartapp_tx_baud_cb, s);
    s->tx_baud_timer_active = false;
}

static void uartapp_realize(DeviceState *dev, Error **errp)
{
    STMP3770UARTAppState *s = STMP3770_UARTAPP(dev);

    qemu_chr_fe_set_handlers(&s->chr, uartapp_can_receive, uartapp_receive,
                             uartapp_event, NULL, s, NULL, true);
}

static void uartapp_instance_finalize(Object *obj)
{
    STMP3770UARTAppState *s = STMP3770_UARTAPP(obj);

    if (s->tx_baud_timer) {
        timer_free(s->tx_baud_timer);
        s->tx_baud_timer = NULL;
    }
}

static const VMStateDescription vmstate_uartapp = {
    .name = "stmp3770-uartapp",
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl0, STMP3770UARTAppState),
        VMSTATE_UINT32(ctrl1, STMP3770UARTAppState),
        VMSTATE_UINT32(ctrl2, STMP3770UARTAppState),
        VMSTATE_UINT32(linectrl, STMP3770UARTAppState),
        VMSTATE_UINT32(linectrl2, STMP3770UARTAppState),
        VMSTATE_UINT32(intr, STMP3770UARTAppState),
        VMSTATE_UINT32(data, STMP3770UARTAppState),
        VMSTATE_UINT32(stat_writable, STMP3770UARTAppState),
        VMSTATE_UINT32(intr_status, STMP3770UARTAppState),
        VMSTATE_UINT8_ARRAY(tx_fifo, STMP3770UARTAppState, UARTAPP_FIFO_DEPTH),
        VMSTATE_UINT8_ARRAY(rx_fifo, STMP3770UARTAppState, UARTAPP_FIFO_DEPTH),
        VMSTATE_UINT8(tx_count, STMP3770UARTAppState),
        VMSTATE_UINT8(tx_rptr, STMP3770UARTAppState),
        VMSTATE_UINT8(tx_wptr, STMP3770UARTAppState),
        VMSTATE_UINT8(rx_count, STMP3770UARTAppState),
        VMSTATE_UINT8(rx_rptr, STMP3770UARTAppState),
        VMSTATE_UINT8(rx_wptr, STMP3770UARTAppState),
        VMSTATE_BOOL(overrun, STMP3770UARTAppState),
        VMSTATE_UINT32(tx_xfer_count, STMP3770UARTAppState),
        VMSTATE_UINT32(rx_xfer_count, STMP3770UARTAppState),
        VMSTATE_BOOL(tx_dma_active, STMP3770UARTAppState),
        VMSTATE_BOOL(rx_dma_active, STMP3770UARTAppState),
        VMSTATE_BOOL(tx_dma_wait4endcmd, STMP3770UARTAppState),
        VMSTATE_BOOL(rx_dma_wait4endcmd, STMP3770UARTAppState),
        VMSTATE_BOOL(tx_baud_timer_active, STMP3770UARTAppState),
        VMSTATE_TIMER_PTR(tx_baud_timer, STMP3770UARTAppState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property uartapp_properties[] = {
    DEFINE_PROP_CHR("chardev", STMP3770UARTAppState, chr),
};

static void uartapp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = uartapp_realize;
    device_class_set_legacy_reset(dc, uartapp_reset);
    dc->vmsd = &vmstate_uartapp;
    device_class_set_props(dc, uartapp_properties);
}

static const TypeInfo uartapp_type_info = {
    .name          = TYPE_STMP3770_UARTAPP,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STMP3770UARTAppState),
    .instance_init = uartapp_init,
    .instance_finalize = uartapp_instance_finalize,
    .class_init    = uartapp_class_init,
};

static int stmp3770_uartapp_dma_handler(STMP3770DMAState *dma, int channel,
                                        STMP3770DMAEvent event, void *buf,
                                        size_t len, void *opaque)
{
    STMP3770UARTAppState *s = STMP3770_UARTAPP(opaque);
    size_t i;

    if (channel == s->tx_dma_channel) {
        if (event == STMP3770_DMA_EVENT_PIO) {
            uint32_t *pio = (uint32_t *)buf;
            if (len >= sizeof(uint32_t)) {
                uartapp_apply_sct(&s->ctrl1, pio[0], 0, UARTAPP_CTRL1_RW_MASK);
            }
            return (int)len;
        }

        if (event == STMP3770_DMA_EVENT_DATA_WRITE) {
            const uint8_t *src = (const uint8_t *)buf;

            if (s->ctrl1 & UARTAPP_CTRL1_RUN) {
                uartapp_start_tx_dma(s);
            }
            if (s->tx_dma_active) {
                for (i = 0; i < len; i++) {
                    if (!uartapp_tx_push(s, src[i])) {
                        break;
                    }
                }
                uartapp_tx_process(s);
                return (int)i;
            }
            return 0;
        }
    } else if (channel == s->rx_dma_channel) {
        if (event == STMP3770_DMA_EVENT_PIO) {
            uint32_t *pio = (uint32_t *)buf;
            if (len >= sizeof(uint32_t)) {
                uartapp_apply_sct(&s->ctrl0, pio[0], 0, 0xFFFFFFFFU);
            }
            return (int)len;
        }

        if (event == STMP3770_DMA_EVENT_DATA_READ) {
            uint8_t *dst = (uint8_t *)buf;

            if (s->ctrl0 & UARTAPP_CTRL0_RUN) {
                uartapp_start_rx_dma(s);
            }
            if (s->rx_dma_active) {
                for (i = 0; i < len; i++) {
                    uint8_t byte;
                    if (s->rx_count == 0) {
                        uartapp_rx_process(s);
                    }
                    if (!uartapp_rx_pop(s, &byte)) {
                        break;
                    }
                    dst[i] = byte;
                }
                return (int)i;
            }
            return 0;
        }
    }

    return 0;
}

static void stmp3770_uartapp_tx_dma_completion(STMP3770DMAState *dma,
                                               int channel, void *opaque)
{
    STMP3770UARTAppState *s = STMP3770_UARTAPP(opaque);

    if (channel == s->tx_dma_channel) {
        uartapp_dma_complete_tx(s);
    } else if (channel == s->rx_dma_channel) {
        uartapp_dma_complete_rx(s);
    }
}

void stmp3770_uartapp_set_dma(STMP3770UARTAppState *s,
                              STMP3770DMAState *dma,
                              int tx_channel, int rx_channel)
{
    s->dma = dma;
    s->tx_dma_channel = tx_channel;
    s->rx_dma_channel = rx_channel;

    if (!dma) {
        return;
    }

    stmp3770_dma_set_channel_handler(dma, tx_channel,
                                     stmp3770_uartapp_dma_handler, s);
    stmp3770_dma_set_channel_handler(dma, rx_channel,
                                     stmp3770_uartapp_dma_handler, s);
    stmp3770_dma_set_channel_completion_callback(dma, tx_channel,
                                                 stmp3770_uartapp_tx_dma_completion,
                                                 s);
    stmp3770_dma_set_channel_completion_callback(dma, rx_channel,
                                                 stmp3770_uartapp_tx_dma_completion,
                                                 s);
}

static void uartapp_register_types(void)
{
    type_register_static(&uartapp_type_info);
}

type_init(uartapp_register_types)
