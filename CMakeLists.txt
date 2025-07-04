cmake_minimum_required(VERSION 3.20)

project(star)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(star SHARED
  src/main.cpp
        src/Offsets.hpp
)
target_include_directories(star PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/includes/luau
)
set_target_properties(star PROPERTIES
  OUTPUT_NAME "star"
  SUFFIX ".dylib"
)

target_compile_definitions(star PRIVATE
  STAR_EXPORTS
)

set_target_properties(star PROPERTIES
  INSTALL_RPATH "@loader_path"
)
