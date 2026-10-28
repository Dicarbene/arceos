#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/arceos-c-stdlib.XXXXXX")
trap 'rm -f "$build_dir/stdlib_test" "$build_dir/fopen_test" "$build_dir/fopen_test.o" "$build_dir/ubsan_test" "$build_dir/ubsan.log"; rmdir "$build_dir"' EXIT HUP INT TERM

# Keep all test builds on the same fail-fast sanitizer configuration.
cc_sanitized() {
    "${CC:-cc}" -fsanitize=address,undefined -fno-sanitize-recover=undefined \
        -fno-omit-frame-pointer "$@"
}

# A recoverable UBSan report must never leave this test step green.
cc_sanitized -std=gnu11 -O2 "$script_dir/ubsan_failure.c" -o "$build_dir/ubsan_test"
if "$build_dir/ubsan_test" >"$build_dir/ubsan.log" 2>&1; then
    echo "FAIL: UBSan did not terminate an invalid program" >&2
    exit 1
fi
if ! grep -q 'runtime error:' "$build_dir/ubsan.log"; then
    cat "$build_dir/ubsan.log" >&2
    echo "FAIL: sanitizer failure did not come from UBSan" >&2
    exit 1
fi

# The warnings suppressed below come from unrelated existing stdlib functions.
cc_sanitized -std=gnu11 -O2 -Wall -Wextra -Werror \
    -Wno-unused-parameter -Wno-implicit-fallthrough -fno-builtin \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    "$script_dir/stdlib_fault_injection.c" -o "$build_dir/stdlib_test"
"$build_dir/stdlib_test"

# Use axlibc's headers for its FILE layout. Hidden symbols and section GC keep
# unrelated stdio definitions from replacing the host runtime's functions.
cc_sanitized -std=gnu11 -O2 -Wall -Wno-unused-parameter \
    -nostdinc -I"$script_dir/../../include" -fno-builtin -ffreestanding \
    -fvisibility=hidden -ffunction-sections -fdata-sections \
    -c "$script_dir/fopen_failure.c" -o "$build_dir/fopen_test.o"
objcopy --localize-hidden "$build_dir/fopen_test.o"
cc_sanitized -Wl,--gc-sections \
    "$build_dir/fopen_test.o" -o "$build_dir/fopen_test"
"$build_dir/fopen_test"
echo "PASS: fopen allocation/open failures preserve errno and release FILE allocations"
