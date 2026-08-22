cmake_minimum_required(VERSION 3.16)
project(CountCodeLines LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(count_lines count_lines.cpp)
