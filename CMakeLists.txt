cmake_minimum_required(VERSION 3.19)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

project(Dots)

## Glob dots runtime files.
file(GLOB_RECURSE 
    DotsRuntimeSource
    ${CMAKE_CURRENT_SOURCE_DIR}/DotsRuntime/*.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/DotsRuntime/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DotsRuntime/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/DotsRuntime/*.cxx
    ${CMAKE_CURRENT_SOURCE_DIR}/DotsRuntime/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/DotsRuntime/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DotsRuntime/*.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/DotsRuntime/*.inl

    ${CMAKE_CURRENT_SOURCE_DIR}/DotsRuntime/*.natvis
)

if(FULL_STATIC)
    add_library(DotsRuntime STATIC ${DotsRuntimeSource})
else()
    add_library(DotsRuntime SHARED ${DotsRuntimeSource})
    target_compile_definitions(DotsRuntime
        PRIVATE DOTSRUNTIME_EXPORTS 
    )
endif(FULL_STATIC)

target_include_directories(DotsRuntime
    PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/DotsRuntime>
)

## Glob dots files.
file(GLOB_RECURSE 
    DotsSource
    ${CMAKE_CURRENT_SOURCE_DIR}/Dots/*.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/Dots/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Dots/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Dots/*.cxx
    ${CMAKE_CURRENT_SOURCE_DIR}/Dots/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Dots/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Dots/*.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/Dots/*.inl

    ${CMAKE_CURRENT_SOURCE_DIR}/Dots/*.natvis
)

add_library(Dots STATIC ${DotsSource})

target_include_directories(Dots
    PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Dots/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Dots>
)

target_link_libraries(Dots
    PUBLIC DotsRuntime)

target_compile_definitions(DotsRuntime
    PRIVATE ENABLE_GUID_COMPONENT 
)
target_compile_definitions(Dots
    PRIVATE ENABLE_GUID_COMPONENT 
)

if(FULL_STATIC)
target_compile_definitions(DotsRuntime
    PRIVATE FULL_STATIC 
)
target_compile_definitions(Dots
    PRIVATE FULL_STATIC 
)
endif(FULL_STATIC)