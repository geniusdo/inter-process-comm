cmake_minimum_required(VERSION 3.10)
project(IPC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-O2 -march=native)
endif()

option(BUILD_UNIT_TESTS "Build unit tests" ON)

find_package(Threads REQUIRED)

include(FetchContent)

FetchContent_Declare(
  asio
  GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
  GIT_TAG asio-1-28-0  
)
FetchContent_MakeAvailable(asio)

include_directories(${asio_SOURCE_DIR}/asio/include)

if(BUILD_UNIT_TESTS)
    add_subdirectory(ut)
endif()

