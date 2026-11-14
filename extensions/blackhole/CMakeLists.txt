# artful-blackhole: a black hole behind the scene, on the library's background pass (artfSceneSetBackgroundPass).
# Apart from the library: a program that draws none links none of it. It is built with the library's flags, and
# its light on the CPU once for each instruction set, as the library's kernels are, so that every processor and
# every kernel draw the same bytes. It takes three of the library's inner headers (src/core/threads.h,
# src/software/simd.h, src/vulkan/spirv.h) and calls what they declare: it goes with the library it was built with.

add_library(artful-blackhole STATIC hole.c fluid.c holeShader.c lensPortable.c)
add_library(artful::blackhole ALIAS artful-blackhole)
set_target_properties(artful-blackhole PROPERTIES EXPORT_NAME blackhole)
target_link_libraries(artful-blackhole PUBLIC artful)
target_include_directories(artful-blackhole
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
  PRIVATE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
  target_compile_options(artful-blackhole PRIVATE /W3 /utf-8 /Gy /fp:precise /std:c11)
  target_compile_definitions(artful-blackhole PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  # (as the library: no a*b+c fused into one rounding, or the kernels would not draw the same pixels)
  target_compile_options(artful-blackhole PRIVATE -Wall -Wno-parentheses -Wno-unused-function -ffp-contract=off -ffunction-sections -fdata-sections)
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(artful-blackhole PRIVATE -O3 -fno-math-errno -fno-trapping-math)
  endif()
endif()

if(artful_cpu MATCHES "amd64|x86_64|x64")
  target_sources(artful-blackhole PRIVATE lensAvx2.c lensSse41.c)
  target_compile_definitions(artful-blackhole PRIVATE ARTFUL_HAVE_AVX2 ARTFUL_HAVE_SSE41)
  if(MSVC)
    set_source_files_properties(lensAvx2.c PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
  else()
    set_source_files_properties(lensAvx2.c PROPERTIES COMPILE_OPTIONS "${artful_avx2_options}")
    set_source_files_properties(lensSse41.c PROPERTIES COMPILE_OPTIONS "-msse4.1")
  endif()
elseif(artful_cpu MATCHES "aarch64|arm64" AND NOT MSVC)
  target_sources(artful-blackhole PRIVATE lensNeon.c)
  target_compile_definitions(artful-blackhole PRIVATE ARTFUL_HAVE_NEON)
endif()

if(ARTFUL_VULKAN)
  target_compile_definitions(artful-blackhole PRIVATE ARTFUL_VULKAN)
endif()
