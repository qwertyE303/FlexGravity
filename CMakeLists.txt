cmake_minimum_required(VERSION 3.20)

project(FlexGravity VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# BML+ 0.3.13 SDK and the Virtools 2.1 SDK are kept outside the source tree.
# Override these two paths on the command line if the layout changes.
set(BML_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../deps/BMLPlus-SDK-v0.3.13-Release"
        CACHE PATH "BML+ 0.3.13 SDK root")
set(VT_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../deps/Virtools-SDK-2.1"
        CACHE PATH "Virtools 2.1 SDK root")

if (NOT EXISTS "${BML_SDK_DIR}/include/BML/IMod.h")
    message(FATAL_ERROR "BML SDK not found at ${BML_SDK_DIR}")
endif ()
if (NOT EXISTS "${VT_SDK_DIR}/Include/CKAll.h")
    message(FATAL_ERROR "Virtools SDK not found at ${VT_SDK_DIR}")
endif ()

add_library(FlexGravity SHARED
        src/FlexGravity.cpp
)

target_include_directories(FlexGravity PRIVATE
        "${BML_SDK_DIR}/include"
        "${VT_SDK_DIR}/Include"
)

target_link_libraries(FlexGravity PRIVATE
        "${BML_SDK_DIR}/lib/BMLPlus.lib"
        "${VT_SDK_DIR}/Lib/CK2.lib"
        "${VT_SDK_DIR}/Lib/VxMath.lib"
)

target_compile_definitions(FlexGravity PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
        NOMINMAX
)

target_compile_options(FlexGravity PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
        $<$<CXX_COMPILER_ID:MSVC>:/Zc:strictStrings->
        $<$<CXX_COMPILER_ID:MSVC>:/Zc:char8_t->
        $<$<CXX_COMPILER_ID:MSVC>:/W3>
)

set_target_properties(FlexGravity PROPERTIES
        OUTPUT_NAME "FlexGravity"
        PREFIX ""
        SUFFIX ".bmodp"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
)

# Keep the import library and PDB next to the build output.
set_target_properties(FlexGravity PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
