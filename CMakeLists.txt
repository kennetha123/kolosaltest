cmake_minimum_required(VERSION 3.15)
project(MyIPCApp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Use vcpkg toolchain if available
if(DEFINED ENV{VCPKG_ROOT})
    message(STATUS "Using vcpkg toolchain from $ENV{VCPKG_ROOT}")
endif()

# Find oatpp packages (disabled for now)
# find_package(oatpp REQUIRED)

# Executables - lock-free versions
add_executable(server src/server.cpp)
add_executable(worker src/worker.cpp)

# Link libraries
target_link_libraries(server PRIVATE pthread rt)
target_link_libraries(worker PRIVATE pthread rt)

install(TARGETS server worker DESTINATION bin)
