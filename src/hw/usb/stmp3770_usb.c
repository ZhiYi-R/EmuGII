/*
 * STMP3770 USB OTG controller emulation
 *
 * Based on STMP3770 Reference Manual Chapter 18
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
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/address-spaces.h"
#include "hw/usb/stmp3770_usb.h"

/* Register offsets */
#define REG_ID              0x000
#define REG_HWGENERAL       0x004
#define REG_HWHOST          0x008
#define REG_HWDEVICE        0x00C
#define REG_HWTXBUF         0x010
#define REG_HWRXBUF         0x014
#define REG_GPTIMER0LD      0x080
#define REG_GPTIMER0CTRL    0x084
#define REG_GPTIMER1LD      0x088
#define REG_GPTIMER1CTRL    0x08C
#define REG_SBUSCFG         0x090
#define REG_CAPLENGTH       0x100
#define REG_HCIVERSION      0x102
#define REG_HCSPARAMS       0x104
#define REG_HCCPARAMS       0x108
#define REG_DCIVERSION      0x120
#define REG_DCCPARAMS       0x124
#define REG_USBCMD          0x140
#define REG_USBSTS          0x144
#define REG_USBINTR         0x148
#define REG_FRINDEX         0x14C
#define REG_DEVICEADDR      0x154
#define REG_ENDPTLISTADDR   0x158
#define REG_ASYNCLISTADDR   0x158
#define REG_TTCTRL          0x15C
#define REG_BURSTSIZE       0x160
#define REG_TXFILLTUNING    0x164
#define REG_ULPIVIEWPORT    0x170
#define REG_ENDPTNAK        0x178
#define REG_ENDPTNAKEN      0x17C
#define REG_CONFIGFLAG      0x180
#define REG_PORTSC1         0x184
#define REG_OTGSC           0x1A4
#define REG_USBMODE         0x1A8
#define REG_ENDPTSETUPSTAT  0x1AC
#define REG_ENDPTPRIME      0x1B0
#define REG_ENDPTFLUSH      0x1B4
#define REG_ENDPTSTAT       0x1B8
#define REG_ENDPTCOMPLETE   0x1BC
#define REG_ENDPTCTRL0      0x1C0
#define REG_ENDPTCTRL1      0x1C4
#define REG_ENDPTCTRL2      0x1C8
#define REG_ENDPTCTRL3      0x1CC
#define REG_ENDPTCTRL4      0x1D0

#define USBCMD_RST          (1U << 1)
#define USBCMD_RUN          (1U << 0)
#define USBCMD_PSE          (1U << 4)
#define USBCMD_ASE          (1U << 5)
#define USBCMD_IAA          (1U << 6)
#define USBCMD_WRITABLE_MASK        0x00FFEB7FU

/*
 * USBSTS W1C bits per Table 276:
 *   bits 0-8: UI/UEI/PCI/FRI/SEI/AAI/URI/SRI/SLI
 *   bit 10 (ULPII) is RW but "not present in this implementation" -> treat as RO 0
 *   bit 16 (NAKI) is RO (set/cleared by hardware from ENDPTNAK/ENDPTNAKEN state)
 *   bits 18-19: UAI/UPI
 *   bits 24-25: TI0/TI1
 */
#define USBSTS_W1C_MASK             0x030C01FF
/*
 * USBINTR writable bits per Table 278:
 *   Same layout as USBSTS W1C, plus bit 16 (NAKE) which enables the RO NAKI
 *   status.  Bit 10 (ULPIE) is "not used" and kept RO 0.
 */
#define USBINTR_WRITABLE_MASK        0x030D01FF
#define USBINTR_NAKE_BIT             (1U << 16)
#define USBSTS_AAI                  (1U << 5)
#define USBSTS_URI                  (1U << 6)
#define USBSTS_SRI                  (1U << 7)
#define USBSTS_FRI                  (1U << 3)
#define USBSTS_HCH                  (1U << 12)
#define USBSTS_RCL                  (1U << 13)
#define USBSTS_PS                   (1U << 14)
#define USBSTS_AS                   (1U << 15)
#define FRINDEX_MASK                0x3FFFU
#define FRINDEX_PERIOD_US           125

#define GPTIMER_RUN                  (1U << 31)
#define GPTIMER_RESET                (1U << 30)
#define GPTIMER_REPEAT               (1U << 24)
#define GPTIMER_CONTROL_WRITABLE_MASK (GPTIMER_RUN | GPTIMER_RESET | GPTIMER_REPEAT)
#define GPTIMER_COUNT_MASK           0x00FFFFFF
#define USBSTS_GPTIMER0              (1U << 24)
#define USBSTS_GPTIMER1              (1U << 25)
#define USBSTS_PCI                   (1U << 2)

#define OTGSC_ONEMST                 (1U << 13)
#define OTGSC_ONEMSS                 (1U << 21)

#define PORTSC1_PSPD_HIGH            (1U << 26)

#define USBCTRL_USBCMD_DEVICE_RESET     0x00080000
#define USBCTRL_BURSTSIZE_RESET         0x00001010
#define USBCTRL_OTGSC_DEVICE_RESET      0x00000020
#define USBCTRL_USBINTR_WRITABLE_MASK   USBINTR_WRITABLE_MASK
/*
 * DEVICEADDR (device mode) / PERIODICLISTBASE (host mode) share offset 0x154.
 * Device: USBADR[31:25] + USBADRA[24], bits 23:0 RO.
 * Host:   PERBASE[31:12], bits 11:0 RO.
 */
#define USBCTRL_DEVICEADDR_WRITABLE_MASK 0xFF000000
#define USBCTRL_PERIODICLISTBASE_WRITABLE_MASK 0xFFFFF000
#define DEVICEADDR_USBADRA            (1U << 24)
/*
 * ENDPTLISTADDR (device mode) / ASYNCLISTADDR (host mode) share offset 0x158.
 * Device: EPBASE[31:11], bits 10:0 RO.
 * Host:   ASYBASE[31:5], bits 4:0 RO.
 */
