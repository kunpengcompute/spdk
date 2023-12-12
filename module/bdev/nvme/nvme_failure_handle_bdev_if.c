#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/tree.h"
#include "bdev_nvme.h"
#include "spdk/nvme_failure_handle.h"

static struct bypass_fn_table g_bypass_if;
static struct ctrlr_detect_fn_table g_detect_if;

static int
nvme_ns_cmp(struct nvme_ns *ns1, struct nvme_ns *ns2)
{
    return ns1->id - ns2->id;
}

RB_GENERATE_STATIC(nvme_ns_tree, nvme_ns, node, nvme_ns_cmp);

struct spdk_nvme_ctrlr *find_spdk_nvme_ctrlr(struct spdk_pci_device *dev)
{
    return g_detect_if.ctrlr_detector(dev);
}

void _do_set_bypass_flag(void *ctx)
{
    char *cache_name;
    struct nvme_ctrlr *ctrlr;
    struct nvme_ns *ns, *tmp;
    ctrlr = (struct nvme_ctrlr *)ctx;
    RB_FOREACH_SAFE(ns, nvme_ns_tree, &ctrlr->namespaces, tmp) {
        // 获取cache设备的名称
        cache_name = ns->bdev->disk.name;
        g_bypass_if.bypass_if(cache_name);
    }
}

bool have_cas_device(void *ctx)
{
    char *dev_name;
    struct nvme_ns *ns, *tmp;
    struct nvme_ctrlr *ctrlr = (struct nvme_ctrlr *)ctx;
    RB_FOREACH_SAFE(ns, nvme_ns_tree, &ctrlr->namespaces, tmp) {
        // 获取cache设备的名称
        dev_name = ns->bdev->disk.name;
        if (g_bypass_if.have_cache(dev_name))
            return true;
    }
    return false;
}

void set_bypass_set_if(struct bypass_fn_table *bypass_if)
{
    g_bypass_if.bypass_if = bypass_if->bypass_if;
    g_bypass_if.have_cache = bypass_if->have_cache;
}

void set_failure_detect_if(struct ctrlr_detect_fn_table *detect_if)
{
    g_detect_if.ctrlr_detector = detect_if->ctrlr_detector;
}