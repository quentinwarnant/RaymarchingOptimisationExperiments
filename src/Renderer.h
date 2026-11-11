#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectXMath.h>
#include "VolumeData.h"

struct ImDrawData;

enum class RaymarchMode : int { Naive, Cached };

struct RenderSettings
{
    RaymarchMode mode = RaymarchMode::Naive;
    int primarySteps = 180;
    int shadowSteps = 55;
    int gridResolution = 256;
    float densityScale = 18.948f;
    float extinction = 3.0f;
    float scattering = 0.557f;
    float stepScale = 1.0f;
    float densityThreshold = 0.054f;
    float densityGamma = 0.8f;
    float shadowDensityScale = 1.093f;
    float phaseAnisotropy = 0.516f;
    float directLightIntensity = 1.007f;
    float multipleScattering = 0.452f;
    float lightingSkipEpsilon = 0.0018f;
    float powderStrength = 0.690f;
    float ambientLight = 0.155f;
    float exposure = 1.25f;
    float contrast = 1.08f;
    float displayGamma = 2.2f;
    float backgroundIntensity = 0.987f;
    DirectX::XMFLOAT3 lightColor{ 1.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT3 ambientColor{ 0.702f, 0.702f, 0.702f };
    DirectX::XMFLOAT3 lightDirection{ 0.277f, 0.417f, 0.168f };
    float cameraYaw = -1.0297f;
    float cameraPitch = -0.1920f;
    float cameraDistance = 4.025f;
    float lightRotationSpeed = 0.25f;
    bool enableSurfaceNormals = true;
    bool enableTileClassify = false;
    bool useNormalLUT = true;
    // When true, uses 4-tap forward-diff normals instead of 6-tap central. Only takes
    // effect when useNormalLUT is off (LUT path is already 1 tap).
    bool useReducedTapNormals = false;
    // When true, dispatches the *_CountCS variants that write per-pixel
    // DenseDensityAt sample counts into m_sampleCountTexture, then runs
    // HeatmapDisplayCS to false-colour the count on top of the main output.
    bool showDensityHeatmap = false;
    // Which sample counter the heatmap visualises.
    //   0 = DenseVolume (DenseDensityAt)   — main density fetch cost.
    //   1 = CachedDepth (CacheNormalDotLight & finite-diff normal taps).
    //   2 = both (sum) — combined texture-fetch pressure.
    int heatmapCounterMode = 0;
    int heatmapMaxSamples = 256;
    // Temporal frame reuse: reproject prior frame's colour where geometry alone moved
    // (camera translation/rotation). Any lighting/settings change globally invalidates.
    // Never overlaps partial-sample accumulation, so no ghosting.
    bool useTemporalReuse = false;
    // Motion-adaptive step count: when camera or light is moving (auto-orbit,
    // rotateLight, or per-frame delta exceeds a small threshold), scale
    // primarySteps × 0.7 and stepScale × 1/0.7 so the total volume coverage is
    // preserved but the loop iteration count drops. Motion perceptually masks the
    // coarser sampling. Single-frame — no ghosting, no history.
    bool motionAdaptiveSteps = false;
    // Sub-Nyquist light cache: only rebuild the coarse depth cache when the light
    // direction has moved by more than `lightCacheAngleThresholdDeg` degrees since
    // the last build. Between rebuilds, the render lerps between the previous and
    // latest cache to hide the update seam. For rotating light we predict the
    // next-threshold pose one step ahead so the interpolation window brackets
    // current; for manual light changes we snap to the fresh cache once ready.
    // Rebuild remains synchronous on the graphics queue — the perf win is fewer
    // rebuilds per second, not hidden cost.
    bool useLightCacheSubNyquist = true;
    bool useAmortizedCache = false;
    // Degrees of light-direction change between cache rebuilds. Small threshold
    // (2°) — near-continuous rebuilds; large (15°) — visible transmittance drift
    // between poses smoothed only by interpolation.
    float lightCacheAngleThresholdDeg = 5.0f;
    // Debug overlay: green pixels reused from history, red pixels required a march.
    bool debugTemporalReuse = false;
    int normalLUTResolution = 192;
    bool useDistanceField = false;
    bool debugDistanceField = false;
    bool useSplitTrace = false;
    bool useTileVariantDispatch = false;
    bool debugTiles = false;
    bool autoOrbit = false;
    bool rotateLight = true;
    bool debugVoxels = false;
    bool rebuildCache = true;
    int vsyncMode = 1;
};

struct RendererStats
{
    float gpuTotalMs = 0.0f;
    float gpuRenderMs = 0.0f;
    float gpuRenderSampleMs = 0.0f;
    // Unsmoothed per-frame total (t1 - t0 across the whole Render call), so the
    // graph can display raw cache-build spikes instead of the EMA-averaged value.
    float gpuTotalSampleMs = 0.0f;
    float gpuVoxelizeMs = 0.0f;
    float gpuLinkMs = 0.0f;
    float gpuResolveMs = 0.0f;
    float lastCacheBuildMs = 0.0f;
    float cpuActiveMs = 0.0f;
    uint32_t occupiedCells = 0;
    uint32_t compactCacheCells = 0;
    uint32_t totalCells = 0;
    uint32_t volumeWidth = 0;
    uint32_t volumeHeight = 0;
    uint32_t volumeDepth = 0;
    uint32_t activeBricks = 0;
    uint64_t sourceActiveVoxels = 0;
    uint64_t denseVolumeBytes = 0;
    uint64_t sparseVolumeBytes = 0;
    uint64_t normalLUTBytes = 0;
    uint64_t cacheBytes = 0;
    // Per-frame raw samples so true min/max come from actual per-frame values
    // (including cache-rebuild spikes) rather than the EMA-smoothed number.
    std::array<float, 180> gpuHistory{};
    std::array<float, 180> gpuTotalHistory{};
};

class Renderer
{
public:
    ~Renderer();
    bool Initialize(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();
    void Resize(uint32_t width, uint32_t height);
    // Swap the active VDB volume: waits GPU idle, tears down VDB-owned resources,
    // loads the new file, recreates textures/buffers, and invalidates the transmittance
    // cache + normal LUT bake + temporal history so the next frame rebuilds everything
    // consistent with the new density field.
    bool LoadVolume(const std::filesystem::path& path);
    void BeginFrame();
    void Render(RenderSettings& settings, ImDrawData* drawData, float timeSeconds);
    void WaitForGpu();
    bool CaptureOutput(std::vector<uint8_t>& rgbaPixels);

    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputSrv() const;
    const RendererStats& GetStats() const { return m_stats; }
    const std::string& GetLastError() const { return m_lastError; }
    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }

private:
    static constexpr uint32_t FrameCount = 2;
    static constexpr uint32_t TimestampCount = 10;

