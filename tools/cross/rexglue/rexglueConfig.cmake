# Wrapper around the Windows SDK package for cross builds: load it as-is, then point
# rex::rexglue (used by the codegen custom command) at the Linux host binary.
get_filename_component(_rexglue_tools "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${_rexglue_tools}/rexglue-sdk-win/lib/cmake/rexglue/rexglueConfig.cmake")

set(_rexglue_host_exe "${_rexglue_tools}/rexglue-sdk/bin/rexglue")
foreach(_cfg RELEASE RELWITHDEBINFO DEBUG)
  set_property(TARGET rex::rexglue PROPERTY IMPORTED_LOCATION_${_cfg} "${_rexglue_host_exe}")
endforeach()
