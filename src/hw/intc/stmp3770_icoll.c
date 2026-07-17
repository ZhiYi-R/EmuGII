/*
 * STMP3770 Interrupt Collector (ICOLL)
 *
 * Based on STMP3770 Reference Manual Chapter 5
 *
 * Features:
 * - 64 interrupt sources with 4-level priority (0-3)
 * - Vectorized interrupt handling
 * - 8 sources (28-35) can be routed to FIQ
 * - Per-source enable and software trigger
 * - Nested interrupt support
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
#include "qemu/timer.h"
#include "hw/intc/stmp3770_icoll.h"

/* Register offsets */
#define REG_VECTOR          0x000
#define REG_LEVELACK        0x010
#define REG_CTRL            0x020
#define REG_STAT            0x030
#define REG_RAW0            0x040
#define REG_RAW1            0x050
#define REG_PRIORITY0       0x060
#define REG_PRIORITY1       0x070
#define REG_PRIORITY2       0x080
#define REG_PRIORITY3       0x090
#define REG_PRIORITY4       0x0A0
#define REG_PRIORITY5       0x0B0
#define REG_PRIORITY6       0x0C0
#define REG_PRIORITY7       0x0D0
#define REG_PRIORITY8       0x0E0
#define REG_PRIORITY9       0x0F0
#define REG_PRIORITY10      0x100
#define REG_PRIORITY11      0x110
#define REG_PRIORITY12      0x120
#define REG_PRIORITY13      0x130
#define REG_PRIORITY14      0x140
#define REG_PRIORITY15      0x150
#define REG_VBASE           0x160
#define REG_DEBUG           0x170
#define REG_DEBUGRD0        0x180
#define REG_DEBUGRD1        0x190
#define REG_DEBUGFLAG       0x1A0
#define REG_DEBUGRDREQ0     0x1B0
#define REG_DEBUGRDREQ1     0x1C0
#define REG_VERSION         0x1D0

/* Register SET/CLR/TOG offsets */
#define REG_SET             0x4
#define REG_CLR             0x8
#define REG_TOG             0xC

/* CTRL register bits (STMP3770 ref: SFTRST/CLKGATE upper, pitch/final enables mid) */
#define CTRL_SFTRST             (1U << 31)
#define CTRL_CLKGATE            (1U << 30)
#define CTRL_VECTOR_PITCH_MASK  0x00E00000
#define CTRL_VECTOR_PITCH_SHIFT 21
#define CTRL_BYPASS_FSM         (1U << 20)
#define CTRL_NO_NESTING         (1U << 19)
#define CTRL_ARM_RSE_MODE       (1U << 18)
#define CTRL_FIQ_FINAL_ENABLE   (1U << 17)
#define CTRL_IRQ_FINAL_ENABLE   (1U << 16)
#define CTRL_FIQ_ENABLE_MASK    0x000000FFU
#define CTRL_RW_MASK            0xC0FF00FFU

#define PRIORITY_RW_MASK        0x0F0F0F0FU
#define VBASE_RW_MASK           0xFFFFFFFCU
#define ICOLL_VERSION           0x02000000U
#define ICOLL_DEBUGRD0          0xECA94567U
#define ICOLL_DEBUGRD1          0x1356DA98U

/* Normal IRQ FSM states (DEBUG VECTOR_FSM field values from Chapter 5, Table 86) */
enum {
    ICOLL_FSM_IDLE = 0x000,
    ICOLL_FSM_MULTICYCLE1 = 0x001,
    ICOLL_FSM_MULTICYCLE2 = 0x002,
    ICOLL_FSM_PENDING = 0x004,
    ICOLL_FSM_ISR_RUNNING = 0x020,
};

/* APBH is synchronous with HCLK; cold reset leaves HCLK at 24 MHz. */
#define ICOLL_RESET_CLOCK_HZ           24000000ULL
#define ICOLL_SFTRST_CLOCKS            4ULL
#define ICOLL_SFTRST_DELAY_NS          \
    ((ICOLL_SFTRST_CLOCKS * NANOSECONDS_PER_SECOND + \
      ICOLL_RESET_CLOCK_HZ - 1) / ICOLL_RESET_CLOCK_HZ)
