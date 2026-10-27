#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Sourced by run_dev_l0_native.sh. All commands compile the original
# source directly through SPDK CUnit suites; no L0/DPDK device is initialized.
# The caller provides ROOT, CUNIT_PREFIX, NATIVE_OUT_DIR, CC, the CFLAGS array,
# and emit_profile NAME BINARY (run, merge LLVM profiles, export NAME.info).

env_native_flags=(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
	-Wmissing-declarations -Werror)
env_native_cc=${CC:-${cc:-clang}}
env_native_cunit="$CUNIT_PREFIX/lib/libcunit.a"
if [[ ! -f "$env_native_cunit" ]]; then
	env_native_cunit="$CUNIT_PREFIX/lib64/libcunit.a"
fi
if [[ ! -f "$env_native_cunit" ]]; then
	printf 'CUnit static library is missing under %s/lib{,64}/.\n' "$CUNIT_PREFIX" >&2
	return 2
fi

if [[ $(uname -s) == Darwin ]]; then
	env_native_gc=-Wl,-dead_strip
else
	env_native_gc=-Wl,--gc-sections
fi

for env_native_name in l0 l0_mmap_platform env memory; do
	printf 'Building env_dpdk/%s_ut\n' "$env_native_name"
	"$env_native_cc" "${CFLAGS[@]}" "${env_native_flags[@]}" \
		"-I$ROOT/test/unit/lib/env_dpdk/env.c/stubs" \
		"$ROOT/test/unit/lib/env_dpdk/$env_native_name.c/${env_native_name}_ut.c" \
		"$ROOT/lib/log/log.c" "$ROOT/lib/log/log_flags.c" \
		"$env_native_cunit" -pthread "$env_native_gc" \
		-o "$NATIVE_OUT_DIR/${env_native_name}_ut"
	emit_profile "$env_native_name" "$NATIVE_OUT_DIR/${env_native_name}_ut"
done

unset env_native_flags env_native_cc env_native_cunit env_native_gc env_native_name
