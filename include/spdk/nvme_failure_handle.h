#ifndef NVME_FAILURE_HANDLE_H
#define NVME_FAILURE_HANDLE_H

#include "spdk/env.h"

struct bypass_fn_table {
    void (*bypass_if)(char* name);
    void (*have_cache)(char* name);
};

struct ctrlr_detect_fn_table {
    struct spdk_nvme_ctrlr *(*ctrlr_detector)(struct spdk_pci_device *dev);
};

void set_bypass_flag(struct spdk_pci_device *dev);
void *convert_to_nvme_ctrlr(void *ctx);
void _do_set_bypass_flag(void *ctx);
void set_bypass_set_if(struct bypass_fn_table *bypass_if);
void set_failure_detect_if(struct ctrlr_detect_fn_table *detect_if);
struct spdk_nvme_ctrlr *fine_spdk_nvme_ctrlr(struct spdk_pci_device *dev);
bool have_cas_device(void *ctrlr);

#endif