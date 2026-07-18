/*
 * MMIO profile for STMP3770 / EmuGII
 *
 * Counts CPU memory accesses whose physical address falls in the SoC
 * peripheral region (0x80000000-0x80ffffff).  On this machine
 * qemu_plugin_hwaddr_is_io() does not report these as IO, but the
 * physical address from qemu_plugin_hwaddr_phys_addr() is still valid,
 * so we filter by the known peripheral base region.
 */

#include <inttypes.h>
#include <stdio.h>
#include <glib.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

/* SoC peripheral region used by STMP3770 */
#define PERIPH_BASE 0x80000000ULL
#define PERIPH_MASK 0xff000000ULL

typedef struct {
    uint64_t paddr;
    uint64_t reads;
    uint64_t writes;
} PageCount;

static GMutex lock;
static GHashTable *pages; /* key: uint64_t paddr, value: PageCount* */
static uint64_t total_accesses;

static gint sort_by_total(gconstpointer a, gconstpointer b)
{
    const PageCount *pa = (const PageCount *)a;
    const PageCount *pb = (const PageCount *)b;
    uint64_t ta = pa->reads + pa->writes;
    uint64_t tb = pb->reads + pb->writes;
    if (ta < tb) return 1;
    if (ta > tb) return -1;
    return 0;
}

static void vcpu_mem(unsigned int cpu_index, qemu_plugin_meminfo_t meminfo,
                     uint64_t vaddr, void *udata)
{
    struct qemu_plugin_hwaddr *hwaddr = qemu_plugin_get_hwaddr(meminfo, vaddr);
    uint64_t paddr;
    bool is_write;
    PageCount *pc;
    uint64_t *key;

    if (!hwaddr) {
        return;
    }

    paddr = qemu_plugin_hwaddr_phys_addr(hwaddr);
    if ((paddr & PERIPH_MASK) != PERIPH_BASE) {
        return;
    }

    is_write = qemu_plugin_mem_is_store(meminfo);

    g_mutex_lock(&lock);
    total_accesses++;
    pc = (PageCount *) g_hash_table_lookup(pages, &paddr);
    if (!pc) {
        key = g_new(uint64_t, 1);
        *key = paddr;
        pc = g_new0(PageCount, 1);
        pc->paddr = paddr;
        g_hash_table_insert(pages, key, pc);
    }
    if (is_write) {
        pc->writes++;
    } else {
        pc->reads++;
    }
    g_mutex_unlock(&lock);
}

static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        qemu_plugin_register_vcpu_mem_cb(insn, vcpu_mem,
                                          QEMU_PLUGIN_CB_NO_REGS,
                                          QEMU_PLUGIN_MEM_RW, NULL);
    }
}

static void plugin_exit(qemu_plugin_id_t id, void *p)
{
    g_autoptr(GString) report = g_string_new("");
    GList *values;
    GList *it;

    g_string_append_printf(report,
        "mmio_profile total peripheral accesses: %" PRIu64 "\n", total_accesses);
    g_string_append_printf(report,
        "paddr, reads, writes, total\n");

    values = g_hash_table_get_values(pages);
    values = g_list_sort(values, sort_by_total);

    for (it = values; it; it = it->next) {
        PageCount *pc = (PageCount *)it->data;
        g_string_append_printf(report,
            "0x%08" PRIx64 ", %" PRIu64 ", %" PRIu64 ", %" PRIu64 "\n",
            pc->paddr, pc->reads, pc->writes, pc->reads + pc->writes);
    }
    g_list_free(values);

    qemu_plugin_outs(report->str);
}

QEMU_PLUGIN_EXPORT
int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
                        int argc, char **argv)
{
    pages = g_hash_table_new(g_int64_hash, g_int64_equal);
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