#define ICOLL_MULTICYCLE_CLOCKS        1ULL

/* STAT register bits */
#define STAT_VECTOR_NUMBER_MASK 0x3F

#define TYPE_STMP3770_ICOLL "stmp3770-icoll"

struct STMP3770ICOLLState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;               /* IRQ output to CPU */
    qemu_irq fiq;               /* FIQ output to CPU */
    qemu_irq icoll_busy;        /* OR of all enabled IRQ requests to clock control */

    /* Registers */
    uint32_t ctrl;
    uint32_t vbase;
    /* Priority registers - one per 4 interrupt sources */
    /* Each priority reg contains: ENABLE[3:0], PRIORITY[3:0] for 4 sources */
    uint32_t priority[16];      /* PRIORITY0-15, covers all 64 sources */

    /* Raw interrupt status */
    uint64_t raw_status;        /* Bits 0-63: raw interrupt inputs */
    uint64_t request_holding;   /* Snapshot sampled by the normal IRQ FSM */

    /* In-service tracking for nested interrupts */
    uint32_t level_active[4];   /* In-service source number plus one */
    uint32_t current_level;     /* Current service level */

    /* FIQ enable for sources 28-35 */
    uint8_t fiq_enable;         /* 8 bits for sources 28-35 */
    uint32_t debugflag;

    /* Highest priority pending IRQ vector number */
    uint32_t vector;
    bool vector_pending;

    /* Normal IRQ FSM state and pending vector during the two HCLK delay */
    uint32_t fsm_state;
    uint32_t selected_vector;
    uint32_t saved_vector;

    /* APBH/HCLK rate for soft-reset and multicycle FSM delays */
    uint32_t hclk_hz;

    QEMUTimer *sftrst_timer;
    QEMUTimer *multicycle_timer;
};

static uint64_t stmp3770_icoll_sftrst_delay_ns(const STMP3770ICOLLState *s)
{
    uint32_t hz = s->hclk_hz ? s->hclk_hz : ICOLL_RESET_CLOCK_HZ;

    return (ICOLL_SFTRST_CLOCKS * NANOSECONDS_PER_SECOND + hz - 1) / hz;
}

void stmp3770_icoll_set_hclk_rate(void *opaque, uint32_t hclk_hz)
{
    STMP3770ICOLLState *s = STMP3770_ICOLL(opaque);

    s->hclk_hz = hclk_hz ? hclk_hz : ICOLL_RESET_CLOCK_HZ;
}

static uint64_t stmp3770_icoll_multicycle_step_ns(const STMP3770ICOLLState *s)
{
    uint32_t hz = s->hclk_hz ? s->hclk_hz : ICOLL_RESET_CLOCK_HZ;

    return (ICOLL_MULTICYCLE_CLOCKS * NANOSECONDS_PER_SECOND + hz - 1) / hz;
}

static void stmp3770_icoll_multicycle_tick(void *opaque);

static void stmp3770_icoll_reset(DeviceState *dev);
static void stmp3770_icoll_update(STMP3770ICOLLState *s);

static void stmp3770_icoll_finish_soft_reset(void *opaque)
{
    STMP3770ICOLLState *s = STMP3770_ICOLL(opaque);

    stmp3770_icoll_reset(DEVICE(s));
    stmp3770_icoll_update(s);
}

static int stmp3770_icoll_highest_active_level(const STMP3770ICOLLState *s)
{
    int level;

    for (level = 3; level >= 0; level--) {
        if (s->level_active[level]) {
            return level;
        }
    }

    return -1;
}

static uint64_t stmp3770_icoll_live_requests(const STMP3770ICOLLState *s)
{
    uint64_t requests = s->raw_status;
    int source;

    for (source = 0; source < 64; source++) {
        uint32_t priority = s->priority[source / 4];
        uint32_t field = (source % 4) * 8;

        if ((priority >> (field + 3)) & 1) {
            requests |= 1ULL << source;
        }
    }

    return requests;
}