#define USBCTRL_ENDPTLISTADDR_WRITABLE_MASK 0xFFFFF800
#define USBCTRL_ASYNCLISTADDR_WRITABLE_MASK 0xFFFFFFE0
#define USBCTRL_TTCTRL_WRITABLE_MASK    0x7F000000
#define USBCTRL_BURSTSIZE_WRITABLE_MASK 0x0000FFFF
#define USBCTRL_TXFILLTUNING_WRITABLE_MASK 0x003F007F
#define USBCTRL_USBMODE_WRITABLE_MASK   0x0000003F
#define USBCTRL_ENDPOINT_BITMAP_MASK     0x001F001F
#define USBCTRL_ENDPTSETUP_MASK          0x0000001F
#define USBCTRL_ENDPTCTRL0_WRITABLE_MASK 0x000D000D
#define USBCTRL_ENDPTCTRL0_RESET         0x00800080
#define USBCTRL_ENDPTCTRLN_ACTION_MASK   0x00400040
#define USBCTRL_ENDPTCTRLN_WRITABLE_MASK 0x00EF00EF
#define ENDPTCTRLN_TXR               (1U << 22)
#define ENDPTCTRLN_RXR               (1U << 6)
#define ENDPTCTRLN_TXD               (1U << 17)
#define ENDPTCTRLN_RXD               (1U << 1)

#define USBCTRL_ID_RESET            0x0042FA05
#define USBCTRL_ARC_GENERAL_RESET   0x00000015
#define USBCTRL_HWHOST_RESET        0x10020001
#define USBCTRL_HWDEVICE_RESET      0x0000000B
#define USBCTRL_HWTXBUF_RESET       0x00050810
#define USBCTRL_HWRXBUF_RESET       0x00000610
#define USBCTRL_HCSPARAMS_RESET     0x00010011
#define USBCTRL_HCCPARAMS_RESET     0x00000006
#define USBCTRL_DCCPARAMS_RESET     0x00000185

#define PORTSC1_PP          (1U << 12)
#define PORTSC1_PR          (1U << 8)
#define PORTSC1_PE          (1U << 2)
#define PORTSC1_CCS         (1U << 0)

#define PORTSC1_CSC         (1U << 1)
#define PORTSC1_PEC         (1U << 3)
#define PORTSC1_OCC         (1U << 5)
#define PORTSC1_OCA         (1U << 4)
#define PORTSC1_FPR         (1U << 6)
#define PORTSC1_SUSP        (1U << 7)
#define PORTSC1_PHCD        (1U << 23)
#define PORTSC1_WKOC        (1U << 22)
#define PORTSC1_WKDC        (1U << 21)
#define PORTSC1_WKCN        (1U << 20)
#define PORTSC1_PFSC        (1U << 24)
#define PORTSC1_PTC         (0xFU << 16)
#define PORTSC1_PIC         (0x3U << 14)
#define PORTSC1_PO          (1U << 13)
#define PORTSC1_LS          (0x3U << 10)
#define PORTSC1_HSP         (1U << 9)
#define PORTSC1_PTW         (1U << 28)
#define PORTSC1_PSPD        (0x3U << 26)
#define PORTSC1_PTS         (0x3U << 30)
#define PORTSC1_STS         (1U << 29)

#define PORTSC1_W1C_MASK    (PORTSC1_CSC | PORTSC1_PEC | PORTSC1_OCC)
#define PORTSC1_ALWAYS_RW   (PORTSC1_PTS | PORTSC1_STS | PORTSC1_PFSC | \
                             PORTSC1_PHCD | PORTSC1_WKOC | PORTSC1_WKDC | \
                             PORTSC1_WKCN | PORTSC1_PTC | PORTSC1_PIC | \
                             PORTSC1_FPR)
#define PORTSC1_HOST_RW     (PORTSC1_PP | PORTSC1_PR | PORTSC1_SUSP | PORTSC1_PE)
#define PORTSC1_KNOWN_MASK  (PORTSC1_ALWAYS_RW | PORTSC1_HOST_RW | \
                             PORTSC1_W1C_MASK | PORTSC1_PTW | PORTSC1_PSPD | \
                             PORTSC1_PO | PORTSC1_LS | PORTSC1_HSP | \
                             PORTSC1_OCA | PORTSC1_CCS)

#define OTGSC_EN_MASK       0x7F000000U
#define OTGSC_STATUS_MASK   0x007F0000U
#define OTGSC_STATUS_INPUT_MASK 0x00007F00U
#define OTGSC_CTRL_MASK     0x000000FFU
#define OTGSC_KNOWN_MASK    (OTGSC_EN_MASK | OTGSC_STATUS_MASK | \
                             OTGSC_STATUS_INPUT_MASK | OTGSC_CTRL_MASK)

/* dTD token field bits. */
#define DTD_STATUS_ACTIVE      (1U << 0)
#define DTD_STATUS_HALTED      (1U << 1)
#define DTD_STATUS_DATA_BUF_ERR (1U << 2)
#define DTD_STATUS_TX_ERR      (1U << 3)
#define DTD_IOC                (1U << 15)
#define DTD_TOTAL_BYTES_SHIFT  16
#define DTD_TOTAL_BYTES_MASK   (0x7FFFU << DTD_TOTAL_BYTES_SHIFT)
#define DTD_NEXT_TERMINATE     (1U << 0)

/* dQH capability field bits. */
#define QH_IOS                 (1U << 15)
#define QH_MAX_PKT_SHIFT       16
#define QH_MAX_PKT_MASK        (0x07FFU << QH_MAX_PKT_SHIFT)
#define QH_ZLT                 (1U << 29)

/* USBSTS.UI - USB interrupt (transfer complete). */
#define USBSTS_UI              (1U << 0)

/* Forward declarations (used by device-mode transfer helpers below). */
static void usb_update_irq(STMP3770USBState *s);
static bool usb_host_mode(const STMP3770USBState *s);

typedef struct STMP3770USBGPTimerCBInfo {
    STMP3770USBState *s;
    unsigned int idx;
} STMP3770USBGPTimerCBInfo;

/* ---- Device-mode dQH/dTD memory access helpers ---- */

/*
 * Calculate the dQH address for a given endpoint and direction.
 * Layout: EPBASE + (ep * 2 + is_tx) * 64
 */
static hwaddr usb_dqh_addr(STMP3770USBState *s, unsigned int ep, bool is_tx)
{
    return (s->endptlistaddr & USBCTRL_ENDPTLISTADDR_WRITABLE_MASK) +
           (hwaddr)(ep * 2 + (is_tx ? 1 : 0)) * USB_DQH_SIZE;
}

/* Write a 32-bit little-endian word to guest physical memory. */
static void usb_mem_write32(hwaddr addr, uint32_t val)
{
    uint32_t le = cpu_to_le32(val);
    address_space_write(&address_space_memory, addr,
                        MEMTXATTRS_UNSPECIFIED, &le, 4);
}

