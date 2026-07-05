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

#ifndef STMP3770_H
#define STMP3770_H

#include "target/arm/cpu.h"
#include "qom/object.h"
#include "hw/sysbus.h"
#include "hw/char/pl011.h"
#include "hw/audio/stmp3770_audio.h"
#include "hw/display/stmp3770_lcdif.h"
#include "hw/gpio/stmp3770_pinctrl.h"
#include "hw/intc/stmp3770_icoll.h"
#include "hw/misc/stmp3770_clkctrl.h"
#include "hw/misc/stmp3770_digctl.h"
#include "hw/misc/stmp3770_lradc.h"
#include "hw/misc/stmp3770_power.h"
#include "hw/misc/stmp3770_ocotp.h"
#include "hw/rtc/stmp3770_rtc.h"
#include "hw/timer/stmp3770_timer.h"
#include "hw/timer/stmp3770_pwm.h"
#include "hw/dma/stmp3770_dma.h"
#include "hw/block/stmp3770_gpmi.h"
#include "hw/i2c/stmp3770_i2c.h"
#include "hw/ssi/stmp3770_ssp.h"
#include "hw/usb/stmp3770_usbphy.h"
#include "hw/usb/stmp3770_usb.h"

#define TYPE_STMP3770 "stmp3770"
OBJECT_DECLARE_SIMPLE_TYPE(STMP3770State, STMP3770)

/* Number of peripheral instances */
#define STMP3770_NUM_UARTS       2
#define STMP3770_NUM_TIMERS      4
#define STMP3770_NUM_SSPS        2
#define STMP3770_NUM_GPIO_BANKS  4
#define STMP3770_NUM_I2C         1
#define STMP3770_NUM_LCDIF       1
#define STMP3770_NUM_IRQS        64

struct STMP3770State {
    /*< private >*/
    DeviceState parent_obj;

    /*< public >*/
    ARMCPU cpu;

    /* Interrupt controller - ICOLL */
    STMP3770ICOLLState *icoll;

    /* Clock control - CLKCTRL */
    STMP3770CLKCTRLState *clkctrl;

    /* Digital control - DIGCTL */
    STMP3770DIGCTLState *digctl;

    /* Power supply - POWER */
    STMP3770PowerState *power;

    /* Pin control and GPIO - PINCTRL */
    STMP3770PINCTRLState *pinctrl;

    /* UARTs - using PL011 */
    PL011State *uart[STMP3770_NUM_UARTS];

    /* Timers and rotary decoder - TIMROT */
    STMP3770TimerState *timer;

    /* On-Chip OTP controller */
    STMP3770OCOTPState *ocotp;

    /* Real-time clock */
    STMP3770RTCState *rtc;

    /* DMA controllers */
    STMP3770DMAState *apbh_dma;
    STMP3770DMAState *apbx_dma;

    /* GPMI (NAND Flash controller) */
    STMP3770GPMIState *gpmi;

    /* BCH/ECC controller */
    STMP3770BCHState *bch;

    /* I2C controller */
    STMP3770I2CState *i2c;

    /* LCDIF controller */
    STMP3770LCDIFState *lcdif;

    /* Audio DAC/ADC controllers */
    STMP3770AudioDACState *audio_dac;
    STMP3770AudioADCState *audio_adc;

    /* SSP controllers */
    STMP3770SSPState *ssp[STMP3770_NUM_SSPS];

    /* PWM controller */
    STMP3770PWMState *pwm;

    /* LRADC controller */
    STMP3770LRADCState *lradc;

    /* USB PHY and OTG controller */
    STMP3770USBPHYState *usbphy;
    STMP3770USBState *usb;

    /* Memory regions */
    MemoryRegion sram;          /* 512KB on-chip SRAM */
    MemoryRegion dflpt;         /* Hardware L1 page-table RAM */
};

/* Memory map - based on STMP3770 Reference Manual */

