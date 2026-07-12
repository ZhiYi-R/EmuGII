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

#ifndef STMP3770_UARTAPP_H
#define STMP3770_UARTAPP_H

#include "chardev/char-fe.h"
#include "hw/sysbus.h"

#define TYPE_STMP3770_UARTAPP "stmp3770-uartapp"
OBJECT_DECLARE_SIMPLE_TYPE(STMP3770UARTAppState, STMP3770_UARTAPP)

#define UARTAPP_FIFO_DEPTH 16

struct STMP3770UARTAppState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    CharBackend chr;

    uint32_t ctrl0;
    uint32_t ctrl1;
    uint32_t ctrl2;
    uint32_t linectrl;
    uint32_t linectrl2;
    uint32_t intr;
    uint32_t data;
    uint32_t stat_writable;

    uint32_t intr_status;

    uint8_t tx_fifo[UARTAPP_FIFO_DEPTH];
    uint8_t rx_fifo[UARTAPP_FIFO_DEPTH];
    uint8_t tx_count;
    uint8_t tx_rptr;
    uint8_t tx_wptr;
    uint8_t rx_count;
    uint8_t rx_rptr;
    uint8_t rx_wptr;

    bool overrun;

    uint32_t tx_xfer_count;
    uint32_t rx_xfer_count;
    bool tx_dma_active;
    bool rx_dma_active;
    bool tx_dma_wait4endcmd;
    bool rx_dma_wait4endcmd;

    struct STMP3770DMAState *dma;
    int tx_dma_channel;
    int rx_dma_channel;
};

void stmp3770_uartapp_set_dma(STMP3770UARTAppState *s,
                              struct STMP3770DMAState *dma,
                              int tx_channel, int rx_channel);

#endif
