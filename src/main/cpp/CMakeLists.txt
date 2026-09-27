cmake_minimum_required(VERSION 3.22)
project(modelpack_anime4k_acnet CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(modelpack SHARED modelpack.cpp)

# 确保产物命名为 libmodelpack.so（无版本后缀）
set_target_properties(modelpack PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "modelpack"
)