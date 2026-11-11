cbuffer Constants : register(b0)
{
    float4 CameraPositionTime;
    float4 CameraTargetMode;
    float4 ResolutionSteps;
    float4 VolumeParameters;
    float4 LightDirectionGrid;
    float4 VolumeMin;
    float4 VolumeMax;
    float4 CloudParameters;
    float4 VolumeGridParameters;
    float4 SparseGridParameters;
    float4 DensityTransferParameters;
    float4 LightingParameters;
    float4 LightColorParameters;
    float4 AmbientColorParameters;
    float4 CacheParameters;
    // Precomputed light-cache frame (see Renderer::PopulateConstants).
    // *Scaled: xyz = axis / (2 * dot(abs(axis), halfExtent)), w = bias so that
    // uvw_i = saturate(dot(p, *Scaled.xyz) + *Scaled.w). *Axis holds unit axis.
    float4 LightCacheRightScaled;
    float4 LightCacheUpScaled;
    float4 LightCacheForwardScaled;
    float4 LightCacheRightAxis;
    float4 LightCacheUpAxis;
    float4 LightCacheForwardAxis;
    // Sub-Nyquist light cache: second *Scaled set for physical buffer B's
    // light-frame axes. When A and B were baked at different directions,
    // sampling both at the same uvw slides shadows over the geometry —
    // SampleCachedDepthWorld computes uvw_B independently. When the feature
    // is off (or A==B direction), these mirror the primary *Scaled entries
    // and the sample is bit-exact identical.
    float4 LightCacheRightScaledB;
    float4 LightCacheUpScaledB;
    float4 LightCacheForwardScaledB;
    // Plain (unscaled) B-frame axes. Used by normal-tap gradient reconstruction
    // to compose world-space gradients from B's uvw-space finite differences
    // BEFORE blending, so a jump in A's axes at rebuild does not rotate the
    // reconstructed normal discontinuously.
    float4 LightCacheRightAxisB;
    float4 LightCacheUpAxisB;
    float4 LightCacheForwardAxisB;
    // Cache-build column interval: x=start column, y=end column (exclusive).
    // Scene shaders ignore this; ResolveColumnsCS uses it to amortize hidden
    // cache builds over multiple frames.
    float4 CacheBuildRange;
    // Previous-frame camera state for temporal frame reuse. Set to the current-frame
    // values on the first frame / after a global invalidation so reprojection maps to
    // identity and yields nothing. .w of each is unused for now.
    float4 PrevCameraPositionTime;
    float4 PrevCameraTargetMode;
};

cbuffer DispatchConstants : register(b1)
{
    uint4 DispatchParameters;
};

RWTexture2D<float4> Output : register(u0);
RWStructuredBuffer<uint2> CacheMetaWrite : register(u1);
RWStructuredBuffer<uint> FineDepthWrite : register(u2);
RWTexture3D<float> CachedDepthWrite : register(u3);
RWStructuredBuffer<uint> StatsWrite : register(u4);
RWTexture3D<float> RemappedVolumeWrite : register(u5);
RWTexture3D<float> RemappedVolumeMip3Write : register(u6);
RWStructuredBuffer<uint> TileClassificationWrite : register(u7);
RWTexture3D<float4> NormalLUTWrite : register(u8);
RWTexture3D<float> DistanceFieldWriteA : register(u9);
RWTexture3D<float> DistanceFieldWriteB : register(u10);
// Per-pixel `float2(firstHitT, lastHitT)`, produced by FindHitIntervalCS and consumed by
// SplitCachedRenderCS. This is the interval-finding + shading split (option 1) A/B path:
// pass A does a thin DDA-over-DF walk to locate the first and last non-empty DF cells
// along the ray; pass B then does linear marching only within that interval. Sentinel:
// firstHit >= 1e29 means "sky pixel" (ray missed the box or the DF entirely).
RWStructuredBuffer<float2> HitIntervalWrite : register(u11);
// Option 2 (tile-variant dispatch): compacted non-sky tile list + indirect args
// buffer. Populated by FindHitIntervalCS (which folds the tile-list build in) and
// consumed by SplitCachedRenderIndirectCS via ExecuteIndirect. See Option 2 section
// further below for the full documentation.
RWStructuredBuffer<uint> TileListWrite : register(u12);
RWStructuredBuffer<uint> IndirectDispatchArgsWrite : register(u13);
// Per-pixel counter of DenseDensityAt calls, populated only by the *_CountCS PSO
// variants (compiled with -D ENABLE_DENSITY_COUNTING=1) and read by HeatmapDisplayCS.
// Zero-initialised at the start of each shading dispatch. Kept as a Texture2D<uint> so
// the heatmap display shader can sample it with the same descriptor heap as the light-
// cache textures.
RWTexture2D<uint> SampleCountWrite : register(u14);
// Temporal frame reuse: per-pixel validity marker. Written by TemporalReprojectCS
// (1 = reused successfully, 0 = full march required). Read by the main render entry
// points to early-out. Cleared to 0 by the CPU each frame the reuse is disabled or
// globally invalidated, so the check is a no-op.
RWTexture2D<uint> ReuseValidMask : register(u15);
// Sub-Nyquist normal LUT: second normal snapshot (physical buffer B's frame). Baked
// only when we write physical B's transmittance cache; between rebuilds this holds
// the last-baked normals in B's frame axes so the runtime shader can sample LUT_A at
// uvwA and LUT_B at uvwB and blend, avoiding the every-threshold "shadow flash" that
// would otherwise be visible with a single-LUT snapshot.
RWTexture3D<float4> NormalLUTWriteB : register(u16);
StructuredBuffer<uint2> CacheMetaRead : register(t0);
StructuredBuffer<uint> FineDepthRead : register(t1);
Texture3D<float> CachedDepthRead : register(t2);
Texture3D<float> DenseVolume : register(t3);
Texture3D<uint> SparsePage : register(t4);
StructuredBuffer<float> SparseBricks : register(t5);
Texture3D<float> RemappedVolume : register(t6);
StructuredBuffer<uint> TileClassificationRead : register(t7);
Texture3D<float4> NormalLUT : register(t8);
Texture3D<float> DistanceField : register(t9);
// Previous-frame colour texture for temporal reuse. Populated by CopyResource from
// the previous frame's Output. Sampled at the reprojected pixel by TemporalReprojectCS.
Texture2D<float4> HistoryColorRead : register(t10);
// Sub-Nyquist light-cache: second coarse depth cache (physical buffer B). When the
// feature is off, the SRV descriptor at this slot points at the same underlying
// resource as CachedDepthRead (t2), so SampleCachedDepth's lerp(A, B, blend) with
// blend=0 (CacheParameters.w) is bit-exact identical to reading only A.
Texture3D<float> CachedDepthReadB : register(t11);
// LUT_B: baked-normals snapshot for physical buffer B. When sub-N cross-fade is
// inactive this SRV aliases NormalLUT (t8) so w=0 lerp returns LUT_A unchanged.
Texture3D<float4> NormalLUTB : register(t12);
// Distance-field encoding. The bake writes normalised distances-to-nearest-dense in
// [0, horizon] where horizon = 16/128 = 0.125 (see BakeDistanceFieldInitCS). Since the
// stored dynamic range only uses ~12% of a 0..1 texture, we scale by DF_ENCODE_SCALE to
// use the full R8_UNORM range and get better precision-per-bit. Bake writes value*SCALE,
// render/debug reads back sample/SCALE. horizon*SCALE = 1.0 saturates exactly at the
// texture max.
static const float DF_ENCODE_SCALE = 8.0;
static const float DF_DECODE_SCALE = 1.0 / DF_ENCODE_SCALE;

SamplerState LinearClamp : register(s0);
static const uint InvalidCell = 0xffffffffu;

uint GridResolution() { return (uint)LightDirectionGrid.w; }
uint Flatten(uint3 p) { uint n = GridResolution(); return p.x + n * (p.y + n * p.z); }

float Hash31(float3 p)
{
    p = frac(p * 0.1031); p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}

float2 PixelJitter(uint2 pixel)
{
    float2 p = float2(pixel);
    float2 jitter;
    jitter.x = Hash31(float3(p, 17.0));
    jitter.y = Hash31(float3(p.yx, 53.0));
    return jitter - 0.5;
}

float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float3 VolumeCoordinates(float3 p)
{
    return (p - VolumeMin.xyz) * CacheParameters.xyz;
}

float RemapCurve(float density)
{
    float threshold = saturate(DensityTransferParameters.x);
    float remapped = saturate((density - threshold) / max(1.0 - threshold, 1e-4));
    return pow(remapped, max(DensityTransferParameters.y, 0.05));
}

float RemapDensity(float density)
{
    return RemapCurve(density) * VolumeParameters.x;
}

#ifdef ENABLE_DENSITY_COUNTING
// Per-thread counters (HLSL file-scope static in a compute shader is per-invocation).
// All three set to 0 at each entry point and packed into SampleCountWrite before return.
// - g_densitySampleCount:     bumped inside DenseDensityAt (RemappedVolume samples).
// - g_cachedDepthSampleCount: bumped inside SampleCachedDepth (CachedDepthRead samples).
// - g_dfSampleCount:          bumped inside SampleDistanceField (DistanceField loads).
// Packing (fits R32_UINT UAV):
//   bits  0..11 (12) : dense count (0..4095)
//   bits 12..21 (10) : depth count (0..1023)
//   bits 22..31 (10) : df    count (0..1023)
// Values above these ranges are saturated at the packing step so we don't corrupt
// neighbouring fields. 4095 dense/1023 secondary is well above worst-case per-pixel.
static uint g_densitySampleCount = 0;
static uint g_cachedDepthSampleCount = 0;
static uint g_dfSampleCount = 0;
#endif

float DenseDensityAt(float3 p)
{
#ifdef ENABLE_DENSITY_COUNTING
    ++g_densitySampleCount;
#endif
    // No bounds check: RemappedVolume's border voxels are zero-padded by the VDB bake
    // and LinearClamp already clamps uvw, so samples outside the box return 0. This
    // saves 6 compares + an `any` reduction per sample, at ~2 samples per march step
    // (current + prefetch). Verified: cache-test MAE unchanged.
    float3 uvw = VolumeCoordinates(p);
    return RemappedVolume.SampleLevel(LinearClamp, uvw, 0) * VolumeParameters.x;
}

static const uint OccupancyBitsResolution = 32;

float OccupancyAt(float3 p)
{
    // Same rationale as DenseDensityAt: border texels are zero, LinearClamp handles
    // out-of-box samples naturally. Called from the DDA/occupancy-skip inner loop.
    float3 uvw = VolumeCoordinates(p);
    return RemappedVolume.SampleLevel(LinearClamp, uvw, 3);
}

float SparseDensityLoad(int3 voxel)
{
    int3 dimensions = (int3)VolumeGridParameters.xyz;
    if (any(voxel < 0) || any(voxel >= dimensions))
        return 0.0;
    uint brickSize = (uint)VolumeGridParameters.w;
    uint3 brick = (uint3)voxel / brickSize;
    uint brickIndex = SparsePage.Load(int4(brick, 0));
    if (brickIndex == InvalidCell)
        return 0.0;
    uint3 local = (uint3)voxel % brickSize;
    uint localIndex = local.x + brickSize * (local.y + brickSize * local.z);
    return SparseBricks[brickIndex * brickSize * brickSize * brickSize + localIndex];
}

float SparseDensityAt(float3 p)
{
    float3 uvw = VolumeCoordinates(p);
    if (any(uvw < 0.0) || any(uvw > 1.0))
        return 0.0;
    float3 voxelPosition = uvw * (VolumeGridParameters.xyz - 1.0);
    int3 baseVoxel = (int3)floor(voxelPosition);
    float3 blend = frac(voxelPosition);
    float d000 = SparseDensityLoad(baseVoxel);
    float d100 = SparseDensityLoad(baseVoxel + int3(1,0,0));
    float d010 = SparseDensityLoad(baseVoxel + int3(0,1,0));
    float d110 = SparseDensityLoad(baseVoxel + int3(1,1,0));
    float d001 = SparseDensityLoad(baseVoxel + int3(0,0,1));
    float d101 = SparseDensityLoad(baseVoxel + int3(1,0,1));
    float d011 = SparseDensityLoad(baseVoxel + int3(0,1,1));
    float d111 = SparseDensityLoad(baseVoxel + int3(1,1,1));
    float nearSlice = lerp(lerp(d000,d100,blend.x),lerp(d010,d110,blend.x),blend.y);
    float farSlice = lerp(lerp(d001,d101,blend.x),lerp(d011,d111,blend.x),blend.y);
    return RemapDensity(lerp(nearSlice,farSlice,blend.z));
}

