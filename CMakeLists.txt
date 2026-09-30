cmake_minimum_required(VERSION 3.20)
project(fork_windows_hook)

include(FetchContent)
FetchContent_Declare(
    forking_machine
    GIT_REPOSITORY https://github.com/michal-maruska/fork-plugin-core/
    GIT_TAG        0.4
)
FetchContent_MakeAvailable(forking_machine)

add_executable(fork-windows-hook src/main.cpp)
target_link_libraries(fork-windows-hook PRIVATE forking_machine::forking_machine)
set_target_properties(fork-windows-hook PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# Concept:
# target_compile_features(forking_machine INTERFACE cxx_std_20)