/*
 * Read a dTD (32 bytes) from guest memory into a local buffer.
 * Returns true on success.
 */
static bool usb_dtd_read(hwaddr dtd_addr, uint32_t dtd[8])
{
    if (address_space_read(&address_space_memory, dtd_addr,
                           MEMTXATTRS_UNSPECIFIED, dtd,
                           USB_DTD_SIZE) != MEMTX_OK) {
        return false;
    }
    for (int i = 0; i < 8; i++) {
        dtd[i] = ldl_le_p(&dtd[i]);
    }
    return true;
}

/* Write a dTD (32 bytes) back to guest memory. */
static void usb_dtd_write(hwaddr dtd_addr, const uint32_t dtd[8])
{
    uint32_t le[8];
    for (int i = 0; i < 8; i++) {
        le[i] = cpu_to_le32(dtd[i]);
    }
    address_space_write(&address_space_memory, dtd_addr,
                        MEMTXATTRS_UNSPECIFIED, le, USB_DTD_SIZE);
}

/* Read a contiguous buffer from guest memory using dTD page pointers. */
static bool usb_dtd_buffer_read(const uint32_t dtd[8],
                                uint8_t *buf, uint32_t max_len,
                                uint32_t *transferred)
{
    uint32_t total = (dtd[1] & DTD_TOTAL_BYTES_MASK) >> DTD_TOTAL_BYTES_SHIFT;
    uint32_t to_read = MIN(total, max_len);
    uint32_t page0_addr = dtd[2];  /* page[0] has full address */
    uint32_t done = 0;

    while (done < to_read) {
        unsigned int page_idx = done / USB_DTD_PAGE_SIZE;
        uint32_t page_offset = done % USB_DTD_PAGE_SIZE;
        uint32_t chunk;
        MemTxResult res;

        if (page_idx >= USB_DTD_MAX_PAGES) {
            break;
        }
        chunk = MIN(to_read - done, USB_DTD_PAGE_SIZE - page_offset);

        hwaddr src = (page_idx == 0)
            ? (page0_addr + page_offset)
            : ((dtd[2 + page_idx] & ~0xFFFU) + page_offset);

        res = address_space_read(&address_space_memory, src,
                                 MEMTXATTRS_UNSPECIFIED,
                                 buf + done, chunk);
        if (res != MEMTX_OK) {
            *transferred = done;
            return false;
        }
        done += chunk;
    }
    *transferred = done;
    return true;
}

/* Write a contiguous buffer to guest memory using dTD page pointers. */
static bool usb_dtd_buffer_write(const uint32_t dtd[8],
                                 const uint8_t *buf, uint32_t len,
                                 uint32_t *transferred)
{
    uint32_t total = (dtd[1] & DTD_TOTAL_BYTES_MASK) >> DTD_TOTAL_BYTES_SHIFT;
    uint32_t to_write = MIN(len, total);
    uint32_t page0_addr = dtd[2];
    uint32_t done = 0;

    while (done < to_write) {
        unsigned int page_idx = done / USB_DTD_PAGE_SIZE;
        uint32_t page_offset = done % USB_DTD_PAGE_SIZE;
        uint32_t chunk;
        MemTxResult res;

        if (page_idx >= USB_DTD_MAX_PAGES) {
            break;
        }
        chunk = MIN(to_write - done, USB_DTD_PAGE_SIZE - page_offset);

        hwaddr dst = (page_idx == 0)
            ? (page0_addr + page_offset)
            : ((dtd[2 + page_idx] & ~0xFFFU) + page_offset);

        res = address_space_write(&address_space_memory, dst,
                                  MEMTXATTRS_UNSPECIFIED,
                                  buf + done, chunk);
        if (res != MEMTX_OK) {
            *transferred = done;
            return false;
        }
        done += chunk;
    }
    *transferred = done;
    return true;
}

/*
 * Execute a device-mode IN transfer (device sends data to host).
 * Reads data from the dTD buffer, marks the dTD complete, and sets
 * ENDPTCOMPLETE / USBSTS.UI.
 */
static void usb_device_in_transfer(STMP3770USBState *s, unsigned int ep)
{
    hwaddr qh_addr = usb_dqh_addr(s, ep, true);
    uint32_t dtd[8];
    hwaddr dtd_addr;
    uint32_t bytes_transferred = 0;
    uint8_t buf[16 * 1024];

    /* Read the overlay dTD from the QH (offset 0x08 in QH). */
    dtd_addr = qh_addr + 0x08;
    if (!usb_dtd_read(dtd_addr, dtd)) {
        s->vh_in_status = VH_IN_STATUS_ERROR;
        return;
    }

    /* Check if the dTD is active. */
    if (!(dtd[1] & DTD_STATUS_ACTIVE)) {
        s->vh_in_status = VH_IN_STATUS_ERROR;
        return;
    }

    /* Read the data from the dTD buffer (for inspection). */
    usb_dtd_buffer_read(dtd, buf, sizeof(buf), &bytes_transferred);

    /* Update dTD status: clear active, set bytes transferred. */
    dtd[1] &= ~DTD_STATUS_ACTIVE;
    dtd[1] = (dtd[1] & ~DTD_TOTAL_BYTES_MASK) |
             (bytes_transferred << DTD_TOTAL_BYTES_SHIFT);
    usb_dtd_write(dtd_addr, dtd);

    /* Set ENDPTCOMPLETE (TX bit = 16 + ep) and clear ENDPTSTAT. */
    s->endptcomplete |= (1U << (16 + ep));
    s->endptstat &= ~(1U << (16 + ep));
    s->usbsts |= USBSTS_UI;
    s->vh_in_status = bytes_transferred & VH_IN_STATUS_BYTES;
    usb_update_irq(s);
}

/*
 * Execute a device-mode OUT transfer (device receives data from host).
 * Writes data from the virtual host buffer to the dTD buffer, marks the
 * dTD complete, and sets ENDPTCOMPLETE / USBSTS.UI.
 */
