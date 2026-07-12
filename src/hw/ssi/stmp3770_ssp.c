/*
 * STMP3770 SSP (Synchronous Serial Port) controller
 *
 * Based on STMP3770 Reference Manual Chapter 16
 *
 * Features:
 * - Minimal register stub sufficient for firmware probing
 * - Reports FIFO empty and not busy
 * - Supports SET/CLR/TOG aliases
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
#include "hw/ssi/stmp3770_ssp.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"

/* SET/CLR/TOG sub-offsets */
#define SCT_SET 0x4
#define SCT_CLR 0x8
#define SCT_TOG 0xC

/* HW_SSP_TIMING bit fields */
#define SSP_TIMING_TIMEOUT_MASK 0xFFFF0000U
#define SSP_TIMING_CLOCK_DIVIDE_SHIFT 8
#define SSP_TIMING_CLOCK_DIVIDE_MASK 0x0000FF00U
#define SSP_TIMING_CLOCK_RATE_MASK 0x000000FFU

/* SPI receive timeout: 128 HCLK cycles (Reference Manual Table 775) */
#define SSP_RECV_TIMEOUT_HCLK 128

static inline bool stmp3770_ssp_enabled(STMP3770SSPState *s)
{
    return (s->ctrl0 & (SSP_CTRL0_SFTRST | SSP_CTRL0_CLKGATE)) == 0;
}

static void stmp3770_ssp_apply_sct(uint32_t *reg, uint32_t value, int sct)
{
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
        *reg = value;
        break;
    }
}

static void stmp3770_ssp_write_timing(STMP3770SSPState *s, uint32_t value, int sct)
{
    uint32_t old = s->timing;
    uint32_t divide;

    stmp3770_ssp_apply_sct(&s->timing, value, sct);

    divide = (s->timing & SSP_TIMING_CLOCK_DIVIDE_MASK) >>
             SSP_TIMING_CLOCK_DIVIDE_SHIFT;
    if (divide != 0 && (divide < 2 || divide > 254 || (divide & 1))) {
        s->timing = (s->timing & ~SSP_TIMING_CLOCK_DIVIDE_MASK) |
                    (old & SSP_TIMING_CLOCK_DIVIDE_MASK);
    }
}

static void stmp3770_ssp_update_irq(STMP3770SSPState *s)
{
    uint32_t pending = (s->ctrl1 & SSP_CTRL1_ERROR_IRQ_STATUS_MASK) >> 1;

    qemu_set_irq(s->irq_error,
                 (pending & s->ctrl1 & SSP_CTRL1_ERROR_IRQ_ENABLE_MASK) != 0);
}

static unsigned int stmp3770_ssp_bytes_per_word(STMP3770SSPState *s)
{
    unsigned int word_length = (s->ctrl1 >> 4) & 0xFU;
    unsigned int bytes_per_word = (word_length + 7U) / 8U;

    return bytes_per_word ? bytes_per_word : 1;
}

static void stmp3770_ssp_cancel_recv_timeout(STMP3770SSPState *s)
{
    timer_del(s->recv_timeout_timer);
}

static void stmp3770_ssp_start_recv_timeout(STMP3770SSPState *s)
{
    if (s->hclk_hz == 0 || s->fifo_count != 1 ||
        !(s->ctrl0 & SSP_CTRL0_RUN) || !stmp3770_ssp_enabled(s) ||
        (s->status & SSP_STATUS_RECV_TIMEOUT_STAT)) {
        return;
    }

    uint64_t ns = ((uint64_t)SSP_RECV_TIMEOUT_HCLK * NANOSECONDS_PER_SECOND +
                   s->hclk_hz - 1) / s->hclk_hz;

    timer_mod(s->recv_timeout_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);
}

static void stmp3770_ssp_recv_timeout_expired(void *opaque)
{
    STMP3770SSPState *s = STMP3770_SSP(opaque);

    if (s->fifo_count != 1 || !(s->ctrl0 & SSP_CTRL0_RUN) ||
        !stmp3770_ssp_enabled(s) ||
        (s->status & SSP_STATUS_RECV_TIMEOUT_STAT)) {
        return;
    }

    s->status |= SSP_STATUS_RECV_TIMEOUT_STAT;
    s->ctrl1 |= SSP_CTRL1_RECV_TIMEOUT_IRQ;
    stmp3770_ssp_update_irq(s);
}