    enum DescriptorIndex : uint32_t
    {
        ImGuiFont, OutputUav, DensityUav, LinksUav, TransmittanceUav, StatsUav,
        RemappedVolumeUav, RemappedVolumeMip3Uav, TileClassificationUav,
        NormalLUTUav,
        DistanceFieldAUav, DistanceFieldBUav,
        HitIntervalUav,
        TileListUav, IndirectDispatchArgsUav,
        SampleCountUav,
        ReuseValidUav,
        // MUST stay at the end so its position within the UAV subrange (index 16)
        // matches the shader binding `NormalLUTWriteB : register(u16)`. Inserting
        // this earlier shifts every subsequent slot and silently rebinds all
        // downstream UAVs to the wrong shader registers (the LUT_B bake would
        // land in the distance-field texture, and LUT_B stays uninitialised).
        NormalLUTBUav,
        OutputSrv, DensitySrv, LinksSrv, TransmittanceSrv, DenseVolumeSrv,
        SparsePageSrv, SparseBrickSrv,
        RemappedVolumeSrv, TileClassificationSrv, NormalLUTSrv, DistanceFieldSrv,
        HistoryColorSrv,
        TransmittanceSrvB,
        NormalLUTSrvB,
        DescriptorCount
    };

    struct FrameContext
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        uint64_t fenceValue = 0;
    };

