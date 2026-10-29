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

# Engine-free core: maths, field formulas, scanner data.  These files never
# include CKAll.h and never touch a CKObject, so they can be unit tested.
set(FLEXGRAVITY_CORE_SOURCES
        src/core/BodySamples.cpp
        src/core/expr/Expr.cpp
        src/core/expr/FunctionField.cpp
        src/core/expr/Parser.cpp
        src/core/FieldData.cpp
        src/core/FieldLines.cpp
        src/core/MarchingTetrahedra.cpp
        src/core/Potential.cpp
        src/core/RegionVolume.cpp
        src/core/Sources.cpp
)

# The version lives in CMakeLists.txt only; the header is generated.
configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in"
        "${CMAKE_BINARY_DIR}/generated/version.h"
        @ONLY
)

add_library(FlexGravity SHARED
        src/diagnostics/Command.cpp
        src/diagnostics/Reports.cpp
        src/mod/FlexGravityMod.cpp
        src/mod/Lifecycle.cpp
        src/runtime/FieldRuntime.cpp
        src/scan/Scanner.cpp
        src/viz/Equipotentials.cpp
        src/viz/FieldLines.cpp
        src/viz/FieldSampler.cpp
        src/viz/Overlay.cpp
        src/vt/EngineAccess.cpp
        src/vt/EngineBindings.cpp
        src/vt/ForceBackend.cpp
        src/vt/Geometry.cpp
        ${FLEXGRAVITY_CORE_SOURCES}
)

target_include_directories(FlexGravity PRIVATE
        "${BML_SDK_DIR}/include"
        "${VT_SDK_DIR}/Include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_BINARY_DIR}/generated"
)

target_precompile_headers(FlexGravity PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/Pch.h")

target_link_libraries(FlexGravity PRIVATE
        "${BML_SDK_DIR}/lib/BMLPlus.lib"
        "${VT_SDK_DIR}/Lib/CK2.lib"
        "${VT_SDK_DIR}/Lib/VxMath.lib"
)

target_compile_definitions(FlexGravity PRIVATE
        WIN32
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
        NOMINMAX
        # The debug overlay links against the ImGui copy that BML+ exports.
        # That copy is built with BMLImGuiConfig.h (BGRA packed colors), so this
        # translation unit must be compiled with the identical configuration.
        IMGUI_USER_CONFIG=\"BMLImGuiConfig.h\"
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

# Engine-free regression tests.  They run without Virtools or Ballance and
# pin the formulas to the values verified in game.  They are developer tools,
# not part of the mod, so they are OFF by default and a checkout without tests/
# still configures and builds the mod.
option(FLEXGRAVITY_BUILD_TESTS "Build the engine-free core tests" OFF)

if (FLEXGRAVITY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_core.cpp")
    add_executable(flexgravity_tests
            "${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_core.cpp"
            ${FLEXGRAVITY_CORE_SOURCES}
    )

    target_include_directories(flexgravity_tests PRIVATE
            "${VT_SDK_DIR}/Include"
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )

    target_link_libraries(flexgravity_tests PRIVATE
            "${VT_SDK_DIR}/Lib/VxMath.lib"
    )

    target_compile_definitions(flexgravity_tests PRIVATE
            WIN32
            _CRT_SECURE_NO_WARNINGS
            _CRT_NONSTDC_NO_WARNINGS
            NOMINMAX
    )

    target_compile_options(flexgravity_tests PRIVATE
            $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
            $<$<CXX_COMPILER_ID:MSVC>:/W3>
    )

    set_target_properties(flexgravity_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
    )

    # Timing harness; run manually, never part of the shipped mod.
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../bench/bench_core.cpp")
    add_executable(flexgravity_bench
            "${CMAKE_CURRENT_SOURCE_DIR}/../bench/bench_core.cpp"
            ${FLEXGRAVITY_CORE_SOURCES}
    )

    target_include_directories(flexgravity_bench PRIVATE
            "${VT_SDK_DIR}/Include"
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )

    target_link_libraries(flexgravity_bench PRIVATE
            "${VT_SDK_DIR}/Lib/VxMath.lib"
    )

    target_compile_definitions(flexgravity_bench PRIVATE
            _CRT_SECURE_NO_WARNINGS
            _CRT_NONSTDC_NO_WARNINGS
            NOMINMAX
    )

    target_compile_options(flexgravity_bench PRIVATE
            $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
            $<$<CXX_COMPILER_ID:MSVC>:/W3>
    )

    set_target_properties(flexgravity_bench PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
    )
    endif ()
endif ()
