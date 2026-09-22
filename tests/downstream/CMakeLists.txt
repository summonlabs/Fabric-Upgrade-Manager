# Independent downstream consumer: proves the installed and exported
# FabricUpgradeManager package can be found and used from another project.
cmake_minimum_required(VERSION 3.25)
project(FabricUpgradeManagerDownstream LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(FabricUpgradeManager 1.0 REQUIRED)
find_package(Threads REQUIRED)

add_executable(downstream main.cpp)
target_link_libraries(downstream PRIVATE fum::core fum::synthetic)

enable_testing()
add_test(NAME downstream_runs COMMAND downstream)
