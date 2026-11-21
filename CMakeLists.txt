cmake_minimum_required(VERSION 3.16)
project(artful VERSION 0.4.0 DESCRIPTION "A 3D renderer in plain C11, with no dependencies" LANGUAGES C)

# Plain C11: no third-party libraries, and no SDK: Vulkan is loaded at run time and its shaders are
# built by the library itself.
#
#   src/core/       memory, threads: artful-core, which the others take
#   src/2d/         drawing in 2D, lit, on the CPU: artful-2d, on the core alone
#   src/scene/      what is drawn: meshes (packed, ordered, cut into meshlets), textures, instances
#   src/render/     the choice of a backend
#   src/software/   the CPU backend: kernels.inc is built once for each instruction set
#   src/vulkan/     the GPU backend (ARTFUL_VULKAN)
#   src/metal/      Metal (ARTFUL_METAL, off: never run, and behind; see docs/limits.md)
#   extensions/     what is built apart, on the library's public API: extensions/blackhole (ARTFUL_BLACKHOLE)

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
# The black hole, a library of its own (artful-blackhole) on the background pass; the tools and tests need it.
option(ARTFUL_BLACKHOLE "Build artful-blackhole, the black hole behind a scene" ${artful_top})
# Metal was written without a Mac and has never run; its shaders are made ahead of time and are behind Vulkan's.
# Off: an Apple build draws through Vulkan (MoltenVK) or the CPU.
option(ARTFUL_METAL "Build the Metal backend (Apple only; never run, and without what came after 0.4)" OFF)
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