static void usb_device_out_transfer(STMP3770USBState *s, unsigned int ep)
{
    hwaddr qh_addr = usb_dqh_addr(s, ep, false);
    uint32_t dtd[8];
    hwaddr dtd_addr;
    uint32_t bytes_transferred = 0;
    uint32_t out_len = s->vh_out_len;
    g_autofree uint8_t *buf = NULL;

    /* Read the overlay dTD from the QH (offset 0x08 in QH). */
    dtd_addr = qh_addr + 0x08;
    if (!usb_dtd_read(dtd_addr, dtd)) {
        return;
    }

    /* Check if the dTD is active. */
    if (!(dtd[1] & DTD_STATUS_ACTIVE)) {
        return;
    }

    /* Read OUT data from guest memory (provided by test harness). */
    if (out_len > 0) {
        buf = g_malloc(out_len);
        if (address_space_read(&address_space_memory, s->vh_out_addr,
                               MEMTXATTRS_UNSPECIFIED, buf,
                               out_len) != MEMTX_OK) {
            return;
        }
    }

    /* Write data to the dTD buffer. */
    if (out_len > 0) {
        usb_dtd_buffer_write(dtd, buf, out_len, &bytes_transferred);
    } else {
        bytes_transferred = 0;
    }

    /* Update dTD status: clear active, set bytes transferred. */
    dtd[1] &= ~DTD_STATUS_ACTIVE;
    dtd[1] = (dtd[1] & ~DTD_TOTAL_BYTES_MASK) |
             (bytes_transferred << DTD_TOTAL_BYTES_SHIFT);
    usb_dtd_write(dtd_addr, dtd);

    /* Set ENDPTCOMPLETE (RX bit = ep) and clear ENDPTSTAT. */
    s->endptcomplete |= (1U << ep);
    s->endptstat &= ~(1U << ep);
    s->usbsts |= USBSTS_UI;
    usb_update_irq(s);
}

/*
 * Inject a SETUP packet on the given endpoint.
 * Writes 8 bytes to the dQH setup buffer and sets ENDPTSETUPSTAT.
 */
static void usb_device_setup_inject(STMP3770USBState *s, unsigned int ep)
{
    hwaddr qh_addr = usb_dqh_addr(s, ep, false);
    uint32_t setup[2];

    setup[0] = s->vh_setup_lo;
    setup[1] = s->vh_setup_hi;

    /* Write SETUP data to QH setup buffer (offset 0x2C in QH). */
    usb_mem_write32(qh_addr + 0x2C, setup[0]);
    usb_mem_write32(qh_addr + 0x30, setup[1]);

    /* Set ENDPTSETUPSTAT for this endpoint. */
    s->endptsetupstat |= (1U << ep);
    s->usbsts |= USBSTS_UI;
    usb_update_irq(s);
}

/*
 * Process a virtual host action (triggered by writing to REG_VH_ACTION).
 */
static void usb_vh_process_action(STMP3770USBState *s, uint32_t action)
{
    unsigned int ep = action & 0x1F;
    unsigned int type = (action >> VH_ACTION_TYPE_SHIFT) & 0x7;

    if (ep >= STMP3770_USB_NUM_ENDPOINTS) {
        return;
    }

    /* Only valid in device mode. */
    if (usb_host_mode(s)) {
        return;
    }

    switch (type) {
    case VH_ACTION_SETUP:
        usb_device_setup_inject(s, ep);
        break;
    case VH_ACTION_IN:
        s->vh_in_status = 0;
        usb_device_in_transfer(s, ep);
        break;
    case VH_ACTION_OUT:
        usb_device_out_transfer(s, ep);
        break;
    default:
        break;
    }
}

static void usb_update_irq(STMP3770USBState *s)
{
    uint32_t usb_irq = s->usbsts & s->usbintr & USBINTR_WRITABLE_MASK;
    uint32_t otgsc_status = (s->otgsc & OTGSC_STATUS_MASK) >> 16;
    uint32_t otgsc_en = (s->otgsc & OTGSC_EN_MASK) >> 24;
    uint32_t otgsc_irq = (otgsc_status & otgsc_en) & 0x7FU;

    qemu_set_irq(s->irq, (usb_irq | otgsc_irq) != 0);
}

static bool usb_host_mode(const STMP3770USBState *s)
{
    return (s->usbmode & 0x3) == 0x3;
}

static void usb_update_frindex_timer(STMP3770USBState *s)
{
    ptimer_transaction_begin(s->frindex_ptimer);
    if (usb_host_mode(s) && (s->usbcmd & USBCMD_RUN)) {
        ptimer_run(s->frindex_ptimer, 0);
    } else {
        ptimer_stop(s->frindex_ptimer);
    }
    ptimer_transaction_commit(s->frindex_ptimer);
}

static void usb_frindex_tick(void *opaque)
{
    STMP3770USBState *s = opaque;
    uint32_t next = (s->frindex + 1) & FRINDEX_MASK;

    if (next == 0) {
        s->usbsts |= USBSTS_FRI;
    }
    s->frindex = next;
    s->usbsts |= USBSTS_SRI;
    usb_update_irq(s);
}

static void usb_port_reset_timeout(void *opaque)
{
    STMP3770USBState *s = opaque;

    s->portsc1 &= ~PORTSC1_PR;
    s->portsc1 |= PORTSC1_PE | PORTSC1_PEC;

    if (s->portsc1 & PORTSC1_PFSC) {
        s->portsc1 &= ~(PORTSC1_PSPD | PORTSC1_HSP);
    } else {
        s->portsc1 = (s->portsc1 & ~PORTSC1_PSPD) |
                     PORTSC1_PSPD_HIGH | PORTSC1_HSP;
    }

    s->usbsts |= USBSTS_PCI;
    usb_update_irq(s);
}

static void usb_otgsc_1ms_tick(void *opaque)
{
    STMP3770USBState *s = opaque;

    s->otgsc ^= OTGSC_ONEMST;
    s->otgsc |= OTGSC_ONEMSS;
    usb_update_irq(s);
}

static void usb_gptimer_configure(STMP3770USBState *s, unsigned int idx,
                                  bool reload)
{
    ptimer_state *ptimer = s->gptimer_ptimer[idx];
    uint32_t control = s->gptimer[idx];
    uint64_t count;

    ptimer_transaction_begin(ptimer);
    if (!(control & GPTIMER_RUN)) {
        ptimer_stop(ptimer);
    }
    if (reload) {
        ptimer_set_limit(ptimer, (uint64_t)s->gptimer_load[idx] + 1, 1);
        ptimer_set_count(ptimer, (uint64_t)s->gptimer_load[idx] + 1);
    }
    if (control & GPTIMER_RUN) {
        count = ptimer_get_count(ptimer);
        if (reload || count != 0) {
            ptimer_run(ptimer, (control & GPTIMER_REPEAT) == 0);
        } else {
            s->gptimer[idx] &= ~GPTIMER_RUN;
        }
    }
    ptimer_transaction_commit(ptimer);
}

