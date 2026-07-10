/*
 * SigmaTel STMP3770 SoC emulation
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
#include "qapi/error.h"
#include "hw/arm/stmp3770.h"
#include "hw/qdev-properties.h"
#include "hw/boards.h"
#include "hw/char/pl011.h"
#include "hw/misc/unimp.h"
#include "chardev/char-fe.h"
#include "system/system.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "hw/qdev-core.h"
#include "target/arm/cpu-qom.h"
#include "qemu/module.h"

#define DFLPT_NUM_MPTES          8
#define DFLPT_PTE2048_OFFSET     0x2000
#define DFLPT_PTE2048_POINTER    0x80000000U
#define DFLPT_PTE2048_AP_MASK    (0x3U << 10)
#define DFLPT_PTE2048_DOMAIN_MASK (0xFU << 5)
#define DFLPT_PTE2048_BUFFERABLE (1U << 2)
#define DFLPT_PTE2048_RW_MASK    (DFLPT_PTE2048_AP_MASK | \
                                  DFLPT_PTE2048_DOMAIN_MASK | \
                                  DFLPT_PTE2048_BUFFERABLE)
#define DFLPT_PTE2048_FIXED_BITS (DFLPT_PTE2048_POINTER | (1U << 4) | 0x2U)

static int stmp3770_dflpt_find_mpte(STMP3770State *s, hwaddr addr)
{
    hwaddr entry = addr >> 2;
    int i;

    for (i = 0; i < DFLPT_NUM_MPTES; i++) {
        if (stmp3770_digctl_get_mpte_loc(s->digctl, i) == entry) {
            return i;
        }
    }

    return -1;
}

static MemTxResult stmp3770_dflpt_read(void *opaque, hwaddr addr,
                                       uint64_t *data, unsigned size,
                                       MemTxAttrs attrs)
{
    STMP3770State *s = opaque;
    int idx;

    if (addr == DFLPT_PTE2048_OFFSET) {
        *data = s->dflpt_pte_2048;
        return MEMTX_OK;
    }

    idx = stmp3770_dflpt_find_mpte(s, addr);
    if (idx >= 0) {
        *data = s->dflpt_mpte[idx];
    } else {
        *data = 0;
    }

    return MEMTX_OK;
}

static MemTxResult stmp3770_dflpt_write(void *opaque, hwaddr addr,
                                        uint64_t data, unsigned size,
                                        MemTxAttrs attrs)
{
    STMP3770State *s = opaque;
    int idx;

    if (addr == DFLPT_PTE2048_OFFSET) {
        s->dflpt_pte_2048 = DFLPT_PTE2048_FIXED_BITS |
                            ((uint32_t)data & DFLPT_PTE2048_RW_MASK);
        return MEMTX_OK;
    }

    idx = stmp3770_dflpt_find_mpte(s, addr);
    if (idx < 0) {
        return MEMTX_ERROR;
    }

    s->dflpt_mpte[idx] = (uint32_t)data;
    return MEMTX_OK;
}

static const MemoryRegionOps stmp3770_dflpt_ops = {
    .read_with_attrs = stmp3770_dflpt_read,
    .write_with_attrs = stmp3770_dflpt_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void stmp3770_reset(DeviceState *dev)
{
    STMP3770State *s = STMP3770(dev);

    memset(s->dflpt_mpte, 0, sizeof(s->dflpt_mpte));
    s->dflpt_pte_2048 = DFLPT_PTE2048_FIXED_BITS | (0x3U << 10);
}

static void stmp3770_dig_reset(void *opaque)
{
    STMP3770State *s = opaque;

    device_cold_reset(DEVICE(s->icoll));
    device_cold_reset(DEVICE(s->clkctrl));
    stmp3770_digctl_dig_reset(s->digctl);
    device_cold_reset(DEVICE(s->pinctrl));
    device_cold_reset(DEVICE(s->timer));
    device_cold_reset(DEVICE(s->ocotp));
    device_cold_reset(DEVICE(s->apbh_dma));
    device_cold_reset(DEVICE(s->apbx_dma));
    device_cold_reset(DEVICE(s->gpmi));
    device_cold_reset(DEVICE(s->bch));
    device_cold_reset(DEVICE(s->i2c));
    device_cold_reset(DEVICE(s->lcdif));
    device_cold_reset(DEVICE(s->audio_dac));
    device_cold_reset(DEVICE(s->audio_adc));
    device_cold_reset(DEVICE(s->pwm));
    device_cold_reset(DEVICE(s->lradc));
    device_cold_reset(DEVICE(s->usbphy));
    device_cold_reset(DEVICE(s->usb));
    device_cold_reset(DEVICE(s->ssp[0]));
    device_cold_reset(DEVICE(s->ssp[1]));
    device_cold_reset(DEVICE(s->uart[0]));
    device_cold_reset(DEVICE(s->uart[1]));
    device_cold_reset(DEVICE(s));
}

static void stmp3770_init(Object *obj)
{
    STMP3770State *s = STMP3770(obj);
    int i;

    /* Initialize ARM926EJ-S CPU */
    object_initialize_child(obj, "cpu", &s->cpu, ARM_CPU_TYPE_NAME("arm926"));

    /* Initialize interrupt controller (ICOLL) */
    s->icoll = STMP3770_ICOLL(object_new(TYPE_STMP3770_ICOLL));
    object_property_add_child(obj, "icoll", OBJECT(s->icoll));
    object_unref(OBJECT(s->icoll));

    /* Initialize clock controller (CLKCTRL) */
    s->clkctrl = STMP3770_CLKCTRL(object_new(TYPE_STMP3770_CLKCTRL));
    object_property_add_child(obj, "clkctrl", OBJECT(s->clkctrl));
    object_unref(OBJECT(s->clkctrl));

    /* Initialize power supply controller (POWER) */
    s->power = STMP3770_POWER(object_new(TYPE_STMP3770_POWER));
    object_property_add_child(obj, "power", OBJECT(s->power));
    object_unref(OBJECT(s->power));

    /* Initialize digital controller (DIGCTL) */
    s->digctl = STMP3770_DIGCTL(object_new(TYPE_STMP3770_DIGCTL));
    object_property_add_child(obj, "digctrl", OBJECT(s->digctl));
    object_unref(OBJECT(s->digctl));

    /* Initialize pin control and GPIO (PINCTRL) */
    s->pinctrl = STMP3770_PINCTRL(object_new(TYPE_STMP3770_PINCTRL));
    object_property_add_child(obj, "pinctrl", OBJECT(s->pinctrl));
    object_unref(OBJECT(s->pinctrl));

    /* Initialize timers and rotary decoder (TIMROT) */
    s->timer = STMP3770_TIMER(object_new(TYPE_STMP3770_TIMER));
    object_property_add_child(obj, "timer", OBJECT(s->timer));
    object_unref(OBJECT(s->timer));

    /* Initialize on-chip OTP controller (OCOTP) */
    s->ocotp = STMP3770_OCOTP(object_new(TYPE_STMP3770_OCOTP));
    object_property_add_child(obj, "ocotp", OBJECT(s->ocotp));
    object_unref(OBJECT(s->ocotp));

    /* Initialize real-time clock (RTC) */
    s->rtc = STMP3770_RTC(object_new(TYPE_STMP3770_RTC));
    object_property_add_child(obj, "rtc", OBJECT(s->rtc));
    object_unref(OBJECT(s->rtc));

    /* Initialize DMA controllers */
    s->apbh_dma = STMP3770_DMA(object_new(TYPE_STMP3770_APBH_DMA));
    object_property_add_child(obj, "apbh-dma", OBJECT(s->apbh_dma));
    object_unref(OBJECT(s->apbh_dma));

    s->apbx_dma = STMP3770_DMA(object_new(TYPE_STMP3770_APBX_DMA));
    object_property_add_child(obj, "apbx-dma", OBJECT(s->apbx_dma));
    object_unref(OBJECT(s->apbx_dma));

    /* Initialize GPMI (NAND Flash controller) */
    s->gpmi = STMP3770_GPMI(object_new(TYPE_STMP3770_GPMI));
    object_property_add_child(obj, "gpmi", OBJECT(s->gpmi));
    object_unref(OBJECT(s->gpmi));

    /* Initialize BCH/ECC controller */
    s->bch = STMP3770_BCH(object_new(TYPE_STMP3770_BCH));
    object_property_add_child(obj, "bch", OBJECT(s->bch));
    object_unref(OBJECT(s->bch));

    /* Initialize I2C controller */
    s->i2c = STMP3770_I2C(object_new(TYPE_STMP3770_I2C));
    object_property_add_child(obj, "i2c", OBJECT(s->i2c));
    object_unref(OBJECT(s->i2c));

    /* Initialize LCDIF controller */
    s->lcdif = STMP3770_LCDIF(object_new(TYPE_STMP3770_LCDIF));
    object_property_add_child(obj, "lcdif", OBJECT(s->lcdif));
    object_unref(OBJECT(s->lcdif));

    /* Initialize Audio DAC and ADC controllers */
    s->audio_dac = STMP3770_AUDIO_DAC(object_new(TYPE_STMP3770_AUDIO_DAC));
    object_property_add_child(obj, "audio-dac", OBJECT(s->audio_dac));
    object_unref(OBJECT(s->audio_dac));

    s->audio_adc = STMP3770_AUDIO_ADC(object_new(TYPE_STMP3770_AUDIO_ADC));
    object_property_add_child(obj, "audio-adc", OBJECT(s->audio_adc));
    object_unref(OBJECT(s->audio_adc));

    /* Initialize PWM controller */
    s->pwm = STMP3770_PWM(object_new(TYPE_STMP3770_PWM));
    object_property_add_child(obj, "pwm", OBJECT(s->pwm));
    object_unref(OBJECT(s->pwm));

    /* Initialize LRADC controller */
    s->lradc = STMP3770_LRADC(object_new(TYPE_STMP3770_LRADC));
    object_property_add_child(obj, "lradc", OBJECT(s->lradc));
    object_unref(OBJECT(s->lradc));

    /* Initialize USB PHY and OTG controller */
    s->usbphy = STMP3770_USBPHY(object_new(TYPE_STMP3770_USBPHY));
    object_property_add_child(obj, "usbphy", OBJECT(s->usbphy));
    object_unref(OBJECT(s->usbphy));

    s->usb = STMP3770_USB(object_new(TYPE_STMP3770_USB));
    object_property_add_child(obj, "usb", OBJECT(s->usb));
    object_unref(OBJECT(s->usb));

    /* Initialize SSP controllers */
    for (i = 0; i < STMP3770_NUM_SSPS; i++) {
        s->ssp[i] = STMP3770_SSP(object_new(TYPE_STMP3770_SSP));
        object_property_add_child(obj, "ssp[*]", OBJECT(s->ssp[i]));
        object_unref(OBJECT(s->ssp[i]));
    }

    /* Initialize UARTs */
    for (i = 0; i < STMP3770_NUM_UARTS; i++) {
        s->uart[i] = PL011(object_new(TYPE_PL011));
        object_property_add_child(obj, "uart[*]", OBJECT(s->uart[i]));
        object_unref(OBJECT(s->uart[i]));
    }
}