/* DEBUG/DMA state machine values */
#define SSP_DEBUG_CSM_INDEX  (1U << 10)
#define SSP_DEBUG_MMC_CMD    (1U << 12)
#define SSP_DEBUG_MMC_RESP   (3U << 12)
#define SSP_DEBUG_MMC_TX     (5U << 12)
#define SSP_DEBUG_MMC_RX     (7U << 12)
#define SSP_DEBUG_DMA_IDLE   (0U << 16)
#define SSP_DEBUG_DMA_BUSY   (4U << 16)
#define SSP_DEBUG_DMA_DONE   (5U << 16)
#define SSP_DEBUG_CMD_OE     (1U << 19)
#define SSP_DEBUG_MSTK_TPC   (3U << 20)
#define SSP_DEBUG_MSTK_RW    (7U << 20)
#define SSP_DEBUG_DSM_WORD   (2U << 24)

static void stmp3770_ssp_update_debug(STMP3770SSPState *s)
{
    uint32_t debug = 0;
    bool run = s->ctrl0 & SSP_CTRL0_RUN;
    bool enable = s->ctrl0 & SSP_CTRL0_ENABLE;
    bool data_xfer = s->ctrl0 & SSP_CTRL0_DATA_XFER;
    bool dma_enable = s->ctrl1 & SSP_CTRL1_DMA_ENABLE;
    unsigned int ssp_mode = s->ctrl1 & SSP_CTRL1_SSP_MODE_MASK;
    bool is_sd_mmc = (ssp_mode == 0x3U || ssp_mode == 0x7U);
    bool is_ms = (ssp_mode == 0x4U);

    if (run && enable && (is_sd_mmc || is_ms)) {
        debug |= SSP_DEBUG_CSM_INDEX;
        debug |= SSP_DEBUG_CMD_OE;
    }

    if (is_sd_mmc && run && enable) {
        if (data_xfer) {
            if (s->ctrl0 & SSP_CTRL0_READ) {
                debug |= SSP_DEBUG_MMC_RX;
            } else {
                debug |= SSP_DEBUG_MMC_TX;
            }
        } else if (s->ctrl0 & SSP_CTRL0_GET_RESP) {
            debug |= SSP_DEBUG_MMC_RESP;
        } else {
            debug |= SSP_DEBUG_MMC_CMD;
        }
    }

    if (is_ms && run && enable) {
        if (data_xfer) {
            debug |= SSP_DEBUG_MSTK_RW;
        } else {
            debug |= SSP_DEBUG_MSTK_TPC;
        }
    }

    if ((is_sd_mmc || is_ms) && run && data_xfer) {
        debug |= SSP_DEBUG_DSM_WORD;
        if (s->fifo_count == 0 && s->words_remaining > 0) {
            debug |= (1U << 27); /* DATA_STALL */
        }
    }

    if (dma_enable) {
        if (run && s->words_remaining > 0) {
            debug |= SSP_DEBUG_DMA_BUSY;
        } else {
            debug |= SSP_DEBUG_DMA_DONE;
        }
    } else {
        debug |= SSP_DEBUG_DMA_IDLE;
    }

    s->debug = debug;
}

static void stmp3770_ssp_update_dma_status(STMP3770SSPState *s)
{
    uint32_t dma_bits = 0;
    bool run = s->ctrl0 & SSP_CTRL0_RUN;
    bool dma_enable = s->ctrl1 & SSP_CTRL1_DMA_ENABLE;

    if (dma_enable) {
        if (run) {
            dma_bits |= SSP_STATUS_DMAREQ;
        } else {
            dma_bits |= (SSP_STATUS_DMAEND | SSP_STATUS_DMATERM);
        }
    }

    s->status &= ~(SSP_STATUS_DMASENSE | SSP_STATUS_DMATERM |
                   SSP_STATUS_DMAREQ | SSP_STATUS_DMAEND);
    s->status |= dma_bits;
}