static void usb_gptimer_tick(void *opaque)
{
    STMP3770USBGPTimerCBInfo *info = opaque;
    STMP3770USBState *s = info->s;
    unsigned int idx = info->idx;

    s->usbsts |= USBSTS_GPTIMER0 << idx;
    if (!(s->gptimer[idx] & GPTIMER_REPEAT)) {
        s->gptimer[idx] &= ~GPTIMER_RUN;
    }
    usb_update_irq(s);
}

static void usb_controller_reset(STMP3770USBState *s)
{
    s->usbcmd = USBCTRL_USBCMD_DEVICE_RESET;
    s->usbsts = 0;
    s->usbintr = 0;
    s->frindex = 0;
    s->device_addr = 0;
    s->device_addr_staged = 0;
    s->endptlistaddr = 0;
    s->asynclistaddr = 0;
    s->ttctrl = 0;
    s->burstsize = USBCTRL_BURSTSIZE_RESET;
    s->txfilltuning = 0;
    s->endptnak = 0;
    s->endptnaken = 0;
    s->portsc1 = 0;
    s->otgsc = USBCTRL_OTGSC_DEVICE_RESET;
    s->usbmode = 0;
    s->usbmode_written = false;
    s->endptsetupstat = 0;
    s->endptprime = 0;
    s->endptflush = 0;
    s->endptstat = 0;
    s->endptcomplete = 0;
    memset(s->endptctrl, 0, sizeof(s->endptctrl));
    s->endptctrl[0] = USBCTRL_ENDPTCTRL0_RESET;
    memset(s->gptimer_load, 0, sizeof(s->gptimer_load));
    memset(s->gptimer, 0, sizeof(s->gptimer));
    for (unsigned int i = 0; i < 2; i++) {
        ptimer_transaction_begin(s->gptimer_ptimer[i]);
        ptimer_stop(s->gptimer_ptimer[i]);
        ptimer_set_limit(s->gptimer_ptimer[i], 0, 1);
        ptimer_transaction_commit(s->gptimer_ptimer[i]);
    }

    ptimer_transaction_begin(s->port_reset_ptimer);
    ptimer_stop(s->port_reset_ptimer);
    ptimer_transaction_commit(s->port_reset_ptimer);

    ptimer_transaction_begin(s->otgsc_1ms_ptimer);
    ptimer_stop(s->otgsc_1ms_ptimer);
    ptimer_set_limit(s->otgsc_1ms_ptimer, 1000, 1);
    ptimer_run(s->otgsc_1ms_ptimer, 0);
    ptimer_transaction_commit(s->otgsc_1ms_ptimer);

    ptimer_transaction_begin(s->frindex_ptimer);
    ptimer_stop(s->frindex_ptimer);
    ptimer_transaction_commit(s->frindex_ptimer);

    s->vh_setup_lo = 0;
    s->vh_setup_hi = 0;
    s->vh_out_addr = 0;
    s->vh_out_len = 0;
    s->vh_in_status = 0;
}

static uint64_t usb_read(void *opaque, hwaddr offset, unsigned size)
{
    STMP3770USBState *s = opaque;

    switch (offset) {
    case REG_CAPLENGTH:
        if (size == 1) {
            return 0x40;
        }
        break;
    case REG_HCIVERSION:
        if (size == 2) {
            return 0x0100;
        }
        break;
    case REG_DCIVERSION:
        if (size == 2) {
            return 0x0001;
        }
        break;
    default:
        if (size == 4) {
            break;
        }
        /* Fall through to the common invalid-access report below. */
        break;
    }

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-usb: unsupported read size %u at offset "
                      HWADDR_FMT_plx "\n", size, offset);
        return 0;
    }

    switch (offset) {
    case REG_ID:
        return USBCTRL_ID_RESET;
    case REG_HWGENERAL:
        return USBCTRL_ARC_GENERAL_RESET;
    case REG_HWHOST:
        return USBCTRL_HWHOST_RESET;
    case REG_HWDEVICE:
        return USBCTRL_HWDEVICE_RESET;
    case REG_HWTXBUF:
        return USBCTRL_HWTXBUF_RESET;
    case REG_HWRXBUF:
        return USBCTRL_HWRXBUF_RESET;
    case REG_GPTIMER0LD:
    case REG_GPTIMER1LD:
        return s->gptimer_load[(offset - REG_GPTIMER0LD) / 8];
    case REG_GPTIMER0CTRL:
    case REG_GPTIMER1CTRL:
    {
        unsigned int idx = (offset - REG_GPTIMER0CTRL) / 8;
        uint32_t count;

        ptimer_transaction_begin(s->gptimer_ptimer[idx]);
        count = ptimer_get_count(s->gptimer_ptimer[idx]);
        ptimer_transaction_commit(s->gptimer_ptimer[idx]);
        return (s->gptimer[idx] & (GPTIMER_RUN | GPTIMER_REPEAT)) |
               ((count ? count - 1 : 0) & GPTIMER_COUNT_MASK);
    }
    case REG_SBUSCFG:
        return 0;
    case REG_HCSPARAMS:
        return USBCTRL_HCSPARAMS_RESET;
    case REG_HCCPARAMS:
        return USBCTRL_HCCPARAMS_RESET;
    case REG_DCCPARAMS:
        return USBCTRL_DCCPARAMS_RESET;
    case REG_USBCMD:
        return s->usbcmd;
    case REG_USBSTS:
    {
        uint32_t v = s->usbsts;
        if (usb_host_mode(s)) {
            if (s->usbcmd & USBCMD_RUN) {
                v &= ~USBSTS_HCH;
                if (s->usbcmd & USBCMD_PSE) {
                    v |= USBSTS_PS;
                } else {
                    v &= ~USBSTS_PS;
                }
                if (s->usbcmd & USBCMD_ASE) {
                    v |= USBSTS_AS;
                } else {
                    v &= ~USBSTS_AS;
                }
            } else {
                v |= USBSTS_HCH;
                v &= ~(USBSTS_PS | USBSTS_AS);
            }
        } else {
            v &= ~(USBSTS_HCH | USBSTS_PS | USBSTS_AS | USBSTS_RCL);
        }
        return v;
    }
    case REG_USBINTR:
        return s->usbintr;
    case REG_FRINDEX:
        return s->frindex;
    case REG_DEVICEADDR:
        return s->device_addr;
    case REG_ENDPTLISTADDR:
        return s->endptlistaddr;
    case REG_TTCTRL:
        return s->ttctrl;
    case REG_BURSTSIZE:
        return s->burstsize;
    case REG_TXFILLTUNING:
        return s->txfilltuning;
    case REG_ULPIVIEWPORT:
        return 0;
    case REG_ENDPTNAK:
        return s->endptnak;
    case REG_ENDPTNAKEN:
        return s->endptnaken;
    case REG_CONFIGFLAG:
        return 0;
    case REG_PORTSC1:
    {
        uint32_t v = s->portsc1 & PORTSC1_KNOWN_MASK;
        if ((v & PORTSC1_PP) == 0) {
            v &= ~PORTSC1_PR;
        }
        return v;
    }
    case REG_OTGSC:
        return s->otgsc & OTGSC_KNOWN_MASK;
    case REG_USBMODE:
        return s->usbmode;
    case REG_ENDPTSETUPSTAT:
        return s->endptsetupstat;
    case REG_ENDPTPRIME:
        return s->endptprime;
    case REG_ENDPTFLUSH:
        return s->endptflush;
    case REG_ENDPTSTAT:
        return s->endptstat;
    case REG_ENDPTCOMPLETE:
        return s->endptcomplete;
    case REG_ENDPTCTRL0 ... REG_ENDPTCTRL4:
        return s->endptctrl[(offset - REG_ENDPTCTRL0) / 4];
    case REG_VH_SETUP_LO:
        return s->vh_setup_lo;
    case REG_VH_SETUP_HI:
        return s->vh_setup_hi;
    case REG_VH_OUT_ADDR:
        return s->vh_out_addr;
    case REG_VH_OUT_LEN:
        return s->vh_out_len;
    case REG_VH_IN_STATUS:
        return s->vh_in_status;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-usb: read from unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        return 0;
    }
}