static uint64_t stmp3770_icoll_debug_requests(const STMP3770ICOLLState *s)
{
    if (s->ctrl & CTRL_BYPASS_FSM) {
        return stmp3770_icoll_live_requests(s);
    }

    return s->request_holding;
}

static bool stmp3770_icoll_source_routes_to_fiq(const STMP3770ICOLLState *s,
                                                 int source)
{
    return source >= 28 && source <= 35 &&
           ((s->fiq_enable >> (source - 28)) & 1);
}

static bool stmp3770_icoll_fiq_pending(const STMP3770ICOLLState *s)
{
    uint64_t requests = stmp3770_icoll_live_requests(s);
    int source;

    for (source = 28; source <= 35; source++) {
        if (stmp3770_icoll_source_routes_to_fiq(s, source) &&
            (requests & (1ULL << source))) {
            return true;
        }
    }

    return false;
}

static void stmp3770_icoll_find_candidate(const STMP3770ICOLLState *s,
                                          uint64_t requests, int active_level,
                                          uint32_t *selected_vector,
                                          int *selected_level,
                                          bool *has_candidate)
{
    int i;

    *selected_vector = 0;
    *selected_level = -1;
    *has_candidate = false;

    for (i = 0; i < 64; i++) {
        bool active = (requests >> i) & 1;
        int pri_reg = i / 4;
        int pri_bit = (i % 4) * 8;
        uint32_t pri_val = s->priority[pri_reg];
        bool enabled = (pri_val >> (pri_bit + 2)) & 1;
        int level = (pri_val >> pri_bit) & 0x3;

        if (stmp3770_icoll_source_routes_to_fiq(s, i)) {
            continue;
        }
        if (!active || !enabled) {
            continue;
        }
        if (active_level >= 0 &&
            ((s->ctrl & CTRL_NO_NESTING) || level <= active_level)) {
            continue;
        }

        if (!*has_candidate || level > *selected_level ||
            (level == *selected_level && i > *selected_vector)) {
            *selected_vector = i;
            *selected_level = level;
        }
        *has_candidate = true;
    }
}

static uint32_t stmp3770_icoll_debug(const STMP3770ICOLLState *s)
{
    uint64_t requests = stmp3770_icoll_debug_requests(s);
    uint32_t inservice = 0;
    uint32_t requests_by_level = 0;
    uint32_t level_requests = 0;
    uint32_t vector_fsm = s->fsm_state;
    int source;

    for (int level = 0; level < 4; level++) {
        if (s->level_active[level]) {
            inservice |= 1U << level;
        }
    }

    for (source = 0; source < 64; source++) {
        uint32_t priority = s->priority[source / 4];
        uint32_t field = (source % 4) * 8;
        uint32_t level = (priority >> field) & 0x3;
        bool enabled = (priority >> (field + 2)) & 1;

        if ((requests & (1ULL << source)) && enabled &&
            !stmp3770_icoll_source_routes_to_fiq(s, source)) {
            requests_by_level |= 1U << level;
        }
    }

    if (s->fsm_state == ICOLL_FSM_PENDING ||
        s->fsm_state == ICOLL_FSM_ISR_RUNNING) {
        uint32_t priority = s->priority[s->vector / 4];
        uint32_t field = (s->vector % 4) * 8;

        level_requests = 1U << ((priority >> field) & 0x3);
    }

    return (inservice << 28) |
           (level_requests << 24) |
           (requests_by_level << 20) |
           ((s->ctrl & CTRL_FIQ_FINAL_ENABLE &&
             stmp3770_icoll_fiq_pending(s)) ? (1U << 17) : 0) |
           ((s->ctrl & CTRL_IRQ_FINAL_ENABLE && s->vector_pending) ?
            (1U << 16) : 0) |
           vector_fsm;
}

