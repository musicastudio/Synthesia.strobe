cmake_minimum_required(VERSION 3.20)
project(SynthesiaStrobe CXX)

# Synthesia.exe is a 64-bit binary despite installing under Program Files (x86), so a wrong-arch
# build would simply fail to load. Refuse it here instead.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "synthesia.strobe must be built for x64")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/W4 /permissive- /EHsc)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

add_subdirectory(third_party/minhook EXCLUDE_FROM_ALL)

# The plugin itself: a version.dll proxy that Synthesia loads from its own folder.
add_library(version SHARED src/synthesia_strobe.cpp src/version.def)
target_include_directories(version PRIVATE src third_party/minhook/include)
target_link_libraries(version PRIVATE minhook Winmm)
set_target_properties(version PROPERTIES OUTPUT_NAME "version" PREFIX "" SUFFIX ".dll")

# Strobe logic self-check, runnable without Synthesia or a keyboard.
add_executable(strobe_test tools/strobe_test.cpp)
target_include_directories(strobe_test PRIVATE src)