static void stmp3770_ssp_fifo_set_empty(STMP3770SSPState *s)
{
    s->fifo_count = 0;
    s->status |= SSP_STATUS_FIFO_EMPTY;
    s->status &= ~SSP_STATUS_FIFO_FULL;
    s->status &= ~SSP_STATUS_RECV_TIMEOUT_STAT;
    stmp3770_ssp_cancel_recv_timeout(s);
}

static void stmp3770_ssp_fifo_load(STMP3770SSPState *s)
{
    if (s->fifo_count) {
        s->status |= SSP_STATUS_FIFO_OVRFLW;
        s->ctrl1 |= SSP_CTRL1_FIFO_OVERRUN_IRQ;
        stmp3770_ssp_update_irq(s);
    }

    s->fifo_count = 1;
    s->status |= SSP_STATUS_FIFO_FULL;
    s->status &= ~SSP_STATUS_FIFO_EMPTY;
}

static void stmp3770_ssp_complete_data_word(STMP3770SSPState *s)
{
    if (s->words_remaining == 0) {
        return;
    }

    s->words_remaining--;

    stmp3770_ssp_fifo_set_empty(s);

    if (s->words_remaining == 0) {
        s->ctrl0 &= ~SSP_CTRL0_RUN;
        s->status &= ~(SSP_STATUS_BUSY | SSP_STATUS_CMD_BUSY |
                       SSP_STATUS_DATA_BUSY);
    }

    stmp3770_ssp_update_debug(s);
    stmp3770_ssp_update_dma_status(s);
}

static bool stmp3770_ssp_has_sct_alias(hwaddr offset)
{
    return offset == SSP_CTRL0 || offset == SSP_CMD0 || offset == SSP_CTRL1;
}

static void stmp3770_ssp_reset(DeviceState *dev)
{
    STMP3770SSPState *s = STMP3770_SSP(dev);

    s->ctrl0 = SSP_CTRL0_RESET_VALUE;
    s->ctrl1 = SSP_CTRL1_RESET_VALUE;
    s->words_remaining = 0;
    s->cmd0 = 0;
    s->cmd1 = 0;
    s->compref = 0;
    s->compmask = 0;
    s->timing = 0;
    s->data = 0;
    s->sdresp[0] = 0;
    s->sdresp[1] = 0;
    s->sdresp[2] = 0;
    s->sdresp[3] = 0;
    s->status = SSP_STATUS_RESET_VALUE;
    s->debug = 0;
    s->sspclk_rate = 0;
    s->fifo_count = 0;

    stmp3770_ssp_cancel_recv_timeout(s);
    stmp3770_ssp_update_debug(s);
    stmp3770_ssp_update_dma_status(s);
    stmp3770_ssp_update_irq(s);
}

static uint64_t stmp3770_ssp_read(void *opaque, hwaddr offset, unsigned size)
{
    STMP3770SSPState *s = STMP3770_SSP(opaque);
    hwaddr reg = offset & ~0xFULL;

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-ssp: unsupported read size %u at offset "
                      HWADDR_FMT_plx "\n", size, offset);
        return 0;
    }

    if ((offset & 0xFULL) && stmp3770_ssp_has_sct_alias(reg)) {
        return 0;
    }

    switch (offset) {
    case SSP_CTRL0:
        return s->ctrl0;
    case SSP_CTRL1:
        return s->ctrl1;
    case SSP_CMD0:
        return s->cmd0;
    case SSP_CMD1:
        return s->cmd1;
    case SSP_COMPREF:
        return s->compref;
    case SSP_COMPMASK:
        return s->compmask;
    case SSP_TIMING:
        return s->timing;
    case SSP_DATA:
        if ((s->ctrl0 & SSP_CTRL0_RUN) && stmp3770_ssp_enabled(s) &&
            s->fifo_count == 0) {
            s->status |= SSP_STATUS_FIFO_UNDRFLW;
            s->ctrl1 |= SSP_CTRL1_FIFO_UNDERRUN_IRQ;
            stmp3770_ssp_update_irq(s);
        }
        if ((s->ctrl0 & SSP_CTRL0_RUN) && stmp3770_ssp_enabled(s)) {
            stmp3770_ssp_complete_data_word(s);
        }
        return s->data;
    case SSP_SDRESP0:
        return s->sdresp[0];
    case SSP_SDRESP1:
        return s->sdresp[1];
    case SSP_SDRESP2:
        return s->sdresp[2];
    case SSP_SDRESP3:
        return s->sdresp[3];
    case SSP_STATUS:
        return s->status;
    case SSP_DEBUG:
        stmp3770_ssp_update_debug(s);
        return s->debug;
    case SSP_VERSION:
        return SSP_VERSION_VALUE;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-ssp: read from unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        return 0;
    }
}

