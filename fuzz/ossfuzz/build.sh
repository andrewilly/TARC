#!/bin/bash -eu
# OSS-Fuzz build script for TARC
# https://google.github.io/oss-fuzz/getting-started/new-project-guide/

SRC="${SRC:-.}"
OUT="${OUT:-$SRC/fuzz/ossfuzz/out}"
WORK="${WORK:-$SRC/fuzz/ossfuzz/work}"

mkdir -p "$OUT" "$WORK"

CXXFLAGS="${CXXFLAGS:--std=c++17 -Wall -O1 -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer}"
CFLAGS="${CFLAGS:--std=c11 -Wall -O1 -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer}"
LIB_FUZZING_ENGINE="${LIB_FUZZING_ENGINE:--fsanitize=fuzzer}"

INCLUDES="-I$SRC/include"

LDFLAGS="-ldl -lzstd -llz4 -llzma -lbrotlienc -lbrotlidec -lbrotlicommon $LIB_FUZZING_ENGINE"

# Compile core objects
$CXX $CXXFLAGS $INCLUDES -c "$SRC/src/ui.cpp" -o "$WORK/ui.o"
$CXX $CXXFLAGS $INCLUDES -c "$SRC/src/io.cpp" -o "$WORK/io.o"
$CXX $CXXFLAGS $INCLUDES -c "$SRC/src/engine.cpp" -o "$WORK/engine.o"
$CC  $CFLAGS  $INCLUDES -c "$SRC/src/xxhash.c" -o "$WORK/xxhash.o"

# Compile fuzz target
$CXX $CXXFLAGS $INCLUDES -c "$SRC/fuzz/fuzz_archive.cpp" -o "$WORK/fuzz_archive.o"

# Link
$CXX $CXXFLAGS \
    "$WORK/fuzz_archive.o" \
    "$WORK/ui.o" \
    "$WORK/io.o" \
    "$WORK/engine.o" \
    "$WORK/xxhash.o" \
    -o "$OUT/fuzz_archive" \
    $LDFLAGS

# Optionally create a seed corpus from test archives
if [ -d "$SRC/test" ]; then
    mkdir -p "$OUT/fuzz_archive_seed_corpus"
    find "$SRC/test" -name "*.strk" -exec cp {} "$OUT/fuzz_archive_seed_corpus/" \; 2>/dev/null || true
fi
