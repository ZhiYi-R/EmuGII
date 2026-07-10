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

#ifndef STMP3770_UARTDBG_H
#define STMP3770_UARTDBG_H

#include "chardev/char-fe.h"
#include "hw/sysbus.h"

#define TYPE_STMP3770_UARTDBG "stmp3770-uartdbg"
OBJECT_DECLARE_SIMPLE_TYPE(STMP3770UARTDebugState, STMP3770_UARTDBG)

struct STMP3770UARTDebugState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    CharBackend chr;

    uint32_t rsr;
    uint32_t ibrd;
    uint32_t fbrd;
    uint32_t lcr;
    uint32_t cr;
    uint32_t ifls;
    uint32_t imsc;
    uint32_t ris;
    uint32_t dmacr;
    uint16_t rx_fifo[16];
    uint8_t rx_pos;
    uint8_t rx_count;
};

#endif
