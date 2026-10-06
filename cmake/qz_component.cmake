# Dual-mode component registration (docs/COMPONENTS.md section 6).
#
# In components/qz_<name>/CMakeLists.txt:
#
#   include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/qz_component.cmake)
#   qz_component(
#       [SRC_DIRS <dirs...>]             # globbed *.cpp/*.c, default: src
#       [INCLUDE_DIRS <dirs...>]         # public include dirs, default: include
#       [PRIV_INCLUDE_DIRS <dirs...>]    # default: src
#       [REQUIRES <components...>]       # public dependencies (qz_* names; "cjson" is mapped)
#       [PRIV_REQUIRES <components...>]  # private dependencies (IDF component names allowed
#                                        # only for IDF-only components)
#       [PLATFORM]                       # IDF-only glue: relaxed warning set (IDF macros use C casts)
#   )
#
# Under ESP-IDF this forwards to idf_component_register(); on the host it builds a static library
# named after the component directory. Quartz warning flags apply to C++ sources only, so vendored
# C (Bosch SensorAPI) is left untouched. Adding a source file needs no CMake edit.
#
# qz_component MUST be a macro: during IDF's early requirement-expansion pass idf_component_register
# is itself a macro that records the component's requirements in the *caller's* scope and then
# return()s from the component's CMakeLists.txt. Wrapping it in a function would hide both.

include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/qz_flags.cmake)

# Third-party dependency names used in qz_component() -> IDF component names.
function(_qz_map_idf_deps out)
  set(_res "")
  foreach(_d IN LISTS ARGN)
    if(_d STREQUAL "cjson")
      list(APPEND _res espressif__cjson)
    else()
      list(APPEND _res ${_d})
    endif()
  endforeach()
  set(${out} "${_res}" PARENT_SCOPE)
endfunction()

# Compile features and warning flags shared by IDF and host targets.
function(_qz_apply_flags target platform)
  get_target_property(_type ${target} TYPE)
  if(_type STREQUAL "INTERFACE_LIBRARY")
    return()
  endif()
  target_compile_features(${target} PRIVATE cxx_std_${QZ_CXX_STANDARD})
  if(platform)
    set(_warn ${QZ_WARNING_FLAGS_PLATFORM})
  else()
    set(_warn ${QZ_WARNING_FLAGS})
  endif()
  target_compile_options(${target} PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:${_warn}>"
    "$<$<COMPILE_LANGUAGE:CXX>:${QZ_CXX_ONLY_FLAGS}>")
endfunction()

# Host-side static library for one pure component.
function(_qz_host_component platform)
  cmake_parse_arguments(ARG "" "" "SRC_DIRS;INCLUDE_DIRS;PRIV_INCLUDE_DIRS;REQUIRES;PRIV_REQUIRES" ${ARGN})
  get_filename_component(_target "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
  set(_srcs "")
  foreach(_dir IN LISTS ARG_SRC_DIRS)
    file(GLOB _found CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/${_dir}/*.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/${_dir}/*.c")
    list(APPEND _srcs ${_found})
  endforeach()
  if(_srcs)
    add_library(${_target} STATIC ${_srcs})
    set(_vis PUBLIC)
  else()
    add_library(${_target} INTERFACE)
    set(_vis INTERFACE)
  endif()
  foreach(_d IN LISTS ARG_INCLUDE_DIRS)
    target_include_directories(${_target} ${_vis} "${CMAKE_CURRENT_SOURCE_DIR}/${_d}")
  endforeach()
  if(_srcs)
    foreach(_d IN LISTS ARG_PRIV_INCLUDE_DIRS)
      target_include_directories(${_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/${_d}")
    endforeach()
    target_link_libraries(${_target} PUBLIC ${ARG_REQUIRES} PRIVATE ${ARG_PRIV_REQUIRES})
    set_target_properties(${_target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  else()
    target_link_libraries(${_target} INTERFACE ${ARG_REQUIRES})
  endif()
  _qz_apply_flags(${_target} "${platform}")
endfunction()

macro(qz_component)
  cmake_parse_arguments(_QZ "PLATFORM" ""
    "SRC_DIRS;INCLUDE_DIRS;PRIV_INCLUDE_DIRS;REQUIRES;PRIV_REQUIRES" ${ARGN})
  if(NOT _QZ_SRC_DIRS)
    set(_QZ_SRC_DIRS src)
  endif()
  if(NOT _QZ_INCLUDE_DIRS)
    set(_QZ_INCLUDE_DIRS include)
  endif()
  if(NOT _QZ_PRIV_INCLUDE_DIRS)
    set(_QZ_PRIV_INCLUDE_DIRS src)
  endif()

  if(COMMAND idf_component_register)
    _qz_map_idf_deps(_qz_req ${_QZ_REQUIRES})
    _qz_map_idf_deps(_qz_preq ${_QZ_PRIV_REQUIRES})
    idf_component_register(
      SRC_DIRS ${_QZ_SRC_DIRS}
      INCLUDE_DIRS ${_QZ_INCLUDE_DIRS}
      PRIV_INCLUDE_DIRS ${_QZ_PRIV_INCLUDE_DIRS}
      REQUIRES ${_qz_req}
      PRIV_REQUIRES ${_qz_preq})
    # Not reached during IDF's early-expansion pass (the macro above return()s there).
    _qz_apply_flags(${COMPONENT_LIB} "${_QZ_PLATFORM}")
  else()
    _qz_host_component("${_QZ_PLATFORM}"
      SRC_DIRS ${_QZ_SRC_DIRS}
      INCLUDE_DIRS ${_QZ_INCLUDE_DIRS}
      PRIV_INCLUDE_DIRS ${_QZ_PRIV_INCLUDE_DIRS}
      REQUIRES ${_QZ_REQUIRES}
      PRIV_REQUIRES ${_QZ_PRIV_REQUIRES})
  endif()
endmacro()