static void usb_write(void *opaque, hwaddr offset,
                      uint64_t value, unsigned size)
{
    STMP3770USBState *s = opaque;

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-usb: unsupported write size %u at offset "
                      HWADDR_FMT_plx "\n", size, offset);
        return;
    }

    switch (offset) {
    case REG_USBCMD:
        s->usbcmd = (uint32_t)value & USBCMD_WRITABLE_MASK;
        if (s->usbcmd & USBCMD_RST) {
            usb_controller_reset(s);
        }
        /*
         * IAA doorbell (host only): software writes 1 to USBCMD.IAA to
         * request an interrupt on the next async schedule advance.  Since
         * the async schedule is not modelled, immediately set USBSTS.AAI
         * and self-clear USBCMD.IAA.
         */
        if (s->usbcmd & USBCMD_IAA) {
            s->usbcmd &= ~USBCMD_IAA;
            if (usb_host_mode(s)) {
                s->usbsts |= USBSTS_AAI;
            }
        }
        usb_update_frindex_timer(s);
        usb_update_irq(s);
        break;
    case REG_USBSTS:
        /* Write-1-to-clear */
        s->usbsts &= ~((uint32_t)value & USBSTS_W1C_MASK);
        usb_update_irq(s);
        break;
    case REG_USBINTR:
        s->usbintr = (uint32_t)value & USBCTRL_USBINTR_WRITABLE_MASK;
        usb_update_irq(s);
        break;
    case REG_FRINDEX:
        if (usb_host_mode(s)) {
            s->frindex = (uint32_t)value & FRINDEX_MASK;
        }
        break;
    case REG_DEVICEADDR:
        if (usb_host_mode(s)) {
            /* Host mode: PERIODICLISTBASE - bits 31:12 writable */
            s->device_addr = (uint32_t)value &
                             USBCTRL_PERIODICLISTBASE_WRITABLE_MASK;
        } else {
            /*
             * Device mode: DEVICEADDR - USBADR[31:25] + USBADRA[24].
             * USBADRA staged-address mechanism:
             *   USBADRA=0: USBADR updates immediately.
             *   USBADRA=1: USBADR staged in hidden register, loaded on
             *              next IN ACK on EP0; cleared on OUT/SETUP or reset.
             */
            uint32_t val = (uint32_t)value & USBCTRL_DEVICEADDR_WRITABLE_MASK;
            if (val & DEVICEADDR_USBADRA) {
                /* Stage the new USBADR, keep old device_addr visible */
                s->device_addr_staged = val & ~DEVICEADDR_USBADRA;
                s->device_addr = (s->device_addr & ~DEVICEADDR_USBADRA) |
                                 DEVICEADDR_USBADRA;
            } else {
                s->device_addr_staged = 0;
                s->device_addr = val;
            }
        }
        break;
    case REG_ENDPTLISTADDR:
        if (usb_host_mode(s)) {
            /* Host mode: ASYNCLISTADDR - bits 31:5 writable */
            s->endptlistaddr = (uint32_t)value &
                               USBCTRL_ASYNCLISTADDR_WRITABLE_MASK;
        } else {
            /* Device mode: ENDPTLISTADDR - bits 31:11 writable */
            s->endptlistaddr = (uint32_t)value &
                               USBCTRL_ENDPTLISTADDR_WRITABLE_MASK;
        }
        break;
    case REG_TTCTRL:
        s->ttctrl = (uint32_t)value & USBCTRL_TTCTRL_WRITABLE_MASK;
        break;
    case REG_BURSTSIZE:
        s->burstsize = (uint32_t)value & USBCTRL_BURSTSIZE_WRITABLE_MASK;
        break;
    case REG_TXFILLTUNING:
        s->txfilltuning = (uint32_t)value & USBCTRL_TXFILLTUNING_WRITABLE_MASK;
        break;
    case REG_ENDPTNAK:
        /* Write-1-to-clear */
        s->endptnak &= ~((uint32_t)value & USBCTRL_ENDPOINT_BITMAP_MASK);
        break;
    case REG_ENDPTNAKEN:
        s->endptnaken = (uint32_t)value & USBCTRL_ENDPOINT_BITMAP_MASK;
        break;
    case REG_PORTSC1:
    {
        uint32_t val = (uint32_t)value;
        uint32_t writable = PORTSC1_ALWAYS_RW;
        uint32_t old_portsc1 = s->portsc1;
        bool old_pe = (old_portsc1 & PORTSC1_PE) != 0;
        bool old_pr = (old_portsc1 & PORTSC1_PR) != 0;

        if ((s->usbmode & 0x3) == 0x3) {
            /* Host mode: PP/PR/SUSP/PE are writable. */
            writable |= PORTSC1_HOST_RW;
        }

        /* Update all writable, non-W1C fields. */
        s->portsc1 &= ~writable;
        s->portsc1 |= val & writable;

        /* PEC is set on a PE transition (0->1 or 1->0). */
        bool new_pe = (s->portsc1 & PORTSC1_PE) != 0;
        if (old_pe != new_pe) {
            s->portsc1 |= PORTSC1_PEC;
        }

        /* W1C: writing 1 clears CSC/PEC/OCC. */
        s->portsc1 &= ~(val & PORTSC1_W1C_MASK);

        s->portsc1 &= PORTSC1_KNOWN_MASK;

        /* PR is only meaningful when port power is on. */
        if ((s->portsc1 & PORTSC1_PP) == 0) {
            s->portsc1 &= ~PORTSC1_PR;
        }

        bool new_pr = (s->portsc1 & PORTSC1_PR) != 0;
        if (!old_pr && new_pr) {
            ptimer_transaction_begin(s->port_reset_ptimer);
            ptimer_set_limit(s->port_reset_ptimer, 10000, 1);
            ptimer_run(s->port_reset_ptimer, 1);
            ptimer_transaction_commit(s->port_reset_ptimer);
        } else if (old_pr && !new_pr) {
            ptimer_transaction_begin(s->port_reset_ptimer);
            ptimer_stop(s->port_reset_ptimer);
            ptimer_transaction_commit(s->port_reset_ptimer);
        }
        break;
    }
    case REG_OTGSC:
    {
        uint32_t val = (uint32_t)value;

        /* Status bits (22:16) are write-1-to-clear. */
        s->otgsc &= ~(val & OTGSC_STATUS_MASK);

        /* Enable (30:24) and control (7:0) fields are fully writable. */
        s->otgsc &= ~(OTGSC_EN_MASK | OTGSC_CTRL_MASK);
        s->otgsc |= val & (OTGSC_EN_MASK | OTGSC_CTRL_MASK);

        s->otgsc &= OTGSC_KNOWN_MASK;
        usb_update_irq(s);
        break;
    }
    case REG_USBMODE:
        if (!s->usbmode_written) {
            s->usbmode = (uint32_t)value & USBCTRL_USBMODE_WRITABLE_MASK;
            s->usbmode_written = true;
            usb_update_frindex_timer(s);
        }
        break;
    case REG_ENDPTSETUPSTAT:
        s->endptsetupstat &= ~((uint32_t)value & USBCTRL_ENDPTSETUP_MASK);
        break;
    case REG_ENDPTPRIME:
        /*
         * Priming an endpoint prepares it for a transfer.  In real hardware
         * the controller reads the dQH/dTD from memory and sets ENDPTSTAT
         * when the endpoint is ready.  We set ENDPTSTAT immediately (the
         * dQH/dTD is parsed on demand when a transfer is triggered by the
         * virtual host).  ENDPTPRIME self-clears.
         */
        s->endptprime = 0;
        s->endptstat |= (uint32_t)value & USBCTRL_ENDPOINT_BITMAP_MASK;
        break;
    case REG_ENDPTFLUSH:
        s->endptflush = 0;
        s->endptstat &= ~((uint32_t)value & USBCTRL_ENDPOINT_BITMAP_MASK);
        break;
    case REG_ENDPTCOMPLETE:
        s->endptcomplete &= ~((uint32_t)value & USBCTRL_ENDPOINT_BITMAP_MASK);
        break;
    case REG_ENDPTCTRL0:
        s->endptctrl[0] = USBCTRL_ENDPTCTRL0_RESET |
                          ((uint32_t)value & USBCTRL_ENDPTCTRL0_WRITABLE_MASK);
        break;
    case REG_ENDPTCTRL1 ... REG_ENDPTCTRL4:
    {
        uint32_t idx = (offset - REG_ENDPTCTRL0) / 4;
        uint32_t val = (uint32_t)value & USBCTRL_ENDPTCTRLN_WRITABLE_MASK;

        /* TXR/RXR are self-clearing and reset the TXD/RXD data toggle. */
        if (val & ENDPTCTRLN_TXR) {
            val &= ~(ENDPTCTRLN_TXR | ENDPTCTRLN_TXD);
        }
        if (val & ENDPTCTRLN_RXR) {
            val &= ~(ENDPTCTRLN_RXR | ENDPTCTRLN_RXD);
        }

        s->endptctrl[idx] = val;
        break;
    }
    case REG_GPTIMER0LD:
    case REG_GPTIMER1LD:
        s->gptimer_load[(offset - REG_GPTIMER0LD) / 8] =
            (uint32_t)value & GPTIMER_COUNT_MASK;
        break;
    case REG_GPTIMER0CTRL:
    case REG_GPTIMER1CTRL:
    {
        unsigned int idx = (offset - REG_GPTIMER0CTRL) / 8;
        uint32_t control = (uint32_t)value;
        bool reload = (control & GPTIMER_RESET) != 0;

        s->gptimer[idx] = control &
                          (GPTIMER_CONTROL_WRITABLE_MASK & ~GPTIMER_RESET);
        usb_gptimer_configure(s, idx, reload);
        break;
    }
    case REG_VH_SETUP_LO:
        s->vh_setup_lo = (uint32_t)value;
        break;
    case REG_VH_SETUP_HI:
        s->vh_setup_hi = (uint32_t)value;
        break;
    case REG_VH_OUT_ADDR:
        s->vh_out_addr = (uint32_t)value;
        break;
    case REG_VH_OUT_LEN:
        s->vh_out_len = (uint32_t)value;
        break;
    case REG_VH_ACTION:
        usb_vh_process_action(s, (uint32_t)value);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "stmp3770-usb: write to unimplemented offset "
                      HWADDR_FMT_plx "\n", offset);
        break;
    }
}