static void stmp3770_ssp_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    STMP3770SSPState *s = STMP3770_SSP(opaque);
    int sct = offset & 0xF;
    hwaddr reg = offset & ~0xFULL;

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-ssp: unsupported write size %u at offset "
                      HWADDR_FMT_plx "\n", size, offset);
        return;
    }

    if (sct && !stmp3770_ssp_has_sct_alias(reg)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-ssp: register has no SCT alias at "
                      HWADDR_FMT_plx "\n", offset);
        return;
    }
    if (reg != SSP_CTRL0 && (s->ctrl0 & SSP_CTRL0_SFTRST)) {
        return;
    }

    switch (reg) {
    case SSP_CTRL0:
    {
        uint32_t old_ctrl0 = s->ctrl0;

        stmp3770_ssp_apply_sct(&s->ctrl0, (uint32_t)value, sct);
        if (s->ctrl0 & SSP_CTRL0_SFTRST) {
            stmp3770_ssp_reset(DEVICE(s));
        } else if (!(old_ctrl0 & SSP_CTRL0_RUN) &&
                   (s->ctrl0 & SSP_CTRL0_RUN)) {
            s->words_remaining = s->ctrl0 & 0xffffU;
            if (s->words_remaining == 0) {
                s->ctrl0 &= ~SSP_CTRL0_RUN;
            } else {
                s->status = SSP_STATUS_PRESENT | SSP_STATUS_MS_PRESENT |
                            SSP_STATUS_SD_PRESENT |
                            SSP_STATUS_BUSY | SSP_STATUS_CMD_BUSY |
                            SSP_STATUS_DATA_BUSY;
                if (s->fifo_count) {
                    s->status |= SSP_STATUS_FIFO_FULL;
                } else {
                    s->status |= SSP_STATUS_FIFO_EMPTY;
                }
                stmp3770_ssp_start_recv_timeout(s);
            }
        } else if ((old_ctrl0 & SSP_CTRL0_RUN) &&
                   !(s->ctrl0 & SSP_CTRL0_RUN)) {
            s->status &= ~(SSP_STATUS_BUSY | SSP_STATUS_CMD_BUSY |
                           SSP_STATUS_DATA_BUSY);
            stmp3770_ssp_cancel_recv_timeout(s);
        }

        stmp3770_ssp_update_debug(s);
        stmp3770_ssp_update_dma_status(s);
        break;
    }
    case SSP_CTRL1:
    {
        uint32_t old_ctrl1 = s->ctrl1;

        stmp3770_ssp_apply_sct(&s->ctrl1, (uint32_t)value, sct);
        if (s->ctrl0 & SSP_CTRL0_RUN) {
            s->ctrl1 = (s->ctrl1 & ~0xFFU) | (old_ctrl1 & 0xFFU);
        }
        stmp3770_ssp_update_irq(s);
        stmp3770_ssp_update_debug(s);
        stmp3770_ssp_update_dma_status(s);
        break;
    }
    case SSP_CMD0:
        stmp3770_ssp_apply_sct(&s->cmd0, (uint32_t)value, sct);
        s->cmd0 &= SSP_CMD0_WRITABLE_MASK;
        break;
    case SSP_CMD1:
        stmp3770_ssp_apply_sct(&s->cmd1, (uint32_t)value, sct);
        break;
    case SSP_COMPREF:
        stmp3770_ssp_apply_sct(&s->compref, (uint32_t)value, sct);
        break;
    case SSP_COMPMASK:
        stmp3770_ssp_apply_sct(&s->compmask, (uint32_t)value, sct);
        break;
    case SSP_TIMING:
        stmp3770_ssp_write_timing(s, (uint32_t)value, sct);
        break;
    case SSP_DATA:
        s->data = (uint32_t)value;
        if (stmp3770_ssp_enabled(s)) {
            stmp3770_ssp_fifo_load(s);
            if ((s->ctrl0 & SSP_CTRL0_RUN) && stmp3770_ssp_enabled(s)) {
                stmp3770_ssp_complete_data_word(s);
            }
        }
        break;
    case SSP_SDRESP0:
    case SSP_SDRESP1:
    case SSP_SDRESP2:
    case SSP_SDRESP3:
    case SSP_STATUS:
    case SSP_DEBUG:
    case SSP_VERSION:
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-ssp: write to unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        break;
    }
}