float DensityAt(float3 p, bool useSparse)
{
    return useSparse ? SparseDensityAt(p) : DenseDensityAt(p);
}

float3 DensityNormal(float3 p, float centerDensity, bool useSparse)
{
    float3 epsilon = (VolumeMax.xyz - VolumeMin.xyz) / VolumeGridParameters.xyz * 1.5;
    float3 gradient = float3(
        DensityAt(p + float3(epsilon.x,0,0),useSparse) - centerDensity,
        DensityAt(p + float3(0,epsilon.y,0),useSparse) - centerDensity,
        DensityAt(p + float3(0,0,epsilon.z),useSparse) - centerDensity);
    float gradientLength = length(gradient);
    return gradientLength > 1e-5 ? -gradient / gradientLength : float3(0,1,0);
}

float VoxelEdgeMask(float3 p)
{
    float3 uvw = saturate((p - VolumeMin.xyz) / (VolumeMax.xyz - VolumeMin.xyz));
    float3 cellPosition = uvw * GridResolution();
    float3 distanceToPlane = min(frac(cellPosition), 1.0 - frac(cellPosition));
    float edgeDistance = min(max(distanceToPlane.x, distanceToPlane.y),
        min(max(distanceToPlane.x, distanceToPlane.z), max(distanceToPlane.y, distanceToPlane.z)));
    return 1.0 - smoothstep(0.025, 0.075, edgeDistance);
}

bool IntersectBox(float3 origin, float3 direction, out float nearT, out float farT)
{
    float3 safeDirection = lerp(-1.0, 1.0, step(0.0, direction)) * max(abs(direction), 1e-6);
    float3 t0 = (VolumeMin.xyz - origin) / safeDirection;
    float3 t1 = (VolumeMax.xyz - origin) / safeDirection;
    float3 lo = min(t0,t1), hi = max(t0,t1);
    nearT = max(max(lo.x,lo.y), max(lo.z,0.0)); farT = min(hi.x,min(hi.y,hi.z));
    return farT > nearT;
}

// All light-cache frame data is precomputed on CPU (see Renderer::PopulateConstants)
// and read directly from the constant buffer. This kills the normalize + 2 cross-product
// + 6-dot-product recompute that used to happen inside every LightCacheUVW call, and —
// more importantly for register pressure — removes ~9 scalars of local right/up/forward
// state from the caller's live set at the tap site inside CacheGradientDotLight.

void LightCacheFrame(out float3 right, out float3 up, out float3 forward,
    out float3 cacheMinimum, out float3 cacheMaximum)
{
    right   = LightCacheRightAxis.xyz;
    up      = LightCacheUpAxis.xyz;
    forward = LightCacheForwardAxis.xyz;
    // Reconstruct cacheMin/Max in cache space from the packed scaled axes:
    // scaled.w = 0.5 - centerDot * invExtent; centerDot = 0.5*(1-scaled.w)/invExtent,
    // and centerDot ± halfExtent = centerDot ± 0.5/invExtent. So:
    //   min = (centerDot - halfExtent), max = (centerDot + halfExtent)
    // With invExt_i = length(scaled_i.xyz) / length(axis_i.xyz) = length(scaled_i.xyz)
    // (axis is unit), we recover halfExtent = 0.5 / length(scaled.xyz). Only used by
    // legacy debug helpers; cheap enough to keep here.
    float invExtRight   = length(LightCacheRightScaled.xyz);
    float invExtUp      = length(LightCacheUpScaled.xyz);
    float invExtForward = length(LightCacheForwardScaled.xyz);
    float3 halfExt = float3(0.5/invExtRight, 0.5/invExtUp, 0.5/invExtForward);
    float3 centerDot = float3(
        (0.5 - LightCacheRightScaled.w)   / invExtRight,
        (0.5 - LightCacheUpScaled.w)      / invExtUp,
        (0.5 - LightCacheForwardScaled.w) / invExtForward);
    cacheMinimum = centerDot - halfExt;
    cacheMaximum = centerDot + halfExt;
}

float3 WorldToLightCache(float3 p, float3 right, float3 up, float3 forward)
{
    return float3(dot(p, right), dot(p, up), dot(p, forward));
}

// Combined helper: returns uvw AND the frame axes. Frame axes now come from constants
// so no local frame is materialised.
float3 LightCacheUVWAndFrame(float3 p, out float3 right, out float3 up, out float3 forward)
{
    right   = LightCacheRightAxis.xyz;
    up      = LightCacheUpAxis.xyz;
    forward = LightCacheForwardAxis.xyz;
    return saturate(float3(
        dot(p, LightCacheRightScaled.xyz)   + LightCacheRightScaled.w,
        dot(p, LightCacheUpScaled.xyz)      + LightCacheUpScaled.w,
        dot(p, LightCacheForwardScaled.xyz) + LightCacheForwardScaled.w));
}

// UVW-only variant for callers (e.g. LUT-based CacheGradientDotLight) that don't need
// the frame axes. Compiles down to 3 dot-mad-saturate instructions with zero temps.
float3 LightCacheUVW(float3 p)
{
    return saturate(float3(
        dot(p, LightCacheRightScaled.xyz)   + LightCacheRightScaled.w,
        dot(p, LightCacheUpScaled.xyz)      + LightCacheUpScaled.w,
        dot(p, LightCacheForwardScaled.xyz) + LightCacheForwardScaled.w));
}

// Buffer-B uvw. Used by SampleCachedDepthWorld to read physical B at the
// light-frame axes it was baked with when sub-Nyquist has A and B baked at
// different directions. When the feature is off, *ScaledB mirrors *Scaled.
float3 LightCacheUVW_B(float3 p)
{
    return saturate(float3(
        dot(p, LightCacheRightScaledB.xyz)   + LightCacheRightScaledB.w,
        dot(p, LightCacheUpScaledB.xyz)      + LightCacheUpScaledB.w,
        dot(p, LightCacheForwardScaledB.xyz) + LightCacheForwardScaledB.w));
}

float3 LightCacheToWorld(float3 p, float3 right, float3 up, float3 forward)
{
    return right * p.x + up * p.y + forward * p.z;
}

float3 Background(float3 ray)
{
    float3 sky = lerp(float3(0.025,0.035,0.06), float3(0.22,0.34,0.55), saturate(ray.y * 0.5 + 0.5));
    return (sky + pow(saturate(dot(ray, normalize(LightDirectionGrid.xyz))), 512.0)
        * float3(4.0,3.2,2.2)) * LightingParameters.w;
}

float Phase(float c)
{
    float g = clamp(LightingParameters.x, -0.9, 0.9);
    return (1.0-g*g) / (12.56637 * pow(max(1.0+g*g-2.0*g*c, 1e-3), 1.5));
}

// Wrapped sampler for the CachedDepthRead SRVs. Under ENABLE_DENSITY_COUNTING we
// bump the per-thread depth-tap counter so the heatmap can visualise finite-diff
// normal costs (SampleNormalFromCache*, CacheNormalDotLight). Non-count PSOs compile
// this down to a straight SampleLevel with no register overhead.
//
// Sub-Nyquist light-cache dual-frame-axis sample.
//
// Reads physical A at uvwA (its own light-frame uvw) and physical B at uvwB (its own).
// When A and B were baked at different light directions (rotating light between
// rebuilds), sampling both at a shared uvw introduces a geometric offset in whichever
// buffer isn't aligned with that uvw's frame — visible as (a) shadows sliding over
// clouds and (b) at rebuild moments the ping-pong role flip abruptly reverses the sign
// of that offset, producing a per-rebuild "flash" on the shading normal derived from
// finite differences of blended samples. Both artifacts vanish if each buffer is
// sampled at its own bake-frame uvw.
//
// When the sub-Nyquist feature is off (or A and B are baked at the same direction),
// the CPU fills *ScaledB to mirror *Scaled and uvwA == uvwB, so this is bit-exact
// identical to a single-uvw single-fetch path (the two fetches resolve to the same
// value; lerp collapses to A).
float SampleCachedDepth(float3 uvwA, float3 uvwB)
{
#ifdef ENABLE_DENSITY_COUNTING
    ++g_cachedDepthSampleCount;
#endif
    float a = CachedDepthRead.SampleLevel(LinearClamp, uvwA, 0);
    float b = CachedDepthReadB.SampleLevel(LinearClamp, uvwB, 0);
    return lerp(a, b, CacheParameters.w);
}

// Per-buffer single-tap samplers. Used by the normal-tap gradient reconstruction
// which must build gradA_world (from A axes) and gradB_world (from B axes)
// separately, then blend in world space. Blending scalar sample values first and
// decomposing with only A axes rotates the reconstructed normal by (dirBakedInA -
// dirBakedInB) at every ping-pong rebuild — the visible per-threshold "flash".
float SampleCachedDepthA(float3 uvwA)
{
#ifdef ENABLE_DENSITY_COUNTING
    ++g_cachedDepthSampleCount;
#endif
    return CachedDepthRead.SampleLevel(LinearClamp, uvwA, 0);
}

float SampleCachedDepthB(float3 uvwB)
{
#ifdef ENABLE_DENSITY_COUNTING
    ++g_cachedDepthSampleCount;
#endif
    return CachedDepthReadB.SampleLevel(LinearClamp, uvwB, 0);
}

// World-space entry point used by the direct transmittance query (CachedOpticalDepth).
// Computes both uvws internally.
float SampleCachedDepthWorld(float3 p)
{
    return SampleCachedDepth(LightCacheUVW(p), LightCacheUVW_B(p));
}

// Wrapped point-load of the coarse distance field. Under ENABLE_DENSITY_COUNTING we
// bump the per-thread DF counter for the heatmap; non-count PSOs compile to a straight
// Load with no register overhead. DDA/DF is called on every march step when the
// Empty-space DDA skipping optimisation is enabled.
// Option 5 decode: the field is stored scaled (see DF_ENCODE_SCALE) so multiply back
// down. Fused into the return so the compiler treats it as a free constant multiply.
//
// Prior experiments (reverted, do NOT retry without measurement):
// - Load() -> SampleLevel(LinearClamp) to go via L1TEX cache: sampler descriptor
//   setup fattens the call site; SampleDistanceFieldCell showed up as its own
//   6.06% self-time row and peak-reg rose. Measured 0.256 ms -> 0.35 ms interactive.
float SampleDistanceFieldCell(int3 cell)
{
#ifdef ENABLE_DENSITY_COUNTING
    ++g_dfSampleCount;
#endif
    return DistanceField.Load(int4(cell, 0)) * DF_DECODE_SCALE;
}

float CachedOpticalDepth(float3 p)
{
    return SampleCachedDepthWorld(p);
}

float CachedLight(float3 p, float startDensity)
{
    // p is always inside the volume box (called from TraceVolume between nearT/farT),
    // so IntersectBox(p + light*eps, light) always returns true — that check was
    // pure register pressure at the peak-live-reg site. CachedOpticalDepth safely
    // saturates its uvw, so out-of-box samples clamp to the border.
    return exp(-CachedOpticalDepth(p));
}

// Octahedral encoding: unit vector -> float2 in [-1,1]. Compact 16-bit-per-normal storage.
float2 OctEncode(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z) + 1e-8);
    float2 e = n.z >= 0.0 ? n.xy
                          : (1.0 - abs(n.yx)) * float2(n.x >= 0 ? 1 : -1, n.y >= 0 ? 1 : -1);
    return e;
}

float3 OctDecode(float2 e)
{
    float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.x += n.x >= 0 ? -t : t;
    n.y += n.y >= 0 ? -t : t;
    return normalize(n);
}

