cmake_minimum_required(VERSION 3.25)
project(lle LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  set(LLE_ARCH_FLAG -mcpu=native)
else()
  set(LLE_ARCH_FLAG -march=native)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -flto ${LLE_ARCH_FLAG}")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-flto")
set(CMAKE_CXX_FLAGS_DEBUG "-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-fsanitize=address,undefined")

find_package(Threads REQUIRED)

add_library(lle INTERFACE)
target_include_directories(lle INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lle INTERFACE Threads::Threads)

set(LLE_WARNINGS -Wall -Wextra)
set(LLE_HOT_FLAGS -fno-exceptions -fno-rtti)

add_subdirectory(tools)

include(CTest)
if(BUILD_TESTING)
  include(FetchContent)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(googletest)

  function(lle_test name)
    add_executable(${name} tests/${name}.cpp ${ARGN})
    target_link_libraries(${name} PRIVATE lle GTest::gtest_main)
    target_compile_options(${name} PRIVATE ${LLE_WARNINGS})
    add_test(NAME ${name} COMMAND ${name})
  endfunction()

  lle_test(test_codecs)
  lle_test(test_soupbin)
  lle_test(test_ring)
  lle_test(test_flat_map)
  lle_test(test_histogram)
  lle_test(test_book)
  lle_test(test_engine)
endif()