static void stmp3770_icoll_enter_service(STMP3770ICOLLState *s)
{
    uint32_t source = s->vector;
    uint32_t priority = s->priority[source / 4];
    uint32_t level = (priority >> ((source % 4) * 8)) & 0x3;

    if (!s->vector_pending) {
        return;
    }

    s->level_active[level] = source + 1;
    s->current_level = level;
    s->vector_pending = false;
}

static void stmp3770_icoll_write_ctrl(STMP3770ICOLLState *s, uint32_t val,
                                      bool is_set, bool is_clr, bool is_tog)
{
    uint32_t old_ctrl = s->ctrl;
    uint32_t next_ctrl;
    bool old_sftrst;
    bool next_sftrst;

    if (is_set) {
        next_ctrl = old_ctrl | val;
    } else if (is_clr) {
        next_ctrl = old_ctrl & ~val;
    } else if (is_tog) {
        next_ctrl = old_ctrl ^ val;
    } else {
        next_ctrl = val;
    }
    next_ctrl &= CTRL_RW_MASK;

    old_sftrst = old_ctrl & CTRL_SFTRST;
    next_sftrst = next_ctrl & CTRL_SFTRST;
    s->ctrl = next_ctrl;
    s->fiq_enable = s->ctrl & CTRL_FIQ_ENABLE_MASK;

    if (!(old_ctrl & CTRL_BYPASS_FSM) &&
        (s->ctrl & CTRL_BYPASS_FSM)) {
        s->vector_pending = false;
    }

    if (!next_sftrst) {
        timer_del(s->sftrst_timer);
        return;
    }

    /* The manual specifies that a simultaneous gate makes SFTRST ineffective. */
    if (s->ctrl & CTRL_CLKGATE) {
        timer_del(s->sftrst_timer);
        return;
    }

    if (!old_sftrst) {
        timer_mod(s->sftrst_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  stmp3770_icoll_sftrst_delay_ns(s));
    }
}

static bool stmp3770_icoll_busy_from(const STMP3770ICOLLState *s,
                                      uint64_t requests)
{
    int i;

    for (i = 0; i < 64; i++) {
        if (!(requests & (1ULL << i))) {
            continue;
        }
        if (stmp3770_icoll_source_routes_to_fiq(s, i)) {
            return true;
        }
        int pri_reg = i / 4;
        int pri_bit = (i % 4) * 8;
        if ((s->priority[pri_reg] >> (pri_bit + 2)) & 1) {
            return true;
        }
    }

    return false;
}

