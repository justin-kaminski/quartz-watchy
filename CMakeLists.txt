# Quartz firmware (ESP-IDF project). Host-side unit tests and the simulator build from host/.
cmake_minimum_required(VERSION 3.22)

# Short git hash for the About screen and the console `version` / `ready` event. QZ_GIT_HASH is
# consumed by qz_platform (IdfSystem::firmware). Empty when git or .git is absent (source
# tarball): the firmware then reports no hash. "-dirty" = tracked files differ from HEAD. The
# value is evaluated when CMake configures; HEAD / reflog changes re-trigger the configure step, a
# plain edit of a tracked file does not (run `tools/fw.sh reconfigure` before flashing a build you
# want labelled exactly).
set(QZ_GIT_HASH "")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/.git")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" --no-optional-locks rev-parse --short=7 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    OUTPUT_VARIABLE _qz_hash
    RESULT_VARIABLE _qz_hash_rc
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(_qz_hash_rc EQUAL 0 AND _qz_hash)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" --no-optional-locks status --porcelain --untracked-files=no
      WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
      OUTPUT_VARIABLE _qz_status
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    if(_qz_status)
      set(_qz_hash "${_qz_hash}-dirty")
    endif()
    set(QZ_GIT_HASH "${_qz_hash}")
  endif()
  foreach(_qz_ref IN ITEMS ".git/HEAD" ".git/logs/HEAD")
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/${_qz_ref}")
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                   "${CMAKE_CURRENT_LIST_DIR}/${_qz_ref}")
    endif()
  endforeach()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(quartz)
