# The farbfeld example plugin: plain C against include/ifritview_plugin.h.
# Built into <build>/plugins beside the executable, where a build tree's
# ifritview finds it; never installed.
enable_language(C)
add_library(ifrit_farbfeld MODULE farbfeld.c)
get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(multi_config)
  set(dir "${CMAKE_BINARY_DIR}/$<CONFIG>/plugins")  # beside <build>/<config>/ifritview.exe
else()
  set(dir "${CMAKE_BINARY_DIR}/plugins")
endif()
set_target_properties(ifrit_farbfeld PROPERTIES
  OUTPUT_NAME farbfeld
  PREFIX ""
  C_STANDARD 99
  C_VISIBILITY_PRESET hidden
  LIBRARY_OUTPUT_DIRECTORY "${dir}")
target_include_directories(ifrit_farbfeld PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(ifrit_farbfeld PRIVATE ${IFRIT_WARNINGS})