static const MemoryRegionOps stmp3770_ssp_ops = {
    .read = stmp3770_ssp_read,
    .write = stmp3770_ssp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void stmp3770_ssp_init(Object *obj)
{
    STMP3770SSPState *s = STMP3770_SSP(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->sspclk_rate = 0;
    s->hclk_hz = 0;
    s->recv_timeout_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                         stmp3770_ssp_recv_timeout_expired, s);

    memory_region_init_io(&s->iomem, obj, &stmp3770_ssp_ops, s,
                          TYPE_STMP3770_SSP, 0x2000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq_dma);
    sysbus_init_irq(sbd, &s->irq_error);
}

static void stmp3770_ssp_finalize(Object *obj)
{
    STMP3770SSPState *s = STMP3770_SSP(obj);

    timer_free(s->recv_timeout_timer);
}

static int stmp3770_ssp_post_load(void *opaque, int version_id)
{
    STMP3770SSPState *s = STMP3770_SSP(opaque);

    if (version_id < 2 && (s->ctrl0 & SSP_CTRL0_RUN)) {
        s->words_remaining = s->ctrl0 & 0xffffU;
    }
    if (version_id < 4) {
        s->fifo_count = (s->status & SSP_STATUS_FIFO_FULL) ? 1 : 0;
    }
    stmp3770_ssp_start_recv_timeout(s);
    stmp3770_ssp_update_debug(s);
    stmp3770_ssp_update_dma_status(s);
    stmp3770_ssp_update_irq(s);

    return 0;
}

static const VMStateDescription vmstate_stmp3770_ssp = {
    .name = "stmp3770-ssp",
    .version_id = 5,
    .minimum_version_id = 1,
    .post_load = stmp3770_ssp_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl0, STMP3770SSPState),
        VMSTATE_UINT32(ctrl1, STMP3770SSPState),
        VMSTATE_UINT32(cmd0, STMP3770SSPState),
        VMSTATE_UINT32(cmd1, STMP3770SSPState),
        VMSTATE_UINT32(compref, STMP3770SSPState),
        VMSTATE_UINT32(compmask, STMP3770SSPState),
        VMSTATE_UINT32(timing, STMP3770SSPState),
        VMSTATE_UINT32(data, STMP3770SSPState),
        VMSTATE_UINT32_ARRAY(sdresp, STMP3770SSPState, 4),
        VMSTATE_UINT32(status, STMP3770SSPState),
        VMSTATE_UINT32(debug, STMP3770SSPState),
        VMSTATE_UINT32_V(words_remaining, STMP3770SSPState, 2),
        VMSTATE_UINT32_V(sspclk_rate, STMP3770SSPState, 3),
        VMSTATE_UINT8_V(fifo_count, STMP3770SSPState, 4),
        VMSTATE_UINT32_V(hclk_hz, STMP3770SSPState, 5),
        VMSTATE_TIMER_PTR(recv_timeout_timer, STMP3770SSPState),
        VMSTATE_END_OF_LIST()
    }
};

