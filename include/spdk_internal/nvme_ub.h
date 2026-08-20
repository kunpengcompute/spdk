/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Huawei Technologies Co., Ltd.
 */

#ifndef SPDK_INTERNAL_NVME_UB_H
#define SPDK_INTERNAL_NVME_UB_H

#include "spdk/stdinc.h"

#define SPDK_NVME_UB_OOB_MAGIC			0x32424f55u /* "UOB2" */
#define SPDK_NVME_UB_OOB_VERSION		2u
#define SPDK_NVME_UB_OOB_MAX_SIZE		(4u * 1024u * 1024u)
#define SPDK_NVME_UB_OOB_MAX_ENDPOINTS		64u
#define SPDK_NVME_UB_OOB_MAX_REGIONS		1024u
#define SPDK_NVME_UB_OOB_MAX_CONTEXT_SIZE	(64u * 1024u)
#define SPDK_NVME_UB_EID_SIZE			16u
#define SPDK_NVME_UB_MAX_SGL_DESCRIPTORS	16u

/* Vendor-specific SGL subtype used only by the SPDK UB transport. */
#define SPDK_NVME_UB_SGL_SUBTYPE_NPU		0xfu

enum spdk_nvme_ub_oob_msg_type {
	SPDK_NVME_UB_OOB_CONNECT = 1,
	SPDK_NVME_UB_OOB_CONNECT_RSP = 2,
};

#pragma pack(push, 1)
struct spdk_nvme_ub_oob_header {
	uint32_t magic;
	uint16_t version;
	uint16_t msg_type;
	uint32_t length;
	int32_t status;
	uint16_t qid;
	uint16_t reserved0;
	uint32_t endpoint_count;
	uint32_t region_count;
	uint64_t registry_generation;
};

/* The existing CPU command/response path resources. */
struct spdk_nvme_ub_oob_cpu_info {
	uint8_t seg_eid[SPDK_NVME_UB_EID_SIZE];
	uint32_t seg_uasid;
	uint64_t seg_va;
	uint64_t seg_len;
	uint32_t seg_flag;
	uint32_t seg_token_id;
	uint8_t jetty_eid[SPDK_NVME_UB_EID_SIZE];
	uint32_t jetty_uasid;
	uint32_t jetty_id;
	uint8_t trans_mode;
	uint8_t reserved[7];
};

/* Followed immediately by rjetty_context_size bytes. */
struct spdk_nvme_ub_oob_endpoint {
	uint32_t record_size;
	uint32_t endpoint_id;
	uint32_t token;
	uint32_t rjetty_context_size;
};

/* Followed immediately by segment_context_size bytes. */
struct spdk_nvme_ub_oob_region {
	uint32_t record_size;
	uint32_t region_id;
	uint32_t endpoint_id;
	uint32_t token;
	uint64_t user_base;
	uint64_t remote_base;
	uint64_t length;
	uint32_t segment_context_size;
	uint32_t reserved;
};
#pragma pack(pop)

static inline bool
spdk_nvme_ub_range_contains(uint64_t base, uint64_t range_len, uint64_t addr,
			    uint64_t length)
{
	return length != 0 && addr >= base && length <= range_len &&
	       addr - base <= range_len - length;
}

#endif /* SPDK_INTERNAL_NVME_UB_H */