static void stmp3770_realize(DeviceState *dev, Error **errp)
{
    STMP3770State *s = STMP3770(dev);
    MemoryRegion *system_memory = get_system_memory();
    int i;

    /* Realize the CPU */
    if (!qdev_realize(DEVICE(&s->cpu), NULL, errp)) {
        return;
    }

    /* Create and map on-chip SRAM (512KB at 0x00000000) */
    if (!memory_region_init_ram(&s->sram, OBJECT(dev), "stmp3770.sram",
                                STMP3770_SRAM_SIZE, errp)) {
        return;
    }
    memory_region_add_subregion(system_memory, STMP3770_SRAM_ADDR, &s->sram);

    /*
     * STMP37xx exposes a sparse hardware first-level page-table window:
     * 8 movable MPTEs plus a semi-programmable fixed PTE at entry 2048.
     */
    memory_region_init_io(&s->dflpt, OBJECT(dev), &stmp3770_dflpt_ops, s,
                          "stmp3770.dflpt", STMP3770_DFLPT_SIZE);
    memory_region_add_subregion(system_memory, STMP3770_DFLPT_ADDR, &s->dflpt);

    /* Realize interrupt controller (ICOLL) */
    if (!sysbus_realize(SYS_BUS_DEVICE(s->icoll), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->icoll), 0, STMP3770_ICOLL_ADDR);

    /* Connect ICOLL IRQ and FIQ to CPU */
    sysbus_connect_irq(SYS_BUS_DEVICE(s->icoll), 0,
                       qdev_get_gpio_in(DEVICE(&s->cpu), ARM_CPU_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->icoll), 1,
                       qdev_get_gpio_in(DEVICE(&s->cpu), ARM_CPU_FIQ));

    /* Realize clock controller (CLKCTRL) */
    if (!sysbus_realize(SYS_BUS_DEVICE(s->clkctrl), errp)) {
        return;
    }
    stmp3770_clkctrl_set_dig_reset_callback(s->clkctrl, stmp3770_dig_reset, s);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->clkctrl), 0, STMP3770_CLKCTRL_ADDR);

    /* Realize power supply controller (POWER) */
    if (!sysbus_realize(SYS_BUS_DEVICE(s->power), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->power), 0, STMP3770_POWER_ADDR);

    /* Realize digital controller (DIGCTL) */
    if (!sysbus_realize(SYS_BUS_DEVICE(s->digctl), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->digctl), 0, STMP3770_DIGCTL_ADDR);
    stmp3770_clkctrl_set_hclk_rate_callback(s->clkctrl,
                                             stmp3770_digctl_set_hclk_rate,
                                             s->digctl);

    /* Realize pin control and GPIO (PINCTRL) */
    if (!sysbus_realize(SYS_BUS_DEVICE(s->pinctrl), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->pinctrl), 0, STMP3770_PINCTRL_ADDR);

    /* Connect GPIO bank interrupts to ICOLL */
    sysbus_connect_irq(SYS_BUS_DEVICE(s->pinctrl), 0,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_GPIO0));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->pinctrl), 1,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_GPIO1));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->pinctrl), 2,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_GPIO2));
    /* Bank 3 has no dedicated ICOLL IRQ line in the current STMP3770 map; leave unconnected. */

    /* Realize timers and rotary decoder (TIMROT) */
    if (!sysbus_realize(SYS_BUS_DEVICE(s->timer), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->timer), 0, STMP3770_TIMERS_ADDR);

    /* Connect timer interrupts to ICOLL */
    sysbus_connect_irq(SYS_BUS_DEVICE(s->timer), 0,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_TIMER0));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->timer), 1,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_TIMER1));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->timer), 2,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_TIMER2));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->timer), 3,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_TIMER3));

    /* Realize OCOTP */
    if (!sysbus_realize(SYS_BUS_DEVICE(s->ocotp), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->ocotp), 0, STMP3770_OCOTP_ADDR);

    /* Realize RTC */
    if (!sysbus_realize(SYS_BUS_DEVICE(s->rtc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->rtc), 0, STMP3770_RTC_ADDR);

    /* RTC exposes distinct ICOLL sources for alarm and 1 ms tick interrupts */
    sysbus_connect_irq(SYS_BUS_DEVICE(s->rtc), 0,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_RTC_ALARM));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->rtc), 1,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_RTC_1MSEC));

    /* Realize APBH DMA */
    if (!sysbus_realize(SYS_BUS_DEVICE(s->apbh_dma), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->apbh_dma), 0, STMP3770_APBH_ADDR);

    /* APBH DMA channel IRQs */
    sysbus_connect_irq(SYS_BUS_DEVICE(s->apbh_dma), 0,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_LCDIF_DMA));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->apbh_dma), 1,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_SSP1_DMA));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->apbh_dma), 2,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_SSP2_DMA));
    /* Channel 3 is reserved; channels 4-7 serve GPMI/NAND */
    sysbus_connect_irq(SYS_BUS_DEVICE(s->apbh_dma), 4,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_GPMI_DMA));

    /* Realize APBX DMA */
    if (!sysbus_realize(SYS_BUS_DEVICE(s->apbx_dma), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->apbx_dma), 0, STMP3770_APBX_ADDR);

    /* Realize GPMI */
    if (!sysbus_realize(SYS_BUS_DEVICE(s->gpmi), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->gpmi), 0, STMP3770_GPMI_ADDR);

    /* Connect GPMI to APBH DMA channels 4-7 */
    stmp3770_gpmi_set_dma(s->gpmi, s->apbh_dma, 4);

    /* Connect GPMI DMA completion IRQ */
    sysbus_connect_irq(SYS_BUS_DEVICE(s->gpmi), 0,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_GPMI_ERROR));

    /* Realize BCH */
    if (!sysbus_realize(SYS_BUS_DEVICE(s->bch), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->bch), 0, STMP3770_BCH_ADDR);

    sysbus_connect_irq(SYS_BUS_DEVICE(s->bch), 0,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_ECC8));

    /* Link BCH back to GPMI so ECC completions can raise the BCH IRQ */
    stmp3770_gpmi_set_bch(s->gpmi, s->bch);

    /* Realize I2C */
    if (!sysbus_realize(SYS_BUS_DEVICE(s->i2c), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->i2c), 0, STMP3770_I2C_ADDR);

    sysbus_connect_irq(SYS_BUS_DEVICE(s->i2c), 0,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_I2C_DMA));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->i2c), 1,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_I2C_ERROR));

    /* Realize LCDIF */
    if (!sysbus_realize(SYS_BUS_DEVICE(s->lcdif), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->lcdif), 0, STMP3770_LCDIF_ADDR);

    sysbus_connect_irq(SYS_BUS_DEVICE(s->lcdif), 0,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_LCDIF_ERROR));

    stmp3770_lcdif_set_dma(s->lcdif, s->apbh_dma, 0);
    stmp3770_lcdif_set_pinctrl(s->lcdif, s->pinctrl);

    /* Realize Audio DAC */
    if (!sysbus_realize(SYS_BUS_DEVICE(s->audio_dac), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->audio_dac), 0, STMP3770_AUDIODAC_ADDR);

    sysbus_connect_irq(SYS_BUS_DEVICE(s->audio_dac), 0,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_DAC_ERROR));

    stmp3770_audio_dac_set_dma(s->audio_dac, s->apbx_dma, 1);

    /* Realize Audio ADC */
    if (!sysbus_realize(SYS_BUS_DEVICE(s->audio_adc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->audio_adc), 0, STMP3770_AUDIOADC_ADDR);

    sysbus_connect_irq(SYS_BUS_DEVICE(s->audio_adc), 0,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_ADC_ERROR));

    stmp3770_audio_adc_set_dma(s->audio_adc, s->apbx_dma, 0);

    /* Realize PWM controller */
    if (!sysbus_realize(SYS_BUS_DEVICE(s->pwm), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->pwm), 0, STMP3770_PWM_ADDR);

    /* Realize LRADC controller */
    if (!sysbus_realize(SYS_BUS_DEVICE(s->lradc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->lradc), 0, STMP3770_LRADC_ADDR);

    /* Realize USB PHY */
    if (!sysbus_realize(SYS_BUS_DEVICE(s->usbphy), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->usbphy), 0, STMP3770_USBPHY_ADDR);

    /* Realize USB OTG controller */
    if (!sysbus_realize(SYS_BUS_DEVICE(s->usb), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->usb), 0, STMP3770_USB_ADDR);

    sysbus_connect_irq(SYS_BUS_DEVICE(s->usb), 0,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_USB));

    /* Realize SSP controllers */
    for (i = 0; i < STMP3770_NUM_SSPS; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(s->ssp[i]), errp)) {
            return;
        }
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->ssp[0]), 0, STMP3770_SSP1_ADDR);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->ssp[1]), 0, STMP3770_SSP2_ADDR);

    sysbus_connect_irq(SYS_BUS_DEVICE(s->ssp[0]), 0,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_SSP1_DMA));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->ssp[0]), 1,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_SSP1_ERROR));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->ssp[1]), 0,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_SSP2_DMA));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->ssp[1]), 1,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_SSP2_ERROR));

    /* APBX DMA channel IRQs */
    sysbus_connect_irq(SYS_BUS_DEVICE(s->apbx_dma), 0,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_ADC_DMA));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->apbx_dma), 1,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_DAC_DMA));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->apbx_dma), 2,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_SPDIF_DMA));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->apbx_dma), 3,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_I2C_DMA));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->apbx_dma), 4,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_SAIF1_DMA));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->apbx_dma), 5,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_DRI_DMA));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->apbx_dma), 6,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_UART_RX_DMA));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->apbx_dma), 7,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_UART_TX_DMA));

    /* Realize UARTs */
    /* Debug UART at 0x80070000 */
    if (!sysbus_realize(SYS_BUS_DEVICE(s->uart[0]), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->uart[0]), 0, STMP3770_DBGUART_ADDR);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->uart[0]), 0,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_DBGUART));

    /* App UART at 0x8006C000 */
    if (!sysbus_realize(SYS_BUS_DEVICE(s->uart[1]), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->uart[1]), 0, STMP3770_APPUART_ADDR);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->uart[1]), 0,
                       qdev_get_gpio_in(DEVICE(s->icoll), STMP3770_IRQ_UART_ERROR));

    /*
     * Child devices get their own reset during realize, but the SoC-local
     * DFLPT sparse-window state lives on this container device.
     */
    stmp3770_reset(dev);
}

static void stmp3770_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = stmp3770_realize;
    device_class_set_legacy_reset(dc, stmp3770_reset);
    /* This SoC is not user-creatable, only instantiated by machine */
    dc->user_creatable = false;
}

static const TypeInfo stmp3770_type_info = {
    .name          = TYPE_STMP3770,
    .parent        = TYPE_DEVICE,
    .instance_size = sizeof(STMP3770State),
    .instance_init = stmp3770_init,
    .class_init    = stmp3770_class_init,
};

static void stmp3770_register_types(void)
{
    type_register_static(&stmp3770_type_info);
}

type_init(stmp3770_register_types)
