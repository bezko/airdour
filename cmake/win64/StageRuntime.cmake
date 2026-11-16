# Copy runtime DLLs next to a target at build time.
#
# Invoked from POST_BUILD with:
#   -DSTAGE_DEST=<target dir> -DSTAGE_FILES=<;-list of dll paths>
#
# Unlike a configure-time if(EXISTS), this does not bake stale decisions into
# the build tree: adding/removing a waf DLL is picked up on the next build.

if(NOT STAGE_DEST)
  message(FATAL_ERROR "StageRuntime.cmake: STAGE_DEST is required")
endif()

foreach(_src IN LISTS STAGE_FILES)
  if(EXISTS "${_src}")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_src}" "${STAGE_DEST}"
      RESULT_VARIABLE _rc
    )
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "Failed to stage ${_src} -> ${STAGE_DEST} (${_rc})")
    endif()
  else()
    message(STATUS "StageRuntime: skip missing ${_src}")
  endif()
endforeach()