static const MemoryRegionOps usb_ops = {
    .read = usb_read,
    .write = usb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void usb_reset(DeviceState *dev)
{
    STMP3770USBState *s = STMP3770_USB(dev);

    usb_controller_reset(s);

    usb_update_irq(s);
}

static void usb_realize(DeviceState *dev, Error **errp)
{
    STMP3770USBState *s = STMP3770_USB(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &usb_ops, s,
                          TYPE_STMP3770_USB, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

}

static void usb_finalize(Object *obj)
{
    STMP3770USBState *s = STMP3770_USB(obj);

    for (unsigned int i = 0; i < 2; i++) {
        ptimer_free(s->gptimer_ptimer[i]);
    }
    g_free(s->gptimer_cb_info);
    ptimer_free(s->port_reset_ptimer);
    ptimer_free(s->otgsc_1ms_ptimer);
    ptimer_free(s->frindex_ptimer);
}

static const VMStateDescription vmstate_usb = {
    .name = "stmp3770-usb",
    .version_id = 8,
    .minimum_version_id = 8,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(usbcmd, STMP3770USBState),
        VMSTATE_UINT32(usbsts, STMP3770USBState),
        VMSTATE_UINT32(usbintr, STMP3770USBState),
        VMSTATE_UINT32(frindex, STMP3770USBState),
        VMSTATE_UINT32(device_addr, STMP3770USBState),
        VMSTATE_UINT32_V(device_addr_staged, STMP3770USBState, 7),
        VMSTATE_UINT32(endptlistaddr, STMP3770USBState),
        VMSTATE_UINT32(asynclistaddr, STMP3770USBState),
        VMSTATE_UINT32_V(ttctrl, STMP3770USBState, 2),
        VMSTATE_UINT32(burstsize, STMP3770USBState),
        VMSTATE_UINT32(txfilltuning, STMP3770USBState),
        VMSTATE_UINT32(endptnak, STMP3770USBState),
        VMSTATE_UINT32(endptnaken, STMP3770USBState),
        VMSTATE_UINT32(portsc1, STMP3770USBState),
        VMSTATE_UINT32(otgsc, STMP3770USBState),
        VMSTATE_UINT32(usbmode, STMP3770USBState),
        VMSTATE_BOOL_V(usbmode_written, STMP3770USBState, 2),
        VMSTATE_UINT32(endptsetupstat, STMP3770USBState),
        VMSTATE_UINT32(endptprime, STMP3770USBState),
        VMSTATE_UINT32(endptflush, STMP3770USBState),
        VMSTATE_UINT32(endptstat, STMP3770USBState),
        VMSTATE_UINT32(endptcomplete, STMP3770USBState),
        VMSTATE_UINT32_ARRAY(endptctrl, STMP3770USBState,
                             STMP3770_USB_MIGRATION_ENDPOINTS),
        VMSTATE_UINT32_ARRAY_V(gptimer_load, STMP3770USBState, 2, 4),
        VMSTATE_UINT32_ARRAY(gptimer, STMP3770USBState, 2),
        VMSTATE_ARRAY_OF_POINTER_TO_STRUCT(gptimer_ptimer,
                                           STMP3770USBState, 2, 4,
                                           vmstate_ptimer, ptimer_state),
        VMSTATE_PTIMER(port_reset_ptimer, STMP3770USBState),
        VMSTATE_PTIMER(otgsc_1ms_ptimer, STMP3770USBState),
        VMSTATE_PTIMER(frindex_ptimer, STMP3770USBState),
        VMSTATE_UINT32_V(vh_setup_lo, STMP3770USBState, 8),
        VMSTATE_UINT32_V(vh_setup_hi, STMP3770USBState, 8),
        VMSTATE_UINT32_V(vh_out_addr, STMP3770USBState, 8),
        VMSTATE_UINT32_V(vh_out_len, STMP3770USBState, 8),
        VMSTATE_UINT32_V(vh_in_status, STMP3770USBState, 8),
        VMSTATE_END_OF_LIST()
    }
};

static void usb_init(Object *obj)
{
    STMP3770USBState *s = STMP3770_USB(obj);

    s->gptimer_cb_info = g_new0(STMP3770USBGPTimerCBInfo, 2);
    for (unsigned int i = 0; i < 2; i++) {
        s->gptimer_cb_info[i].s = s;
        s->gptimer_cb_info[i].idx = i;
        s->gptimer_ptimer[i] = ptimer_init(usb_gptimer_tick,
                                           &s->gptimer_cb_info[i],
                                           PTIMER_POLICY_NO_COUNTER_ROUND_DOWN |
                                           PTIMER_POLICY_TRIGGER_ONLY_ON_DECREMENT);
        ptimer_transaction_begin(s->gptimer_ptimer[i]);
        ptimer_set_freq(s->gptimer_ptimer[i], 1000000);
        ptimer_set_limit(s->gptimer_ptimer[i], 0, 1);
        ptimer_transaction_commit(s->gptimer_ptimer[i]);
    }

    s->port_reset_ptimer = ptimer_init(usb_port_reset_timeout, s,
                                       PTIMER_POLICY_NO_COUNTER_ROUND_DOWN |
                                       PTIMER_POLICY_TRIGGER_ONLY_ON_DECREMENT);
    ptimer_transaction_begin(s->port_reset_ptimer);
    ptimer_set_freq(s->port_reset_ptimer, 1000000);
    ptimer_set_limit(s->port_reset_ptimer, 0, 1);
    ptimer_transaction_commit(s->port_reset_ptimer);

    s->otgsc_1ms_ptimer = ptimer_init(usb_otgsc_1ms_tick, s,
                                      PTIMER_POLICY_NO_COUNTER_ROUND_DOWN |
                                      PTIMER_POLICY_TRIGGER_ONLY_ON_DECREMENT);
    ptimer_transaction_begin(s->otgsc_1ms_ptimer);
    ptimer_set_freq(s->otgsc_1ms_ptimer, 1000000);
    ptimer_set_limit(s->otgsc_1ms_ptimer, 0, 1);
    ptimer_transaction_commit(s->otgsc_1ms_ptimer);

    s->frindex_ptimer = ptimer_init(usb_frindex_tick, s,
                                    PTIMER_POLICY_NO_COUNTER_ROUND_DOWN |
                                    PTIMER_POLICY_TRIGGER_ONLY_ON_DECREMENT);
    ptimer_transaction_begin(s->frindex_ptimer);
    ptimer_set_freq(s->frindex_ptimer, 1000000);
    ptimer_set_limit(s->frindex_ptimer, FRINDEX_PERIOD_US, 1);
    ptimer_transaction_commit(s->frindex_ptimer);

    usb_controller_reset(s);
}

static void usb_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = usb_realize;
    device_class_set_legacy_reset(dc, usb_reset);
    dc->vmsd = &vmstate_usb;
}

static const TypeInfo usb_type_info = {
    .name          = TYPE_STMP3770_USB,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STMP3770USBState),
    .instance_init = usb_init,
    .instance_finalize = usb_finalize,
    .class_init    = usb_class_init,
};

static void usb_register_types(void)
{
    type_register_static(&usb_type_info);
}

type_init(usb_register_types)
