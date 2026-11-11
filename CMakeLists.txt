cmake_minimum_required(VERSION 3.24)
project(DX12Raymarching LANGUAGES CXX)
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.90.9
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(winpixevent
    URL https://www.nuget.org/api/v2/package/WinPixEventRuntime/1.0.240308001)
FetchContent_MakeAvailable(winpixevent)
set(WINPIX_INCLUDE_DIR "${winpixevent_SOURCE_DIR}/Include/WinPixEventRuntime")
set(WINPIX_LIB "${winpixevent_SOURCE_DIR}/bin/x64/WinPixEventRuntime.lib")
set(WINPIX_DLL "${winpixevent_SOURCE_DIR}/bin/x64/WinPixEventRuntime.dll")

find_package(OpenVDB CONFIG REQUIRED)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui_lib PRIVATE UNICODE _UNICODE)
target_link_libraries(imgui_lib PUBLIC d3d12 dxgi)

add_executable(DX12Raymarching WIN32
    src/main.cpp
    src/Renderer.cpp
    src/Renderer.h
    src/VolumeData.cpp
    src/VolumeData.h)
target_sources(DX12Raymarching PRIVATE shaders/Volume.hlsl)
set_source_files_properties(shaders/Volume.hlsl PROPERTIES HEADER_FILE_ONLY TRUE)
target_compile_definitions(DX12Raymarching PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN USE_PIX=1)
target_include_directories(DX12Raymarching PRIVATE "${WINPIX_INCLUDE_DIR}")
target_link_libraries(DX12Raymarching PRIVATE imgui_lib OpenVDB::openvdb d3d12 dxgi dxguid winmm "${WINPIX_LIB}")

