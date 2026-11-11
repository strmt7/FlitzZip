# Exercise the actual probe producers in owned source/module/output fixtures.
# Fixed archive metadata detects unnecessary writes without elapsed
# measurements.
cmake_minimum_required(VERSION 3.24)

set(_zstd_probe_outputs
    raw.c raw.c.source-identity huffman.c huffman.c.source-identity
    dictionary.c dictionary.c.source-identity)
set(_zstd_probe_sentinel "2000-01-01T00:00:00Z")
set(_zstd_probe_sources
    compress/zstd_compress_internal.h decompress/huf_decompress.c
    dictBuilder/zdict.c common/zstd_internal.h)

if(DEFINED PROBE_ACTION)
  if(PROBE_ACTION STREQUAL "stamp")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E tar cf fixed-metadata.tar --format=pax
              "--mtime=2000-01-01 00:00:00 UTC" -- ${_zstd_probe_outputs}
      WORKING_DIRECTORY "${OUTPUT_ROOT}"
      RESULT_VARIABLE result
      TIMEOUT 10)
    if(NOT result STREQUAL "0")
      message(FATAL_ERROR "Fixed probe metadata archive creation failed")
    endif()
    foreach(name IN LISTS _zstd_probe_outputs)
      file(REMOVE "${OUTPUT_ROOT}/${name}")
    endforeach()
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E tar xf fixed-metadata.tar
      WORKING_DIRECTORY "${OUTPUT_ROOT}"
      RESULT_VARIABLE result
      TIMEOUT 10)
    if(NOT result STREQUAL "0")
      message(FATAL_ERROR "Fixed probe metadata archive extraction failed")
    endif()
  elseif(PROBE_ACTION STREQUAL "generate")
    set(FLITZZIP_SOURCE_ROOT "${REPO_ROOT}")
    include("${MODULE_ROOT}/RawBlockProbe.cmake")
    include("${MODULE_ROOT}/HuffmanTableProbe.cmake")
    include("${MODULE_ROOT}/DictionaryMergeProbe.cmake")
    flitzzip_generate_zstd_raw_block_probe(
      "${SOURCE_ROOT}/compress/zstd_compress_internal.h" "${OUTPUT_ROOT}/raw.c")
    flitzzip_generate_zstd_huffman_probe(
      "${SOURCE_ROOT}/decompress/huf_decompress.c" "${OUTPUT_ROOT}/huffman.c")
    flitzzip_generate_zstd_dictionary_probe("${SOURCE_ROOT}/dictBuilder/zdict.c"
                                            "${OUTPUT_ROOT}/dictionary.c")
  else()
    message(FATAL_ERROR "Unknown probe fixture action")
  endif()
  return()
endif()

# Run the same producers in fresh CMake scope; reject a timeout or unexpected
# process failure even when testing intentional source-identity rejection.
function(run_zstd_probe_fixture action modules source output expected_error)
  execute_process(
    COMMAND
      "${CMAKE_COMMAND}" "-DREPO_ROOT=${REPO_ROOT}" "-DPROBE_ACTION=${action}"
      "-DMODULE_ROOT=${modules}" "-DSOURCE_ROOT=${source}"
      "-DOUTPUT_ROOT=${output}" -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
    WORKING_DIRECTORY "${output}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
    TIMEOUT 30)
  if(expected_error STREQUAL "")
    if(NOT result STREQUAL "0")
      message(FATAL_ERROR "Probe fixture ${action} failed: ${stdout}${stderr}")
    endif()
  elseif(NOT result STREQUAL "1" OR NOT "${stdout}${stderr}" MATCHES
                                    "${expected_error}")
    message(
      FATAL_ERROR "Probe source guard failed: ${result}; ${stdout}${stderr}")
  endif()
endfunction()

# Keep byte identities independent of the producer's text comparisons and its
# metadata sidecars. Prefix snapshots belong to the calling fixture scope.
function(snapshot_zstd_probe_outputs output prefix)
  foreach(name IN LISTS _zstd_probe_outputs)
    file(SHA256 "${output}/${name}" hash)
    string(MAKE_C_IDENTIFIER "${prefix}_${name}" key)
    set(${key}
        "${hash}"
        PARENT_SCOPE)
  endforeach()
