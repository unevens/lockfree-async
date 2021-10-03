cmake_minimum_required(VERSION 3.0.0)

project(LockFreeTest)

set(CMAKE_CXX_STANDARD 17)

include_directories(../)

add_executable(LockFreeTest LockFreeTest.cpp)

if(UNIX)
find_package (Threads)
target_link_libraries (LockFreeTest ${CMAKE_THREAD_LIBS_INIT})
endif(UNIX)
