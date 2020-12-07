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
        PRIVATE MAKEDLL 
    )
endif(${DOTS_BUILD_AS_STATIC})

if(DOTS_BUILD_CODEBASE)
    ## Glob codebase files.
    file(GLOB_RECURSE 
        DotsCodebaseSource
        ${CMAKE_CURRENT_SOURCE_DIR}/Codebase/*.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/Codebase/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Codebase/*.c
        ${CMAKE_CURRENT_SOURCE_DIR}/Codebase/*.cxx
        ${CMAKE_CURRENT_SOURCE_DIR}/Codebase/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Codebase/*.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Codebase/*.hxx
        ${CMAKE_CURRENT_SOURCE_DIR}/Codebase/*.inl
        ${CMAKE_CURRENT_SOURCE_DIR}/Codebase/*.natvis
        ${CMAKE_CURRENT_SOURCE_DIR}/Database/*.natvis
    )

    if(${DOTS_BUILD_AS_STATIC})
        add_library(DotsCodebase STATIC ${DotsCodebaseSource})
    else()
        add_library(DotsCodebase SHARED ${DotsCodebaseSource})
        target_compile_definitions(DotsCodebase
            PRIVATE MAKEDLL 
        )
    endif(${DOTS_BUILD_AS_STATIC})

    target_include_directories(DotsCodebase
        PUBLIC 
        ${CMAKE_CURRENT_SOURCE_DIR}/Codebase/include
        ${CMAKE_CURRENT_SOURCE_DIR}/Database
    )
    target_link_libraries(DotsCodebase PUBLIC DotsDatabase)
endif(DOTS_BUILD_CODEBASE)