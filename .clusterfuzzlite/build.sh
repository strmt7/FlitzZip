#!/bin/bash -eu

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
  -DSUPERZIP_ENABLE_HIP=0
)

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

mapfile -t LZMA_SDK_OBJECTS < <(build_lzma_sdk_objects "$OUT/lzma-sdk-objects")
"$CXX" $CXXFLAGS "${COMMON_FLAGS[@]}" \
  fuzz/sevenzip_fuzzer.cpp \
  src/sevenzip/sevenzip_adapter.cpp \
  src/core/file_publish.cpp \
  src/core/path_safety.cpp \
  src/core/progress.cpp \
  "${LZMA_SDK_OBJECTS[@]}" \
  -o "$OUT/superzip_sevenzip_fuzzer" \
  "$LIB_FUZZING_ENGINE"

cp fuzz/superzip.dict "$OUT/superzip_archive_index_fuzzer.dict"
cp fuzz/superzip.dict "$OUT/superzip_path_safety_fuzzer.dict"
cp fuzz/superzip.dict "$OUT/superzip_iso_fuzzer.dict"
cp fuzz/superzip.dict "$OUT/superzip_cab_header_fuzzer.dict"
cp fuzz/superzip.dict "$OUT/superzip_rpm_header_fuzzer.dict"
cp fuzz/superzip.dict "$OUT/superzip_sevenzip_fuzzer.dict"
cp fuzz/*.options "$OUT/"