add_executable(VdbInspect src/VdbInspect.cpp src/VolumeData.cpp src/VolumeData.h)
target_compile_definitions(VdbInspect PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(VdbInspect PRIVATE OpenVDB::openvdb)

get_filename_component(WINDOWS_KITS_ROOT
    "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]"
    ABSOLUTE)
find_program(DXC_EXECUTABLE NAMES dxc
    HINTS
        "${WINDOWS_KITS_ROOT}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
        "${WINDOWS_KITS_ROOT}/bin/10.0.26100.0/x64"
        "${WINDOWS_KITS_ROOT}/bin/10.0.22621.0/x64")
if(NOT DXC_EXECUTABLE)
    message(FATAL_ERROR "DXC was not found. Install a recent Windows SDK.")
endif()

add_custom_command(TARGET DX12Raymarching POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:DX12Raymarching>/shaders"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:DX12Raymarching>/Assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets"
        "$<TARGET_FILE_DIR:DX12Raymarching>/Assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WINPIX_DLL}"
        "$<TARGET_FILE_DIR:DX12Raymarching>/WinPixEventRuntime.dll"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E ResolveColumnsCS
        -Zi -Zss -Qembed_debug
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/ResolveColumnsCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E NaiveRenderCS
        -Zi -Zss -Qembed_debug -D ENABLE_DISTANCE_FIELD=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/NaiveRenderCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug -D USE_SUB_NYQUIST=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug -D USE_NORMAL_LUT=1 -D USE_SUB_NYQUIST=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderLutCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug -D ENABLE_DISTANCE_FIELD=1 -D USE_SUB_NYQUIST=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderDFCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug -D ENABLE_DISTANCE_FIELD=1 -D USE_NORMAL_LUT=1 -D USE_SUB_NYQUIST=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderDFLutCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    # NoSubN variants of the four primary cached PSOs (surface normals on, no debug/
    # heatmap/reduced-tap). Compiled without USE_SUB_NYQUIST so DXC dead-strips every
    # SampleCachedDepthB, NormalLUTB, and gradB world-space blend — dropping peak
    # register pressure and unlocking a higher SM warp occupancy tier. Selected at
    # dispatch time when RenderSettings::useLightCacheSubNyquist is false.
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderNoSubNCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug -D USE_NORMAL_LUT=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderLutNoSubNCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug -D ENABLE_DISTANCE_FIELD=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderDFNoSubNCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug -D ENABLE_DISTANCE_FIELD=1 -D USE_NORMAL_LUT=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderDFLutNoSubNCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug -D DISABLE_SURFACE_NORMALS=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderNoSNCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug -D USE_NORMAL_LUT=1 -D DISABLE_SURFACE_NORMALS=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderLutNoSNCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug -D ENABLE_DISTANCE_FIELD=1 -D DISABLE_SURFACE_NORMALS=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderDFNoSNCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug -D ENABLE_DISTANCE_FIELD=1 -D USE_NORMAL_LUT=1 -D DISABLE_SURFACE_NORMALS=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderDFLutNoSNCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug -D ENABLE_VOXEL_DEBUG=1 -D USE_SUB_NYQUIST=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderDebugCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    # Reduced-tap normal variants: 4-tap forward diff instead of 6-tap central. Only
    # meaningful when the normal LUT is off (LUT path is already 1 tap). One PSO per DDA
    # mode. SN off doesn't need one (no gradient at all).
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug -D USE_REDUCED_TAP_NORMAL=1 -D USE_SUB_NYQUIST=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderReducedTapCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug -D USE_REDUCED_TAP_NORMAL=1 -D ENABLE_DISTANCE_FIELD=1 -D USE_SUB_NYQUIST=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderDFReducedTapCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    # Density-sample-counting variants for the ImGui heatmap debug view. These emit a
    # per-pixel DenseDensityAt call count into u14 and are only dispatched when
    # RenderSettings::showDensityHeatmap is set. Mirror the full non-count matrix
    # (LUT × DF × NoSN × ReducedTap) so toggles show accurate cost deltas in the
    # overlay without polluting the shipping PSOs.
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug -D ENABLE_DENSITY_COUNTING=1 -D USE_SUB_NYQUIST=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderCountCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug -D ENABLE_DENSITY_COUNTING=1 -D USE_NORMAL_LUT=1 -D USE_SUB_NYQUIST=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderLutCountCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug -D ENABLE_DENSITY_COUNTING=1 -D ENABLE_DISTANCE_FIELD=1 -D USE_SUB_NYQUIST=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderDFCountCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug -D ENABLE_DENSITY_COUNTING=1 -D ENABLE_DISTANCE_FIELD=1 -D USE_NORMAL_LUT=1 -D USE_SUB_NYQUIST=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderDFLutCountCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug -D ENABLE_DENSITY_COUNTING=1 -D DISABLE_SURFACE_NORMALS=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderNoSNCountCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug -D ENABLE_DENSITY_COUNTING=1 -D USE_NORMAL_LUT=1 -D DISABLE_SURFACE_NORMALS=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderLutNoSNCountCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug -D ENABLE_DENSITY_COUNTING=1 -D ENABLE_DISTANCE_FIELD=1 -D DISABLE_SURFACE_NORMALS=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderDFNoSNCountCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug -D ENABLE_DENSITY_COUNTING=1 -D ENABLE_DISTANCE_FIELD=1 -D USE_NORMAL_LUT=1 -D DISABLE_SURFACE_NORMALS=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderDFLutNoSNCountCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug -D ENABLE_DENSITY_COUNTING=1 -D USE_REDUCED_TAP_NORMAL=1 -D USE_SUB_NYQUIST=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderReducedTapCountCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E CachedRenderCS
        -Zi -Zss -Qembed_debug -D ENABLE_DENSITY_COUNTING=1 -D USE_REDUCED_TAP_NORMAL=1 -D ENABLE_DISTANCE_FIELD=1 -D USE_SUB_NYQUIST=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/CachedRenderDFReducedTapCountCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E NaiveRenderCS
        -Zi -Zss -Qembed_debug -D ENABLE_DENSITY_COUNTING=1
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/NaiveRenderCountCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E HeatmapDisplayCS
        -Zi -Zss -Qembed_debug
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/HeatmapDisplayCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E TemporalReprojectCS
        -Zi -Zss -Qembed_debug
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/TemporalReprojectCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E TemporalReuseDebugCS
        -Zi -Zss -Qembed_debug
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/TemporalReuseDebugCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E BakeRemappedVolumeCS
        -Zi -Zss -Qembed_debug
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/BakeRemappedVolumeCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E BakeOccupancyCS
        -Zi -Zss -Qembed_debug
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/BakeOccupancyCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E ClassifyTilesCS
        -Zi -Zss -Qembed_debug
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/ClassifyTilesCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E DebugTileOverlayCS
        -Zi -Zss -Qembed_debug
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/DebugTileOverlayCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E BakeNormalLUTCS
        -Zi -Zss -Qembed_debug
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/BakeNormalLUTCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E BakeNormalLUTBCS
        -Zi -Zss -Qembed_debug
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/BakeNormalLUTBCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E BakeDistanceFieldInitCS
        -Zi -Zss -Qembed_debug
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/BakeDistanceFieldInitCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E BakeDistanceFieldPropagateCS
        -Zi -Zss -Qembed_debug
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/BakeDistanceFieldPropagateCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E DebugDistanceFieldCS
        -Zi -Zss -Qembed_debug
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/DebugDistanceFieldCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E FindHitIntervalCS
        -Zi -Zss -Qembed_debug
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/FindHitIntervalCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E SplitCachedRenderCS
        -Zi -Zss -Qembed_debug
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/SplitCachedRenderCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E ClearTileDispatchCS
        -Zi -Zss -Qembed_debug
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/ClearTileDispatchCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E SplitCachedRenderIndirectCS
        -Zi -Zss -Qembed_debug
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/SplitCachedRenderIndirectCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl"
    COMMAND "${DXC_EXECUTABLE}" -nologo -O3 -T cs_6_0 -E SplitCachedSkyCS
        -Zi -Zss -Qembed_debug
        -Fo "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/SplitCachedSkyCS.cso"
        -Fd "$<TARGET_FILE_DIR:DX12Raymarching>/shaders/"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Volume.hlsl")

add_custom_command(TARGET VdbInspect POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:VdbInspect>/Assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Altocumlus.vdb"
        "$<TARGET_FILE_DIR:VdbInspect>/Assets/Altocumlus.vdb")

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT DX12Raymarching)

add_test(NAME CacheParity
    COMMAND DX12Raymarching --cache-test)
set_tests_properties(CacheParity PROPERTIES
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:DX12Raymarching>"
    TIMEOUT 180)

add_custom_target(RunCacheParity
    COMMAND "$<TARGET_FILE:DX12Raymarching>" --cache-test
    COMMAND ${CMAKE_COMMAND} -E cat
        "$<TARGET_FILE_DIR:DX12Raymarching>/cache-test-results/report.json"
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:DX12Raymarching>"
    DEPENDS DX12Raymarching
    USES_TERMINAL)