static void stmp3770_icoll_update(STMP3770ICOLLState *s)
{
    int active_level;
    uint32_t selected_vector;
    int selected_level;
    bool has_candidate;
    bool irq_enabled = s->ctrl & CTRL_IRQ_FINAL_ENABLE;
    bool fiq_enabled = s->ctrl & CTRL_FIQ_FINAL_ENABLE;
    uint64_t live = stmp3770_icoll_live_requests(s);
    bool busy = stmp3770_icoll_busy_from(s, live) &&
                !(s->ctrl & (CTRL_SFTRST | CTRL_CLKGATE));

    qemu_set_irq(s->icoll_busy, busy);

    if (s->ctrl & (CTRL_SFTRST | CTRL_CLKGATE)) {
        timer_del(s->multicycle_timer);
        s->fsm_state = ICOLL_FSM_IDLE;
        s->selected_vector = 0;
        s->saved_vector = 0;
        s->vector = 0;
        s->vector_pending = false;
        s->request_holding = 0;
        qemu_set_irq(s->irq, 0);
        qemu_set_irq(s->fiq, 0);
        return;
    }

    if (s->ctrl & CTRL_BYPASS_FSM) {
        timer_del(s->multicycle_timer);
        s->fsm_state = ICOLL_FSM_IDLE;
        s->selected_vector = 0;
        s->saved_vector = 0;
        s->vector_pending = false;
        s->request_holding = live;
        s->vector = 0;
        for (int i = 63; i >= 0; i--) {
            if ((live & (1ULL << i)) &&
                !stmp3770_icoll_source_routes_to_fiq(s, i)) {
                s->vector = i;
                break;
            }
        }
        qemu_set_irq(s->irq, 0);
        qemu_set_irq(s->fiq, fiq_enabled && stmp3770_icoll_fiq_pending(s));
        return;
    }

    active_level = stmp3770_icoll_highest_active_level(s);
    s->current_level = active_level >= 0 ? active_level : 0;

    /*
     * During the two HCLK delay, the request holding register is closed.
     * Re-evaluate the closed snapshot in case the priority of a captured
     * source changes, but do not reopen the holding register.
     */
    if (s->fsm_state == ICOLL_FSM_MULTICYCLE1 ||
        s->fsm_state == ICOLL_FSM_MULTICYCLE2) {
        stmp3770_icoll_find_candidate(s, s->request_holding, active_level,
                                      &selected_vector, &selected_level,
                                      &has_candidate);
        if (has_candidate) {
            s->selected_vector = selected_vector;
            /*
             * If the timer was stopped (e.g. by CLKGATE), resume the
             * remaining delay so the FSM can reach PENDING.
             */
            if (!timer_pending(s->multicycle_timer)) {
                timer_mod(s->multicycle_timer,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          stmp3770_icoll_multicycle_step_ns(s));
            }
            qemu_set_irq(s->fiq, fiq_enabled && stmp3770_icoll_fiq_pending(s));
            qemu_set_irq(s->irq, 0);
            return;
        }

        timer_del(s->multicycle_timer);
        s->fsm_state = active_level >= 0 ? ICOLL_FSM_ISR_RUNNING
                                         : ICOLL_FSM_IDLE;
        s->vector = active_level >= 0 ? s->saved_vector : 0;
        qemu_set_irq(s->fiq, fiq_enabled && stmp3770_icoll_fiq_pending(s));
        qemu_set_irq(s->irq, 0);
        return;
    }

    /* A vector has been computed and is waiting for CPU acknowledgement. */
    if (s->vector_pending) {
        qemu_set_irq(s->fiq, fiq_enabled && stmp3770_icoll_fiq_pending(s));
        qemu_set_irq(s->irq, irq_enabled && s->vector_pending);
        return;
    }

    /* Normal operation: the holding register is open and samples live requests. */
    s->request_holding = live;

    stmp3770_icoll_find_candidate(s, s->request_holding, active_level,
                                  &selected_vector, &selected_level,
                                  &has_candidate);

    if (has_candidate) {
        s->saved_vector = s->vector;
        s->vector = 0;
        s->selected_vector = selected_vector;
        s->fsm_state = ICOLL_FSM_MULTICYCLE1;
        timer_mod(s->multicycle_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  stmp3770_icoll_multicycle_step_ns(s));
        qemu_set_irq(s->fiq, fiq_enabled && stmp3770_icoll_fiq_pending(s));
        qemu_set_irq(s->irq, 0);
        return;
    }

    /* No candidate: update the FSM state and clear the vector when idle. */
    if (active_level < 0) {
        s->fsm_state = ICOLL_FSM_IDLE;
        s->vector = 0;
    } else {
        s->fsm_state = ICOLL_FSM_ISR_RUNNING;
        /* s->vector already reflects the in-service interrupt. */
    }

    qemu_set_irq(s->fiq, fiq_enabled && stmp3770_icoll_fiq_pending(s));
    qemu_set_irq(s->irq, 0);
}

static void stmp3770_icoll_set_irq(void *opaque, int irq, int level)
{
    STMP3770ICOLLState *s = STMP3770_ICOLL(opaque);

    if (level) {
        s->raw_status |= (1ULL << irq);
    } else {
        s->raw_status &= ~(1ULL << irq);
    }

    stmp3770_icoll_update(s);
}