static void stmp3770_ssp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, stmp3770_ssp_reset);
    dc->vmsd = &vmstate_stmp3770_ssp;
}

static const TypeInfo stmp3770_ssp_type_info = {
    .name          = TYPE_STMP3770_SSP,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STMP3770SSPState),
    .instance_init = stmp3770_ssp_init,
    .instance_finalize = stmp3770_ssp_finalize,
    .class_init    = stmp3770_ssp_class_init,
};

static void stmp3770_ssp_dma_load_word(STMP3770SSPState *s,
                                        const uint8_t *src,
                                        unsigned int bytes_per_word)
{
    uint32_t word = 0;
    unsigned int b;

    for (b = 0; b < bytes_per_word; b++) {
        word |= (uint32_t)src[b] << (b * 8);
    }
    s->data = word;
}

static void stmp3770_ssp_dma_store_word(STMP3770SSPState *s, uint8_t *dst,
                                        unsigned int bytes_per_word)
{
    uint32_t word = s->data;
    unsigned int b;

    for (b = 0; b < bytes_per_word; b++) {
        dst[b] = (word >> (b * 8)) & 0xff;
    }
}

static int stmp3770_ssp_dma_handler(STMP3770DMAState *dma, int channel,
                                    STMP3770DMAEvent event, void *buf,
                                    size_t len, void *opaque)
{
    STMP3770SSPState *s = STMP3770_SSP(opaque);
    unsigned int bytes_per_word = stmp3770_ssp_bytes_per_word(s);
    size_t i;

    if (event == STMP3770_DMA_EVENT_PIO) {
        uint32_t *pio = (uint32_t *)buf;

        if (len >= sizeof(uint32_t)) {
            stmp3770_ssp_write(s, SSP_CTRL0, pio[0], 4);
        }
        return (int)len;
    }

    if (event == STMP3770_DMA_EVENT_DATA_WRITE) {
        const uint8_t *src = (const uint8_t *)buf;

        for (i = 0; i + bytes_per_word <= len; i += bytes_per_word) {
            if (s->words_remaining == 0) {
                break;
            }
            stmp3770_ssp_dma_load_word(s, &src[i], bytes_per_word);
            stmp3770_ssp_fifo_load(s);
            stmp3770_ssp_complete_data_word(s);
            if (!(s->ctrl0 & SSP_CTRL0_RUN)) {
                i += bytes_per_word;
                break;
            }
        }
        return (int)i;
    }

    if (event == STMP3770_DMA_EVENT_DATA_READ) {
        uint8_t *dst = (uint8_t *)buf;

        for (i = 0; i + bytes_per_word <= len; i += bytes_per_word) {
            if (s->words_remaining == 0) {
                break;
            }
            stmp3770_ssp_dma_store_word(s, &dst[i], bytes_per_word);
            stmp3770_ssp_complete_data_word(s);
            if (!(s->ctrl0 & SSP_CTRL0_RUN)) {
                i += bytes_per_word;
                break;
            }
        }
        return (int)i;
    }

    return 0;
}

void stmp3770_ssp_set_clk_rate(STMP3770SSPState *s, uint32_t sspclk_hz)
{
    s->sspclk_rate = sspclk_hz;
}

void stmp3770_ssp_set_hclk_rate(STMP3770SSPState *s, uint32_t hclk_hz)
{
    s->hclk_hz = hclk_hz;
    stmp3770_ssp_cancel_recv_timeout(s);
    stmp3770_ssp_start_recv_timeout(s);
}

void stmp3770_ssp_set_dma(STMP3770SSPState *s, STMP3770DMAState *dma,
                          int channel)
{
    s->dma = dma;
    s->dma_channel = channel;

    if (!dma) {
        return;
    }

    stmp3770_dma_set_channel_handler(dma, channel, stmp3770_ssp_dma_handler,
                                     s);
}

static void stmp3770_ssp_register_types(void)
{
    type_register_static(&stmp3770_ssp_type_info);
}

type_init(stmp3770_ssp_register_types)