// Reconstruct world-space gradient of the light-cache scalar field via 6-tap central
// finite differences. Sub-Nyquist correctness: gradient is reconstructed independently
// for each physical buffer (A and B may be baked at different light directions) and
// then blended in WORLD space. Blending scalar samples first and then decomposing
// with only A's axes rotates the reconstructed normal by (dirBakedInA - dirBakedInB)
// at every ping-pong rebuild — the visible per-threshold "flash".
float3 SampleNormalFromCacheWithDelta(float3 uvwCenter, float3 uvwCenterB, float delta)
{
    float3 rightA, upA, forwardA, cacheMinimum, cacheMaximum;
    LightCacheFrame(rightA, upA, forwardA, cacheMinimum, cacheMaximum);

    float3 gradA_world = 0.0;
    {
        float dXp = SampleCachedDepthA(saturate(uvwCenter + float3(delta, 0, 0)));
        float dXm = SampleCachedDepthA(saturate(uvwCenter - float3(delta, 0, 0)));
        float dYp = SampleCachedDepthA(saturate(uvwCenter + float3(0, delta, 0)));
        float dYm = SampleCachedDepthA(saturate(uvwCenter - float3(0, delta, 0)));
        float dZp = SampleCachedDepthA(saturate(uvwCenter + float3(0, 0, delta)));
        float dZm = SampleCachedDepthA(saturate(uvwCenter - float3(0, 0, delta)));
        gradA_world = (dXp - dXm) * rightA + (dYp - dYm) * upA + (dZp - dZm) * forwardA;
    }
#ifdef USE_SUB_NYQUIST
    float3 gradB_world = 0.0;
    {
        float dXp = SampleCachedDepthB(saturate(uvwCenterB + float3(delta, 0, 0)));
        float dXm = SampleCachedDepthB(saturate(uvwCenterB - float3(delta, 0, 0)));
        float dYp = SampleCachedDepthB(saturate(uvwCenterB + float3(0, delta, 0)));
        float dYm = SampleCachedDepthB(saturate(uvwCenterB - float3(0, delta, 0)));
        float dZp = SampleCachedDepthB(saturate(uvwCenterB + float3(0, 0, delta)));
        float dZm = SampleCachedDepthB(saturate(uvwCenterB - float3(0, 0, delta)));
        gradB_world = (dXp - dXm) * LightCacheRightAxisB.xyz
                    + (dYp - dYm) * LightCacheUpAxisB.xyz
                    + (dZp - dZm) * LightCacheForwardAxisB.xyz;
    }
    float3 worldGradient = lerp(gradA_world, gradB_world, CacheParameters.w);
#else
    float3 worldGradient = gradA_world;
#endif
    float gradientLength = length(worldGradient);
    return gradientLength > 1e-5 ? -worldGradient / gradientLength : float3(0, 1, 0);
}

// Variant used by the runtime shading path where the caller already holds the A cache
// frame. Reconstructs gradA in world space using the caller-provided A frame, gradB
// via the cbuffer B axes, then blends world-space vectors.
float3 SampleNormalFromCacheWithFrame(float3 uvwCenter, float3 uvwCenterB, float delta,
    float3 right, float3 up, float3 forward)
{
    float3 gradA_world = 0.0;
    {
        float dXp = SampleCachedDepthA(saturate(uvwCenter + float3(delta, 0, 0)));
        float dXm = SampleCachedDepthA(saturate(uvwCenter - float3(delta, 0, 0)));
        gradA_world += (dXp - dXm) * right;
    }
    {
        float dYp = SampleCachedDepthA(saturate(uvwCenter + float3(0, delta, 0)));
        float dYm = SampleCachedDepthA(saturate(uvwCenter - float3(0, delta, 0)));
        gradA_world += (dYp - dYm) * up;
    }
    {
        float dZp = SampleCachedDepthA(saturate(uvwCenter + float3(0, 0, delta)));
        float dZm = SampleCachedDepthA(saturate(uvwCenter - float3(0, 0, delta)));
        gradA_world += (dZp - dZm) * forward;
    }
#ifdef USE_SUB_NYQUIST
    float3 gradB_world = 0.0;
    {
        float dXp = SampleCachedDepthB(saturate(uvwCenterB + float3(delta, 0, 0)));
        float dXm = SampleCachedDepthB(saturate(uvwCenterB - float3(delta, 0, 0)));
        gradB_world += (dXp - dXm) * LightCacheRightAxisB.xyz;
    }
    {
        float dYp = SampleCachedDepthB(saturate(uvwCenterB + float3(0, delta, 0)));
        float dYm = SampleCachedDepthB(saturate(uvwCenterB - float3(0, delta, 0)));
        gradB_world += (dYp - dYm) * LightCacheUpAxisB.xyz;
    }
    {
        float dZp = SampleCachedDepthB(saturate(uvwCenterB + float3(0, 0, delta)));
        float dZm = SampleCachedDepthB(saturate(uvwCenterB - float3(0, 0, delta)));
        gradB_world += (dZp - dZm) * LightCacheForwardAxisB.xyz;
    }
    float3 worldGradient = lerp(gradA_world, gradB_world, CacheParameters.w);
#else
    float3 worldGradient = gradA_world;
#endif
    float gradientLength = length(worldGradient);
    return gradientLength > 1e-5 ? -worldGradient / gradientLength : float3(0, 1, 0);
}

// Fused sampler that returns `dot(surfaceNormal, L)` as a scalar for the shading hot
// path. Sub-Nyquist correctness requires per-buffer world-gradient reconstruction
// (see SampleNormalFromCacheWithDelta comment), so the earlier register-saving
// "axis-dot-L in uvw space" trick is no longer valid — it implicitly assumed a single
// world axis frame for the whole gradient. We now materialise world-space gradA and
// gradB explicitly, blend, then dot with L. This costs a few extra registers but the
// finite-differences here are already the register-heaviest part of TraceVolume; the
// correctness win at every threshold-crossing rebuild is required.
float CacheNormalDotLight(float3 uvwCenter, float3 uvwCenterB, float delta,
    float3 rightA, float3 upA, float3 forwardA,
    float3 L, float fallbackDotL)
{
    float3 gradA_world = 0.0;
#ifdef USE_SUB_NYQUIST
    float3 gradB_world = 0.0;
#endif
#ifdef USE_REDUCED_TAP_NORMAL
    // Reduced-tap variant: 4-tap forward differences instead of 6-tap central. Saves 3
    // texture fetches per buffer per gradient. Cost: gradient direction is biased by one
    // delta on each axis, but the density transfer function usually masks it.
    float dCa = SampleCachedDepthA(uvwCenter);
    {
        float dXp = SampleCachedDepthA(saturate(uvwCenter + float3(delta, 0, 0)));
        float dYp = SampleCachedDepthA(saturate(uvwCenter + float3(0, delta, 0)));
        float dZp = SampleCachedDepthA(saturate(uvwCenter + float3(0, 0, delta)));
        gradA_world = (dXp - dCa) * rightA + (dYp - dCa) * upA + (dZp - dCa) * forwardA;
    }
#ifdef USE_SUB_NYQUIST
    float dCb = SampleCachedDepthB(uvwCenterB);
    {
        float dXp = SampleCachedDepthB(saturate(uvwCenterB + float3(delta, 0, 0)));
        float dYp = SampleCachedDepthB(saturate(uvwCenterB + float3(0, delta, 0)));
        float dZp = SampleCachedDepthB(saturate(uvwCenterB + float3(0, 0, delta)));
        gradB_world = (dXp - dCb) * LightCacheRightAxisB.xyz
                    + (dYp - dCb) * LightCacheUpAxisB.xyz
                    + (dZp - dCb) * LightCacheForwardAxisB.xyz;
    }
#endif
#else
    {
        float dXp = SampleCachedDepthA(saturate(uvwCenter + float3(delta, 0, 0)));
        float dXm = SampleCachedDepthA(saturate(uvwCenter - float3(delta, 0, 0)));
        float dYp = SampleCachedDepthA(saturate(uvwCenter + float3(0, delta, 0)));
        float dYm = SampleCachedDepthA(saturate(uvwCenter - float3(0, delta, 0)));
        float dZp = SampleCachedDepthA(saturate(uvwCenter + float3(0, 0, delta)));
        float dZm = SampleCachedDepthA(saturate(uvwCenter - float3(0, 0, delta)));
        gradA_world = (dXp - dXm) * rightA + (dYp - dYm) * upA + (dZp - dZm) * forwardA;
    }
#ifdef USE_SUB_NYQUIST
    {
        float dXp = SampleCachedDepthB(saturate(uvwCenterB + float3(delta, 0, 0)));
        float dXm = SampleCachedDepthB(saturate(uvwCenterB - float3(delta, 0, 0)));
        float dYp = SampleCachedDepthB(saturate(uvwCenterB + float3(0, delta, 0)));
        float dYm = SampleCachedDepthB(saturate(uvwCenterB - float3(0, delta, 0)));
        float dZp = SampleCachedDepthB(saturate(uvwCenterB + float3(0, 0, delta)));
        float dZm = SampleCachedDepthB(saturate(uvwCenterB - float3(0, 0, delta)));
        gradB_world = (dXp - dXm) * LightCacheRightAxisB.xyz
                    + (dYp - dYm) * LightCacheUpAxisB.xyz
                    + (dZp - dZm) * LightCacheForwardAxisB.xyz;
    }
#endif
#endif
#ifdef USE_SUB_NYQUIST
    float3 worldGradient = lerp(gradA_world, gradB_world, CacheParameters.w);
#else
    float3 worldGradient = gradA_world;
#endif
    float denomSq = dot(worldGradient, worldGradient);
    return denomSq > 1e-10 ? -dot(worldGradient, L) * rsqrt(denomSq) : fallbackDotL;
}

float3 SampleNormalFromCache(float3 uvwCenter, float3 uvwCenterB)
{
    float delta = 2.0 / max((float)GridResolution(), 1.0);
    return SampleNormalFromCacheWithDelta(uvwCenter, uvwCenterB, delta);
}

float3 CacheGradientNormal(float3 p)
{
    float3 right, up, forward;
    float3 uvwCenter = LightCacheUVWAndFrame(p, right, up, forward);
    if (DispatchParameters.z != 0)
    {
        // Two-LUT path: LUT_A snapshots A's normal frame, LUT_B B's frame. Sample
        // each at its own uvw, then blend in world space with the cache blend
        // weight. When sub-N is inactive, LUT_B aliases LUT_A and w=0 -> the blend
        // collapses to LUT_A unchanged.
        float3 nA = NormalLUT.SampleLevel(LinearClamp, uvwCenter, 0).xyz;
#ifdef USE_SUB_NYQUIST
        float3 uvwB = LightCacheUVW_B(p);
        float3 nB = NormalLUTB.SampleLevel(LinearClamp, uvwB, 0).xyz;
        float3 blended = lerp(nA, nB, CacheParameters.w);
#else
        float3 blended = nA;
#endif
        float bl = length(blended);
        return bl > 1e-5 ? blended / bl : float3(0, 1, 0);
    }
    float3 uvwCenterB = LightCacheUVW_B(p);
    float delta = 2.0 / max((float)GridResolution(), 1.0);
    return SampleNormalFromCacheWithFrame(uvwCenter, uvwCenterB, delta, right, up, forward);
}

// Runtime hot-path variant: returns `dot(surfaceNormal, L)` as a single scalar. Same
// value the shading cascade needs, but never materialises a float3 normal — kills the
// 44-reg peak in TraceVolume by dropping the sampler's local live set from ~18 to ~11
// scalars and freeing right/up/forward immediately after the axis projections.
float CacheGradientDotLight(float3 p)
{
#ifdef USE_NORMAL_LUT
    // LUT-only variant: baked octahedral (or float3) normals sampled from a 3D LUT. This
    // path doesn't need right/up/forward at all — only uvw. Compiling out the finite-
    // difference branch drops the kernel peak register allocation, because DXC would
    // otherwise reserve enough registers for the worst-case FD path even if it's never
    // taken at runtime.
    float3 uvwCenter = LightCacheUVW(p);
    float3 L = normalize(LightDirectionGrid.xyz);
    float3 nA = NormalLUT.SampleLevel(LinearClamp, uvwCenter, 0).xyz;
#ifdef USE_SUB_NYQUIST
    float3 uvwCenterB = LightCacheUVW_B(p);
    float3 nB = NormalLUTB.SampleLevel(LinearClamp, uvwCenterB, 0).xyz;
    float3 encoded = lerp(nA, nB, CacheParameters.w);
#else
    float3 encoded = nA;
#endif
    float encLen = length(encoded);
    return encLen > 1e-5 ? dot(encoded, L) / encLen : L.y;
#else
    float3 right, up, forward;
    float3 uvwCenter = LightCacheUVWAndFrame(p, right, up, forward);
    float3 uvwCenterB = LightCacheUVW_B(p);
    float3 L = normalize(LightDirectionGrid.xyz);
    float delta = 2.0 / max((float)GridResolution(), 1.0);
    return CacheNormalDotLight(uvwCenter, uvwCenterB, delta, right, up, forward, L, L.y);
#endif
}

float NaiveLight(float3 p)
{
    float3 lightDirection = normalize(LightDirectionGrid.xyz);
    float boxNear, boxFar;
    if (!IntersectBox(p + lightDirection * 0.002, lightDirection, boxNear, boxFar)) return 1.0;
    uint steps = max((uint)ResolutionSteps.w,1u);
    float stepLength = boxFar/steps, opticalDepth = 0.0;
    [loop] for (uint i=0; i<steps; ++i)
    {
        opticalDepth += DenseDensityAt(p + lightDirection*((i+0.5)*stepLength))*stepLength;
        if (opticalDepth*VolumeParameters.y*DensityTransferParameters.z > 12.0) break;
    }
    return exp(-opticalDepth*VolumeParameters.y*DensityTransferParameters.z);
}

