# ===========================================================================
# FlexGravity - legacy Ballance Mod Loader (BML) 0.3.40+ target
# ---------------------------------------------------------------------------
# Produces FlexGravity.bmod for the original loader.  The BML+ target in
# ../CMakeLists.txt is untouched by this build; both compile the same shared
# sources in ../src, the differences are:
#
#   * a compatibility header set in ../src/bml-old/BML that redeclares BML's
#     interfaces against the Virtools 2.1 SDK instead of BML's trimmed
#     virtools/ headers (the SDK is needed for the virtual CKRenderContext
#     drawing calls);
#   * ExecuteBB's two missing force helpers (BmlLegacyExecuteBB.cpp);
#   * a mod-owned Dear ImGui context and renderer (BmlLegacyOverlay.cpp),
#     because the original loader exports no ImGui symbol at all;
#   * no field-line / equipotential overlays: the code, the configuration keys
#     and the log lines are compiled out via FLEXGRAVITY_LEGACY_BML.
#
# Configure with:
#   cmake -S bml-old -B ..\build-bml-old -G "Visual Studio 17 2022" -A Win32
# ===========================================================================
cmake_minimum_required(VERSION 3.20)

project(FlexGravityBmlOld VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(BML_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/BML-dev-0340"
        CACHE PATH "Legacy BML 0.3.40 SDK root (only lib/Release/BML.lib is used)")
set(VT_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/Virtools-SDK-2.1"
        CACHE PATH "Virtools 2.1 SDK root")
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/imgui-1.92.9b/imgui-1.92.9b"
        CACHE PATH "Dear ImGui source root")

if (NOT EXISTS "${BML_SDK_DIR}/lib/Release/BML.lib")
    message(FATAL_ERROR "Legacy BML import library not found at ${BML_SDK_DIR}/lib/Release/BML.lib")
endif ()
if (NOT EXISTS "${VT_SDK_DIR}/Include/CKAll.h")
    message(FATAL_ERROR "Virtools SDK not found at ${VT_SDK_DIR}")
endif ()
if (NOT EXISTS "${IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR "Dear ImGui sources not found at ${IMGUI_DIR}")
endif ()

set(SRC "${CMAKE_CURRENT_SOURCE_DIR}/../src")

# Same engine-free core as the BML+ target, minus the field-line tracer and the
# marching-tetrahedra mesher that only the removed overlays use.
set(FLEXGRAVITY_CORE_SOURCES
        ${SRC}/core/BodySamples.cpp
        ${SRC}/core/expr/Expr.cpp
        ${SRC}/core/expr/FunctionField.cpp
        ${SRC}/core/expr/Parser.cpp
        ${SRC}/core/FieldData.cpp
        ${SRC}/core/Potential.cpp
        ${SRC}/core/RegionVolume.cpp
        ${SRC}/core/Sources.cpp
)

if (NOT EXISTS "${SRC}/version.h.in")
    message(FATAL_ERROR "version.h.in not found at ${SRC}")
endif ()

configure_file(
        "${SRC}/version.h.in"
        "${CMAKE_BINARY_DIR}/generated/version.h"
        @ONLY
)

add_library(FlexGravityBmlOld SHARED
        ${SRC}/diagnostics/Command.cpp
        ${SRC}/diagnostics/Reports.cpp
        ${SRC}/mod/FlexGravityMod.cpp
        ${SRC}/mod/Lifecycle.cpp
        ${SRC}/runtime/FieldRuntime.cpp
        ${SRC}/scan/Scanner.cpp
        ${SRC}/viz/FieldSampler.cpp
        ${SRC}/viz/Overlay.cpp
        ${SRC}/vt/EngineAccess.cpp
        ${SRC}/vt/EngineBindings.cpp
        ${SRC}/vt/ForceBackend.cpp
        ${SRC}/vt/Geometry.cpp
        ${FLEXGRAVITY_CORE_SOURCES}
        # Legacy host layer.
        ${SRC}/bml-old/BmlLegacyExecuteBB.cpp
        ${SRC}/bml-old/BmlLegacyOverlay.cpp
        # Dear ImGui: the loader has none, so the overlay ships its own copy.
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
)

target_include_directories(FlexGravityBmlOld PRIVATE
        "${SRC}/bml-old"          # BML/ compat headers (must precede everything)
        "${SRC}"
        "${VT_SDK_DIR}/Include"
        "${IMGUI_DIR}"
        "${CMAKE_BINARY_DIR}/generated"
)

target_link_libraries(FlexGravityBmlOld PRIVATE
        "${BML_SDK_DIR}/lib/Release/BML.lib"
        "${VT_SDK_DIR}/Lib/CK2.lib"
        "${VT_SDK_DIR}/Lib/VxMath.lib"
)

target_compile_definitions(FlexGravityBmlOld PRIVATE
        FLEXGRAVITY_LEGACY_BML=1
        # Match BML+'s ImGui build (BMLImGuiConfig.h): vertex colors are packed
        # BGRA, so the renderer can copy them straight into a D3D DWORD.
        IMGUI_USE_BGRA_PACKED_COLOR
        WIN32
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
        NOMINMAX
)

target_compile_options(FlexGravityBmlOld PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
        $<$<CXX_COMPILER_ID:MSVC>:/Zc:strictStrings->
        $<$<CXX_COMPILER_ID:MSVC>:/Zc:char8_t->
        $<$<CXX_COMPILER_ID:MSVC>:/W3>
)

# Dear ImGui pulls in the same heavy Virtools/BML headers as every mod
# translation unit; sharing them through the mod's PCH cuts a full build from
# minutes to seconds.  The four ImGui sources are excluded because they must
# not see windows.h/CKAll.h before imgui.h.
target_precompile_headers(FlexGravityBmlOld PRIVATE "${SRC}/Pch.h")
set_source_files_properties(
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON
)

set_target_properties(FlexGravityBmlOld PROPERTIES
        OUTPUT_NAME "FlexGravity"
        PREFIX ""
        SUFFIX ".bmod"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