endfunction()

# Compare every output against a caller-owned byte snapshot. When stamped,
# require the fixed metadata sentinel too, rather than sampling a running clock.
function(require_zstd_probe_outputs output prefix stamped)
  foreach(name IN LISTS _zstd_probe_outputs)
    file(SHA256 "${output}/${name}" actual)
    string(MAKE_C_IDENTIFIER "${prefix}_${name}" key)
    if(NOT actual STREQUAL "${${key}}")
      message(FATAL_ERROR "Probe bytes changed unexpectedly: ${name}")
    endif()
    if(stamped)
      file(TIMESTAMP "${output}/${name}" actual_stamp "%Y-%m-%dT%H:%M:%SZ" UTC)
      if(NOT actual_stamp STREQUAL _zstd_probe_sentinel)
        message(FATAL_ERROR "Unchanged probe output was rewritten: ${name}")
      endif()
    endif()
  endforeach()
endfunction()

# A real bridge edit must regenerate only its C output and its declared bridge
# identity. Dictionary provenance covers imported regions, not the owned bridge;
# the native build-input inventory separately binds that bridge's complete
# bytes.
function(check_zstd_probe_bridges modules source output)
  foreach(kind IN ITEMS raw huffman dictionary)
    if(kind STREQUAL "raw")
      set(bridge "${modules}/raw_block_probe.c")
    elseif(kind STREQUAL "huffman")
      set(bridge "${modules}/huffman_table_probe.c")
    else()
      set(bridge "${modules}/dictionary_merge_probe.c")
    endif()
    configure_file("${bridge}" "${bridge}.original" COPYONLY)
    file(APPEND "${bridge}" "\n/* Independent owned bridge transition. */\n")
    run_zstd_probe_fixture(generate "${modules}" "${source}" "${output}" "")
    foreach(name IN LISTS _zstd_probe_outputs)
      file(SHA256 "${output}/${name}" actual)
      string(MAKE_C_IDENTIFIER "baseline_${name}" key)
      set(should_change FALSE)
      if(name STREQUAL "${kind}.c"
         OR (NOT kind STREQUAL "dictionary" AND name STREQUAL
                                                "${kind}.c.source-identity"))
        set(should_change TRUE)
      endif()
      if((should_change AND actual STREQUAL "${${key}}")
         OR (NOT should_change AND NOT actual STREQUAL "${${key}}"))
        message(
          FATAL_ERROR "Bridge transition did not match output contract: ${name}"
        )
      endif()
    endforeach()
    configure_file("${bridge}.original" "${bridge}" COPYONLY)
    run_zstd_probe_fixture(generate "${modules}" "${source}" "${output}" "")
    require_zstd_probe_outputs("${output}" baseline FALSE)
  endforeach()
endfunction()

# Mutate private copies of the three primary inputs. Earlier generators in a
# rejected sequence must not rewrite otherwise admitted output or provenance.
function(check_zstd_probe_source_rejection modules source output fixture)
  set(inputs "${_zstd_probe_sources}")
  set(invalid "${fixture}/invalid-source")
  foreach(relative IN LISTS inputs)
    get_filename_component(parent "${invalid}/${relative}" DIRECTORY)
    file(MAKE_DIRECTORY "${parent}")
    configure_file("${source}/${relative}" "${invalid}/${relative}" COPYONLY)
  endforeach()
  run_zstd_probe_fixture(stamp "${modules}" "${source}" "${output}" "")
  foreach(index RANGE 0 2 1)
    list(GET inputs ${index} relative)
    file(APPEND "${invalid}/${relative}" "\n/* Independent source drift. */\n")
    if(index EQUAL 0)
      set(cause "Raw block probe source identity mismatch")
    elseif(index EQUAL 1)
      set(cause "Huffman table probe source identity mismatch")
    else()
      set(cause "Dictionary merge probe source identity mismatch")
    endif()
    run_zstd_probe_fixture(generate "${modules}" "${invalid}" "${output}"
                           "${cause}")
    require_zstd_probe_outputs("${output}" baseline TRUE)
    configure_file("${source}/${relative}" "${invalid}/${relative}" COPYONLY)
  endforeach()