static void stmp3770_icoll_multicycle_tick(void *opaque)
{
    STMP3770ICOLLState *s = STMP3770_ICOLL(opaque);

    switch (s->fsm_state) {
    case ICOLL_FSM_MULTICYCLE1:
        s->fsm_state = ICOLL_FSM_MULTICYCLE2;
        timer_mod(s->multicycle_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  stmp3770_icoll_multicycle_step_ns(s));
        break;
    case ICOLL_FSM_MULTICYCLE2:
        s->fsm_state = ICOLL_FSM_PENDING;
        s->vector = s->selected_vector;
        s->vector_pending = true;
        qemu_set_irq(s->irq, (s->ctrl & CTRL_IRQ_FINAL_ENABLE) &&
                             s->vector_pending);
        break;
    default:
        break;
    }
}

static uint64_t icoll_read_subword(uint32_t value, hwaddr offset, unsigned size)
{
    unsigned shift = (offset & 3) * 8;
    uint32_t mask = (size >= 4) ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1u);

    return (value >> shift) & mask;
}

static uint32_t stmp3770_icoll_vector_pitch(const STMP3770ICOLLState *s)
{
    uint32_t field = (s->ctrl & CTRL_VECTOR_PITCH_MASK) >>
                     CTRL_VECTOR_PITCH_SHIFT;

    return field <= 1 ? 4 : field * 4;
}

static uint64_t stmp3770_icoll_read(void *opaque, hwaddr offset, unsigned size)
{
    STMP3770ICOLLState *s = STMP3770_ICOLL(opaque);
    hwaddr base = offset & ~0xFULL;
    uint32_t value = 0;

    /*
     * SET/CLR/TOG aliases are at offsets +0x4/+0x8/+0xC.  Per the STMP3770
     * register conventions, reads from these aliases always return 0.
     */
    if (offset & 0xC) {
        return 0;
    }

    switch (base) {
    case REG_VECTOR:
        value = s->vbase + s->vector * stmp3770_icoll_vector_pitch(s);
        if (s->ctrl & CTRL_ARM_RSE_MODE) {
            stmp3770_icoll_enter_service(s);
            stmp3770_icoll_update(s);
        }
        break;

    case REG_LEVELACK:
        /* LEVELACK is a write-only interrupt level acknowledge register;
         * reads return 0 on hardware. */
        value = 0;
        break;

    case REG_CTRL:
        value = s->ctrl;
        break;

    case REG_VBASE:
        value = s->vbase;
        break;

    case REG_STAT:
        value = s->vector & STAT_VECTOR_NUMBER_MASK;
        break;

    case REG_RAW0:
        value = (uint32_t)(s->raw_status & 0xFFFFFFFF);
        break;

    case REG_RAW1:
        value = (uint32_t)(s->raw_status >> 32);
        break;

    case REG_PRIORITY0 ... REG_PRIORITY15:
        value = s->priority[(base - REG_PRIORITY0) / 0x10];
        break;

    case REG_DEBUG:
        value = stmp3770_icoll_debug(s);
        break;

    case REG_DEBUGRD0:
        value = ICOLL_DEBUGRD0;
        break;

    case REG_DEBUGRD1:
        value = ICOLL_DEBUGRD1;
        break;

    case REG_DEBUGFLAG:
        value = s->debugflag;
        break;

    case REG_DEBUGRDREQ0:
        value = (uint32_t)stmp3770_icoll_debug_requests(s);
        break;

    case REG_DEBUGRDREQ1:
        value = (uint32_t)(stmp3770_icoll_debug_requests(s) >> 32);
        break;

    case REG_VERSION:
        value = ICOLL_VERSION;
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                     "%s: bad offset 0x%" HWADDR_PRIx "\n", __func__, offset);
        break;
    }

    return icoll_read_subword(value, offset, size);
}