float4 TraceVolume(uint2 pixel, bool useCache)
{
    float2 dimensions = ResolutionSteps.xy;
    float2 jitter = PixelJitter(pixel);
    float2 ndc = ((float2(pixel) + 0.5 + jitter * 0.9) / dimensions) * 2.0 - 1.0;
    ndc.x *= dimensions.x/dimensions.y;
    float3 origin = CameraPositionTime.xyz;
    float3 forward = normalize(CameraTargetMode.xyz-origin);
    float3 right = normalize(cross(forward,float3(0,1,0))), up = cross(right,forward);
    float3 ray = normalize(forward + ndc.x*right*0.58 - ndc.y*up*0.58);
    // Background sky is only combined into the final colour after the loop. Don't
    // cache it here — the 3-scalar `color` would stay live across the entire ray
    // march, contributing to the 40+ live-reg peak at the depth taps. Recompute it
    // once at the end from `ray`, which is alive anyway.
    float nearT,farT; if (!IntersectBox(origin,ray,nearT,farT)) return float4(Background(ray),1);
#ifdef ENABLE_VOXEL_DEBUG
    uint debugSteps = CameraTargetMode.w > 0.5 ? GridResolution() * 3 : 1;
    uint steps=max(max((uint)ResolutionSteps.z,1u), debugSteps);
#else
    // Voxel-grid debug overlay compiled out: `debugSteps` and its `CameraTargetMode.w`
    // read never enter the register file. The DDA/occupancy gates below use the same
    // #ifdef so the compiler doesn't have to keep `CameraTargetMode.w > 0.5` live.
    uint steps=max((uint)ResolutionSteps.z,1u);
#endif
    float stepLength=(farT-nearT)/steps*VolumeParameters.w, viewT=1.0;
    float3 scattered=0; float phase=Phase(dot(ray, normalize(LightDirectionGrid.xyz)));
    float rayOffset = InterleavedGradientNoise(float2(pixel));
#ifdef ENABLE_DISTANCE_FIELD
    bool useDF = DispatchParameters.w != 0
    #ifdef ENABLE_VOXEL_DEBUG
        && CameraTargetMode.w < 0.5
    #endif
        ;
    bool dfActive = useDF;
#else
    // DDA-free variant: the whole distance-field cell-walk is compiled out of this
    // shader entry so its state (dfActive/boxExtent/raySign/rayNonZero/cellExitUvw/
    // tToFace) never occupies live registers. Occupancy is measurably higher; use the
    // *_DF entry when the "Empty Space DDA Skipping" toggle is on.
    const bool useDF = false;
#endif
    // DDA / occupancy setup is deferred into the branches that need it so the compiler
    // doesn't have to keep boxExtent/raySign/occ* live across every loop iteration.
    // Every persistent float3 costs 3 scalar registers — a straight path to lower
    // occupancy in this texture-bound shader.
    const float dfDim = 128.0;
    const float invDfDim = 1.0 / 128.0;
    float t = nearT + rayOffset * stepLength;
    // Density prefetch state: at the end of each iteration we issue the density fetch for
    // the NEXT iteration's sample position so the texture return latency (dominant LGSB
    // stall in the profile) overlaps with the current iteration's lighting math. Skip
    // paths (DDA cell walk, occupancy jump) invalidate the pending value because they
    // change `t` unpredictably.
    // Prefetch the next iteration's density fetch NOW so its LGSB latency is hidden by
    // this iteration's lighting cascade. Removing this prefetch dropped peak regs 33→28
    // but hurt interactive perf 0.13→0.15 ms (occupancy 27.3%→23.9%, SM throughput
    // 25.1%→20.2%) — warps stall on texture fetches with no in-flight work. Do NOT
    // revert. Skip paths (DDA cell walk, occupancy jump) invalidate the pending value
    // because they change `t` unpredictably.
    float pendingDensity = 0.0;
    bool havePending = false;
    [loop] for(uint i=0;i<steps;++i)
    {
        if(t>farT) break;
        float3 p=origin+ray*t;
#ifdef ENABLE_DISTANCE_FIELD
        if (dfActive)
        {
            // Match DenseDensityAt's UV transform exactly so DF-empty <=> naive-empty at
            // the same world position.
            float3 uvw = saturate(VolumeCoordinates(p));
            // Load the DF value for the current cell (nearest neighbour, not trilinear).
            // If we're clearly inside an empty cell, do 3D-DDA to advance t to the next
            // DF cell boundary along the ray. This is voxel-traversal: adjacent rays
            // with nearly-identical direction end up at nearly-identical cell boundaries,
            // so their post-skip t values match to within a smooth continuous variation
            // (no accumulated per-pixel step-count divergence).
            uint3 cell = uint3(clamp(uvw * dfDim, 0.0, dfDim - 1.0));
            float dfHere = SampleDistanceFieldCell(int3(cell));
            if (dfHere > 0.5 * invDfDim)
            {
                // Determine how many DF cells we can safely walk before needing to sample
                // the DF again. dfHere is the world-space distance from this cell centre to
                // the nearest dense cell; every cell within (dfHere - safety) of us is
                // guaranteed empty. Leave 1 cell of runway so the final cell we enter is
                // still guaranteed empty.
                float cellsSafe = clamp(floor(dfHere * dfDim) - 1.0, 1.0, 96.0);
                // Walk that many DDA cell-exit steps in one go. Each iteration only advances
                // t along one axis face, so we're guaranteed to LAND on a DF cell boundary
                // every time. Adjacent pixels sharing this cell take identical walks and end
                // up at identical boundaries — the property that eliminates block artifacts.
                // Locally scoped setup: these values are only needed here, so declaring
                // them inside the branch shortens their live range and preserves warp
                // occupancy in the hot outer loop.
                float3 boxExtent = VolumeMax.xyz - VolumeMin.xyz;
                float3 raySign = sign(ray);
                float3 rayNonZero = ray + raySign * 1e-30;
                [loop] for (uint j = 0; j < uint(cellsSafe); ++j)
                {
                    if (t > farT) break;
                    float3 uvwj = saturate((origin + ray * t - VolumeMin.xyz) / boxExtent);
                    float3 cj = floor(clamp(uvwj * dfDim, 0.0, dfDim - 1.0));
                    float3 cellExitUvw = select(raySign > 0.0, cj + 1.0, cj) * invDfDim;
                    float3 tToFace = (cellExitUvw - uvwj) * boxExtent / rayNonZero;
                    tToFace = select(tToFace > 0.0, tToFace, 1e9);
                    t += min(tToFace.x, min(tToFace.y, tToFace.z)) + stepLength * 0.01;
                }
                havePending = false;
                continue;
            }
            else
            {
                // Snap t back onto this pixel's jitter grid (nearT + rayOffset*stepLength + k*stepLength)
                // so post-skip marching samples at the same phase the pixel would have used
                // in the pure-naive path.
                float baseT = nearT + rayOffset * stepLength;
                float k = ceil((t - baseT) / stepLength);
                t = baseT + k * stepLength;
                dfActive = false;
            }
        }
#endif  // ENABLE_DISTANCE_FIELD
        // Only use the coarse-mip occupancy skip when DF is disabled globally. When DF is
        // on, once a wave has switched off DF we want the remainder of the ray to march
        // exactly like naive to avoid layering two skip mechanisms with different
        // resolutions (which shows up as blocky boundaries).
        // Sample occupancy every 16th iteration. Every-iter testing chewed 30% of the
        // shader's TEX bandwidth (per Nsight input-dependencies) — the answer is
        // stable within a small neighbourhood, so we amortise across a burst of
        // marches. Costs at most 15 wasted march steps at empty->dense boundaries;
        // shipping-quality PSF is unchanged. i-cadence avoids adding loop state.
        // Cache-test measurements: no cadence 0.086 ms → &3 0.080 → &7 0.078 →
        // &15 0.077 → &31 0.077 (flatline). Picked &15 as the safest low-cadence
        // that captures ~99% of the gain; higher cadence saves nothing on this
        // workload and risks burning march budget on empty space in interactive
        // scenes with more edge tiles.
        if (!useDF
#ifdef ENABLE_VOXEL_DEBUG
            && CameraTargetMode.w < 0.5
#endif
            && ((i & 15u) == 0u)
            && OccupancyAt(p) < 0.0005)
        {
            // Occupancy-only skip is the fallback for the naive-mip path. Locally scoped
            // setup for the same live-range reason as the DDA block above.
            const uint occRes = 32;
            float3 occCellSize = (VolumeMax.xyz - VolumeMin.xyz) / float(occRes);
            float occCellDistance = max(occCellSize.x, max(occCellSize.y, occCellSize.z));
            uint occSkipSteps = max(1u, (uint)floor(occCellDistance / max(stepLength, 1e-5) * 0.5));
            float skipSteps = float(occSkipSteps + 1);
            t += stepLength * skipSteps;
            havePending = false;
            continue;
        }
        t += stepLength;
        float density;
        if (havePending) density = pendingDensity;
        else density = DenseDensityAt(p);
        {
            float3 pNext = origin + ray * t;
            pendingDensity = DenseDensityAt(pNext);
            havePending = true;
        }
        if(density>0.001)
        {
            float attenuation=exp(-density*VolumeParameters.y*stepLength);
            float contribution=viewT*(1.0-attenuation);
            // Gate the full lighting cascade on whether this sample's scattered-light
            // contribution can possibly be visible. `contribution * scatteringCoef` is the
            // exact scalar we multiply into `scattered`; if it's below the user threshold,
            // skip the expensive texture fetches (CachedLight, normal LUT) and phase math.
            // We still update viewT/early-exit so alpha/silhouette are correct.
            bool visible = (contribution * VolumeParameters.z) > LightColorParameters.w;
            [branch] if (visible)
            {
                // ---- Normal-based tint first ---------------------------------------
                // CacheGradientDotLight is the peak-live-reg point of the shader. Its
                // 6 depth taps + frame axes dominate register pressure; when the user
                // disables surface normals via the ImGui toggle we compile out the
                // entire subtree with #ifndef DISABLE_SURFACE_NORMALS so DXC doesn't
                // reserve registers for the worst-case sampler path.
#ifdef DISABLE_SURFACE_NORMALS
                const float normalMod = 1.0;
#else
                float normalMod = 1.0;
                {
                    float normalizedDensity = saturate(density / max(VolumeParameters.x, 1e-4));
                    float surfaceBand = 1.0 - smoothstep(0.18, 0.82, normalizedDensity);
                    if (surfaceBand > 0.01)
                    {
                        // Fused normal-dot-light sampler: returns one scalar directly,
                        // avoiding a float3 normal (and its 3 accumulator regs + the
                        // 9 regs of right/up/forward) alive across the six depth taps.
                        float dotNL = CacheGradientDotLight(p);
                        float normalLighting = 0.24 + 1.02 * saturate(dotNL);
                        normalMod = lerp(1.0, normalLighting, surfaceBand * 0.86);
                    }
                }
#endif

                // ---- Lighting cascade ----------------------------------------------
                float lightT=useCache?CachedLight(p,density):NaiveLight(p);
                float powder = 1.0 - exp(-density * CloudParameters.x * 1.8);
                float silverLining = pow(saturate(lightT), 0.38) * (0.35 + powder * 0.9);
                float heightAmbient = lerp(0.45, 1.25,
                    saturate((p.y - VolumeMin.y) / (VolumeMax.y - VolumeMin.y)));
                float directLight = lightT * LightingParameters.y
                    * (1.15 + silverLining) * phase * 12.56637;
                directLight += LightingParameters.z * pow(saturate(lightT), 0.28)
                    * (0.25 + powder * 0.75);
                float3 warmSun = LightColorParameters.rgb;
                float3 coolAmbient = AmbientColorParameters.rgb * CloudParameters.y * heightAmbient;
                float3 lightColor = (warmSun * directLight + coolAmbient) * normalMod;
#ifdef ENABLE_VOXEL_DEBUG
                if (CameraTargetMode.w > 0.5)
                {
                    float edgeMask = VoxelEdgeMask(p);
                    lightColor = lerp(lightColor, float3(0.02, 4.0, 0.12), edgeMask);
                    contribution += viewT * edgeMask * 0.08;
                    scattered += viewT * edgeMask * float3(0.002, 0.08, 0.004);
                }
#endif
                scattered += contribution*VolumeParameters.z*lightColor;
            }
            viewT *= attenuation; if(viewT<0.005) break;
        }
    }
    float3 color = scattered + viewT * Background(ray);
    color *= CloudParameters.z;
    color = max((color - 0.18) * CloudParameters.w + 0.18, 0.0);
    color = pow(saturate(color), 1.0 / max(DensityTransferParameters.w, 0.1));
    return float4(color,1);
}