endfunction()

# Require the ordinary configuration to register every consumed source and
# bridge, using the parent's actual generator. No compiler or product workload
# is launched.
function(check_zstd_probe_configuration modules source fixture)
  if(NOT DEFINED GENERATOR OR GENERATOR STREQUAL "")
    message(FATAL_ERROR "Probe graph requires the selected parent generator")
  endif()
  set(generator_args -G "${GENERATOR}")
  if(DEFINED GENERATOR_PLATFORM AND NOT GENERATOR_PLATFORM STREQUAL "")
    list(APPEND generator_args -A "${GENERATOR_PLATFORM}")
  endif()
  if(DEFINED GENERATOR_TOOLSET AND NOT GENERATOR_TOOLSET STREQUAL "")
    list(APPEND generator_args -T "${GENERATOR_TOOLSET}")
  endif()
  execute_process(
    COMMAND
      "${CMAKE_COMMAND}" -S "${REPO_ROOT}/tests/cmake/probe_configuration" -B
      "${fixture}/configuration" ${generator_args} "-DREPO_ROOT=${REPO_ROOT}"
      "-DSOURCE_ROOT=${source}" "-DMODULE_ROOT=${modules}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
    TIMEOUT 30)
  if(NOT result STREQUAL "0")
    message(FATAL_ERROR "Probe configuration graph failed: ${stdout}${stderr}")
  endif()
endfunction()

# Operate only under the caller's new owned scratch directory. Module templates
# may be frozen old source for a negative control; product source is never
# edited.
function(check_zstd_probe_outputs source scratch)
  foreach(relative IN LISTS _zstd_probe_sources)
    if(NOT EXISTS "${source}/${relative}")
      message(FATAL_ERROR "Probe fixture input missing: ${relative}")
    endif()
  endforeach()
  set(fixture "${scratch}/probe-outputs")
  if(EXISTS "${fixture}")
    message(FATAL_ERROR "Refusing a preexisting probe-output fixture")
  endif()
  set(modules "${fixture}/modules")
  set(output "${fixture}/outputs")
  file(MAKE_DIRECTORY "${modules}" "${output}")
  set(template_root "${REPO_ROOT}/tests/zstd")
  if(DEFINED PROBE_MODULE_TEMPLATE_ROOT)
    set(template_root "${PROBE_MODULE_TEMPLATE_ROOT}")
  endif()
  foreach(
    name IN
    ITEMS RawBlockProbe.cmake
          HuffmanTableProbe.cmake
          DictionaryMergeProbe.cmake
          ProbeOutput.cmake
          raw_block_probe.c
          huffman_table_probe.c
          dictionary_merge_probe.c)
    configure_file("${template_root}/${name}" "${modules}/${name}" COPYONLY)
  endforeach()
  run_zstd_probe_fixture(generate "${modules}" "${source}" "${output}" "")
  snapshot_zstd_probe_outputs("${output}" baseline)
  run_zstd_probe_fixture(stamp "${modules}" "${source}" "${output}" "")
  require_zstd_probe_outputs("${output}" baseline TRUE)
  run_zstd_probe_fixture(generate "${modules}" "${source}" "${output}" "")
  require_zstd_probe_outputs("${output}" baseline TRUE)
  foreach(name IN LISTS _zstd_probe_outputs)
    file(WRITE "${output}/${name}" "Independent corrupt generated output.\n")
  endforeach()
  run_zstd_probe_fixture(generate "${modules}" "${source}" "${output}" "")
  require_zstd_probe_outputs("${output}" baseline FALSE)
  check_zstd_probe_bridges("${modules}" "${source}" "${output}")
  check_zstd_probe_source_rejection("${modules}" "${source}" "${output}"
                                    "${fixture}")
  message(STATUS "Probe output transitions and source rejection passed")
endfunction()

if(DEFINED PROBE_FIXTURE_ROOT)
  check_zstd_probe_outputs("${SOURCE_ROOT}" "${PROBE_FIXTURE_ROOT}")
endif()
