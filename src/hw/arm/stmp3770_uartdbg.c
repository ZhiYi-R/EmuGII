/*
 * STMP3770 Debug UART
 *
 * Copyright (C) 2024
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/arm/stmp3770_uartdbg.h"
#include "hw/irq.h"
#include "hw/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define UARTDBG_DR    0x000
#define UARTDBG_RSR   0x004
#define UARTDBG_FR    0x018
#define UARTDBG_IBRD  0x024
#define UARTDBG_FBRD  0x028
#define UARTDBG_LCR_H 0x02c
#define UARTDBG_CR    0x030
#define UARTDBG_IFLS  0x034
#define UARTDBG_IMSC  0x038
#define UARTDBG_RIS   0x03c
#define UARTDBG_MIS   0x040
#define UARTDBG_ICR   0x044
#define UARTDBG_DMACR 0x048

#define UARTDBG_FIFO_DEPTH 16
#define UARTDBG_DR_ERROR_MASK 0x0f00
#define UARTDBG_CR_RW_MASK 0xffc7
#define UARTDBG_IFLS_RW_MASK 0x003f
#define UARTDBG_IMSC_RW_MASK 0x07ff
#define UARTDBG_DMACR_RW_MASK 0x0007

#define UARTDBG_FR_RXFE (1U << 4)
#define UARTDBG_FR_TXFF (1U << 5)
#define UARTDBG_FR_RXFF (1U << 6)
#define UARTDBG_FR_TXFE (1U << 7)
#define UARTDBG_FR_BUSY (1U << 3)
#define UARTDBG_CR_LBE (1U << 7)
#define UARTDBG_CR_TXE (1U << 8)
#define UARTDBG_CR_RXE (1U << 9)
#define UARTDBG_CR_UARTEN (1U << 0)
#define UARTDBG_LCR_FEN (1U << 4)
#define UARTDBG_INT_RX (1U << 4)
#define UARTDBG_INT_TX (1U << 5)

#define UARTDBG_UARTCLK_HZ 24000000ULL

static unsigned uartdbg_fifo_depth(const STMP3770UARTDebugState *s)
{
    return (s->lcr & UARTDBG_LCR_FEN) ? UARTDBG_FIFO_DEPTH : 1;
}

static uint32_t uartdbg_flags(const STMP3770UARTDebugState *s)
{
    uint32_t flags = UARTDBG_FR_TXFE;
    unsigned depth = uartdbg_fifo_depth(s);

    if (s->tx_count != 0) {
        flags &= ~UARTDBG_FR_TXFE;
        flags |= UARTDBG_FR_TXFF | UARTDBG_FR_BUSY;
    }
    if (s->rx_count == 0) {
        flags |= UARTDBG_FR_RXFE;
    }
    if (s->rx_count == depth) {
        flags |= UARTDBG_FR_RXFF;
    }

    return flags;
}

static void uartdbg_update_irq(STMP3770UARTDebugState *s)
{
    qemu_set_irq(s->irq, (s->ris & s->imsc) != 0);
}

static unsigned uartdbg_fifo_threshold(unsigned iflsel)
{
    if (iflsel == 0 || iflsel > 4) {
        return 1;
    }
    if (iflsel == 4) {
        return 14;
    }
    return iflsel * 4;
}

static void uartdbg_update_ris(STMP3770UARTDebugState *s)
{
    unsigned rx_iflsel = (s->ifls >> 3) & 0x7;
    unsigned tx_iflsel = s->ifls & 0x7;
    unsigned rx_threshold = uartdbg_fifo_threshold(rx_iflsel);
    unsigned tx_threshold = uartdbg_fifo_threshold(tx_iflsel);

    s->ris &= ~(UARTDBG_INT_RX | UARTDBG_INT_TX);
    if ((s->cr & (UARTDBG_CR_UARTEN | UARTDBG_CR_RXE)) ==
            (UARTDBG_CR_UARTEN | UARTDBG_CR_RXE) &&
        s->rx_count >= rx_threshold) {
        s->ris |= UARTDBG_INT_RX;
    }
    if ((s->cr & (UARTDBG_CR_UARTEN | UARTDBG_CR_TXE)) ==
            (UARTDBG_CR_UARTEN | UARTDBG_CR_TXE) &&
        s->tx_count < tx_threshold) {
        s->ris |= UARTDBG_INT_TX;
    }

    uartdbg_update_irq(s);
}

static uint64_t uartdbg_bit_time_ns(const STMP3770UARTDebugState *s)
{
    uint64_t divisor = ((uint64_t)s->ibrd << 6) | s->fbrd;

    if (divisor == 0) {
        return 0;
    }
    return divisor * 16 * NANOSECONDS_PER_SECOND / (64 * UARTDBG_UARTCLK_HZ);
}

static uint64_t uartdbg_byte_time_ns(const STMP3770UARTDebugState *s)
{
    uint64_t bit_time = uartdbg_bit_time_ns(s);
    unsigned wlen_field;
    unsigned wlen;
    unsigned parity;
    unsigned stop_bits;
    unsigned frame_bits;

    if (bit_time == 0) {
        return 0;
    }

    wlen_field = (s->lcr >> 5) & 0x3;
    wlen = 5 + wlen_field;
    parity = (s->lcr & 0x2) ? 1 : 0;
    stop_bits = (s->lcr & 0x8) ? 2 : 1;
    frame_bits = 1 + wlen + parity + stop_bits;

    return bit_time * frame_bits;
}

static void uartdbg_put_rx(STMP3770UARTDebugState *s, uint16_t value);

static void uartdbg_tx_send(STMP3770UARTDebugState *s)
{
    uint8_t ch = s->tx_byte;

    s->tx_count = 0;

    if (s->cr & UARTDBG_CR_LBE) {
        uartdbg_put_rx(s, ch);
    } else if (qemu_chr_fe_backend_connected(&s->chr)) {
        qemu_chr_fe_write_all(&s->chr, &ch, 1);
    }
}

static void uartdbg_tx_process(STMP3770UARTDebugState *s)
{
    uint64_t byte_time;

    if (s->tx_count == 0) {
        return;
    }

    if ((s->cr & (UARTDBG_CR_UARTEN | UARTDBG_CR_TXE)) !=
            (UARTDBG_CR_UARTEN | UARTDBG_CR_TXE)) {
        return;
    }

    byte_time = uartdbg_byte_time_ns(s);
    if (byte_time == 0) {
        uartdbg_tx_send(s);
        uartdbg_update_ris(s);
        return;
    }

    if (s->tx_baud_timer_active) {
        return;
    }

    s->tx_baud_timer_active = true;
    timer_mod(s->tx_baud_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + byte_time);
}

static void uartdbg_tx_baud_cb(void *opaque)
{
    STMP3770UARTDebugState *s = STMP3770_UARTDBG(opaque);

    s->tx_baud_timer_active = false;
    uartdbg_tx_send(s);
    uartdbg_update_ris(s);
}

static void uartdbg_reset_fifo(STMP3770UARTDebugState *s)
{
    s->rx_pos = 0;
    s->rx_count = 0;
}

static void uartdbg_put_rx(STMP3770UARTDebugState *s, uint16_t value)
{
    unsigned depth = uartdbg_fifo_depth(s);
    unsigned slot;

    if (s->rx_count == depth) {
        s->rsr |= 1U << 3;
        s->ris |= 1U << 10;
        uartdbg_update_irq(s);
        return;
    }

    slot = (s->rx_pos + s->rx_count) & (depth - 1);
    s->rx_fifo[slot] = value;
    s->rx_count++;
    uartdbg_update_ris(s);
}

static uint32_t uartdbg_get_rx(STMP3770UARTDebugState *s)
{
    unsigned depth = uartdbg_fifo_depth(s);
    uint16_t value = 0;

    if (s->rx_count != 0) {
        value = s->rx_fifo[s->rx_pos];
        s->rx_pos = (s->rx_pos + 1) & (depth - 1);
        s->rx_count--;
        s->rsr = (value & UARTDBG_DR_ERROR_MASK) >> 8;
        uartdbg_update_ris(s);
        qemu_chr_fe_accept_input(&s->chr);
    }

    return value;
}

static int uartdbg_can_receive(void *opaque)
{
    STMP3770UARTDebugState *s = STMP3770_UARTDBG(opaque);

    return uartdbg_fifo_depth(s) - s->rx_count;
}

static void uartdbg_receive(void *opaque, const uint8_t *buf, int size)
{
    STMP3770UARTDebugState *s = STMP3770_UARTDBG(opaque);
    int i;

    if ((s->cr & (UARTDBG_CR_UARTEN | UARTDBG_CR_RXE)) !=
        (UARTDBG_CR_UARTEN | UARTDBG_CR_RXE)) {
        return;
    }

    for (i = 0; i < size; i++) {
        uartdbg_put_rx(s, buf[i]);
    }
}

static void uartdbg_event(void *opaque, QEMUChrEvent event)
{
    STMP3770UARTDebugState *s = STMP3770_UARTDBG(opaque);

    if (event == CHR_EVENT_BREAK) {
        uartdbg_put_rx(s, 1U << 10);
    }
}

static void uartdbg_reset(DeviceState *dev)
{
    STMP3770UARTDebugState *s = STMP3770_UARTDBG(dev);

    s->rsr = 0;
    s->ibrd = 0;
    s->fbrd = 0;
    s->lcr = 0;
    s->cr = UARTDBG_CR_RXE | UARTDBG_CR_TXE;
    s->ifls = 0x12;
    s->imsc = 0;
    s->ris = 0;
    s->dmacr = 0;
    s->tx_count = 0;
    s->tx_baud_timer_active = false;
    if (s->tx_baud_timer) {
        timer_del(s->tx_baud_timer);
    }
    uartdbg_reset_fifo(s);
    uartdbg_update_ris(s);
}

static uint64_t uartdbg_read(void *opaque, hwaddr offset, unsigned size)
{
    STMP3770UARTDebugState *s = STMP3770_UARTDBG(opaque);

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-uartdbg: unsupported read size %u at offset "
                      HWADDR_FMT_plx "\n", size, offset);
        return 0;
    }

    switch (offset) {
    case UARTDBG_DR:
        return uartdbg_get_rx(s);
    case UARTDBG_RSR:
        return s->rsr;
    case UARTDBG_FR:
        return uartdbg_flags(s);
    case UARTDBG_IBRD:
        return s->ibrd;
    case UARTDBG_FBRD:
        return s->fbrd;
    case UARTDBG_LCR_H:
        return s->lcr;
    case UARTDBG_CR:
        return s->cr;
    case UARTDBG_IFLS:
        return s->ifls;
    case UARTDBG_IMSC:
        return s->imsc;
    case UARTDBG_RIS:
        return s->ris;
    case UARTDBG_MIS:
        return s->ris & s->imsc;
    case UARTDBG_DMACR:
        return s->dmacr;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-uartdbg: read from unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        return 0;
    }
}

static void uartdbg_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    STMP3770UARTDebugState *s = STMP3770_UARTDBG(opaque);
    uint8_t ch;

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-uartdbg: unsupported write size %u at offset "
                      HWADDR_FMT_plx "\n", size, offset);
        return;
    }

    switch (offset) {
    case UARTDBG_DR:
        ch = value;
        if ((s->cr & (UARTDBG_CR_UARTEN | UARTDBG_CR_TXE)) ==
            (UARTDBG_CR_UARTEN | UARTDBG_CR_TXE)) {
            if (s->tx_count == 0) {
                s->tx_count = 1;
                s->tx_byte = ch;
                uartdbg_tx_process(s);
            }
        }
        uartdbg_update_ris(s);
        break;
    case UARTDBG_RSR:
        s->rsr = 0;
        break;
    case UARTDBG_IBRD:
        s->ibrd = value & 0xffff;
        break;
    case UARTDBG_FBRD:
        s->fbrd = value & 0x3f;
        break;
    case UARTDBG_LCR_H:
        if ((s->lcr ^ value) & UARTDBG_LCR_FEN) {
            uartdbg_reset_fifo(s);
        }
        s->lcr = value & 0xff;
        uartdbg_update_ris(s);
        break;
    case UARTDBG_CR:
        s->cr = value & UARTDBG_CR_RW_MASK;
        uartdbg_update_ris(s);
        break;
    case UARTDBG_IFLS:
        s->ifls = value & UARTDBG_IFLS_RW_MASK;
        uartdbg_update_ris(s);
        break;
    case UARTDBG_IMSC:
        s->imsc = value & UARTDBG_IMSC_RW_MASK;
        uartdbg_update_irq(s);
        break;
    case UARTDBG_ICR:
        s->ris &= ~(value & UARTDBG_IMSC_RW_MASK);
        uartdbg_update_ris(s);
        break;
    case UARTDBG_DMACR:
        s->dmacr = value & UARTDBG_DMACR_RW_MASK;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-uartdbg: write to unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        break;
    }
}

static const MemoryRegionOps uartdbg_ops = {
    .read = uartdbg_read,
    .write = uartdbg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void uartdbg_init(Object *obj)
{
    STMP3770UARTDebugState *s = STMP3770_UARTDBG(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &uartdbg_ops, s,
                          TYPE_STMP3770_UARTDBG, 0x2000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    s->tx_baud_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, uartdbg_tx_baud_cb, s);
    s->tx_baud_timer_active = false;
}

static void uartdbg_realize(DeviceState *dev, Error **errp)
{
    STMP3770UARTDebugState *s = STMP3770_UARTDBG(dev);

    qemu_chr_fe_set_handlers(&s->chr, uartdbg_can_receive, uartdbg_receive,
                             uartdbg_event, NULL, s, NULL, true);
}

static void uartdbg_instance_finalize(Object *obj)
{
    STMP3770UARTDebugState *s = STMP3770_UARTDBG(obj);

    if (s->tx_baud_timer) {
        timer_free(s->tx_baud_timer);
        s->tx_baud_timer = NULL;
    }
}

static const VMStateDescription vmstate_uartdbg = {
    .name = "stmp3770-uartdbg",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(rsr, STMP3770UARTDebugState),
        VMSTATE_UINT32(ibrd, STMP3770UARTDebugState),
        VMSTATE_UINT32(fbrd, STMP3770UARTDebugState),
        VMSTATE_UINT32(lcr, STMP3770UARTDebugState),
        VMSTATE_UINT32(cr, STMP3770UARTDebugState),
        VMSTATE_UINT32(ifls, STMP3770UARTDebugState),
        VMSTATE_UINT32(imsc, STMP3770UARTDebugState),
        VMSTATE_UINT32(ris, STMP3770UARTDebugState),
        VMSTATE_UINT32(dmacr, STMP3770UARTDebugState),
        VMSTATE_UINT16_ARRAY(rx_fifo, STMP3770UARTDebugState,
                             UARTDBG_FIFO_DEPTH),
        VMSTATE_UINT8(rx_pos, STMP3770UARTDebugState),
        VMSTATE_UINT8(rx_count, STMP3770UARTDebugState),
        VMSTATE_UINT8(tx_count, STMP3770UARTDebugState),
        VMSTATE_UINT8(tx_byte, STMP3770UARTDebugState),
        VMSTATE_BOOL(tx_baud_timer_active, STMP3770UARTDebugState),
        VMSTATE_TIMER_PTR(tx_baud_timer, STMP3770UARTDebugState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property uartdbg_properties[] = {
    DEFINE_PROP_CHR("chardev", STMP3770UARTDebugState, chr),
};

static void uartdbg_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = uartdbg_realize;
    device_class_set_legacy_reset(dc, uartdbg_reset);
    dc->vmsd = &vmstate_uartdbg;
    device_class_set_props(dc, uartdbg_properties);
}

static const TypeInfo uartdbg_type_info = {
    .name = TYPE_STMP3770_UARTDBG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STMP3770UARTDebugState),
    .instance_init = uartdbg_init,
    .instance_finalize = uartdbg_instance_finalize,
    .class_init = uartdbg_class_init,
};

static void uartdbg_register_types(void)
{
    type_register_static(&uartdbg_type_info);
}

type_init(uartdbg_register_types)