// Split-pipeline shading pass. Reads `startT` (the DDA-derived first-hit t written by
// FindHitIntervalCS) and marches linearly from there to farT with the full lighting
// cascade — no DDA, no distance-field state, no occupancy skip. The intent is to give
// this shader its own tighter register allocation than the monolithic TraceVolume, so
// warp occupancy climbs and we hide texture latency better.
float4 SplitTraceVolume(uint2 pixel, float startT, float endT, bool useCache, bool enableSurfaceNormals)
{
    float2 dimensions = float2((uint2)ResolutionSteps.xy);
    float2 jitter = PixelJitter(pixel);
    float2 ndc = ((float2(pixel) + 0.5 + jitter * 0.9) / dimensions) * 2.0 - 1.0;
    ndc.x *= dimensions.x/dimensions.y;
    float3 origin = CameraPositionTime.xyz;
    float3 forward = normalize(CameraTargetMode.xyz-origin);
    float3 right = normalize(cross(forward,float3(0,1,0))), up = cross(right,forward);
    float3 ray = normalize(forward + ndc.x*right*0.58 - ndc.y*up*0.58);
    float3 color = Background(ray);
    // Recover farT from the same box intersect used by the pass A shader. Pass A already
    // guaranteed startT is inside [nearT, farT]; if the ray misses the box (sentinel
    // startT >= 1e30) we bail with just background.
    float nearT, farT;
    if (!IntersectBox(origin, ray, nearT, farT) || startT >= 1e29)
        return float4(color, 1);
    // Tighten farT to the last non-empty t reported by pass A. This is the second half
    // of the interval-finding win: adjacent pixels only ever march the exact slab of the
    // ray that intersects dense DF cells.
    farT = min(farT, endT);
    uint steps = max((uint)ResolutionSteps.z, 1u);
    float stepLength = (farT - nearT) / steps * VolumeParameters.w;
    float viewT = 1.0;
    float3 scattered = 0;
    float phase = Phase(dot(ray, normalize(LightDirectionGrid.xyz)));
    float rayOffset = InterleavedGradientNoise(float2(pixel));
    // Snap startT onto this pixel's jitter grid so the samples we take match the phase
    // the monolithic path uses in its post-DDA marching. Adjacent pixels stay coherent.
    float baseT = nearT + rayOffset * stepLength;
    float t = baseT + max(0.0, ceil((startT - baseT) / stepLength)) * stepLength;
    float pendingDensity = 0.0;
    bool havePending = false;
    [loop] for (uint i = 0; i < steps; ++i)
    {
        if (t > farT) break;
        float3 p = origin + ray * t;
        t += stepLength;
        float density;
        if (havePending) density = pendingDensity;
        else density = DenseDensityAt(p);
        {
            float3 pNext = origin + ray * t;
            pendingDensity = DenseDensityAt(pNext);
            havePending = true;
        }
        if (density > 0.001)
        {
            float attenuation = exp(-density * VolumeParameters.y * stepLength);
            float contribution = viewT * (1.0 - attenuation);
            bool visible = (contribution * VolumeParameters.z) > LightColorParameters.w;
            [branch] if (visible)
            {
                float lightT = useCache ? CachedLight(p, density) : NaiveLight(p);
                float powder = 1.0 - exp(-density * CloudParameters.x * 1.8);
                float silverLining = pow(saturate(lightT), 0.38) * (0.35 + powder * 0.9);
                float heightAmbient = lerp(0.45, 1.25,
                    saturate((p.y - VolumeMin.y) / (VolumeMax.y - VolumeMin.y)));
                float directLight = lightT * LightingParameters.y
                    * (1.15 + silverLining) * phase * 12.56637;
                directLight += LightingParameters.z * pow(saturate(lightT), 0.28)
                    * (0.25 + powder * 0.75);
                float3 warmSun = LightColorParameters.rgb;
                float3 coolAmbient = AmbientColorParameters.rgb * CloudParameters.y * heightAmbient;
                float3 lightColor = warmSun * directLight + coolAmbient;
                float normalizedDensity = saturate(density / max(VolumeParameters.x, 1e-4));
                float surfaceBand = 1.0 - smoothstep(0.18, 0.82, normalizedDensity);
                if (enableSurfaceNormals && AmbientColorParameters.w > 0.5 && surfaceBand > 0.01)
                {
                    float3 shadingNormal = CacheGradientNormal(p);
                    float normalLighting = 0.24 + 1.02
                        * saturate(dot(shadingNormal, normalize(LightDirectionGrid.xyz)));
                    lightColor *= lerp(1.0, normalLighting, surfaceBand * 0.86);
                }
                scattered += contribution * VolumeParameters.z * lightColor;
            }
            viewT *= attenuation; if (viewT < 0.005) break;
        }
    }
    color = scattered + viewT * color;
    color *= CloudParameters.z;
    color = max((color - 0.18) * CloudParameters.w + 0.18, 0.0);
    color = pow(saturate(color), 1.0 / max(DensityTransferParameters.w, 0.1));
    return float4(color, 1);
}

[numthreads(256,1,1)]
void ResolveColumnsCS(
    uint3 groupId : SV_GroupID,
    uint groupIndex : SV_GroupIndex)
{
    uint n = GridResolution();
    uint waveSize = WaveGetLaneCount();
    uint waveLane = WaveGetLaneIndex();
    uint waveIndex = groupIndex / waveSize;
    uint waveCount = 256 / waveSize;
    uint startColumn = (uint)CacheBuildRange.x;
    uint endColumn = (uint)CacheBuildRange.y;
    uint columnIndex = startColumn + groupId.x * waveCount + waveIndex;
    bool validColumn = columnIndex < n * n && columnIndex < endColumn;
    uint2 columnCoordinates = uint2(columnIndex % n, columnIndex / n);

    float accumulatedDepth = 0.0;
    for (uint chunk = 0; chunk < n; chunk += waveSize)
    {
        uint positionInColumn = chunk + waveLane;
        bool activeLane = validColumn && positionInColumn < n;
        uint z = n - 1 - min(positionInColumn, n - 1);
        float segmentDepth = 0.0;
        float density = 0.0;
        if (activeLane)
        {
            float3 right, up, lightDirection, cacheMinimum, cacheMaximum;
            LightCacheFrame(right, up, lightDirection, cacheMinimum, cacheMaximum);
            float cellLength = (cacheMaximum.z - cacheMinimum.z) / n;
            float segmentLength = z == n - 1 ? cellLength * 0.5 : cellLength;
            float3 lightCellPosition = lerp(cacheMinimum, cacheMaximum,
                (float3(columnCoordinates, z) + 0.5) / n);
            float3 cellPosition = LightCacheToWorld(
                lightCellPosition, right, up, lightDirection);
            const float gaussOffset = 0.2886751346;
            float density0 = DenseDensityAt(
                cellPosition + lightDirection * segmentLength * (0.5 - gaussOffset));
            float density1 = DenseDensityAt(
                cellPosition + lightDirection * segmentLength * (0.5 + gaussOffset));
            density = (density0 + density1) * 0.5;
            segmentDepth = density * VolumeParameters.y
                * DensityTransferParameters.z * segmentLength;
            if (density > 0.001)
                InterlockedAdd(StatsWrite[0], 1);
        }

        float prefixDepth = accumulatedDepth + WavePrefixSum(segmentDepth) + segmentDepth;
        accumulatedDepth += WaveActiveSum(segmentDepth);
        if (activeLane)
            CachedDepthWrite[uint3(columnCoordinates, z)] = prefixDepth;
    }
}

[numthreads(8,8,1)]
void ClassifyTilesCS(uint3 id : SV_DispatchThreadID)
{
    float2 dimensions = ResolutionSteps.xy;
    uint2 tileCount = (uint2(dimensions) + uint2(7, 7)) / uint2(8, 8);
    if (any(id.xy >= tileCount)) return;
    float3 origin = CameraPositionTime.xyz;
    float3 forward = normalize(CameraTargetMode.xyz - origin);
    float3 right = normalize(cross(forward, float3(0, 1, 0)));
    float3 up = cross(right, forward);
    float aspect = dimensions.x / dimensions.y;

    // Cast rays at 5 points across the tile (4 corners + centre). We sample the
    // dilated max-pooled mip 3 of RemappedVolume, which by construction cannot
    // miss cloud smaller than the sample spacing - every mip3 texel already
    // aggregates an 8x8x8 block of source voxels (with a 1-voxel dilation).
    const float2 offsets[5] = {
        float2(0.0, 0.0), float2(8.0, 0.0),
        float2(0.0, 8.0), float2(8.0, 8.0),
        float2(4.0, 4.0)
    };
    // Threshold for treating a mip3 sample as "cloud". Mip3 stores max-pooled
    // density with a 1-voxel dilation, so its outer halo has small non-zero
    // values that don't correspond to visible cloud in the render. Use a
    // meaningfully-dense threshold to skip that halo.
    const float DenseThreshold = 0.02;
    float minDensityInCloud = 1.0;
    float maxDensity = 0.0;
    uint denseSamples = 0;
    uint gapSamples = 0;
    uint raysThatHitBox = 0;
    uint raysThatMissedCloud = 0;
    for (uint r = 0; r < 5; ++r)
    {
        float2 pixel = float2(id.xy * 8) + offsets[r];
        float2 ndc = ((pixel + 0.5) / dimensions) * 2.0 - 1.0;
        ndc.x *= aspect;
        float3 ray = normalize(forward + ndc.x * right * 0.58 - ndc.y * up * 0.58);
        float nearT, farT;
        if (!IntersectBox(origin, ray, nearT, farT)) continue;
        ++raysThatHitBox;
        const uint SampleCount = 48;
        // Single-pass version: no per-ray scratch array. Previously we stored 48
        // samples per ray to re-scan in [firstDense..lastDense]; that array +
        // [unroll] on both loops pushed ClassifyTilesCS to 77 live registers.
        // Observations that let us drop it:
        //   * All `d > DenseThreshold` samples in the ray are within
        //     [firstDense..lastDense], so `denseSamples` = total dense count.
        //   * `gapSamples` = (lastDense - firstDense + 1) - denseSamples.
        //   * For minDensityInCloud we hold a running "tail min since last dense
        //     sample"; whenever the next dense sample arrives we know the tail
        //     is fully inside the range, so we commit it. Samples after the true
        //     lastDense stay in the tail and are correctly discarded.
        int firstDense = -1;
        int lastDense = -1;
        uint rayDense = 0;
        float rayMinInCloud = 1.0;
        float tailMin = 1.0;
        for (uint s = 0; s < SampleCount; ++s)
        {
            float t = lerp(nearT, farT, (float(s) + 0.5) / SampleCount);
            float3 p = origin + ray * t;
            float3 uvw = VolumeCoordinates(p);
            // Border voxels of RemappedVolume mip 3 are zero (max-pool of zero-padded
            // mip 0), so LinearClamp gives 0 for out-of-box samples without a check.
            float d = RemappedVolume.SampleLevel(LinearClamp, uvw, 3);
            maxDensity = max(maxDensity, d);
            if (d > DenseThreshold)
            {
                if (firstDense < 0) firstDense = int(s);
                // Commit any pending tail (samples below threshold since the last
                // dense hit) into the confirmed in-range min.
                rayMinInCloud = min(rayMinInCloud, min(tailMin, d));
                tailMin = 1.0;
                ++rayDense;
                lastDense = int(s);
            }
            else if (firstDense >= 0)
            {
                tailMin = min(tailMin, d);
            }
        }
        if (firstDense < 0)
        {
            // Ray crossed the volume box without hitting cloud - this tile
            // straddles the silhouette so it's not safely interior.
            ++raysThatMissedCloud;
            continue;
        }
        minDensityInCloud = min(minDensityInCloud, rayMinInCloud);
        denseSamples += rayDense;
        gapSamples += uint(lastDense - firstDense + 1) - rayDense;
    }
    uint category = 0;
    if (maxDensity > DenseThreshold)
    {
        // Interior requires every test ray that hits the volume box to also hit
        // cloud (no silhouette leaks) AND every hit ray to see one continuous
        // dense stretch (at most 1 gap sample of tolerance across all rays).
        bool allRaysSawCloud = (raysThatHitBox > 0) && (raysThatMissedCloud == 0);
        bool continuousDense = (gapSamples <= 1) && (minDensityInCloud > DenseThreshold);
        category = (allRaysSawCloud && continuousDense) ? 1 : 2;
    }
    TileClassificationWrite[id.y * tileCount.x + id.x] = category;
}

// Debug palette for tile category visualisation.
float3 TileCategoryColor(uint category)
{
    if (category == 0) return float3(0.15, 0.20, 0.40); // sky - blue
    if (category == 1) return float3(0.15, 0.55, 0.20); // interior - green
    return float3(0.75, 0.30, 0.15);                     // surface - orange
}

