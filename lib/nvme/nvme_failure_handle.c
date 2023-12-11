#include "spdk/util.h"
#include "spdk/env.h"
#include "nvme_internal.h"
#include "nvme_pcie_internal.h"
#include "spdk/nvme_failure_handle.h"

void set_bypass_flag(struct spdk_pci_device *dev)
{
    struct spdk_nvme_ctrlr *nctrlr;
    void *ctrlr;

    if (dev == NULL)
        return;
    nctrlr = find_spdk_nvme_ctrlr(dev);
    if (nctrlr == NULL)
        return;
    ctrlr = nctrlr->cb_ctx;
    if (ctrlr == NULL)
        return;

    _do_set_bypass_flag(ctrlr);
}