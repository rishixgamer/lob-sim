cmake_minimum_required(VERSION 3.24)
project(lob_sim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development.Module)
find_package(nanobind CONFIG REQUIRED)
add_subdirectory(../cpp/src/continuous continuous)

nanobind_add_module(_native bindings.cpp)
target_link_libraries(_native PRIVATE faircross_continuous)
install(TARGETS _native LIBRARY DESTINATION lob_sim RUNTIME DESTINATION lob_sim)
