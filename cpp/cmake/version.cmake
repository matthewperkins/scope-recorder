# Writes ${OUT} with SCOPE_RECORDER_VERSION = `git describe` of ${SRC}. Runs on every build,
# but only touches the file when the version changes, so it doesn't trigger needless rebuilds.
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY "${SRC}"
    OUTPUT_VARIABLE version
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT version)
    set(version "unknown")
endif()
set(content "#pragma once\n#define SCOPE_RECORDER_VERSION \"${version}\"\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" previous)
endif()
if(NOT "${previous}" STREQUAL "${content}")
    file(WRITE "${OUT}" "${content}")
endif()