// Read the effective category for a tile, dilated by 1 tile in each direction.
// - Sky is only reported when the tile AND all 8 neighbours are sky (dilate
//   surface/interior outward: any adjacent surface tile means the current one
//   may see cloud edges).
// - Interior is only reported when the tile AND all 8 neighbours are interior
//   (erode interior: skipping surface-band normals is only safe when we know
//   no pixel in the tile sees a cloud edge, so any adjacent surface tile
//   forces us to shade normals).
// Loads are uniform across the group, so the texture cache dedupes to 9 fetches
// per workgroup.
// Read the effective category for a tile, dilated by 1 tile in each direction.
// This hides classifier undersampling errors at tile boundaries: a tile is
// promoted from sky to at least surface if any of its 3x3 neighbours contains
// volume. Loads are uniform across the group, so the texture cache dedupes them
// to 9 fetches total for the whole 8x8 workgroup.
uint DilatedTileCategory(uint2 tile, uint tileCountX, uint tileCountY)
{
    uint best = 0;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            int2 nt = int2(tile) + int2(dx, dy);
            if (nt.x < 0 || nt.y < 0) continue;
            if (uint(nt.x) >= tileCountX || uint(nt.y) >= tileCountY) continue;
            uint c = TileClassificationRead[uint(nt.y) * tileCountX + uint(nt.x)];
            if (c == 2) return 2;
            best = max(best, c);
        }
    }
    return best;
}

[numthreads(8,8,1)]
void NaiveRenderCS(uint3 id:SV_DispatchThreadID, uint3 groupId : SV_GroupID)
{
    uint2 dims = (uint2)ResolutionSteps.xy;
    if (any(id.xy >= dims)) return;
    // Temporal reuse early-out. When enabled, TemporalReprojectCS already wrote a
    // reprojected colour into Output for pixels marked here. Cleared to 0 by the CPU
    // when temporal reuse is disabled or globally invalidated so this is a no-op.
    // One R8_UINT load per pixel — negligible next to the trace cost we skip.
    if (ReuseValidMask[id.xy] != 0u)
    {
#ifdef ENABLE_DENSITY_COUNTING
        SampleCountWrite[id.xy] = 0;
#endif
        return;
    }
    bool classifyEnabled = DispatchParameters.x != 0;
    if (classifyEnabled)
    {
        uint tileCountX = (dims.x + 7) / 8;
        uint tileCountY = (dims.y + 7) / 8;
        uint category = DilatedTileCategory(groupId.xy, tileCountX, tileCountY);
        if (category == 0)
        {
            float2 ndc = ((float2(id.xy) + 0.5) / float2(dims)) * 2.0 - 1.0;
            ndc.x *= float(dims.x) / float(dims.y);
            float3 origin = CameraPositionTime.xyz;
            float3 forward = normalize(CameraTargetMode.xyz - origin);
            float3 right = normalize(cross(forward, float3(0, 1, 0)));
            float3 up = cross(right, forward);
            float3 ray = normalize(forward + ndc.x * right * 0.58 - ndc.y * up * 0.58);
            Output[id.xy] = float4(Background(ray), 1);
#ifdef ENABLE_DENSITY_COUNTING
            SampleCountWrite[id.xy] = 0;
#endif
            return;
        }
    }
    Output[id.xy] = TraceVolume(id.xy, false);
#ifdef ENABLE_DENSITY_COUNTING
    SampleCountWrite[id.xy] = (min(g_densitySampleCount, 4095u) & 0xFFFu) | ((min(g_cachedDepthSampleCount, 1023u) & 0x3FFu) << 12) | ((min(g_dfSampleCount, 1023u) & 0x3FFu) << 22);
#endif
}

[numthreads(8,8,1)]
void CachedRenderCS(uint3 id:SV_DispatchThreadID, uint3 groupId : SV_GroupID)
{
    uint2 dims = (uint2)ResolutionSteps.xy;
    if (any(id.xy >= dims)) return;
    if (ReuseValidMask[id.xy] != 0u)
    {
#ifdef ENABLE_DENSITY_COUNTING
        SampleCountWrite[id.xy] = 0;
#endif
        return;
    }
    bool classifyEnabled = DispatchParameters.x != 0;
    if (classifyEnabled)
    {
        uint tileCountX = (dims.x + 7) / 8;
        uint tileCountY = (dims.y + 7) / 8;
        uint category = DilatedTileCategory(groupId.xy, tileCountX, tileCountY);
        if (category == 0)
        {
            float2 ndc = ((float2(id.xy) + 0.5) / float2(dims)) * 2.0 - 1.0;
            ndc.x *= float(dims.x) / float(dims.y);
            float3 origin = CameraPositionTime.xyz;
            float3 forward = normalize(CameraTargetMode.xyz - origin);
            float3 right = normalize(cross(forward, float3(0, 1, 0)));
            float3 up = cross(right, forward);
            float3 ray = normalize(forward + ndc.x * right * 0.58 - ndc.y * up * 0.58);
            Output[id.xy] = float4(Background(ray), 1);
#ifdef ENABLE_DENSITY_COUNTING
            SampleCountWrite[id.xy] = 0;
#endif
            return;
        }
    }
    Output[id.xy] = TraceVolume(id.xy, true);
#ifdef ENABLE_DENSITY_COUNTING
    SampleCountWrite[id.xy] = (min(g_densitySampleCount, 4095u) & 0xFFFu) | ((min(g_cachedDepthSampleCount, 1023u) & 0x3FFu) << 12) | ((min(g_dfSampleCount, 1023u) & 0x3FFu) << 22);
#endif
}

// Pass A of the interval-finding + shading split. Thread per pixel. Walks the distance
// field with 3D-DDA (voxel-face-aligned so adjacent pixels stay coherent) until it
// enters the first dense-adjacent DF cell. Writes `firstHitT` to HitIntervalWrite so
// pass B (SplitCachedRenderCS) can march linearly from there. No density fetches, no
// lighting, no jitter — cheap and register-light, aiming for high warp occupancy.
[numthreads(8,8,1)]
void FindHitIntervalCS(uint3 id : SV_DispatchThreadID)
{
    uint2 dims = (uint2)ResolutionSteps.xy;
    // Option 2 fold-in: when tile-variant dispatch is enabled (DispatchParameters.y),
    // thread (0,0) of each 8x8 group emits a non-sky tile entry so that ExecuteIndirect
    // can consume the list next. Uses the DILATED classification (matching what the
    // shading pass reads); ClassifyTilesCS has already completed so the read is
    // race-free. Runs BEFORE any ray-miss early-outs below so that tiles whose (0,0)
    // pixel misses the box are still listed for the other pixels in the tile.
    bool variantEnabled = DispatchParameters.y != 0;
    if (variantEnabled && all((id.xy & uint2(7, 7)) == uint2(0, 0)))
    {
        uint tileCountX = (dims.x + 7) / 8;
        uint tileCountY = (dims.y + 7) / 8;
        uint2 tile = id.xy / uint2(8, 8);
        if (all(tile < uint2(tileCountX, tileCountY)))
        {
            uint category = DilatedTileCategory(tile, tileCountX, tileCountY);
            if (category != 0)
            {
                uint slot;
                InterlockedAdd(IndirectDispatchArgsWrite[0], 1, slot);
                TileListWrite[slot] = tile.x | (tile.y << 16);
            }
        }
    }
    if (any(id.xy >= dims)) return;
    uint pixelIndex = id.y * dims.x + id.x;
    float2 dimensions = float2(dims);
    float2 ndc = ((float2(id.xy) + 0.5) / dimensions) * 2.0 - 1.0;
    ndc.x *= dimensions.x / dimensions.y;
    float3 origin = CameraPositionTime.xyz;
    float3 forward = normalize(CameraTargetMode.xyz - origin);
    float3 right = normalize(cross(forward, float3(0, 1, 0)));
    float3 up = cross(right, forward);
    float3 ray = normalize(forward + ndc.x * right * 0.58 - ndc.y * up * 0.58);
    float nearT, farT;
    if (!IntersectBox(origin, ray, nearT, farT))
    {
        HitIntervalWrite[pixelIndex] = float2(1e30, 1e30);
        return;
    }
    const float dfDim = 128.0;
    const float invDfDim = 1.0 / 128.0;
    float3 boxExtent = VolumeMax.xyz - VolumeMin.xyz;
    float3 raySign = sign(ray);
    float3 rayNonZero = ray + raySign * 1e-30;
    float t = nearT;
    // Walk the whole ray tracking first/last non-empty DF cell hits. `firstHit` locks in
    // on the first transition into a non-empty cell; `lastHit` keeps updating on every
    // subsequent non-empty cell. Once both are known the pass B linear march only covers
    // the tight interval [firstHit, lastHit], which is the biggest reduction lever on
    // grazing rays that mostly clip the cloud.
    float firstHit = 1e30;
    float lastHit = nearT;
    // Walk the ray tracking first and last non-empty DF cells. No streak-based early-out
    // because real clouds aren't convex — a ray can pass through cloudy → gap → cloudy,
    // and terminating on the first gap punches visible holes in the far-side cloud.
    // Instead we cap total iterations; if we run out of budget on a very long ray we
    // conservatively extend lastHit to farT.
    [loop] for (uint i = 0; i < 256; ++i)
    {
        if (t >= farT) break;
        float3 uvw = saturate((origin + ray * t - VolumeMin.xyz) / boxExtent);
        uint3 cell = uint3(clamp(uvw * dfDim, 0.0, dfDim - 1.0));
        float dfHere = SampleDistanceFieldCell(int3(cell));
        float3 cj = floor(clamp(uvw * dfDim, 0.0, dfDim - 1.0));
        float3 cellExitUvw = select(raySign > 0.0, cj + 1.0, cj) * invDfDim;
        float3 tToFace = (cellExitUvw - uvw) * boxExtent / rayNonZero;
        tToFace = select(tToFace > 0.0, tToFace, 1e9);
        float tCellExit = min(tToFace.x, min(tToFace.y, tToFace.z));
        if (dfHere <= 0.5 * invDfDim)
        {
            if (firstHit >= 1e29) firstHit = t;
            lastHit = t + tCellExit;
        }
        t += tCellExit + 1e-4;
    }
    if (t < farT && firstHit < 1e29) lastHit = farT;
    HitIntervalWrite[pixelIndex] = float2(firstHit, lastHit);
}

// Pass B of the split pipeline. Reads firstHitT and delegates to the slim
// SplitTraceVolume (no DDA/DF state), which lets the compiler give it a tighter
// register footprint than the monolithic CachedRenderCS.
[numthreads(8,8,1)]
void SplitCachedRenderCS(uint3 id : SV_DispatchThreadID, uint3 groupId : SV_GroupID)
{
    uint2 dims = (uint2)ResolutionSteps.xy;
    if (any(id.xy >= dims)) return;
    bool classifyEnabled = DispatchParameters.x != 0;
    if (classifyEnabled)
    {
        uint tileCountX = (dims.x + 7) / 8;
        uint tileCountY = (dims.y + 7) / 8;
        uint category = DilatedTileCategory(groupId.xy, tileCountX, tileCountY);
        if (category == 0)
        {
            float2 ndc = ((float2(id.xy) + 0.5) / float2(dims)) * 2.0 - 1.0;
            ndc.x *= float(dims.x) / float(dims.y);
            float3 origin = CameraPositionTime.xyz;
            float3 forward = normalize(CameraTargetMode.xyz - origin);
            float3 right = normalize(cross(forward, float3(0, 1, 0)));
            float3 up = cross(right, forward);
            float3 ray = normalize(forward + ndc.x * right * 0.58 - ndc.y * up * 0.58);
            Output[id.xy] = float4(Background(ray), 1);
            return;
        }
    }
    float2 hitInterval = HitIntervalWrite[id.y * dims.x + id.x];
    Output[id.xy] = SplitTraceVolume(id.xy, hitInterval.x, hitInterval.y, true, true);
}

