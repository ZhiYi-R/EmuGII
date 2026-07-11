/*
 * STMP3770 Audio DAC/ADC emulation
 *
 * Based on STMP3770 Reference Manual Chapters 16 (DAC) and 17 (ADC)
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
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "audio/audio.h"
#include "hw/audio/stmp3770_audio.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define AUDIO_VERSION   0x01010000

#define REG_CTRL0       0x000
#define REG_CTRL0_SET   0x004
#define REG_CTRL0_CLR   0x008
#define REG_CTRL0_TOG   0x00C
#define REG_STAT        0x010
#define REG_VERSION     0x200

#define CTRL0_SFTRST    (1U << 31)
#define CTRL0_CLKGATE   (1U << 30)
#define CTRL0_RUN       (1U << 0)

#define STAT_PRESENT    (1U << 31)

#define SAMPLE_RATE     44100
#define SAMPLE_FORMAT   AUDIO_FORMAT_S16
#define SAMPLE_CHANNELS 2

static inline bool audio_enabled(uint32_t ctrl0)
{
    return (ctrl0 & (CTRL0_SFTRST | CTRL0_CLKGATE)) == 0 &&
           (ctrl0 & CTRL0_RUN) != 0;
}

static void audio_apply_sct(uint32_t *reg, uint32_t value, int sct)
{
    switch (sct) {
    case 0:
        *reg = value;
        break;
    case 1:
        *reg |= value;
        break;
    case 2:
        *reg &= ~value;
        break;
    case 3:
        *reg ^= value;
        break;
    }
}

/* ======================================================================== */
/* Audio DAC                                                                */
/* ======================================================================== */

static uint64_t audio_dac_read(void *opaque, hwaddr offset, unsigned size)
{
    STMP3770AudioDACState *s = opaque;

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-audio-dac: unsupported read size %u at offset "
                      HWADDR_FMT_plx "\n", size, offset);
        return 0;
    }

    switch (offset) {
    case REG_CTRL0:
        return s->ctrl0;
    case REG_STAT:
        return STAT_PRESENT;
    case REG_VERSION:
        return AUDIO_VERSION;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-audio-dac: read from unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        return 0;
    }
}

static void audio_dac_write(void *opaque, hwaddr offset,
                            uint64_t value, unsigned size)
{
    STMP3770AudioDACState *s = opaque;
    int sct = (offset >> 2) & 3;

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-audio-dac: unsupported write size %u at offset "
                      HWADDR_FMT_plx "\n", size, offset);
        return;
    }

    switch (offset & ~0xC) {
    case REG_CTRL0:
        audio_apply_sct(&s->ctrl0, (uint32_t)value, sct);
        if (s->voice) {
            AUD_set_active_out(s->voice, audio_enabled(s->ctrl0));
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-audio-dac: write to unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        break;
    }
}

static const MemoryRegionOps audio_dac_ops = {
    .read = audio_dac_read,
    .write = audio_dac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static int stmp3770_audio_dac_dma_handler(STMP3770DMAState *dma,
                                          int channel, STMP3770DMAEvent event,
                                          void *buf, size_t len, void *opaque)
{
    STMP3770AudioDACState *s = opaque;

    if (event == STMP3770_DMA_EVENT_DATA_WRITE && s->voice && len > 0) {
        size_t written = AUD_write(s->voice, buf, len);
        return (int)written;
    }

    return 0;
}

void stmp3770_audio_dac_set_dma(STMP3770AudioDACState *s,
                                STMP3770DMAState *dma, int channel)
{
    if (!dma) {
        return;
    }
    stmp3770_dma_set_channel_handler(dma, channel,
                                     stmp3770_audio_dac_dma_handler, s);
}

static void audio_dac_reset(DeviceState *dev)
{
    STMP3770AudioDACState *s = STMP3770_AUDIO_DAC(dev);

    s->ctrl0 = CTRL0_SFTRST | CTRL0_CLKGATE;
}

static void audio_dac_realize(DeviceState *dev, Error **errp)
{
    STMP3770AudioDACState *s = STMP3770_AUDIO_DAC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    struct audsettings as = {
        .freq = SAMPLE_RATE,
        .nchannels = SAMPLE_CHANNELS,
        .fmt = SAMPLE_FORMAT,
        .endianness = 0,
    };

    memory_region_init_io(&s->iomem, OBJECT(dev), &audio_dac_ops, s,
                          TYPE_STMP3770_AUDIO_DAC, 0x2000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    if (s->card.state && AUD_register_card(TYPE_STMP3770_AUDIO_DAC,
                                             &s->card, errp)) {
        s->voice = AUD_open_out(&s->card, NULL, "stmp3770-audio-dac", s,
                                 NULL, &as);
        if (!s->voice) {
            AUD_log(TYPE_STMP3770_AUDIO_DAC, "Could not open DAC voice\n");
        }
    }
}

static const VMStateDescription vmstate_audio_dac = {
    .name = "stmp3770-audio-dac",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl0, STMP3770AudioDACState),
        VMSTATE_END_OF_LIST()
    }
};

static void audio_dac_init(Object *obj)
{
    STMP3770AudioDACState *s = STMP3770_AUDIO_DAC(obj);

    s->ctrl0 = CTRL0_SFTRST | CTRL0_CLKGATE;
}

