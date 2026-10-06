#!/usr/bin/env bash
set -e

# Usage: ./test.sh [debug|asan|tsan|dev]
#   debug (default) — plain build + ctest
#   asan            — AddressSanitizer/UBSan build + ctest
#   tsan            — ThreadSanitizer build + ctest
#   dev             — debug against local checkouts (../seqc, ../ctt)

MODE="${1:-debug}"

# Ensure malloc/free have not snuck into library code: memory comes from the
# caller's allocator_t.  Matches bare calls only, not allocator.free( etc.
if grep -Prn '(?<![.\w])(malloc|free|realloc|calloc)\s*\(' src/ \
    --include='*.c' --include='*.h'; then
    echo "ERROR: heap allocation found in src/ — use an allocator_t instead" >&2
    exit 1
fi

case "$MODE" in
    debug|dev)
        ;;
    asan)
        export ASAN_OPTIONS=detect_stack_use_after_return=1:detect_leaks=1
        ;;
    tsan)
        export TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1
        ;;
    *)
        echo "usage: $0 [debug|asan|tsan|dev]" >&2
        exit 1
        ;;
esac

cmake --preset "$MODE"
cmake --build --preset "$MODE"
ctest --preset "$MODE"
