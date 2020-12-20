cmake_minimum_required(VERSION 3.19)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

project(Dots )

## Glob database files.
file(GLOB_RECURSE 
    DotsDatabaseSource
    ${CMAKE_CURRENT_SOURCE_DIR}/Database/*.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/Database/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Database/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Database/*.cxx
    ${CMAKE_CURRENT_SOURCE_DIR}/Database/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Database/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Database/*.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/Database/*.inl

    ${CMAKE_CURRENT_SOURCE_DIR}/Database/*.natvis
)

if(${DOTS_BUILD_AS_STATIC})
    message(STATUS "DOTS-Database: Static.")
    add_library(DotsDatabase STATIC ${DotsDatabaseSource})
else()
    add_library(DotsDatabase SHARED ${DotsDatabaseSource})
    target_compile_definitions(DotsDatabase
        PRIVATE DATABASE_DLL 
    )
endif(${DOTS_BUILD_AS_STATIC})

target_include_directories(DotsDatabase
    PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Database/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Database>
)