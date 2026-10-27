# SPDX-License-Identifier: BSD-3-Clause

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../../..)
TEST_FILE = transport_weak_ut.c

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk

all: weak_l0_fixture.so

weak_l0_fixture.so: weak_l0_fixture.c
	$(CC) -Wall -Wextra -Wno-unused-parameter -Wmissing-declarations \
		-fPIC -shared -I$(SPDK_ROOT_DIR)/include $< -o $@

.PHONY: clean_weak_fixture
clean: clean_weak_fixture
clean_weak_fixture:
	$(RM) weak_l0_fixture.so
