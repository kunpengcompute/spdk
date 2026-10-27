# SPDX-License-Identifier: BSD-3-Clause

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../../..)

TEST_FILE = env_ut.c

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk

override CFLAGS := -I$(CURDIR)/stubs $(CFLAGS)
