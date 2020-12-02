cmake_minimum_required(VERSION 3.19)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

project(Dots )

file(GLOB_RECURSE 
    DotsSource
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

add_library(Dots SHARED ${DotsSource})

target_compile_definitions(Dots
    PRIVATE MAKEDLL 
)