// ------------------------------------------------------------------------------------
// Option 2: tile-variant dispatch via ExecuteIndirect.
//
// The tile classifier tags each 8x8 pixel tile as sky (0), interior (1), or surface
// (2). We now use a single non-sky variant with full lighting (the interior-vs-surface
// split turned out unsound: interior tiles routinely contain rays that pass through
// surface-band density transitions internally, so skipping normal shading there caused
// visible tile-boundary artefacts).
//
// Passes:
//   - sky:     SplitCachedSkyCS (per-pixel dispatch, early-exits on non-sky; no
//              marching, cheap despite the launch size).
//   - non-sky: SplitCachedRenderIndirectCS (indirect dispatch, tile-list indexed,
//              full lighting cascade — matches monolithic SplitCachedRenderCS output).
//
// Args buffer layout (3 uints = 1 x D3D12_DISPATCH_ARGUMENTS):
//   [0..2] non-sky: X (populated by atomic add in FindHitIntervalCS), Y=1, Z=1
// Tile list stores packed tile coords: x | (y << 16).
//
// Note: BuildTileListsCS was folded into FindHitIntervalCS — the per-tile append
// happens on groupThreadId (0,0) after the ray walk, saving a whole dispatch.
// (RWStructuredBuffers TileListWrite/IndirectDispatchArgsWrite are declared near
//  HitIntervalWrite above so FindHitIntervalCS can reach them.)
// ------------------------------------------------------------------------------------

// One thread. Resets dispatch X counter and initialises Y=Z=1.
[numthreads(1,1,1)]
void ClearTileDispatchCS(uint3 id : SV_DispatchThreadID)
{
    IndirectDispatchArgsWrite[0] = 0;
    IndirectDispatchArgsWrite[1] = 1;
    IndirectDispatchArgsWrite[2] = 1;
}

// Non-sky pass B: full lighting cascade. Group id indexes the tile list;
// group thread id addresses the pixel within the 8x8 tile.
[numthreads(8,8,1)]
void SplitCachedRenderIndirectCS(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
    uint packed = TileListWrite[groupId.x];
    uint2 tile = uint2(packed & 0xFFFF, packed >> 16);
    uint2 px = tile * 8 + groupThreadId.xy;
    uint2 dims = (uint2)ResolutionSteps.xy;
    if (any(px >= dims)) return;
    float2 hitInterval = HitIntervalWrite[px.y * dims.x + px.x];
    Output[px] = SplitTraceVolume(px, hitInterval.x, hitInterval.y, true, true);
}

// Sky writer for variant dispatch. Runs at full pixel resolution but early-exits on
// non-sky tiles — the marching-free background compute makes this dispatch cheap
// even at 1920x1080.
[numthreads(8,8,1)]
void SplitCachedSkyCS(uint3 id : SV_DispatchThreadID, uint3 groupId : SV_GroupID)
{
    uint2 dims = (uint2)ResolutionSteps.xy;
    if (any(id.xy >= dims)) return;
    uint tileCountX = (dims.x + 7) / 8;
    uint tileCountY = (dims.y + 7) / 8;
    uint category = DilatedTileCategory(groupId.xy, tileCountX, tileCountY);
    if (category != 0) return;
    float2 ndc = ((float2(id.xy) + 0.5) / float2(dims)) * 2.0 - 1.0;
    ndc.x *= float(dims.x) / float(dims.y);
    float3 origin = CameraPositionTime.xyz;
    float3 forward = normalize(CameraTargetMode.xyz - origin);
    float3 right = normalize(cross(forward, float3(0, 1, 0)));
    float3 up = cross(right, forward);
    float3 ray = normalize(forward + ndc.x * right * 0.58 - ndc.y * up * 0.58);
    Output[id.xy] = float4(Background(ray), 1);
}

// Debug pass: blends tile category colour on top of the raymarched output so we
// can inspect classification while still paying the full trace cost.
// Read per-pixel DenseDensityAt sample count from SampleCountWrite (populated by the
// *_CountCS variants) and overlay a false-color heatmap onto Output. Scale is controlled
// via DispatchParameters.z (max count for full-red). Sky pixels (count==0) are left
// alone so the cloud silhouette against the sky stays visible.
float3 HeatmapColour(float t)
{
    // Turbo-ish colour ramp: dark blue → cyan → green → yellow → red.
    // Cheap piecewise gradient (no branches, DXC folds to a couple of lerps).
    float3 c;
    if (t < 0.25)      c = lerp(float3(0.02,0.04,0.30), float3(0.00,0.60,0.90),  t / 0.25);
    else if (t < 0.50) c = lerp(float3(0.00,0.60,0.90), float3(0.00,0.85,0.10), (t-0.25)/0.25);
    else if (t < 0.75) c = lerp(float3(0.00,0.85,0.10), float3(0.98,0.90,0.10), (t-0.50)/0.25);
    else               c = lerp(float3(0.98,0.90,0.10), float3(1.00,0.10,0.05), (t-0.75)/0.25);
    return c;
}

// Temporal reprojection: for each pixel, take the current camera's primary ray, find
// the entry point into the volume bounding box (nearT), then project that world point
// through the PREVIOUS frame's camera to find the pixel it lived at last frame. If
// that prev-pixel is in bounds, copy last frame's colour into Output and mark the
// pixel as reused. Main render entry points early-out on the mask.
//
// Ghost-free by design: this is a full-result reuse of a still-valid prior sample;
// it is NOT a partial-sample accumulation. Global invalidation is handled CPU-side by
// zero-clearing the mask on any settings/lighting change so this pass never fires
// unless all we did was move the camera.
//
// Sky pixels (no box intersection) are left alone — the trace path's Background()
// path is cheap enough that we don't gain by reprojecting sky.
[numthreads(8,8,1)]
void TemporalReprojectCS(uint3 id : SV_DispatchThreadID)
{
    uint2 dims = (uint2)ResolutionSteps.xy;
    if (any(id.xy >= dims)) return;

    // Reconstruct the current-frame primary ray identically to the render entry
    // points (must match exactly or reprojection is off by a pixel).
    float2 dimsF = float2(dims);
    float2 ndc = ((float2(id.xy) + 0.5) / dimsF) * 2.0 - 1.0;
    ndc.x *= dimsF.x / dimsF.y;
    float3 origin = CameraPositionTime.xyz;
    float3 forward = normalize(CameraTargetMode.xyz - origin);
    float3 right = normalize(cross(forward, float3(0, 1, 0)));
    float3 up = cross(right, forward);
    float3 ray = normalize(forward + ndc.x * right * 0.58 - ndc.y * up * 0.58);

    // Only bother reprojecting rays that hit the volume box. Sky pixels are handled
    // by the trace shader's cheap Background() path.
    float nearT, farT;
    if (!IntersectBox(origin, ray, nearT, farT))
    {
        ReuseValidMask[id.xy] = 0u;
        return;
    }

    // Reference world point for this pixel: MIDPOINT of the ray's box intersection,
    // not the entry face. The entry face is a flat rectangle in world space, so
    // anchoring reprojection there smears the cloud onto that plane whenever the
    // camera rotates (the parallax between "colour on box front face" and "colour
    // integrated through the volume" is huge for a volumetric render).
    //
    // The midpoint is a cheap proxy for the volume's representative scattering depth
    // — good enough for most clouds where the bulk of scattering lives near the
    // volume centre. Not perfect, but the parallax error is small enough that the
    // reprojection is a net-positive perf win with only a mild edge-fringe artefact
    // on fast camera swings (which invalidate globally on any settings change anyway).
    float3 worldRef = origin + ray * ((nearT + farT) * 0.5);

    // Project through the previous frame's camera: mirror the entry-point ray
    // reconstruction backwards.
    float3 prevOrigin  = PrevCameraPositionTime.xyz;
    float3 prevForward = normalize(PrevCameraTargetMode.xyz - prevOrigin);
    float3 prevRight   = normalize(cross(prevForward, float3(0, 1, 0)));
    float3 prevUp      = cross(prevRight, prevForward);
    float3 toWorld     = worldRef - prevOrigin;
    // Depth along prev forward. Behind the camera → invalid.
    float prevZ = dot(toWorld, prevForward);
    if (prevZ <= 1e-4)
    {
        ReuseValidMask[id.xy] = 0u;
        return;
    }
    // Inverse of ray = forward + ndc.x*right*0.58 - ndc.y*up*0.58 with the depth
    // normalisation: project onto the right/up plane at forward-depth.
    float invScale = 1.0 / (prevZ * 0.58);
    float prevNdcX = dot(toWorld, prevRight) * invScale;
    float prevNdcY = -dot(toWorld, prevUp)   * invScale;
    prevNdcX *= dimsF.y / dimsF.x;
    // Convert NDC back to pixel space (matching the +0.5 pixel-centre convention).
    float2 prevUv = float2((prevNdcX * 0.5 + 0.5), (prevNdcY * 0.5 + 0.5));
    if (any(prevUv < 0.0) || any(prevUv > 1.0))
    {
        ReuseValidMask[id.xy] = 0u;
        return;
    }
    // Sample history with LinearClamp — small camera pans move sub-pixel so a
    // bilinear reprojection is smoother than nearest-neighbour without ghosting
    // (this is still a fully-resolved history sample, not an accumulation).
    float4 hist = HistoryColorRead.SampleLevel(LinearClamp, prevUv, 0);
    Output[id.xy] = hist;
    ReuseValidMask[id.xy] = 1u;
}

// Debug overlay: colours pixels by temporal-reuse validity. Runs after the main
// render. Green = pixel was reprojected from history (skipped the trace). Red =
// pixel needed a full march. Sky pixels (mask == 0 but never marched) are left alone
// so the cloud silhouette against the background stays visible.
[numthreads(8,8,1)]
void TemporalReuseDebugCS(uint3 id : SV_DispatchThreadID)
{
    uint2 dims = (uint2)ResolutionSteps.xy;
    if (any(id.xy >= dims)) return;
    uint reused = ReuseValidMask[id.xy];
    float4 existing = Output[id.xy];
    float3 tint = reused != 0u ? float3(0.15, 0.9, 0.20)  // green: reused
                               : float3(0.95, 0.20, 0.15); // red:   marched
    Output[id.xy] = float4(lerp(existing.rgb, tint, 0.55), existing.a);
}

[numthreads(8,8,1)]
void HeatmapDisplayCS(uint3 id : SV_DispatchThreadID)
{
    uint2 dims = (uint2)ResolutionSteps.xy;
    if (any(id.xy >= dims)) return;
    uint packed = SampleCountWrite[id.xy];
    // Bit layout matches the entry-point writeout:
    //   bits  0..11 : dense (0..4095)
    //   bits 12..21 : depth (0..1023)
    //   bits 22..31 : df    (0..1023)
    uint dense = packed & 0xFFFu;
    uint depth = (packed >> 12) & 0x3FFu;
    uint df    = (packed >> 22) & 0x3FFu;
    // DispatchParameters.y: 0 = DenseVolume, 1 = CachedDepth, 2 = Sum (all three),
    // 3 = DistanceField.
    uint mode = DispatchParameters.y;
    uint count;
    if (mode == 1u) count = depth;
    else if (mode == 2u) count = dense + depth + df;
    else if (mode == 3u) count = df;
    else count = dense;
    float maxCount = max((float)DispatchParameters.z, 1.0);
    float t = saturate((float)count / maxCount);
    float3 heat = HeatmapColour(t);
    float4 existing = Output[id.xy];
    float3 blended = count == 0 ? existing.rgb : lerp(existing.rgb, heat, 0.85);
    Output[id.xy] = float4(blended, existing.a);
}

[numthreads(8,8,1)]
void DebugTileOverlayCS(uint3 id : SV_DispatchThreadID, uint3 groupId : SV_GroupID)
{
    uint2 dims = (uint2)ResolutionSteps.xy;
    if (any(id.xy >= dims)) return;
    uint tileCountX = (dims.x + 7) / 8;
    uint tileCountY = (dims.y + 7) / 8;
    uint category = DilatedTileCategory(groupId.xy, tileCountX, tileCountY);
    float3 tint = TileCategoryColor(category);
    float4 existing = Output[id.xy];
    float3 blended = lerp(existing.rgb, tint, 0.5);
    Output[id.xy] = float4(blended, existing.a);
}

[numthreads(4,4,4)]
void BakeRemappedVolumeCS(uint3 id : SV_DispatchThreadID)
{
    uint3 dims;
    RemappedVolumeWrite.GetDimensions(dims.x, dims.y, dims.z);
    if (any(id >= dims)) return;
    float3 uvw = (float3(id) + 0.5) / float3(dims);
    float raw = DenseVolume.SampleLevel(LinearClamp, uvw, 0);
    RemappedVolumeWrite[id] = RemapCurve(raw);
}

