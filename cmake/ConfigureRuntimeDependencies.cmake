# Purpose: Generate the common CPU/HIP native runtime manifest. Inputs: Template
# directory, output path, HIP flag and caller provenance. Outputs: JSON with
# truthful HIP scope and common app-local dependencies.
function(superzip_configure_runtime_dependencies template_directory output_path
         hip_enabled)
  set(hip_enabled_json false)
  set(host_prerequisites_json "[]")
  if(hip_enabled)
    set(hip_enabled_json true)
    set(prerequisites_template
        "${template_directory}/superzip-hip-prerequisites.json.in")
    set_property(
      DIRECTORY
      APPEND
      PROPERTY CMAKE_CONFIGURE_DEPENDS "${prerequisites_template}")
    file(READ "${prerequisites_template}" prerequisites LIMIT 65536)
    string(CONFIGURE "${prerequisites}" host_prerequisites_json @ONLY)
  endif()
  configure_file("${template_directory}/superzip-runtime-dependencies.json.in"
                 "${output_path}" @ONLY)
endfunction()
