cmake_minimum_required(VERSION 3.10)
project(MotionDynamics_4TO LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenMP)

include_directories(${PROJECT_SOURCE_DIR}/include)


# -------------------------------
# Windows: put exe in <root>/bin
# Linux: leave default (exe in build/...)
# -------------------------------
if(WIN32)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG   ${PROJECT_SOURCE_DIR}/bin)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${PROJECT_SOURCE_DIR}/bin)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${PROJECT_SOURCE_DIR}/bin)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${PROJECT_SOURCE_DIR}/bin)

    add_executable(MotionDynamics_4TO_updater updater/updater.cpp)

    # optional: warnings for updater
    if(MSVC)
        target_compile_options(MotionDynamics_4TO_updater PRIVATE /W4 /permissive-)
    else()
        target_compile_options(MotionDynamics_4TO_updater PRIVATE -Wall -Wextra -pedantic)
    endif()
endif()

add_executable(MotionDynamics_4TO
    src/main.cpp
    src/md.cpp
    src/ReadInput.cpp
)

if(MSVC)
    target_compile_options(MotionDynamics_4TO PRIVATE /W4 /permissive-)
else()
    target_compile_options(MotionDynamics_4TO PRIVATE -Wall -Wextra -pedantic)
endif()

if(OpenMP_CXX_FOUND)
    target_link_libraries(MotionDynamics_4TO PRIVATE OpenMP::OpenMP_CXX)
endif()
