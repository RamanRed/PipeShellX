#!/usr/bin/env bash
# Architecture layering lint. Fails when:
#   1. a platform header is included outside src/os/ (tests/ and bench/ may
#      use platform APIs to *audit* the abstractions);
#   2. a public header under include/psx/ uses a POSIX/Win32 type;
#   3. a layer includes a layer above it (os → runtime → stream → transport →
#      pipeline, plus the legacy application code on top);
#   4. psx/os/backend.hpp is included outside src/os/ or tests/unit/os/.
set -euo pipefail
cd "$(dirname "$0")/.."

status=0
fail() { echo "layering: $*" >&2; status=1; }

platform_headers='unistd\.h|termios\.h|signal\.h|poll\.h|fcntl\.h|spawn\.h|pthread\.h|sys/[a-z_]+\.h|netinet/[a-z_]+\.h|arpa/[a-z_]+\.h|windows\.h|winsock2\.h|io\.h|process\.h'
while IFS= read -r hit; do
    fail "platform header outside src/os/: $hit"
done < <(grep -rnE "^#include <($platform_headers)>" include src --include='*.cpp' --include='*.hpp' --include='*.h' | grep -v '^src/os/' || true)

while IFS= read -r file; do
    # Comments may name the platform types; code may not.
    while IFS= read -r hit; do
        fail "POSIX/Win32 type in a public psx header: $file:$hit"
    done < <(sed -E 's|//.*$||' "$file" | grep -nE '\b(pid_t|ssize_t|HANDLE|DWORD|struct sigaction|sigset_t|pollfd|rlimit)\b' || true)
done < <(find include/psx -type f -name '*.hpp')

while IFS= read -r hit; do
    fail "platform #ifdef outside src/os/ and CMake: $hit"
done < <(grep -rnE '^\s*#\s*(if|ifdef|elif)\b.*\b(_WIN32|__APPLE__|__linux__)\b' include/psx src/runtime src/stream src/transport src/pipeline 2>/dev/null || true)

layer_rank() {
    case "$1" in
        psx/result.hpp|psx/os/*) echo 0 ;;
        psx/runtime/*) echo 1 ;;
        psx/stream/*) echo 2 ;;
        psx/transport/*) echo 3 ;;
        psx/pipeline/*|psx/inventory/*|psx/observability/*) echo 4 ;;
        *) echo 9 ;;  # legacy application headers sit on top of everything
    esac
}
dir_rank() {
    case "$1" in
        src/os/*|include/psx/os/*|include/psx/result.hpp) echo 0 ;;
        src/runtime/*|include/psx/runtime/*) echo 1 ;;
        src/stream/*|include/psx/stream/*) echo 2 ;;
        src/transport/*|include/psx/transport/*) echo 3 ;;
        src/pipeline/*|include/psx/pipeline/*|src/inventory/*|include/psx/inventory/*) echo 4 ;;
        *) echo 9 ;;
    esac
}
while IFS= read -r file; do
    from=$(dir_rank "$file")
    [ "$from" -eq 9 ] && continue
    while IFS= read -r inc; do
        to=$(layer_rank "$inc")
        if [ "$to" -gt "$from" ]; then
            fail "$file (layer $from) includes $inc (layer $to): layers may only include layers at or below their own"
        fi
    done < <(grep -oE '^#include "psx/[^"]+"' "$file" | sed 's/#include "//; s/"$//' || true)
done < <(find include/psx src -type f \( -name '*.cpp' -o -name '*.hpp' \))

while IFS= read -r hit; do
    fail "backend.hpp is private to src/os/ and tests/unit/os/: $hit"
done < <(grep -rln 'psx/os/backend.hpp' include src tests bench 2>/dev/null | grep -v '^src/os/\|^tests/unit/os/\|^include/psx/os/backend.hpp$' || true)

if [ "$status" -eq 0 ]; then
    echo "layering: OK"
fi
exit "$status"
