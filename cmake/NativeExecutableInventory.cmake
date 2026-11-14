include_guard(GLOBAL)

# Collect real executable declarations after all project directories are
# configured. Explicit target exclusion overrides inherited directory exclusion.
# Excluded declarations remain visible but unqualified, even if a dependency or
# manual build produces them. Eligibility is deliberately determined by these
# declarations rather than by the presence of a possibly stale file.
function(_flitzzip_collect_native_executables directory inherited_exclusion
         output)
  get_property(
    directory_exclusion
    DIRECTORY "${directory}"
    PROPERTY EXCLUDE_FROM_ALL)
  if(inherited_exclusion OR directory_exclusion)
    set(excluded_by_directory ON)
  else()
    set(excluded_by_directory OFF)
  endif()
  get_property(
    targets
    DIRECTORY "${directory}"
    PROPERTY BUILDSYSTEM_TARGETS)
  string(REPLACE "," "$<COMMA>" binary_root "${CMAKE_BINARY_DIR}")
  set(records)
  foreach(target IN LISTS targets)
    get_target_property(type "${target}" TYPE)
    if(NOT type STREQUAL "EXECUTABLE")
      continue()
    endif()
    get_property(
      target_exclusion_set
      TARGET "${target}"
      PROPERTY EXCLUDE_FROM_ALL
      SET)
    if(target_exclusion_set)
      string(CONCAT exclusion "$<TARGET_GENEX_EVAL:${target},"
                    "$<TARGET_PROPERTY:${target},EXCLUDE_FROM_ALL>>")
    else()
      set(exclusion "${excluded_by_directory}")
    endif()
    # TARGET_FILE yields a CMake path; the relative form never records a host
    # root.
    string(CONCAT target_path "$<PATH:RELATIVE_PATH,"
                  "$<TARGET_FILE:${target}>,${binary_root}>")
    string(CONCAT record "{\"target\":\"${target}\","
                  "\"path\":\"${target_path}\","
                  "\"default_build\":$<IF:$<BOOL:${exclusion}>,false,true>}")
    list(APPEND records "${record}")
  endforeach()
  get_property(
    children
    DIRECTORY "${directory}"
    PROPERTY SUBDIRECTORIES)
  foreach(child IN LISTS children)
    _flitzzip_collect_native_executables("${child}" "${excluded_by_directory}"
                                         child_records)
    list(APPEND records ${child_records})
  endforeach()
  set(${output}
      "${records}"
      PARENT_SCOPE)
endfunction()

# Generate configuration-specific declarations without building targets,
# changing their output locations or adding optional executables to the default
# build.
function(flitzzip_write_native_executable_inventory)
  _flitzzip_collect_native_executables("${CMAKE_SOURCE_DIR}" OFF records)
  list(SORT records)
  list(JOIN records ",\n" body)
  string(
    CONCAT content "{\"schema_version\":1,"
           "\"configuration\":\"$<CONFIG>\",\"executables\":[\n${body}\n]}\n")
  file(
    GENERATE
    OUTPUT "${CMAKE_BINARY_DIR}/$<CONFIG>/flitzzip-native-executables.json"
    CONTENT "${content}")
endfunction()
