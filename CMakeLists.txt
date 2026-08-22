# project(AtlasDB LANGUAGES CXX)

# set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# add_executable(AtlasDB src/log.cpp src/main.cpp)

# target_include_directories(AtlasDB PRIVATE ${CMAKE_SOURCE_DIR}/include)

# target_compile_options(
#     AtlasDB PRIVATE
#     "-fmacro-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/="
# )

# target_compile_definitions(
#     AtlasDB
#     PRIVATE
#     ATLASDB_PROJECT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"
# )

cmake_minimum_required(VERSION 3.16)

project(AtlasDB LANGUAGES CXX)

# 生成 compile_commands.json，方便 clangd 等工具读取编译参数
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 递归扫描 src 目录下的所有 C++ 源文件
file(
    GLOB_RECURSE ATLASDB_SOURCE_FILES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)

# 递归扫描 include 目录下的所有头文件
#
# 头文件实际上不需要传给 add_executable 才能参与编译，
# 这里加入主要是为了让 IDE 能在项目文件列表中显示这些头文件。
file(
    GLOB_RECURSE ATLASDB_HEADER_FILES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hh"
)

# 使用扫描得到的源文件和头文件创建可执行程序
add_executable(
    AtlasDB
    ${ATLASDB_SOURCE_FILES}
    ${ATLASDB_HEADER_FILES}
)

# 指定头文件搜索目录
#
# 源码中可以这样包含：
#   #include "log.h"
#   #include "storage/memtable.h"
target_include_directories(
    AtlasDB
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# 将源码路径从 __FILE__ 等宏中移除，避免生成绝对路径
target_compile_options(
    AtlasDB
    PRIVATE
    "-fmacro-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/="
)

# 定义 ATLASDB_PROJECT_DIR 宏
#
# C++ 中可以直接使用：
#   std::cout << ATLASDB_PROJECT_DIR;
target_compile_definitions(
    AtlasDB
    PRIVATE
    ATLASDB_PROJECT_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)