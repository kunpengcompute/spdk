#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# macOS execution of the same CUnit source used by SPDK's Linux Makefile.

set -euo pipefail

: "${ROOT:?Set ROOT to the SPDK checkout}"
: "${CUNIT_PREFIX:?Set CUNIT_PREFIX to a CUnit installation}"
: "${NATIVE_COMPAT_DIR:?Set NATIVE_COMPAT_DIR to the native compatibility headers}"
: "${NATIVE_OUT_DIR:?Set NATIVE_OUT_DIR to the build and coverage output directory}"

mkdir -p "$NATIVE_OUT_DIR"

# Darwin ld has no GNU --wrap. Intercept calloc only in this translation unit;
# the standard Linux UT instead uses SPDK's real ut_mock library and --wrap.
cat > "$NATIVE_OUT_DIR/rpc_native_adapter.c" <<'EOF'
#include <stdlib.h>
#include <stdbool.h>
bool ut_calloc_mocked = false;
void *ut_calloc;
void *__wrap_calloc(size_t count, size_t size)
{
    return ut_calloc_mocked ? ut_calloc : calloc(count, size);
}
#define calloc __wrap_calloc
#include "test/unit/lib/nvmf/nvmf_rpc.c/nvmf_rpc_ut.c"
EOF

"${CC:-clang}" "${CFLAGS[@]}" -std=gnu99 -DSPDK_UNIT_TEST=1 -I"$ROOT" \
	"$NATIVE_OUT_DIR/rpc_native_adapter.c" \
	"$ROOT/lib/json/json_parse.c" "$ROOT/lib/json/json_util.c" "$ROOT/lib/json/json_write.c" \
	"$ROOT/lib/log/log.c" "$ROOT/lib/log/log_flags.c" \
	"$ROOT/lib/util/string.c" "$ROOT/lib/util/strerror_tls.c" "$ROOT/lib/util/math.c" \
	"$CUNIT_PREFIX/lib/libcunit.a" -Wl,-dead_strip -o "$NATIVE_OUT_DIR/nvmf_rpc_ut"

emit_profile rpc "$NATIVE_OUT_DIR/nvmf_rpc_ut"