    struct alignas(16) ShaderConstants
    {
        DirectX::XMFLOAT4 cameraPositionTime;
        DirectX::XMFLOAT4 cameraTargetMode;
        DirectX::XMFLOAT4 resolutionSteps;
        DirectX::XMFLOAT4 volumeParameters;
        DirectX::XMFLOAT4 lightDirectionGrid;
        DirectX::XMFLOAT4 volumeMin;
        DirectX::XMFLOAT4 volumeMax;
        DirectX::XMFLOAT4 cloudParameters;
        DirectX::XMFLOAT4 volumeGridParameters;
        DirectX::XMFLOAT4 sparseGridParameters;
        DirectX::XMFLOAT4 densityTransferParameters;
        DirectX::XMFLOAT4 lightingParameters;
        DirectX::XMFLOAT4 lightColorParameters;
        DirectX::XMFLOAT4 ambientColorParameters;
        DirectX::XMFLOAT4 cacheParameters;
        // Precomputed cache-frame axes so shaders don't recompute normalize/cross/dot
        // every LightCacheUVW call. See PopulateConstants for derivation.
        // *Scaled entries: xyz = axis / (2 * dot(abs(axis), volumeHalfExtent)),
        //                  w  = 0.5 - dot(volumeCenter, axis) / (2 * dot(abs(axis), volumeHalfExtent))
        // So uvw_i = saturate(dot(p, *Scaled.xyz) + *Scaled.w).
        // *Axis entries hold the plain unit vector for callers that need the frame.
        DirectX::XMFLOAT4 lightCacheRightScaled;
        DirectX::XMFLOAT4 lightCacheUpScaled;
        DirectX::XMFLOAT4 lightCacheForwardScaled;
        DirectX::XMFLOAT4 lightCacheRightAxis;
        DirectX::XMFLOAT4 lightCacheUpAxis;
        DirectX::XMFLOAT4 lightCacheForwardAxis;
        // Sub-Nyquist light cache: second set of *Scaled fields for physical
        // buffer B's light-frame axes. When A and B were baked at different
        // light directions, sampling both at the same uvw is geometrically
        // wrong (shadows slide over the geometry as the light rotates). B has
        // its own uvw = dot(p, *ScaledB.xyz) + *ScaledB.w computed with
        // B-frame axes. When sub-N is off / same dir, B's axes mirror A's and
        // the sample is bit-exact identical.
        DirectX::XMFLOAT4 lightCacheRightScaledB;
        DirectX::XMFLOAT4 lightCacheUpScaledB;
        DirectX::XMFLOAT4 lightCacheForwardScaledB;
        // Plain (unscaled) B-frame axes. Required for reconstructing world-space
        // gradients from B's uvw-space finite differences separately from A's, so
        // that at rebuild moments the ping-pong of dirBakedInA does not rotate the
        // reconstructed normal discontinuously. Blend gradA_world with gradB_world
        // AFTER world-space reconstruction; do not blend raw samples then decompose
        // with a single axis frame.
        DirectX::XMFLOAT4 lightCacheRightAxisB;
        DirectX::XMFLOAT4 lightCacheUpAxisB;
        DirectX::XMFLOAT4 lightCacheForwardAxisB;
        DirectX::XMFLOAT4 cacheBuildRange;
        // Previous-frame camera state for temporal reprojection. Set to current-frame
        // values on the first frame / after a global invalidation to make reprojection
        // trivially identity.
        DirectX::XMFLOAT4 prevCameraPositionTime;
        DirectX::XMFLOAT4 prevCameraTargetMode;
    };