static void stmp3770_icoll_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    STMP3770ICOLLState *s = STMP3770_ICOLL(opaque);
    hwaddr base_offset = offset & ~0xF;
    bool is_set = (offset & 0xF) == REG_SET;
    bool is_clr = (offset & 0xF) == REG_CLR;
    bool is_tog = (offset & 0xF) == REG_TOG;
    uint32_t *target = NULL;
    uint32_t val;

    /* Handle sub-word writes (byte/halfword) for bitfield access */
    if (size < 4) {
        unsigned shift = (offset & 3) * 8;
        uint32_t mask = ((1ULL << (size * 8)) - 1) << shift;

        /* Read-modify-write for sub-word access */
        switch (base_offset) {
        case REG_CTRL:
            target = &s->ctrl;
            break;
        case REG_VBASE:
            target = &s->vbase;
            break;
        case REG_PRIORITY0 ... REG_PRIORITY15:
            target = &s->priority[(base_offset - REG_PRIORITY0) / 0x10];
            break;
        case REG_DEBUGFLAG:
            target = &s->debugflag;
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                         "%s: sub-word write to unsupported offset 0x%" HWADDR_PRIx "\n",
                         __func__, offset);
            return;
        }

        if (target) {
            *target = (*target & ~mask) | ((value << shift) & mask);
        }

        if (base_offset == REG_CTRL) {
            s->ctrl &= CTRL_RW_MASK;
            s->fiq_enable = s->ctrl & CTRL_FIQ_ENABLE_MASK;
        } else if (base_offset == REG_VBASE) {
            s->vbase &= VBASE_RW_MASK;
        } else if (base_offset >= REG_PRIORITY0 &&
                   base_offset <= REG_PRIORITY15) {
            s->priority[(base_offset - REG_PRIORITY0) / 0x10] &=
                PRIORITY_RW_MASK;
        } else if (base_offset == REG_DEBUGFLAG) {
            s->debugflag &= 0xFFFF;
        }

        stmp3770_icoll_update(s);
        return;
    }

    /* Standard 32-bit write handling */
    val = value;
    offset = base_offset;

    switch (offset) {
    case REG_VECTOR:
        /* VECTOR is a write-only side-effect register; it has no SET/CLR/TOG. */
        if (is_set || is_clr || is_tog) {
            return;
        }
        stmp3770_icoll_enter_service(s);
        stmp3770_icoll_update(s);
        return;

    case REG_CTRL:
        stmp3770_icoll_write_ctrl(s, val, is_set, is_clr, is_tog);
        stmp3770_icoll_update(s);
        return;

    case REG_VBASE:
        target = &s->vbase;
        break;

    case REG_LEVELACK:
        /* LEVELACK has no SET/CLR/TOG aliases; only the primary offset is valid. */
        if (is_set || is_clr || is_tog) {
            return;
        }
        for (int level = 0; level < 4; level++) {
            if (val & (1U << level)) {
                s->level_active[level] = 0;
            }
        }
        stmp3770_icoll_update(s);
        return;

    case REG_PRIORITY0 ... REG_PRIORITY15:
        target = &s->priority[(offset - REG_PRIORITY0) / 0x10];
        break;

    case REG_DEBUGFLAG:
        target = &s->debugflag;
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                     "%s: bad offset 0x%" HWADDR_PRIx "\n", __func__, offset);
        return;
    }

    if (target) {
        if (is_set) {
            *target |= val;
        } else if (is_clr) {
            *target &= ~val;
        } else if (is_tog) {
            *target ^= val;
        } else {
            *target = val;
        }
    }

    if (offset == REG_VBASE) {
        s->vbase &= VBASE_RW_MASK;
    } else if (offset >= REG_PRIORITY0 && offset <= REG_PRIORITY15) {
        s->priority[(offset - REG_PRIORITY0) / 0x10] &= PRIORITY_RW_MASK;
    } else if (offset == REG_DEBUGFLAG) {
        s->debugflag &= 0xFFFF;
    }

    stmp3770_icoll_update(s);
}

