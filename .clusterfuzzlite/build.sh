#!/bin/bash -eu
set -euo pipefail

: "${CXX:?ClusterFuzzLite did not provide CXX}"
: "${CXXFLAGS:?ClusterFuzzLite did not provide CXXFLAGS}"
: "${LIB_FUZZING_ENGINE:?ClusterFuzzLite did not provide LIB_FUZZING_ENGINE}"
: "${OUT:?ClusterFuzzLite did not provide OUT}"

CC="${CC:-clang}"
CFLAGS="${CFLAGS:-$CXXFLAGS}"

COMMON_FLAGS=(
  -std=c++20
  -Isrc
  -Ithird_party/lzma_sdk/C
  -Ithird_party/lhasa/lib/public
  -Ithird_party/lhasa/lib
  -DSUPERZIP_ENABLE_HIP=0
)

MINIZ_UPSTREAM_ARCHIVE="third_party/upstream/miniz/3.1.1/miniz-3.1.1-source.zip"
MINIZ_UPSTREAM_EXTRACT="$OUT/miniz-upstream"
MINIZ_UPSTREAM_SOURCE="$MINIZ_UPSTREAM_EXTRACT/miniz-3.1.1"
MINIZ_SOURCES=(miniz.c miniz_tdef.c miniz_tinfl.c)

LZMA_SDK_SOURCES=(
  third_party/lzma_sdk/C/7zArcIn.c
  third_party/lzma_sdk/C/7zBuf.c
  third_party/lzma_sdk/C/7zBuf2.c
  third_party/lzma_sdk/C/7zCrc.c
  third_party/lzma_sdk/C/7zCrcOpt.c
  third_party/lzma_sdk/C/7zDec.c
  third_party/lzma_sdk/C/7zStream.c
  third_party/lzma_sdk/C/Bcj2.c
  third_party/lzma_sdk/C/Bra.c
  third_party/lzma_sdk/C/Bra86.c
  third_party/lzma_sdk/C/BraIA64.c
  third_party/lzma_sdk/C/CpuArch.c
  third_party/lzma_sdk/C/Delta.c
  third_party/lzma_sdk/C/Lzma2Dec.c
  third_party/lzma_sdk/C/LzmaDec.c
  third_party/lzma_sdk/C/Ppmd7.c
  third_party/lzma_sdk/C/Ppmd7Dec.c
)

LHASA_SOURCES=(
  third_party/lhasa/lib/crc16.c
  third_party/lhasa/lib/ext_header.c
  third_party/lhasa/lib/lha_decoder.c
  third_party/lhasa/lib/lha_endian.c
  third_party/lhasa/lib/lha_file_header.c
  third_party/lhasa/lib/lha_input_stream.c
  third_party/lhasa/lib/lha_basic_reader.c
  third_party/lhasa/lib/lha_reader.c
  third_party/lhasa/lib/macbinary.c
  third_party/lhasa/lib/null_decoder.c
  third_party/lhasa/lib/lh1_decoder.c
  third_party/lhasa/lib/lh5_decoder.c
  third_party/lhasa/lib/lh6_decoder.c
  third_party/lhasa/lib/lh7_decoder.c
  third_party/lhasa/lib/lhx_decoder.c
  third_party/lhasa/lib/lk7_decoder.c
  third_party/lhasa/lib/lz5_decoder.c
  third_party/lhasa/lib/lzs_decoder.c
  third_party/lhasa/lib/pm1_decoder.c
  third_party/lhasa/lib/pm2_decoder.c
)

prepare_upstream_miniz() {
  rm -rf "$MINIZ_UPSTREAM_EXTRACT"
  mkdir -p "$MINIZ_UPSTREAM_EXTRACT"
  (cd third_party/upstream/miniz/3.1.1 && tr -d '\r' < SHA256SUMS | sha256sum -c -)
  unzip -q "$MINIZ_UPSTREAM_ARCHIVE" -d "$MINIZ_UPSTREAM_EXTRACT"
  cat > "$MINIZ_UPSTREAM_SOURCE/miniz_export.h" <<'EOF'
#pragma once
#define MINIZ_EXPORT
EOF
  test -f "$MINIZ_UPSTREAM_SOURCE/miniz.h"
}

build_miniz_objects() {
  local source_dir="$1"
  local object_dir="$2"
  mkdir -p "$object_dir"
  local objects=()
  for source_name in "${MINIZ_SOURCES[@]}"; do
    local source="$source_dir/$source_name"
    local object="$object_dir/$(basename "$source_name" .c).o"
    "$CC" $CFLAGS -std=c11 -I"$source_dir" \
      -Wno-conversion -Wno-sign-conversion -c "$source" -o "$object"
    objects+=("$object")
  done
  printf '%s\n' "${objects[@]}"
}

build_lzma_sdk_objects() {
  local object_dir="$1"
  mkdir -p "$object_dir"
  local objects=()
  for source in "${LZMA_SDK_SOURCES[@]}"; do
    local object="$object_dir/$(basename "$source" .c).o"
    "$CC" $CFLAGS -std=c11 -Ithird_party/lzma_sdk/C -DZ7_EXTRACT_ONLY -DZ7_PPMD_SUPPORT \
      -Wno-conversion -Wno-sign-conversion -c "$source" -o "$object"
    objects+=("$object")
  done
  printf '%s\n' "${objects[@]}"
}