/* Interrupt Controller (ICOLL) */
#define STMP3770_ICOLL_ADDR         0x80000000
#define STMP3770_ICOLL_SIZE         0x2000

/* Clock Control (CLKCTRL) */
#define STMP3770_CLKCTRL_ADDR       0x80040000
#define STMP3770_CLKCTRL_SIZE       0x200

/* Power Supply (POWER) */
#define STMP3770_POWER_ADDR         0x80044000
#define STMP3770_POWER_SIZE         0x200

/* Digital Control (DIGCTL) */
#define STMP3770_DIGCTL_ADDR        0x8001C000
#define STMP3770_DIGCTL_SIZE        0x2000

/* Pin Control and GPIO (PINCTRL) */
#define STMP3770_PINCTRL_ADDR       0x80018000
#define STMP3770_PINCTRL_SIZE       0x2000

/* OCOTP (One-Time Programmable memory) */
#define STMP3770_OCOTP_ADDR         0x8002C000
#define STMP3770_OCOTP_SIZE         0x2000

/* USB PHY */
#define STMP3770_USBPHY_ADDR        0x8007C000
#define STMP3770_USBPHY_SIZE        0x2000

/* USB Controller */
#define STMP3770_USB_ADDR           0x80080000
#define STMP3770_USB_SIZE           0x1000

/* APBH DMA Bridge (includes GPMI, SSP1, ECC8) */
#define STMP3770_APBH_ADDR          0x80004000
#define STMP3770_APBH_SIZE          0x2000

/* GPMI (General Purpose Memory Interface / NAND) */
#define STMP3770_GPMI_ADDR          0x8000C000
#define STMP3770_GPMI_SIZE          0x2000

/* BCH/ECC8 controller */
#define STMP3770_BCH_ADDR           0x80008000
#define STMP3770_BCH_SIZE           0x2000

/* SSP1 (Synchronous Serial Port 1) */
#define STMP3770_SSP1_ADDR          0x80010000
#define STMP3770_SSP1_SIZE          0x2000

/* SSP2 (Synchronous Serial Port 2) */
#define STMP3770_SSP2_ADDR          0x80034000
#define STMP3770_SSP2_SIZE          0x2000

/* APBX DMA Bridge */
#define STMP3770_APBX_ADDR          0x80024000
#define STMP3770_APBX_SIZE          0x2000

/* LCD Interface (LCDIF) */
#define STMP3770_LCDIF_ADDR         0x80030000
#define STMP3770_LCDIF_SIZE         0x2000

/* Audio DAC (Audio Out) */
#define STMP3770_AUDIODAC_ADDR      0x80048000
#define STMP3770_AUDIODAC_SIZE      0x2000

/* Audio ADC (Audio In) */
#define STMP3770_AUDIOADC_ADDR      0x8004C000
#define STMP3770_AUDIOADC_SIZE      0x2000

/* LRADC (Low-Resolution ADC) */
#define STMP3770_LRADC_ADDR         0x80050000
#define STMP3770_LRADC_SIZE         0x2000

/* RTC */
#define STMP3770_RTC_ADDR           0x8005C000
#define STMP3770_RTC_SIZE           0x2000

/* PWM */
#define STMP3770_PWM_ADDR           0x80064000
#define STMP3770_PWM_SIZE           0x2000

/* Timers */
#define STMP3770_TIMERS_ADDR        0x80068000
#define STMP3770_TIMERS_SIZE        0x2000

/* I2C */
#define STMP3770_I2C_ADDR           0x80058000
#define STMP3770_I2C_SIZE           0x2000

/* Debug UART */
#define STMP3770_DBGUART_ADDR       0x80070000
#define STMP3770_DBGUART_SIZE       0x2000

/* App UART */
#define STMP3770_APPUART_ADDR       0x8006C000
#define STMP3770_APPUART_SIZE       0x2000