static const MemoryRegionOps stmp3770_icoll_ops = {
    .read = stmp3770_icoll_read,
    .write = stmp3770_icoll_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void stmp3770_icoll_reset(DeviceState *dev)
{
    STMP3770ICOLLState *s = STMP3770_ICOLL(dev);
    int i;

    s->ctrl = CTRL_CLKGATE | CTRL_SFTRST |
              CTRL_FIQ_FINAL_ENABLE | CTRL_IRQ_FINAL_ENABLE;
    s->vbase = 0;
    s->vector = 0;
    s->vector_pending = false;
    s->fsm_state = ICOLL_FSM_IDLE;
    s->selected_vector = 0;
    s->saved_vector = 0;
    s->raw_status = 0;
    s->request_holding = 0;
    s->current_level = 0;
    s->fiq_enable = 0;
    s->debugflag = 0;
    s->hclk_hz = ICOLL_RESET_CLOCK_HZ;

    timer_del(s->sftrst_timer);
    timer_del(s->multicycle_timer);

    for (i = 0; i < 16; i++) {
        s->priority[i] = 0;
    }

    for (i = 0; i < 4; i++) {
        s->level_active[i] = 0;
    }
}

static void stmp3770_icoll_init(Object *obj)
{
    STMP3770ICOLLState *s = STMP3770_ICOLL(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &stmp3770_icoll_ops, s,
                         TYPE_STMP3770_ICOLL, 0x2000);
    sysbus_init_mmio(sbd, &s->iomem);

    /* IRQ and FIQ outputs to CPU */
    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->fiq);
    /* ICOLL_BUSY output to clock control for WFI/INTERRUPT_WAIT gating */
    sysbus_init_irq(sbd, &s->icoll_busy);

    s->sftrst_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   stmp3770_icoll_finish_soft_reset, s);
    s->multicycle_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       stmp3770_icoll_multicycle_tick, s);

    /* 64 IRQ inputs from peripherals */
    qdev_init_gpio_in(DEVICE(obj), stmp3770_icoll_set_irq, 64);
}

static void stmp3770_icoll_finalize(Object *obj)
{
    STMP3770ICOLLState *s = STMP3770_ICOLL(obj);

    timer_free(s->sftrst_timer);
    timer_free(s->multicycle_timer);
}

static const VMStateDescription vmstate_stmp3770_icoll = {
    .name = TYPE_STMP3770_ICOLL,
    .version_id = 5,
    .minimum_version_id = 5,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, STMP3770ICOLLState),
        VMSTATE_UINT32(vbase, STMP3770ICOLLState),
        VMSTATE_UINT32(vector, STMP3770ICOLLState),
        VMSTATE_BOOL(vector_pending, STMP3770ICOLLState),
        VMSTATE_UINT32(fsm_state, STMP3770ICOLLState),
        VMSTATE_UINT32(selected_vector, STMP3770ICOLLState),
        VMSTATE_UINT32(saved_vector, STMP3770ICOLLState),
        VMSTATE_UINT32_ARRAY(priority, STMP3770ICOLLState, 16),
        VMSTATE_UINT64(raw_status, STMP3770ICOLLState),
        VMSTATE_UINT64(request_holding, STMP3770ICOLLState),
        VMSTATE_UINT32_ARRAY(level_active, STMP3770ICOLLState, 4),
        VMSTATE_UINT32(current_level, STMP3770ICOLLState),
        VMSTATE_UINT8(fiq_enable, STMP3770ICOLLState),
        VMSTATE_UINT32(debugflag, STMP3770ICOLLState),
        VMSTATE_UINT32(hclk_hz, STMP3770ICOLLState),
        VMSTATE_TIMER_PTR(sftrst_timer, STMP3770ICOLLState),
        VMSTATE_TIMER_PTR(multicycle_timer, STMP3770ICOLLState),
        VMSTATE_END_OF_LIST()
    }
};

static void stmp3770_icoll_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, stmp3770_icoll_reset);
    dc->vmsd = &vmstate_stmp3770_icoll;
}

static const TypeInfo stmp3770_icoll_info = {
    .name          = TYPE_STMP3770_ICOLL,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(STMP3770ICOLLState),
    .instance_init = stmp3770_icoll_init,
    .instance_finalize = stmp3770_icoll_finalize,
    .class_init    = stmp3770_icoll_class_init,
};

static void stmp3770_icoll_register_types(void)
{
    type_register_static(&stmp3770_icoll_info);
}

type_init(stmp3770_icoll_register_types)
