#!/bin/bash -eu

: "${CXX:?ClusterFuzzLite did not provide CXX}"
: "${CXXFLAGS:?ClusterFuzzLite did not provide CXXFLAGS}"
: "${LIB_FUZZING_ENGINE:?ClusterFuzzLite did not provide LIB_FUZZING_ENGINE}"
: "${OUT:?ClusterFuzzLite did not provide OUT}"

COMMON_FLAGS=(
  -std=c++20
  -Isrc
  -DSUPERZIP_ENABLE_HIP=0
)

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

cp fuzz/superzip.dict "$OUT/superzip_archive_index_fuzzer.dict"
cp fuzz/superzip.dict "$OUT/superzip_path_safety_fuzzer.dict"
cp fuzz/superzip.dict "$OUT/superzip_iso_fuzzer.dict"
cp fuzz/*.options "$OUT/"