build_lhasa_objects() {
  local object_dir="$1"
  mkdir -p "$object_dir"
  local objects=()
  for source in "${LHASA_SOURCES[@]}"; do
    local object="$object_dir/$(basename "$source" .c).o"
    "$CC" $CFLAGS -std=c11 -Ithird_party/lhasa/lib/public -Ithird_party/lhasa/lib \
      -Wno-conversion -Wno-sign-conversion -c "$source" -o "$object"
    objects+=("$object")
  done
  printf '%s\n' "${objects[@]}"
}

"$CXX" $CXXFLAGS "${COMMON_FLAGS[@]}" \
  fuzz/archive_index_fuzzer.cpp \
  src/core/archive_index.cpp \
  -o "$OUT/superzip_archive_index_fuzzer" \
  "$LIB_FUZZING_ENGINE"

"$CXX" $CXXFLAGS "${COMMON_FLAGS[@]}" \
  fuzz/path_safety_fuzzer.cpp \
  src/core/path_safety.cpp \
  -o "$OUT/superzip_path_safety_fuzzer" \
  "$LIB_FUZZING_ENGINE"

"$CXX" $CXXFLAGS "${COMMON_FLAGS[@]}" \
  fuzz/iso_fuzzer.cpp \
  src/iso/iso_adapter.cpp \
  src/core/file_publish.cpp \
  src/core/path_safety.cpp \
  src/core/progress.cpp \
  -o "$OUT/superzip_iso_fuzzer" \
  "$LIB_FUZZING_ENGINE"

"$CXX" $CXXFLAGS "${COMMON_FLAGS[@]}" \
  fuzz/cab_header_fuzzer.cpp \
  src/cab/cab_format.cpp \
  src/core/path_safety.cpp \
  -o "$OUT/superzip_cab_header_fuzzer" \
  "$LIB_FUZZING_ENGINE"

"$CXX" $CXXFLAGS "${COMMON_FLAGS[@]}" \
  fuzz/rpm_header_fuzzer.cpp \
  src/rpm/rpm_format.cpp \
  -o "$OUT/superzip_rpm_header_fuzzer" \
  "$LIB_FUZZING_ENGINE"

build_lzma_sdk_objects "$OUT/lzma-sdk-objects" > "$OUT/lzma-sdk-objects.list"
mapfile -t LZMA_SDK_OBJECTS < "$OUT/lzma-sdk-objects.list"
"$CXX" $CXXFLAGS "${COMMON_FLAGS[@]}" \
  fuzz/sevenzip_fuzzer.cpp \
  src/sevenzip/sevenzip_adapter.cpp \
  src/core/file_publish.cpp \
  src/core/path_safety.cpp \
  src/core/progress.cpp \
  "${LZMA_SDK_OBJECTS[@]}" \
  -o "$OUT/superzip_sevenzip_fuzzer" \
  "$LIB_FUZZING_ENGINE"

build_lhasa_objects "$OUT/lhasa-objects" > "$OUT/lhasa-objects.list"
mapfile -t LHASA_OBJECTS < "$OUT/lhasa-objects.list"
"$CXX" $CXXFLAGS "${COMMON_FLAGS[@]}" \
  fuzz/lha_fuzzer.cpp \
  src/lha/lha_adapter.cpp \
  src/core/file_publish.cpp \
  src/core/path_safety.cpp \
  src/core/progress.cpp \
  "${LHASA_OBJECTS[@]}" \
  -o "$OUT/superzip_lha_fuzzer" \
  "$LIB_FUZZING_ENGINE"

prepare_upstream_miniz
build_miniz_objects "$MINIZ_UPSTREAM_SOURCE" "$OUT/miniz-objects" > "$OUT/miniz-objects.list"
mapfile -t MINIZ_OBJECTS < "$OUT/miniz-objects.list"
"$CXX" $CXXFLAGS "${COMMON_FLAGS[@]}" -I"$MINIZ_UPSTREAM_SOURCE" \
  fuzz/xar_fuzzer.cpp \
  src/xar/xar_adapter.cpp \
  src/core/file_publish.cpp \
  src/core/path_safety.cpp \
  src/core/progress.cpp \
  "${MINIZ_OBJECTS[@]}" \
  -o "$OUT/superzip_xar_fuzzer" \
  "$LIB_FUZZING_ENGINE"

cp fuzz/superzip.dict "$OUT/superzip_archive_index_fuzzer.dict"
cp fuzz/superzip.dict "$OUT/superzip_path_safety_fuzzer.dict"
cp fuzz/superzip.dict "$OUT/superzip_iso_fuzzer.dict"
cp fuzz/superzip.dict "$OUT/superzip_cab_header_fuzzer.dict"
cp fuzz/superzip.dict "$OUT/superzip_rpm_header_fuzzer.dict"
cp fuzz/superzip.dict "$OUT/superzip_sevenzip_fuzzer.dict"
cp fuzz/superzip.dict "$OUT/superzip_lha_fuzzer.dict"
cp fuzz/superzip.dict "$OUT/superzip_xar_fuzzer.dict"
cp fuzz/*.options "$OUT/"

for target in \
  superzip_archive_index_fuzzer \
  superzip_path_safety_fuzzer \
  superzip_iso_fuzzer \
  superzip_cab_header_fuzzer \
  superzip_rpm_header_fuzzer \
  superzip_sevenzip_fuzzer \
  superzip_lha_fuzzer \
  superzip_xar_fuzzer
do
  test -x "$OUT/$target"
done