static const Property audio_dac_properties[] = {
    DEFINE_AUDIO_PROPERTIES(STMP3770AudioDACState, card),
};

static void audio_dac_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = audio_dac_realize;
    device_class_set_legacy_reset(dc, audio_dac_reset);
    dc->vmsd = &vmstate_audio_dac;
    device_class_set_props(dc, audio_dac_properties);
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
}

static const TypeInfo audio_dac_type_info = {
    .name          = TYPE_STMP3770_AUDIO_DAC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STMP3770AudioDACState),
    .instance_init = audio_dac_init,
    .class_init    = audio_dac_class_init,
};

/* ======================================================================== */
/* Audio ADC                                                                */
/* ======================================================================== */

static uint64_t audio_adc_read(void *opaque, hwaddr offset, unsigned size)
{
    STMP3770AudioADCState *s = opaque;

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-audio-adc: unsupported read size %u at offset "
                      HWADDR_FMT_plx "\n", size, offset);
        return 0;
    }

    switch (offset) {
    case REG_CTRL0:
        return s->ctrl0;
    case REG_STAT:
        return STAT_PRESENT;
    case REG_VERSION:
        return AUDIO_VERSION;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-audio-adc: read from unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        return 0;
    }
}

static void audio_adc_write(void *opaque, hwaddr offset,
                            uint64_t value, unsigned size)
{
    STMP3770AudioADCState *s = opaque;
    int sct = (offset >> 2) & 3;

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-audio-adc: unsupported write size %u at offset "
                      HWADDR_FMT_plx "\n", size, offset);
        return;
    }

    switch (offset & ~0xC) {
    case REG_CTRL0:
        audio_apply_sct(&s->ctrl0, (uint32_t)value, sct);
        if (s->voice) {
            AUD_set_active_in(s->voice, audio_enabled(s->ctrl0));
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-audio-adc: write to unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        break;
    }
}

static const MemoryRegionOps audio_adc_ops = {
    .read = audio_adc_read,
    .write = audio_adc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static int stmp3770_audio_adc_dma_handler(STMP3770DMAState *dma,
                                          int channel, STMP3770DMAEvent event,
                                          void *buf, size_t len, void *opaque)
{
    STMP3770AudioADCState *s = opaque;

    if (event == STMP3770_DMA_EVENT_DATA_READ && len > 0) {
        if (s->voice) {
            size_t n = AUD_read(s->voice, buf, len);
            if (n < len) {
                memset((uint8_t *)buf + n, 0, len - n);
            }
            return (int)len;
        }
        memset(buf, 0, len);
        return (int)len;
    }

    return 0;
}

void stmp3770_audio_adc_set_dma(STMP3770AudioADCState *s,
                                STMP3770DMAState *dma, int channel)
{
    if (!dma) {
        return;
    }
    stmp3770_dma_set_channel_handler(dma, channel,
                                     stmp3770_audio_adc_dma_handler, s);
}

static void audio_adc_reset(DeviceState *dev)
{
    STMP3770AudioADCState *s = STMP3770_AUDIO_ADC(dev);

    s->ctrl0 = CTRL0_SFTRST | CTRL0_CLKGATE;
}

static void audio_adc_realize(DeviceState *dev, Error **errp)
{
    STMP3770AudioADCState *s = STMP3770_AUDIO_ADC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    struct audsettings as = {
        .freq = SAMPLE_RATE,
        .nchannels = SAMPLE_CHANNELS,
        .fmt = SAMPLE_FORMAT,
        .endianness = 0,
    };

    memory_region_init_io(&s->iomem, OBJECT(dev), &audio_adc_ops, s,
                          TYPE_STMP3770_AUDIO_ADC, 0x2000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    if (s->card.state && AUD_register_card(TYPE_STMP3770_AUDIO_ADC,
                                             &s->card, errp)) {
        s->voice = AUD_open_in(&s->card, NULL, "stmp3770-audio-adc", s,
                                NULL, &as);
        if (!s->voice) {
            AUD_log(TYPE_STMP3770_AUDIO_ADC, "Could not open ADC voice\n");
        }
    }
}

static const VMStateDescription vmstate_audio_adc = {
    .name = "stmp3770-audio-adc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl0, STMP3770AudioADCState),
        VMSTATE_END_OF_LIST()
    }
};

static void audio_adc_init(Object *obj)
{
    STMP3770AudioADCState *s = STMP3770_AUDIO_ADC(obj);

    s->ctrl0 = CTRL0_SFTRST | CTRL0_CLKGATE;
}

static const Property audio_adc_properties[] = {
    DEFINE_AUDIO_PROPERTIES(STMP3770AudioADCState, card),
};

static void audio_adc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = audio_adc_realize;
    device_class_set_legacy_reset(dc, audio_adc_reset);
    dc->vmsd = &vmstate_audio_adc;
    device_class_set_props(dc, audio_adc_properties);
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
}

static const TypeInfo audio_adc_type_info = {
    .name          = TYPE_STMP3770_AUDIO_ADC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STMP3770AudioADCState),
    .instance_init = audio_adc_init,
    .class_init    = audio_adc_class_init,
};

static void stmp3770_audio_register_types(void)
{
    type_register_static(&audio_dac_type_info);
    type_register_static(&audio_adc_type_info);
}

type_init(stmp3770_audio_register_types)