    bool CreateDeviceAndSwapchain(HWND hwnd);
    bool CreateCoreResources();
    bool CreateSizeDependentResources();
    bool CreateVolumeResources(uint32_t resolution);
    bool CreateNormalLUT(uint32_t resolution);
    bool CreateVdbResources();
    bool CreatePipelines();
    void DestroySizeDependentResources();
    void RecordCacheBuild(const RenderSettings& settings, uint32_t writeSlot);
    void RecordBakeVolumes(const RenderSettings& settings);
    void RecordScene(const RenderSettings& settings, bool cacheRebuiltThisFrame);
    void PopulateConstants(RenderSettings& settings, float timeSeconds);
    // Overwrites the light-dependent fields (lightDirectionGrid + the six
    // lightCache*Scaled/Axis frame axes) in the given constants block with the
    // frame derived from `lightDir`. Used to author separate constants blocks
    // for the sub-Nyquist cache build (predictive target dir) versus the scene
    // render (current dir), so the build texture indexes match the direction we
    // baked it for.
    void FillLightConstants(ShaderConstants& constants,
                            const DirectX::XMFLOAT3& lightDir) const;
    // Writes only the *ScaledB fields (buffer-B uvw axes) so that
    // SampleCachedDepthWorld reads physical B at its own light-frame uvw.
    // Called from PopulateConstants during sub-Nyquist blending, when A was
    // baked at cacheLightOld and B was baked at cacheLightNew.
    void FillLightCacheAxesB(ShaderConstants& constants,
                             const DirectX::XMFLOAT3& lightDirB) const;
    ID3D12Resource* PhysicalCache(uint32_t slot) const;
    ID3D12Resource* PhysicalNormalLUT(uint32_t slot) const;
    D3D12_RESOURCE_STATES& PhysicalCacheState(uint32_t slot);
    D3D12_RESOURCE_STATES& PhysicalNormalLUTState(uint32_t slot);
    void RebindCacheDescriptors();
    void ResetAmortizedCacheState();
    DirectX::XMFLOAT3 RotateLightByThreshold(const DirectX::XMFLOAT3& lightDir,
        float thresholdRad, const RenderSettings& settings) const;
    void ReadCompletedTimings(uint32_t frameIndex);
    void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES& current, D3D12_RESOURCE_STATES next);
    void UavBarrier(ID3D12Resource* resource);
    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(uint32_t index) const;
    void SetError(const std::string& message);

    HWND m_hwnd = nullptr;
    uint32_t m_width = 0, m_height = 0, m_frameIndex = 0, m_gridResolution = 0;
    uint32_t m_descriptorSize = 0, m_rtvDescriptorSize = 0;
    uint64_t m_fenceValue = 0, m_timestampFrequency = 1;
    uint32_t m_waveLaneCount = 32;
    HANDLE m_fenceEvent = nullptr;
    bool m_initialized = false, m_cacheValid = false;

    std::array<FrameContext, FrameCount> m_frames;
    std::array<bool, FrameCount> m_cacheBuiltFrames{};
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, FrameCount> m_backBuffers;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, FrameCount> m_timestampReadbacks;
    Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapchain;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap, m_srvHeap;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_queryHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_constantBuffer, m_outputTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_densityBuffer, m_linksBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_transmittanceBuffer, m_statsBuffer;
    // Sub-Nyquist light cache: second transmittance buffer. When the feature is
    // OFF, only m_transmittanceBuffer is written (rebuilds land in A) and B is
    // dormant — CachedDepthReadB descriptor still points at A so shader lerp with
    // blend=0 is bit-exact identical to reading A directly. When ON, the two
    // buffers hold two consecutive cache poses; m_cacheOldPhysical says which
    // physical buffer currently plays the "old" role, and m_cacheBlend maps
    // shader lerp(A, B, .) so that render output moves from old→new as the
    // current light direction moves through the interpolation window.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_transmittanceBufferB;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_transmittanceBufferC;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_denseVolumeTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sparsePageTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sparseBrickBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_remappedVolumeTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_tileClassificationBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_normalLUTTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_normalLUTTextureB;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_normalLUTTextureC;
    uint32_t m_normalLUTResolution = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_distanceFieldTextureA;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_distanceFieldTextureB;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_hitIntervalBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_tileList;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_indirectDispatchArgs;
    // R32_UINT texture that receives per-pixel DenseDensityAt sample counts when the
    // heatmap debug view is enabled. Sized to render dimensions.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sampleCountTexture;
    D3D12_RESOURCE_STATES m_sampleCountState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // Temporal reuse: history colour (prev-frame Output), per-pixel reuse marker.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_historyColorTexture;
    D3D12_RESOURCE_STATES m_historyColorState = D3D12_RESOURCE_STATE_COPY_DEST;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_reuseValidTexture;
    D3D12_RESOURCE_STATES m_reuseValidState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // History-validity state. History is only sampled when this is true.
    bool m_temporalHistoryValid = false;
    // Camera orbit accumulator: independent of wall-clock timeSeconds so the
    // effective angle only advances while autoOrbit is enabled; disabling &
    // re-enabling picks up where it left off. Light rotation mutates
    // settings.lightDirection directly (yaw slider follows), so no
    // accumulator is needed for it.
    float m_orbitAccumulator = 0.0f;
    float m_lastFrameTimeSeconds = 0.0f;
    bool m_hasLastFrameTime = false;
    // Prev-frame camera constants for reprojection.
    DirectX::XMFLOAT4 m_prevCameraPositionTime{};
    DirectX::XMFLOAT4 m_prevCameraTargetMode{};
    // Prev-frame camera pose + light direction for motion-adaptive step-count.
    // Separate from temporal-reuse tracking so it stays valid even when reuse is off.
    float m_motionPrevYaw = 0.0f;
    float m_motionPrevPitch = 0.0f;
    float m_motionPrevDistance = 0.0f;
    DirectX::XMFLOAT3 m_motionPrevLightDir{};
    bool m_motionHasReference = false;
    // Sub-Nyquist light-cache tracking. Both physical buffers (A = m_transmittanceBuffer,
    // B = m_transmittanceBufferB) hold a coarse depth cache for some past light dir.
    // m_cacheOldPhysical says which physical slot currently plays the "old" role
    // (0 = A holds old, 1 = B holds old). m_cacheLightOld/New store the light
    // directions each was built for. m_cacheBlend is the lerp weight fed to the
    // shader — computed so that lerp(SRV_A, SRV_B, m_cacheBlend) yields the
    // correct old→new interpolation regardless of which physical slot is which.
    DirectX::XMFLOAT3 m_cacheLightOld{};
    DirectX::XMFLOAT3 m_cacheLightNew{};
    uint32_t m_cacheOldPhysical = 0;
    float m_cacheBlend = 0.0f;
    int m_slotDisplayA = 0;
    int m_slotDisplayB = 1;
    int m_slotBuild = 2;
    int m_buildColumnCursor = 0;
    uint32_t m_buildColumnsPerFrame = 0;
    DirectX::XMFLOAT3 m_buildTargetDir{ 0.0f, 0.0f, -1.0f };
    bool m_buildActive = false;
    bool m_buildComplete = false;
    bool m_amortizedCacheEnabled = true;
    // True once buffer B has been populated at least once. Until then the "old"
    // and "new" halves may hold identical content (both A) and blend is forced to 0.
    bool m_cacheBothPopulated = false;
    bool m_cacheHasValidB = false;
    // Logical interpolation t (0 = pure old, 1 = pure new). Distinct from
    // m_cacheBlend, which is the *physical* lerp weight passed to the shader
    // (may equal t or 1-t depending on which physical buffer holds "old"). The
    // logical t is used on the CPU to compute the interpolated effective light
    // direction fed into the scene light frame — that way the light frame we
    // render with matches the caches' bake-time frames at t=0 and t=1, and
    // rotates smoothly between them for t in (0,1). Without this, sampling
    // caches at current-frame UVW produces geometry-disconnected shadows.
    float m_cacheLogicalT = 0.0f;
    // Was sub-Nyquist enabled last frame? Toggle detection to reset state.
    bool m_subNyquistWasEnabled = false;
    // Scheduler → Render() handoff: which physical buffer to target on rebuild.
    uint32_t m_pendingCacheWriteSlot = 0;
    // Whether the scheduler decided a rebuild is required this frame.
    bool m_pendingCacheRebuild = false;
    // Whether this rebuild is the first ever write into physical buffer B (so we
    // need to rewrite TransmittanceSrvB descriptor from A → B after the build).
    bool m_pendingFirstBWrite = false;
    // Sub-Nyquist prediction: light direction that the current pending build is
    // targeting. When rebuilding while rotating, this is `currentLight rotated
    // ahead by threshold` so that at the moment of rebuild, the shader's
    // interpolation window brackets currentLight (blend≈0 renders the previous
    // NEW = the freshly-renamed OLD, giving a continuous transition).
    DirectX::XMFLOAT3 m_cachePredictedLightDir{};
    bool m_pendingPredictiveBuild = false;
    bool m_pendingProgressiveBuild = false;
    bool m_pendingCacheSwap = false;
    uint32_t m_pendingCacheBuildRangeStart = 0;
    uint32_t m_pendingCacheBuildRangeEnd = 0;
    uint32_t m_lastCacheRebuildPhysicalSlot = 0;
    // Hash of the last frame's shading-affecting settings. Any mismatch → global
    // invalidate → skip reprojection and mark history dirty.
    uint64_t m_lastSettingsHash = 0;
    D3D12_RESOURCE_STATES m_indirectDispatchArgsState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, FrameCount> m_statsReadbacks;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_resolvePso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_naivePso, m_cachedPso, m_cachedDfPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_cachedLutPso, m_cachedDfLutPso;
    // Sub-Nyquist-off specialisations of the four primary cached PSOs. Compiled
    // without USE_SUB_NYQUIST so DXC dead-strips every B-cache tap / LUT-B sample /
    // gradB blend, dropping peak register pressure and unlocking a higher SM warp
    // occupancy tier. Selected when RenderSettings::useLightCacheSubNyquist == false.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_cachedNoSubNPso, m_cachedDfNoSubNPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_cachedLutNoSubNPso, m_cachedDfLutNoSubNPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_cachedNoSNPso, m_cachedDfNoSNPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_cachedLutNoSNPso, m_cachedDfLutNoSNPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_cachedDebugPso;
    // Reduced-tap normal variants (4-tap forward diff instead of 6-tap central).
    // Meaningful only when the normal LUT is off; used via `useReducedTapNormals` UI.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_cachedReducedTapPso, m_cachedDfReducedTapPso;
    // Sample-counting variants of the render entry points + heatmap display pass.
    // Only used when RenderSettings::showDensityHeatmap is set.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_cachedCountPso, m_naiveCountPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_cachedLutCountPso, m_cachedDfCountPso, m_cachedDfLutCountPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_cachedNoSNCountPso, m_cachedLutNoSNCountPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_cachedDfNoSNCountPso, m_cachedDfLutNoSNCountPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_cachedReducedTapCountPso, m_cachedDfReducedTapCountPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_heatmapDisplayPso;
    // Temporal reuse: reprojection + debug overlay.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_temporalReprojectPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_temporalReuseDebugPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_bakeRemappedPso, m_bakeOccupancyPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_bakeNormalLUTPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_bakeNormalLUTBPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_bakeDistanceFieldInitPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_bakeDistanceFieldPropagatePso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_debugDistanceFieldPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_findHitIntervalPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_splitCachedPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_clearTileDispatchPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_splitCachedIndirectPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_splitCachedSkyPso;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_dispatchSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_classifyTilesPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_debugTileOverlayPso;

    uint8_t* m_constantData = nullptr;
    D3D12_RESOURCE_STATES m_outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_densityState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_linksState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_transmittanceState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_transmittanceStateB = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_transmittanceStateC = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_statsState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_remappedVolumeState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_tileClassificationState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_normalLUTState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_normalLUTStateB = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_normalLUTStateC = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bool m_normalLUTBaked = false;
    bool m_normalLUTBBaked = false;
    bool m_normalLUTBValid = false;
    std::array<bool, 3> m_normalLUTBakedSlots{};
    // Set once per Render() call to record whether the just-completed cache rebuild
    // (if any) wrote to physical B. Consumed by RecordScene's normal-LUT bake logic
    // so it can update LUT_A or LUT_B to match. Meaningless when no rebuild happened.
    bool m_lastCacheRebuildWroteToB = false;
    // Set true on the frame the user toggles sub-Nyquist on. Causes Render() to
    // issue a SECOND cache build into physical B (after the standard writeToA
    // seed) using a look-ahead light direction, so both physicals are populated
    // from the very first frame and no stale/degenerate interp occurs.
    bool m_pendingSubNSeedDualBuild = false;
    // Frame-scoped hint that RecordScene should bake BOTH LUT_A and LUT_B this
    // frame regardless of the standard wroteA/wroteB rebuild gate. Set on the
    // sub-N seed-dual frame and on normal-LUT toggle-on (when B has valid data).
    bool m_forceBakeLUTABBoth = false;
    // Previous-frame useNormalLUT tick state, used to detect toggle-on and force
    // an immediate re-bake of the LUT(s) so the first frame after enable does
    // not interpolate stale data.
    bool m_prevUseNormalLUT = true;
    D3D12_RESOURCE_STATES m_distanceFieldAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES m_distanceFieldBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    float m_bakedDensityThreshold = std::numeric_limits<float>::quiet_NaN();
    float m_bakedDensityGamma = std::numeric_limits<float>::quiet_NaN();
    float m_bakedDensityScale = std::numeric_limits<float>::quiet_NaN();
    bool m_bakeVolumesPending = true;
    VolumeData m_volumeData;
    DirectX::XMFLOAT3 m_effectiveLightDirection{};
    RendererStats m_stats;
    std::string m_lastError;
};
