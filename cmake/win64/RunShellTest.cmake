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

if(WAF_ROOT AND EXISTS "${WAF_ROOT}")
  set(ENV{ARDOUR_DLL_PATH} "${WAF_ROOT}/libs/ardour")
  set(ENV{ARDOUR_BACKEND_PATH}
      "${WAF_ROOT}/libs/backends/dummy;${WAF_ROOT}/libs/backends/wasapi")

  # ardour-vst3-scanner.exe is a *child* process, so Windows resolves its
  # imports from its own directory — where the engine DLLs are not. Without the
  # same lib dirs run-engine-shell-release.bat puts on PATH, the scanner dies at
  # loader init (0xC0000135) and PluginManager::run_vst3_scanner_app () waits on
  # it forever, because no scan timeout is enabled. A VST3 test then hangs
  # instead of reporting the missing plugin.
  foreach(_lib IN ITEMS
      pbd temporal "midi++2" evoral ardour audiographer ptformat lua libltc
      "ctrl-interface/control_protocol" "ctrl-interface/midi_surface"
      backends/dummy backends/wasapi ardour/panners fst)
    set(_dir "${WAF_ROOT}/libs/${_lib}")
    if(EXISTS "${_dir}")
      set(_path "${_dir};${_path}")
    endif()
  endforeach()
endif()

set(ENV{PATH} "${_path}")

execute_process(COMMAND "${TEST_EXE}" ${TEST_ARGS} RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "${TEST_EXE} ${TEST_ARGS} exited ${_rc}")
endif()
