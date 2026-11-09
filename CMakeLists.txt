cmake_minimum_required(VERSION 3.16)
project(artful VERSION 0.4.0 DESCRIPTION "A 3D renderer in plain C11, with no dependencies" LANGUAGES C)

# Plain C11: no third-party libraries, and no SDK: Vulkan is loaded at run time and its shaders are
# built by the library itself.
#
#   src/core/       memory, threads
#   src/scene/      what is drawn: meshes (packed, ordered, cut into meshlets), textures, instances
#   src/render/     the choice of a backend
#   src/software/   the CPU backend: kernels.inc is built once for each instruction set
#   src/vulkan/     the GPU backend (ARTFUL_VULKAN)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(artful_top ON)
else()
  set(artful_top OFF)
endif()
if(EMSCRIPTEN)
  set(artful_vulkan OFF) # (a browser has no Vulkan: the CPU backend is what runs there)
else()
  set(artful_vulkan ON)
endif()
option(ARTFUL_VULKAN "Draw through Vulkan where it is found at run time" ${artful_vulkan})
option(ARTFUL_BUILD_TOOLS "Build artful-render" ${artful_top})
option(ARTFUL_BUILD_TESTS "Build artful-tests" ${artful_top})
# The bridge to cascade (integrations/cascade), a target of its own: from cascade's source folder, or
# from an installed cascade. Neither: the library is all there is, and it names no other library.
set(ARTFUL_CASCADE_DIR "" CACHE PATH "cascade's source folder, to build the bridge to it, its example and its tests")
option(ARTFUL_CASCADE "Build the bridge to an installed cascade (find_package)" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(artful STATIC
  src/core/base.c src/core/threads.c
  src/scene/scene.c src/scene/blocks.c src/scene/meshBuild.c src/scene/simplify.c src/scene/hole.c src/scene/fluid.c src/scene/trace.c
  src/scene/gltf.c src/scene/imageDecode.c src/scene/graph.c src/scene/texture.c src/scene/environment.c src/scene/extras.c src/scene/meshopt.c
  src/render/renderer.c
  src/software/software.c src/software/post.c src/software/shadows.c src/software/denoise.c src/software/kernelsPortable.c)
add_library(artful::artful ALIAS artful)
target_include_directories(artful PRIVATE src)
if(NOT artful_top)
  target_include_directories(artful PUBLIC include)
endif()

if(MSVC)
  # (C11 atomics, in MSVC, are still behind a flag: Visual Studio 2022 17.5 or later)
  target_compile_options(artful PRIVATE /W3 /utf-8 /Gy /fp:precise /std:c11 /experimental:c11atomics)
  target_compile_definitions(artful PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  # -ffp-contract=off: no a*b+c fused into one rounding, so that the kernels draw the same pixels
  target_compile_options(artful PRIVATE -Wall -Wno-parentheses -Wno-unused-function -ffp-contract=off -ffunction-sections -fdata-sections)
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(artful PRIVATE -O3 -fno-math-errno -fno-trapping-math)
  endif()
endif()

# Kernels for what each processor may have; which of them runs is chosen at run time, so only their
# own files may use it.
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" artful_cpu)
if(artful_cpu MATCHES "amd64|x86_64|x64")
  target_sources(artful PRIVATE src/software/kernelsAvx2.c src/software/kernelsSse41.c)
  target_compile_definitions(artful PRIVATE ARTFUL_HAVE_AVX2 ARTFUL_HAVE_SSE41)
  if(MSVC)
    set_source_files_properties(src/software/kernelsAvx2.c PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
  else()
    set(artful_avx2_options -mavx2 -mno-fma)
    if(MINGW AND CMAKE_C_COMPILER_ID STREQUAL "GNU") # (GCC does not align the stack to 32 bytes on 64-bit Windows, PR 54412: no aligned moves there)
      list(APPEND artful_avx2_options -Wa,-muse-unaligned-vector-move)
    endif()
    set_source_files_properties(src/software/kernelsAvx2.c PROPERTIES COMPILE_OPTIONS "${artful_avx2_options}")
    set_source_files_properties(src/software/kernelsSse41.c PROPERTIES COMPILE_OPTIONS "-msse4.1")
  endif()
elseif(artful_cpu MATCHES "aarch64|arm64" AND NOT MSVC)
  target_sources(artful PRIVATE src/software/kernelsNeon.c)
  target_compile_definitions(artful PRIVATE ARTFUL_HAVE_NEON)
endif()

if(ARTFUL_VULKAN)
  target_sources(artful PRIVATE src/vulkan/vulkan.c src/vulkan/spirv.c src/vulkan/shaders.c)
  target_compile_definitions(artful PRIVATE ARTFUL_VULKAN)
endif()
# (never run: see src/metal/metal.c)
if(APPLE)
  target_sources(artful PRIVATE src/metal/metal.c)
  target_compile_definitions(artful PRIVATE ARTFUL_METAL)
endif()

find_package(Threads)
if(Threads_FOUND AND NOT WIN32)
  target_link_libraries(artful PUBLIC Threads::Threads)
endif()
if(UNIX)
  target_link_libraries(artful PUBLIC m ${CMAKE_DL_LIBS})
endif()

if(ARTFUL_BUILD_TOOLS)
  add_executable(artful-render tools/render.c)
  target_link_libraries(artful-render PRIVATE artful)
  target_include_directories(artful-render PRIVATE src)
  if(MINGW)
    target_link_options(artful-render PRIVATE -static)
  endif()
endif()

if(ARTFUL_BUILD_TOOLS AND WIN32)
  # a black hole in a window, to fly around
  add_executable(artful-blackhole WIN32 tools/blackhole.c)
  target_link_libraries(artful-blackhole PRIVATE artful gdi32)
  if(MINGW)
    target_link_options(artful-blackhole PRIVATE -static)
  endif()
endif()

if(ARTFUL_BUILD_TOOLS AND ARTFUL_VULKAN)
  # the shaders as files, for spirv-val: CI checks them, as nothing here can
  add_executable(artful-shaders tools/shaders.c src/vulkan/shaders.c src/vulkan/spirv.c)
  target_link_libraries(artful-shaders PRIVATE artful)
  target_include_directories(artful-shaders PRIVATE src)
endif()

if(ARTFUL_BUILD_TESTS)
  enable_testing()
  add_executable(artful-tests tests/tests.c)
  target_link_libraries(artful-tests PRIVATE artful)
  target_include_directories(artful-tests PRIVATE src)
  if(MINGW)
    target_link_options(artful-tests PRIVATE -static)
  endif()
  add_test(NAME artful-tests COMMAND artful-tests)
endif()

# cmake --install build --prefix <dir>, then find_package(artful) with <dir> in CMAKE_PREFIX_PATH
if(artful_top)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)
  set(artful_cmake_dir ${CMAKE_INSTALL_LIBDIR}/cmake/artful)
  target_include_directories(artful PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  install(TARGETS artful EXPORT artfulTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT artfulTargets NAMESPACE artful:: DESTINATION ${artful_cmake_dir})
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/artfulConfig.cmake "include(CMakeFindDependencyMacro)\nif(NOT WIN32)\n  find_dependency(Threads)\nendif()\ninclude(\"\${CMAKE_CURRENT_LIST_DIR}/artfulTargets.cmake\")\n")
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/artfulConfigVersion.cmake COMPATIBILITY SameMinorVersion)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/artfulConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/artfulConfigVersion.cmake DESTINATION ${artful_cmake_dir})
endif()

if(ARTFUL_CASCADE OR ARTFUL_CASCADE_DIR)
  add_subdirectory(integrations/cascade)
endif()
