cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
      CACHE PATH "VitaSDK toolchain file")
  else()
    message(FATAL_ERROR "VITASDK must point to an installed VitaSDK tree")
  endif()
endif()

project(milestone10_vitasdk_homebrew C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

add_executable(milestone10-vitasdk-homebrew main.c)
target_compile_options(milestone10-vitasdk-homebrew PRIVATE
  -Os
  -Wall
  -Wextra
  -fno-builtin
  -fno-inline
)
target_link_libraries(milestone10-vitasdk-homebrew
  SceLibKernel_stub
)
set_target_properties(milestone10-vitasdk-homebrew PROPERTIES
  LINK_FLAGS "-nostdlib"
)

vita_create_self(milestone10-vitasdk-homebrew.self
  milestone10-vitasdk-homebrew
  CONFIG exports.yml
  UNSAFE
  UNCOMPRESSED
)