# libFuzzer's targets, one for each reader (tools/fuzz/fuzz.c): clang only. All is built with coverage, under
# AddressSanitizer and UndefinedBehaviorSanitizer, so that what a reader does wrong is found where it is done.
option(ARTFUL_FUZZ "Build artful-fuzz-*, libFuzzer's targets for the readers (clang)" OFF)
set(ARTFUL_FUZZ_TARGETS gltf image hdr ktx2 blocks ies cube meshopt cook)
if(ARTFUL_FUZZ)
  if(MINGW)
    # (clang will not say -fsanitize=fuzzer for MinGW, though it has libFuzzer: its coverage, and libFuzzer by name)
    set(ARTFUL_FUZZ_COVERAGE -fsanitize-coverage=inline-8bit-counters,indirect-calls,trace-cmp,pc-table)
    execute_process(COMMAND ${CMAKE_C_COMPILER} -print-resource-dir OUTPUT_VARIABLE artful_clang_resources OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(ARTFUL_FUZZ_LINK ${artful_clang_resources}/lib/windows/libclang_rt.fuzzer-x86_64.a c++)
  else()
    set(ARTFUL_FUZZ_COVERAGE -fsanitize=fuzzer-no-link)
    set(ARTFUL_FUZZ_LINK -fsanitize=fuzzer)
  endif()
  add_compile_options(${ARTFUL_FUZZ_COVERAGE} -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-sanitize=float-cast-overflow -g)
  add_link_options(-fsanitize=address,undefined)
endif()

# The core (memory, threads, the log, the clock): what the 3D library and the 2D one share, and all the 2D one takes.
add_library(artful-core STATIC src/core/base.c src/core/threads.c)
add_library(artful::core ALIAS artful-core)
set_target_properties(artful-core PROPERTIES EXPORT_NAME core)
target_include_directories(artful-core PRIVATE src)

add_library(artful STATIC
  src/scene/scene.c src/scene/blocks.c src/scene/meshBuild.c src/scene/simplify.c src/scene/dag.c src/scene/trace.c src/scene/background.c
  src/scene/gltf.c src/scene/imageDecode.c src/scene/graph.c src/scene/texture.c src/scene/zstd.c src/scene/basis.c src/scene/environment.c src/scene/sky.c src/scene/motion.c src/scene/extras.c src/scene/meshopt.c src/scene/scale.c src/scene/particles.c src/scene/cook.c
  src/scene/lights.c
  src/render/renderer.c src/render/framePasses.c src/scene/views.c
  src/software/software.c src/software/post.c src/software/shadows.c src/software/denoise.c src/software/kernelsPortable.c)
add_library(artful::artful ALIAS artful)
target_include_directories(artful PRIVATE src)
target_link_libraries(artful PUBLIC artful-core)

# artful-2d: pictures and shapes, lit, with soft shadows and gi, on the CPU. It takes only the core: a program that
# draws only in 2D links nothing of the 3D library.
add_library(artful-2d STATIC src/2d/scene2d.c src/2d/draw2d.c src/2d/shadow2d.c src/2d/gi2d.c src/2d/post2d.c src/2d/renderer2d.c)
add_library(artful::2d ALIAS artful-2d)
set_target_properties(artful-2d PROPERTIES EXPORT_NAME 2d)
target_include_directories(artful-2d PRIVATE src)
target_link_libraries(artful-2d PUBLIC artful-core)

foreach(artful_target artful-core artful artful-2d)
  if(NOT artful_top)
    target_include_directories(${artful_target} PUBLIC include)
  endif()
  if(MSVC)
    # (C11 atomics, in MSVC, are still behind a flag: Visual Studio 2022 17.5 or later)
    target_compile_options(${artful_target} PRIVATE /W3 /utf-8 /Gy /fp:precise /std:c11 /experimental:c11atomics)
    target_compile_definitions(${artful_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    # -ffp-contract=off: no a*b+c fused into one rounding, so that the kernels draw the same pixels
    target_compile_options(${artful_target} PRIVATE -Wall -Wno-parentheses -Wno-unused-function -ffp-contract=off -ffunction-sections -fdata-sections)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
      target_compile_options(${artful_target} PRIVATE -O3 -fno-math-errno -fno-trapping-math)
    endif()
  endif()
endforeach()

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
  # (the SPIR-V writer is the core's: the 3D library and the 2D one both build their shaders with it)
  target_sources(artful-core PRIVATE src/vulkan/spirv.c src/shaderir/ir.c src/shaderir/msl.c)
  target_sources(artful PRIVATE src/vulkan/vulkan.c src/vulkan/vulkanTrace.c src/vulkan/vulkanPost.c src/vulkan/shaders.c src/vulkan/frameGraph.c)
  target_compile_definitions(artful PRIVATE ARTFUL_VULKAN)
  target_sources(artful-2d PRIVATE src/2d/vulkan2d.c src/2d/shaders2d.c)
  target_compile_definitions(artful-2d PRIVATE ARTFUL_VULKAN)
endif()
# (never run: see src/metal/metal.c and docs/limits.md)
if(APPLE AND ARTFUL_METAL)
  target_sources(artful PRIVATE src/metal/metal.c)
  target_compile_definitions(artful PRIVATE ARTFUL_METAL)
endif()

find_package(Threads)
if(Threads_FOUND AND NOT WIN32)
  target_link_libraries(artful-core PUBLIC Threads::Threads)
endif()
if(UNIX)
  target_link_libraries(artful-core PUBLIC m ${CMAKE_DL_LIBS})
endif()

if(ARTFUL_BLACKHOLE OR ARTFUL_BUILD_TOOLS OR ARTFUL_BUILD_TESTS)
  add_subdirectory(extensions/blackhole)
endif()

if(ARTFUL_BUILD_TOOLS)
  add_executable(artful-render tools/render.c)
  if(ARTFUL_VULKAN)
    target_compile_definitions(artful-render PRIVATE ARTFUL_VULKAN) # (its --graph reads Vulkan's frame graph)
  endif()
  target_link_libraries(artful-render PRIVATE artful artful-blackhole)
  target_include_directories(artful-render PRIVATE src)
  if(MINGW)
    target_link_options(artful-render PRIVATE -static)
  endif()
endif()

if(ARTFUL_BUILD_TOOLS)
  # how far apart two pictures look, by FLIP: a gate for pictures kept to compare against
  add_executable(artful-flip tools/flipImages.c tools/flip.c)
  target_link_libraries(artful-flip PRIVATE artful)
  if(UNIX)
    target_link_libraries(artful-flip PRIVATE m)
  endif()
  if(MINGW)
    target_link_options(artful-flip PRIVATE -static)
  endif()
endif()

if(ARTFUL_BUILD_TOOLS)
  # what is slow in loading a glTF, made once into a file (docs/plano-de-correcao.md, 4.3)
  add_executable(artful-cook tools/cook.c)
  target_link_libraries(artful-cook PRIVATE artful)
  target_include_directories(artful-cook PRIVATE src)
  if(MINGW)
    target_link_options(artful-cook PRIVATE -static)
  endif()
endif()

if(ARTFUL_BUILD_TOOLS)
  # hours of assets coming and going under a budget: does anything grow? (docs/plano-de-correcao.md, 3.8)
  add_executable(artful-soak tools/soak.c)
  target_link_libraries(artful-soak PRIVATE artful)
  if(MINGW)
    target_link_options(artful-soak PRIVATE -static)
  endif()
endif()

if(ARTFUL_BUILD_TOOLS)
  # a lit 2D room to a PNG, and what its frames take: it links artful-2d alone
  add_executable(artful-render2d tools/render2d.c)
  target_link_libraries(artful-render2d PRIVATE artful-2d)
  if(MINGW)
    target_link_options(artful-render2d PRIVATE -static)
  endif()
endif()

if(ARTFUL_BUILD_TOOLS AND WIN32)
  # a black hole in a window, to fly around (artful-blackhole.exe: the target of that name is the extension)
  add_executable(artful-blackhole-window WIN32 tools/blackhole.c)
  set_target_properties(artful-blackhole-window PROPERTIES OUTPUT_NAME artful-blackhole)
  target_link_libraries(artful-blackhole-window PRIVATE artful artful-blackhole gdi32)
  if(MINGW)
    target_link_options(artful-blackhole-window PRIVATE -static)
  endif()
endif()

if(ARTFUL_BUILD_TOOLS AND ARTFUL_VULKAN)
  # the shaders as files, for spirv-val: CI checks them, as nothing here can
  add_executable(artful-shaders tools/shaders.c src/vulkan/shaders.c)
  target_link_libraries(artful-shaders PRIVATE artful artful-blackhole artful-2d)
  target_include_directories(artful-shaders PRIVATE src extensions/blackhole)
endif()

if(ARTFUL_FUZZ)
  foreach(name ${ARTFUL_FUZZ_TARGETS})
    add_executable(artful-fuzz-${name} tools/fuzz/fuzz.c)
    target_compile_definitions(artful-fuzz-${name} PRIVATE ARTF_FUZZ=${name})
    target_link_libraries(artful-fuzz-${name} PRIVATE artful)
    target_include_directories(artful-fuzz-${name} PRIVATE src)
    target_link_libraries(artful-fuzz-${name} PRIVATE ${ARTFUL_FUZZ_LINK})
  endforeach()
endif()

if(ARTFUL_BUILD_TESTS)
  enable_testing()
  # one program, a file for each area: artful-tests [word...] runs the tests whose names hold a word; --list names them
  add_executable(artful-tests tests/main.c tests/common.c
    tests/core.c tests/backends.c tests/layers.c tests/materials.c tests/files.c tests/streaming.c tests/shadows.c
    tests/post.c tests/tracing.c tests/sky.c tests/motion.c tests/scale.c tests/views.c tests/blackhole.c tests/twod.c
    tests/metrics.c tools/flip.c)
  target_link_libraries(artful-tests PRIVATE artful artful-blackhole artful-2d)
  target_include_directories(artful-tests PRIVATE src extensions/blackhole tools)
  if(ARTFUL_VULKAN)
    target_compile_definitions(artful-tests PRIVATE ARTFUL_VULKAN) # (without it, what sets Vulkan's own switches is left out)
  endif()
  if(NOT MSVC)
    # (the tests make meshes and pictures with the library's own inline functions, artfSinFast and the like: fused,
    # as clang does on ARM unless told not to, a vertex lands elsewhere, and what the simplifier makes of it too)
    target_compile_options(artful-tests PRIVATE -ffp-contract=off)
  endif()
  if(MINGW)
    target_link_options(artful-tests PRIVATE -static)
  endif()
  if(CMAKE_CROSSCOMPILING)
    add_test(NAME artful-tests COMMAND artful-tests)
  else()
    # one CTest test for each of artful-tests' own, asked of it when CTest runs (artful-tests --list): each runs
    # alone (--exact), and one that could not reach what it needs (Vulkan, a window, ray tracing hardware) is
    # skipped, not passed (exit 77); ARTFUL_REQUIRE=vulkan,rt,window makes those failures
    file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/artfulTestsExe-$<CONFIG>.txt CONTENT "$<TARGET_FILE:artful-tests>")
    set(artful_tests_dir ${CMAKE_CURRENT_BINARY_DIR})
    configure_file(cmake/artfulTests.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/artfulTests.cmake @ONLY)
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES ${CMAKE_CURRENT_BINARY_DIR}/artfulTests.cmake)
  endif()
endif()

# cmake --install build --prefix <dir>, then find_package(artful) with <dir> in CMAKE_PREFIX_PATH
if(artful_top)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)
  set(artful_cmake_dir ${CMAKE_INSTALL_LIBDIR}/cmake/artful)
  foreach(artful_target artful-core artful artful-2d)
    target_include_directories(${artful_target} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  endforeach()
  install(TARGETS artful-core artful artful-2d EXPORT artfulTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  if(TARGET artful-blackhole)
    # (artful::blackhole, beside artful::artful)
    install(TARGETS artful-blackhole EXPORT artfulTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY extensions/blackhole/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  endif()
  install(EXPORT artfulTargets NAMESPACE artful:: DESTINATION ${artful_cmake_dir})
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/artfulConfig.cmake "include(CMakeFindDependencyMacro)\nif(NOT WIN32)\n  find_dependency(Threads)\nendif()\ninclude(\"\${CMAKE_CURRENT_LIST_DIR}/artfulTargets.cmake\")\n")
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/artfulConfigVersion.cmake COMPATIBILITY SameMinorVersion)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/artfulConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/artfulConfigVersion.cmake DESTINATION ${artful_cmake_dir})
endif()

if(ARTFUL_CASCADE OR ARTFUL_CASCADE_DIR)
  add_subdirectory(integrations/cascade)
endif()
