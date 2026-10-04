#!/usr/bin/env bash
# Compile the emulator cores (core/*.cpp) to web/core/brick.wasm.
# Only needed after changing something in core/: the built file is part of the repository.
# Needs clang and lld with the WebAssembly target (sudo apt install clang lld), or else Zig's
# bundled clang (pip install ziglang), which is used automatically when clang is missing.
set -euo pipefail
cd "$(dirname "$0")"
TARGET=wasm32
if [ -n "${CXX:-}" ]; then CC_CMD=($CXX)
elif command -v clang++ >/dev/null; then CC_CMD=(clang++)
elif python3 -c 'import ziglang' 2>/dev/null; then CC_CMD=(python3 -m ziglang c++); TARGET=wasm32-freestanding
else echo "clang++ is not installed (sudo apt install clang lld, or pip install ziglang)"; exit 1; fi
"${CC_CMD[@]}" --target=$TARGET -std=c++11 -O2 -nostdlib -ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics \
  -fvisibility=hidden -Wall -Wextra \
  -Wl,--no-entry -Wl,--strip-all -Wl,-z,stack-size=65536 \
  -o web/core/brick.wasm core/brick.cpp core/ht4bit.cpp core/e0c6200.cpp core/wasm_api.cpp
ls -l web/core/brick.wasm
