# DX12 Volumetric Raymarching

A Win32 DirectX 12 compute renderer comparing a conventional nested raymarcher with a voxel transmittance cache. The renderer imports `Assets/Altocumlus.vdb` with OpenVDB, presents through Dear ImGui, and records GPU timings with D3D12 timestamp queries.

## Build

Requirements: Windows 10/11, Visual Studio 2022 with the Desktop C++ workload, CMake 3.24+, Git, vcpkg, and a DirectX 12 GPU.

```powershell
$vcpkg = "C:\Program Files (x86)\Microsoft Visual Studio\2022\Enterprise\VC\vcpkg"
cmake -S . -B build-vdb -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$vcpkg\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_INSTALLED_DIR="$PWD\vcpkg_installed"
cmake --build build-vdb --config Release
.\build-vdb\Release\DX12Raymarching.exe
```

The vcpkg manifest restores OpenVDB and its dependencies. CMake fetches Dear ImGui v1.90.9. Every entry point in `Volume.hlsl` is compiled at build time by DXC to Shader Model 6.0 DXIL. The VDB is copied beside the executable.

`VdbInspect` runs the same production importer without DX12 and prints the source grid, dense dimensions, page-table dimensions, active brick count, and memory use:

```powershell
.\build-vdb\Release\VdbInspect.exe .\Assets\Altocumlus.vdb
```

## Rendering methods

At startup, OpenVDB selects the `density` `FloatGrid` (or the first float grid), finds its active bounding box, and normalizes its density. It creates:

- A regular filtered `R16_FLOAT` 3D texture, capped at 256 voxels on the longest axis.
- A sparse 8³-brick grid with an `R32_UINT` 3D page table and compact structured brick pool. Empty VDB regions consume no brick storage.

**Naive mode** samples the regular 3D texture while marching the camera ray. At every non-empty sample it launches a second dense-texture march toward the directional light and integrates optical depth. Its worst-case density work is approximately `primary steps * shadow steps` per pixel.

**Linked cache mode** separates light transport from view rendering:

1. `ResolveColumnsCS` maps the dense VDB into light-aligned columns and integrates every column in one GPU dispatch.
2. Each hardware wave owns one column and uses `WavePrefixSum` over 32/64-cell chunks. One dispatch replaces the previous sequence of one dispatch and UAV barrier per Z slice.
3. Each occupied column atomically allocates a compact fine-residual chain spanning its active Z range plus a four-cell interpolation halo.
4. A dense coarse prefix level stores one cumulative sample every four cells. Compact fine residuals reconstruct full-resolution depth.
5. Both levels use packed FP16 optical depth; column metadata stores compact-chain offsets and bounds.
6. `CachedRenderCS` uses the same dense primary-ray sampling as naive mode. A density-variation probe adaptively selects 4–8 local correction samples, then one trilinear two-level lookup supplies cumulative optical depth for the remaining light path.

Segments use two-point Gaussian density integration and half-length boundary segments. Because cache columns are light-aligned, links have no projected-neighbour ambiguity. Empty columns allocate no residual chain, while occupied columns store only their active span plus interpolation halo. The cache is rebuilt when density, extinction, light direction, or grid resolution changes.

## Controls and statistics

Naive mode is the default tuning reference. The ImGui panel exposes the complete appearance pipeline:

- Density gain, threshold, gamma, extinction, scattering, and shadow-density gain.
- Henyey-Greenstein phase anisotropy, direct-light intensity, approximate multiple scattering, and powder/silver lining.
- Independent light and ambient colors plus ambient intensity.
- Exposure, contrast, display gamma, and background intensity.
- Primary/shadow integration steps, local cache-correction steps/distance, camera, and light direction.

`Dense reference preset` provides an opaque photographic starting point. Density-transfer and shadow controls also invalidate the linked cache so both modes remain comparable. The panel reports imported dense-grid dimensions, original active voxel count, sparse active brick count, and dense/sparse memory.

The supplied tuned values are the application defaults. Press F1/F2 to switch directly between naive and cached lighting. The directional light can rotate around world Y; `Direction to light` controls its starting direction and elevation, while `Light rotation speed` controls signed angular velocity. Cached mode rebuilds its light cache every frame while rotation is enabled.

## Automated parity test

Build and run the in-app DX12 regression without UI automation:

```powershell
cmake --build build-vdb --config Release --target RunCacheParity
```

The hidden test renders fixed naive and cached sequences, reads the renderer output texture back through DX12, writes `cache-test-results/naive.ppm`, `cached.ppm`, and `report.json`, then fails unless cached mode has mean RGB error at most `1.0` and is at least 5% faster. The report includes occupied and compact residual cell counts, allocated cache memory, cache-build time, and render speedup.

Over the scene, left-drag controls camera yaw/pitch and disables automatic orbit. The mouse wheel controls distance from 0 to 20, allowing the camera to move inside the VDB; a small internal epsilon keeps the view direction defined at zero.

## Current tradeoffs

- The cache targets one directional light. Multiple or local lights need separate caches or a different parameterization.
- Light-aligned columns produce exact one-cell successor links for the directional light, but changing the light direction requires rebuilding the oriented cache.
- The sparse density grid uses fixed 8³ bricks. NanoVDB or a GPU-native VDB tree could reduce page-table and brick-boundary overhead further.
- Cache construction is intentionally synchronous in the graphics queue for clarity. A production renderer could rebuild asynchronously and double-buffer the cache.
