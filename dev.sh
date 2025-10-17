#!/usr/bin/env bash
set -eo pipefail

if [[ -n "$DEBUG" ]]; then
    set -x
fi

case "$1" in
build)
    rm -rf build
    meson setup build
    meson compile -C build

    # Put compile_commands.json at project root (above ./build).
    if [ -f build/compile_commands.json ]; then
        ln -sfn build/compile_commands.json compile_commands.json 2>/dev/null || cp build/compile_commands.json compile_commands.json
    else
        # Fallback for older Meson/Ninja: generate it via ninja's compdb tool.
        ninja -C build -t compdb >compile_commands.json
    fi
    ;;
clean)
    rm -rf ./build
    ;;
esac
