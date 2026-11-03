# Run one airdour_win_shell smoke test with the engine runtime environment.
#
# CTest properties cannot express a Windows PATH containing ';' portably
# before CMake 3.22, so tests go through this script.
#
# -DTEST_EXE=<exe> -DTEST_ARGS=<;list> -DVCPKG_BIN=<prefix/bin>
# -DWAF_ROOT=<waf build dir>

if(NOT TEST_EXE)
  message(FATAL_ERROR "RunShellTest.cmake: TEST_EXE is required")
endif()

set(_path "$ENV{PATH}")
if(VCPKG_BIN AND EXISTS "${VCPKG_BIN}")
  set(_path "${VCPKG_BIN};${_path}")
endif()
set(ENV{PATH} "${_path}")

if(WAF_ROOT AND EXISTS "${WAF_ROOT}")
  set(ENV{ARDOUR_DLL_PATH} "${WAF_ROOT}/libs/ardour")
  set(ENV{ARDOUR_BACKEND_PATH}
      "${WAF_ROOT}/libs/backends/dummy;${WAF_ROOT}/libs/backends/wasapi")
endif()

execute_process(COMMAND "${TEST_EXE}" ${TEST_ARGS} RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "${TEST_EXE} ${TEST_ARGS} exited ${_rc}")
endif()