[numthreads(4,4,4)]
void BakeOccupancyCS(uint3 id : SV_DispatchThreadID)
{
    uint3 mip3Dims;
    RemappedVolumeMip3Write.GetDimensions(mip3Dims.x, mip3Dims.y, mip3Dims.z);
    if (any(id >= mip3Dims)) return;
    uint3 mip0Dims;
    RemappedVolumeWrite.GetDimensions(mip0Dims.x, mip0Dims.y, mip0Dims.z);
    uint3 blockSize = uint3(mip0Dims / mip3Dims);
    int3 base = int3(id * blockSize);
    // Include a one-voxel dilation into each mip3 neighbor to keep skip decisions conservative.
    int3 lo = base - int3(1, 1, 1);
    int3 hi = base + int3(blockSize) + int3(1, 1, 1);
    lo = max(lo, int3(0, 0, 0));
    hi = min(hi, int3(mip0Dims) - int3(1, 1, 1));
    float maxDensity = 0;
    for (int z = lo.z; z <= hi.z; ++z)
    for (int y = lo.y; y <= hi.y; ++y)
    for (int x = lo.x; x <= hi.x; ++x)
    {
        float d = RemappedVolumeWrite[uint3(x, y, z)];
        maxDensity = max(maxDensity, d);
    }
    RemappedVolumeMip3Write[id] = maxDensity;
}

// Per-buffer normal reconstruction used by the two LUT bake entry points. Reads a
// single physical (A or B) at its own uvw and reconstructs the world-space gradient
// using that buffer's own frame axes. Called by BakeNormalLUT{A|B}CS, one per
// physical, so the LUT written by each bake is a snapshot of its own frame.
float3 NormalFromCacheA(float3 uvw, float delta)
{
    float3 rightA, upA, forwardA, cacheMinimum, cacheMaximum;
    LightCacheFrame(rightA, upA, forwardA, cacheMinimum, cacheMaximum);
    float dXp = SampleCachedDepthA(saturate(uvw + float3(delta, 0, 0)));
    float dXm = SampleCachedDepthA(saturate(uvw - float3(delta, 0, 0)));
    float dYp = SampleCachedDepthA(saturate(uvw + float3(0, delta, 0)));
    float dYm = SampleCachedDepthA(saturate(uvw - float3(0, delta, 0)));
    float dZp = SampleCachedDepthA(saturate(uvw + float3(0, 0, delta)));
    float dZm = SampleCachedDepthA(saturate(uvw - float3(0, 0, delta)));
    float3 g = (dXp - dXm) * rightA + (dYp - dYm) * upA + (dZp - dZm) * forwardA;
    float len = length(g);
    return len > 1e-5 ? -g / len : float3(0, 1, 0);
}

float3 NormalFromCacheB(float3 uvw, float delta)
{
    float dXp = SampleCachedDepthB(saturate(uvw + float3(delta, 0, 0)));
    float dXm = SampleCachedDepthB(saturate(uvw - float3(delta, 0, 0)));
    float dYp = SampleCachedDepthB(saturate(uvw + float3(0, delta, 0)));
    float dYm = SampleCachedDepthB(saturate(uvw - float3(0, delta, 0)));
    float dZp = SampleCachedDepthB(saturate(uvw + float3(0, 0, delta)));
    float dZm = SampleCachedDepthB(saturate(uvw - float3(0, 0, delta)));
    float3 g = (dXp - dXm) * LightCacheRightAxisB.xyz
             + (dYp - dYm) * LightCacheUpAxisB.xyz
             + (dZp - dZm) * LightCacheForwardAxisB.xyz;
    float len = length(g);
    return len > 1e-5 ? -g / len : float3(0, 1, 0);
}

[numthreads(4,4,4)]
void BakeNormalLUTCS(uint3 id : SV_DispatchThreadID)
{
    uint3 dims;
    NormalLUTWrite.GetDimensions(dims.x, dims.y, dims.z);
    if (any(id >= dims)) return;
    float3 uvw = (float3(id) + 0.5) / float3(dims);
    float delta = 2.0 / max((float)GridResolution(), 1.0);
    NormalLUTWrite[id] = float4(NormalFromCacheA(uvw, delta), 0.0);
}

[numthreads(4,4,4)]
void BakeNormalLUTBCS(uint3 id : SV_DispatchThreadID)
{
    uint3 dims;
    NormalLUTWriteB.GetDimensions(dims.x, dims.y, dims.z);
    if (any(id >= dims)) return;
    float3 uvw = (float3(id) + 0.5) / float3(dims);
    float delta = 2.0 / max((float)GridResolution(), 1.0);
    NormalLUTWriteB[id] = float4(NormalFromCacheB(uvw, delta), 0.0);
}

// ---------- Empty-space distance field ----------
// A 128^3 R8_UNORM field storing normalized distance-to-nearest-dense-voxel in [0,1].
// Value 0 = inside/adjacent to cloud; 1 = at or beyond the propagation horizon.
// Bake sequence: BakeDistanceFieldInitCS → N passes of BakeDistanceFieldPropagateCS
// (DispatchParameters.w bit 0 selects source/dest between A and B for ping-pong).

[numthreads(4,4,4)]
void BakeDistanceFieldInitCS(uint3 id : SV_DispatchThreadID)
{
    uint3 dfDims;
    DistanceFieldWriteA.GetDimensions(dfDims.x, dfDims.y, dfDims.z);
    if (any(id >= dfDims)) return;

    // Truly conservative init: enumerate every raw voxel in the DF cell's trilinear
    // tap footprint using Load(), and take the max. Since any trilinear sample the
    // render path takes inside this DF cell is a convex combination of these voxels,
    // max(voxels) is a strict upper bound on any trilinear tap value. This guarantees
    // DF-empty <=> every possible naive-path sample <= 0.001, so DF-driven skipping
    // integrates the same zero contribution as naive.
    uint3 rvDims;
    RemappedVolume.GetDimensions(rvDims.x, rvDims.y, rvDims.z);
    float3 uvBase = float3(id) / float3(dfDims);
    float3 uvSize = 1.0 / float3(dfDims);
    // Voxel range covered by [uvBase, uvBase+uvSize] plus 1 voxel border for trilinear tap footprint.
    int3 vStart = int3(floor(uvBase * float3(rvDims))) - 1;
    int3 vEnd = int3(ceil((uvBase + uvSize) * float3(rvDims))) + 1;
    vStart = max(vStart, int3(0, 0, 0));
    vEnd = min(vEnd, int3(rvDims) - 1);
    float density = 0.0;
    [loop] for (int z = vStart.z; z <= vEnd.z; ++z)
    [loop] for (int y = vStart.y; y <= vEnd.y; ++y)
    [loop] for (int x = vStart.x; x <= vEnd.x; ++x)
    {
        density = max(density, RemappedVolume.Load(int4(x, y, z, 0)));
    }
    // Match the render-time density comparison exactly. Naive marches with
    //   density = RemappedVolume.Sample * VolumeParameters.x   and skips density > 0.001.
    // So the effective raw threshold is 0.001 / gain. Using anything higher (e.g. plain
    // 0.001) under-marks cells and lets skipping wave past voxels naive would integrate.
    float rawThreshold = 0.001 / max(VolumeParameters.x, 1e-4);
    const float horizon = 16.0 / 128.0;
    // Encode with DF_ENCODE_SCALE so we use the full 0..1 range of R8_UNORM. See the
    // decode in SampleDistanceFieldCell. Zero maps to zero either way.
    DistanceFieldWriteA[id] = (density > rawThreshold ? 0.0 : horizon) * DF_ENCODE_SCALE;
}

[numthreads(4,4,4)]
void BakeDistanceFieldPropagateCS(uint3 id : SV_DispatchThreadID)
{
    uint3 dims;
    DistanceFieldWriteA.GetDimensions(dims.x, dims.y, dims.z);
    if (any(id >= dims)) return;
    uint direction = DispatchParameters.w & 1u;
    // cellStep is pre-scaled since the field is stored scaled (see DF_ENCODE_SCALE).
    // best/neighbor already come out pre-scaled from previous passes/init.
    float cellStep = DF_ENCODE_SCALE / float(dims.x);
    float best = direction == 0 ? DistanceFieldWriteA[id] : DistanceFieldWriteB[id];
    [unroll] for (int dz = -1; dz <= 1; ++dz)
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    [unroll] for (int dx = -1; dx <= 1; ++dx)
    {
        if (dx == 0 && dy == 0 && dz == 0) continue;
        int3 np = int3(id) + int3(dx, dy, dz);
        if (any(np < 0) || any(np >= int3(dims))) continue;
        float neighbor = direction == 0 ? DistanceFieldWriteA[uint3(np)]
                                        : DistanceFieldWriteB[uint3(np)];
        // Chebyshev metric (+1 cell per pass) — under-estimate of Euclidean, safe for skipping.
        best = min(best, neighbor + cellStep);
    }
    if (direction == 0) DistanceFieldWriteB[id] = best;
    else                DistanceFieldWriteA[id] = best;
}

// Debug visualisation: perform the same DDA traversal used at render-time and count
// how many empty cells each ray skipped through. Colour that count with a traffic-light
// ramp so pixels benefiting the most from DDA skipping read green, and pixels that
// couldn't skip (rays that hit dense material immediately) read red.
[numthreads(8,8,1)]
void DebugDistanceFieldCS(uint3 id : SV_DispatchThreadID)
{
    float2 dimensions = ResolutionSteps.xy;
    if (any(float2(id.xy) >= dimensions)) return;
    float2 ndc = ((float2(id.xy) + 0.5) / dimensions) * 2.0 - 1.0;
    ndc.x *= dimensions.x / dimensions.y;
    float3 origin = CameraPositionTime.xyz;
    float3 forward = normalize(CameraTargetMode.xyz - origin);
    float3 right = normalize(cross(forward, float3(0, 1, 0)));
    float3 up = cross(right, forward);
    float3 ray = normalize(forward + ndc.x * right * 0.58 - ndc.y * up * 0.58);
    float3 background = Output[id.xy].rgb;
    float nearT, farT;
    if (!IntersectBox(origin, ray, nearT, farT))
    {
        Output[id.xy] = float4(background, 1);
        return;
    }

    float3 boxExtent = VolumeMax.xyz - VolumeMin.xyz;
    float boxLongest = max(boxExtent.x, max(boxExtent.y, boxExtent.z));
    const float3 dfDimsF = float3(128.0, 128.0, 128.0);
    float3 raySign = sign(ray);
    float3 rayNonZero = ray + raySign * 1e-30;

    // March through the box using the same hybrid distance-jump + DDA as TraceVolume,
    // counting the total number of DF cells the ray covered via skipping (whether by
    // big distance jump or single DDA cell exit). Rays that skip many empty cells read
    // green; rays that hit dense material quickly read red.
    float cellsCovered = 0.0;
    uint hitDense = 0;
    float t = nearT + 1e-4;
    const uint maxIter = 128;
    [loop] for (uint i = 0; i < maxIter; ++i)
    {
        if (t > farT) break;
        float3 p = origin + ray * t;
        float3 uvw = saturate((p - VolumeMin.xyz) / boxExtent);
        uint3 cell = uint3(clamp(uvw * dfDimsF, float3(0, 0, 0), dfDimsF - 1.0));
        float dfHere = SampleDistanceFieldCell(int3(cell));
        const float cellNormalized = 1.0 / 128.0;
        if (dfHere <= 0.5 * cellNormalized)
        {
            hitDense = 1;
            break;
        }
        float cellsSafe = floor(dfHere * 128.0) - 1.0;
        cellsSafe = clamp(cellsSafe, 1.0, 96.0);
        [loop] for (uint j = 0; j < uint(cellsSafe); ++j)
        {
            if (t > farT) break;
            float3 pj = origin + ray * t;
            float3 uvwj = saturate((pj - VolumeMin.xyz) / boxExtent);
            uint3 cj = uint3(clamp(uvwj * dfDimsF, float3(0, 0, 0), dfDimsF - 1.0));
            float3 cellExitUvw = select(raySign > 0.0, float3(cj + 1), float3(cj)) / dfDimsF;
            float3 deltaUvw = cellExitUvw - uvwj;
            float3 tToFace = deltaUvw * boxExtent / rayNonZero;
            tToFace = select(tToFace > 0.0, tToFace, float3(1e9, 1e9, 1e9));
            float tCellExit = min(tToFace.x, min(tToFace.y, tToFace.z));
            t += tCellExit + 1e-4;
            cellsCovered += 1.0;
        }
    }

    // Normalise: 0..~64 cells covered maps 0..1 for the colour ramp.
    float score = saturate(cellsCovered / 64.0);
    float3 c0 = float3(1.00, 0.15, 0.15); // 0 cells or immediate hit: red
    float3 c1 = float3(1.00, 0.55, 0.10); // moderate skipping: orange
    float3 c2 = float3(0.25, 0.90, 0.30); // deep-sky big jumps: green
    float3 tint = score < 0.5
        ? lerp(c0, c1, saturate(score * 2.0))
        : lerp(c1, c2, saturate((score - 0.5) * 2.0));
    if (hitDense != 0 && cellsCovered < 0.5) tint = c0;

    float mask = 0.85;
    Output[id.xy] = float4(lerp(background, tint, mask), 1);
}
