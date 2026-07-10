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

#include "hw/sysbus.h"

#define TYPE_STMP3770_UARTAPP "stmp3770-uartapp"
OBJECT_DECLARE_SIMPLE_TYPE(STMP3770UARTAppState, STMP3770_UARTAPP)

struct STMP3770UARTAppState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t ctrl0;
    uint32_t ctrl1;
    uint32_t ctrl2;
    uint32_t linectrl;
    uint32_t linectrl2;
    uint32_t intr;
    uint32_t data;
    uint32_t stat_writable;
};

#endif
