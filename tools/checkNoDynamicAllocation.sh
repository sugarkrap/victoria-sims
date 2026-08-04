#!/bin/sh
# Enforces the project's hardest rule: our own code never allocates at run
# time. Checked two ways, because either one alone is easy to fool.
#
#   1. No allocator names appear in the sources we own.
#   2. No object file we compile carries an undefined reference to one.
#
# Platform libraries (X11, Mesa, the browser) allocate internally and are out
# of our reach; that is a known limit, not an exemption for engine code.

set -eu

SOURCE_DIRECTORIES="engine platform render tools"
FORBIDDEN_PATTERN='\b(malloc|calloc|realloc|free|aligned_alloc|posix_memalign|strdup|mmap|brk|sbrk)\b'

failureCount=0

echo "checking sources for allocator use..."
for directory in $SOURCE_DIRECTORIES; do
    [ -d "$directory" ] || continue
    if grep -rInE --include='*.c' --include='*.h' "$FORBIDDEN_PATTERN" "$directory"; then
        echo "error: allocator reference found under $directory/" >&2
        failureCount=$((failureCount + 1))
    fi
done

if [ "$failureCount" -eq 0 ]; then
    echo "  sources clean"
fi

if command -v nm >/dev/null 2>&1; then
    echo "checking compiled objects for allocator symbols..."
    scratchDirectory=$(mktemp -d)
    trap 'rm -rf "$scratchDirectory"' EXIT

    HOST_COMPILER=${HOST_COMPILER:-clang}
    for source in engine/source/*.c render/openGLES2/*.c platform/linux/*.c; do
        objectName="$scratchDirectory/$(basename "$source" .c).o"
        "$HOST_COMPILER" -std=c99 -Iengine/include -c "$source" -o "$objectName"
    done

    if nm --undefined-only "$scratchDirectory"/*.o 2>/dev/null \
        | grep -E ' (malloc|calloc|realloc|free|aligned_alloc|posix_memalign|strdup)$'; then
        echo "error: compiled object references an allocator" >&2
        failureCount=$((failureCount + 1))
    else
        echo "  objects clean"
    fi
else
    echo "  nm unavailable, skipping object symbol check"
fi

if [ "$failureCount" -ne 0 ]; then
    echo "no-dynamic-allocation check FAILED" >&2
    exit 1
fi

echo "no-dynamic-allocation check passed"