/* On-chip SRAM - 512KB */
#define STMP3770_SRAM_ADDR          0x00000000
#define STMP3770_SRAM_SIZE          0x80000     /* 512KB */

/* Hardware DFLPT first-level page-table RAM */
#define STMP3770_DFLPT_ADDR         0x800C0000
#define STMP3770_DFLPT_SIZE         0x4000

/* DRAM - external, up to 128MB typically */
#define STMP3770_DRAM_ADDR          0x40000000
#define STMP3770_DRAM_SIZE          0x08000000  /* 128MB default */

/* Interrupt numbers - from Chapter 5 Interrupt Collector */
#define STMP3770_IRQ_DBGUART        0
#define STMP3770_IRQ_JTAG           1
#define STMP3770_IRQ_SSP2_ERROR     2
#define STMP3770_IRQ_VDD5V          3
#define STMP3770_IRQ_HEADPHONE      4
#define STMP3770_IRQ_DAC_DMA        5
#define STMP3770_IRQ_DAC_ERROR      6
#define STMP3770_IRQ_ADC_DMA        7
#define STMP3770_IRQ_ADC_ERROR      8
#define STMP3770_IRQ_SPDIF_DMA      9
#define STMP3770_IRQ_SPDIF_ERROR    10
#define STMP3770_IRQ_USB            11
#define STMP3770_IRQ_USB_WAKEUP     12
#define STMP3770_IRQ_GPMI_DMA       13
#define STMP3770_IRQ_SSP1_DMA       14
#define STMP3770_IRQ_SSP1_ERROR     15
#define STMP3770_IRQ_GPIO0          16
#define STMP3770_IRQ_GPIO1          17
#define STMP3770_IRQ_GPIO2          18
#define STMP3770_IRQ_SAIF1_DMA      19
#define STMP3770_IRQ_SSP2_DMA       20
#define STMP3770_IRQ_ECC8           21
#define STMP3770_IRQ_RTC_ALARM      22
#define STMP3770_IRQ_UART_TX_DMA    23
#define STMP3770_IRQ_UART_ERROR     24
#define STMP3770_IRQ_UART_RX_DMA    25
#define STMP3770_IRQ_I2C_DMA        26
#define STMP3770_IRQ_I2C_ERROR      27
#define STMP3770_IRQ_TIMER0         28
#define STMP3770_IRQ_TIMER1         29
#define STMP3770_IRQ_TIMER2         30
#define STMP3770_IRQ_TIMER3         31
#define STMP3770_IRQ_BATT_BRNOUT    32
#define STMP3770_IRQ_VDDD_BRNOUT    33
#define STMP3770_IRQ_VDDIO_BRNOUT   34
#define STMP3770_IRQ_VDD18_BRNOUT   35
#define STMP3770_IRQ_TOUCH          36
#define STMP3770_IRQ_LRADC_CH0      37
#define STMP3770_IRQ_LRADC_CH1      38
#define STMP3770_IRQ_LRADC_CH2      39
#define STMP3770_IRQ_LRADC_CH3      40
#define STMP3770_IRQ_LRADC_CH4      41
#define STMP3770_IRQ_LRADC_CH5      42
#define STMP3770_IRQ_LRADC_CH6      43
#define STMP3770_IRQ_LRADC_CH7      44
#define STMP3770_IRQ_LCDIF_DMA      45
#define STMP3770_IRQ_LCDIF_ERROR    46
#define STMP3770_IRQ_DIGCTL_TRAP    47
#define STMP3770_IRQ_RTC_1MSEC      48
#define STMP3770_IRQ_DRI_DMA        49
#define STMP3770_IRQ_DRI_ERROR      50
#define STMP3770_IRQ_GPMI_ERROR     51
#define STMP3770_IRQ_IRDA           52
#define STMP3770_IRQ_DCP_VMI        53
#define STMP3770_IRQ_DCP            54

#endif /* STMP3770_H */
