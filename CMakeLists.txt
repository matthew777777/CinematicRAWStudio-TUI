cmake_minimum_required(VERSION 3.20)
project(CinematicRAWStudioTUI VERSION 0.1.0 LANGUAGES CXX)

option(CRAW_BUILD_TESTS "Build development tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CRAW_DECODER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../CinematicRAWStudio-Decoder"
    CACHE PATH "CinematicRAWStudio-Decoder checkout")

include(FetchContent)
FetchContent_Declare(ftxui
    URL https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v6.1.9.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ftxui)

find_package(Threads REQUIRED)
add_library(craw_develop STATIC
    ${CRAW_DECODER_DIR}/src/Decoder.cpp
    ${CRAW_DECODER_DIR}/src/Reader.cpp
    ${CRAW_DECODER_DIR}/tools/common/Clip.cpp
    src/Develop.cpp
    src/Export.cpp
    src/ClipSummary.cpp
    src/Thumbnail.cpp)
target_include_directories(craw_develop PUBLIC src ${CRAW_DECODER_DIR}/include ${CRAW_DECODER_DIR}/tools)
target_link_libraries(craw_develop PUBLIC Threads::Threads)
if(MSVC)
    target_compile_options(craw_develop PRIVATE /W4 /utf-8)
    target_compile_definitions(craw_develop PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(craw_develop PRIVATE -Wall -Wextra)
endif()

add_executable(craw-tui src/main.cpp src/Tui.cpp)
target_link_libraries(craw-tui PRIVATE craw_develop ftxui::screen ftxui::dom ftxui::component)

if(CRAW_BUILD_TESTS)
    enable_testing()
    add_executable(develop_tests tests/DevelopTests.cpp)
    target_link_libraries(develop_tests PRIVATE craw_develop)
    add_test(NAME develop COMMAND develop_tests)
endif()
if(MSVC)
    target_compile_options(craw-tui PRIVATE /utf-8)
endif()

install(TARGETS craw-tui)
