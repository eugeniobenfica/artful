# The bridge between Artful and cascade: artful::cascade, a target of its own, header only. Artful does
# not know of it, and cascade does not know of Artful: this folder is the one place that names both.
#
# Inside Artful's build: -DARTFUL_CASCADE_DIR=<cascade's source> builds cascade here, or -DARTFUL_CASCADE=ON
# finds an installed one. In an app's own build, with both libraries there as targets or as packages:
#
#   add_subdirectory(<artful>/integrations/cascade artful-cascade)
#   target_link_libraries(my_app PRIVATE artful::cascade)
cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(artful-cascade LANGUAGES C)
  set(artful_cascade_top ON)
else()
  set(artful_cascade_top OFF)
endif()

if(NOT TARGET artful::artful)
  find_package(artful REQUIRED)
endif()
if(NOT TARGET cascade::cascade)
  if(ARTFUL_CASCADE_DIR)
    set(CASCADE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CASCADE_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(CASCADE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(CASCADE_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory("${ARTFUL_CASCADE_DIR}" cascade EXCLUDE_FROM_ALL)
  else()
    find_package(cascade 1 REQUIRED)
  endif()
endif()

add_library(artful_cascade INTERFACE)
add_library(artful::cascade ALIAS artful_cascade)
target_include_directories(artful_cascade INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(artful_cascade INTERFACE artful::artful cascade::cascade)

# a page over a scene
if(ANDROID) # (there the program is the library cascade's activity loads)
  add_library(artful-cascade SHARED example/main.c)
else()
  add_executable(artful-cascade example/main.c)
endif()
target_link_libraries(artful-cascade PRIVATE artful::cascade)
cascade_app(artful-cascade UI example/ui)

# what a presenter goes through, in a real window (ARTFUL_TEST_WINDOW=1)
if(NOT DEFINED ARTFUL_BUILD_TESTS)
  set(ARTFUL_BUILD_TESTS ${artful_cascade_top})
endif()
if(ARTFUL_BUILD_TESTS AND NOT ANDROID)
  enable_testing()
  add_executable(artful-cascade-tests tests/window.c)
  target_link_libraries(artful-cascade-tests PRIVATE artful::cascade)
  cascade_app(artful-cascade-tests)
  add_test(NAME artful-cascade-tests COMMAND artful-cascade-tests)
endif()

# Installing, as find_package(artfulCascade): only where both libraries are installed ones too, since
# what is built here, with nothing of it installed, cannot be pointed at.
get_target_property(artful_cascade_from_source cascade::cascade ALIASED_TARGET)
get_target_property(artful_from_source artful::artful ALIASED_TARGET)
if(NOT artful_cascade_from_source AND (NOT artful_from_source OR artful_top))
  set(artful_cascade_can_install ON)
else()
  set(artful_cascade_can_install OFF)
endif()
option(ARTFUL_CASCADE_INSTALL "Install artful::cascade, for find_package(artfulCascade)" ${artful_cascade_can_install})
if(ARTFUL_CASCADE_INSTALL)
  include(GNUInstallDirs)
  set(artful_cascade_cmake_dir ${CMAKE_INSTALL_LIBDIR}/cmake/artfulCascade)
  install(TARGETS artful_cascade EXPORT artfulCascadeTargets)
  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT artfulCascadeTargets NAMESPACE artful:: DESTINATION ${artful_cascade_cmake_dir})
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/artfulCascadeConfig.cmake
    "include(CMakeFindDependencyMacro)\nfind_dependency(artful)\nfind_dependency(cascade 1)\ninclude(\"\${CMAKE_CURRENT_LIST_DIR}/artfulCascadeTargets.cmake\")\n")
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/artfulCascadeConfig.cmake DESTINATION ${artful_cascade_cmake_dir})
endif()
