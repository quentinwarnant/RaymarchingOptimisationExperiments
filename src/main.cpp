#include "Renderer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <mmsystem.h>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
    uint32_t g_pendingWidth = 1920;
    uint32_t g_pendingHeight = 1080;
    bool g_resizePending = false;

    LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam))
            return true;

        switch (message)
        {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED)
            {
                g_pendingWidth = LOWORD(lParam);
                g_pendingHeight = HIWORD(lParam);
                g_resizePending = true;
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU)
                return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProc(window, message, wParam, lParam);
    }

    void ShiftHistory(std::array<float, 180>& values, float next)
    {
        std::move(values.begin() + 1, values.end(), values.begin());
        values.back() = next;
    }

    struct ImageDifference
    {
        double meanAbsolute = 0.0;
        double rootMeanSquare = 0.0;
        uint8_t maximum = 0;
    };

    ImageDifference CompareImages(
        const std::vector<uint8_t>& reference, const std::vector<uint8_t>& candidate)
    {
        ImageDifference result;
        if (reference.size() != candidate.size() || reference.empty())
            return result;

        double absoluteSum = 0.0;
        double squaredSum = 0.0;
        uint64_t channelCount = 0;
        for (size_t index = 0; index + 3 < reference.size(); index += 4)
        {
            for (size_t channel = 0; channel < 3; ++channel)
            {
                const int difference = static_cast<int>(reference[index + channel]) -
                    static_cast<int>(candidate[index + channel]);
                const uint8_t absoluteDifference = static_cast<uint8_t>(std::abs(difference));
                absoluteSum += absoluteDifference;
                squaredSum += static_cast<double>(difference) * difference;
                result.maximum = std::max(result.maximum, absoluteDifference);
                ++channelCount;
            }
        }
        result.meanAbsolute = absoluteSum / channelCount;
        result.rootMeanSquare = std::sqrt(squaredSum / channelCount);
        return result;
    }

    bool WritePpm(const std::filesystem::path& path, uint32_t width, uint32_t height,
        const std::vector<uint8_t>& rgbaPixels)
    {
        std::ofstream output(path, std::ios::binary);
        if (!output)
            return false;
        output << "P6\n" << width << ' ' << height << "\n255\n";
        for (size_t index = 0; index + 3 < rgbaPixels.size(); index += 4)
            output.write(reinterpret_cast<const char*>(rgbaPixels.data() + index), 3);
        return output.good();
    }

    // Reads a P6 PPM into an RGBA byte buffer matching the layout produced by
    // CaptureOutput. Returns false if the file is missing or the header is
    // malformed / doesn't match `expectedWidth` x `expectedHeight`.
    bool ReadPpmAsRgba(const std::filesystem::path& path, uint32_t expectedWidth,
        uint32_t expectedHeight, std::vector<uint8_t>& rgbaPixels)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return false;
        std::string magic;
        uint32_t width = 0, height = 0, maxValue = 0;
        input >> magic >> width >> height >> maxValue;
        input.get();
        if (magic != "P6" || width != expectedWidth || height != expectedHeight || maxValue != 255)
            return false;
        rgbaPixels.assign(static_cast<size_t>(width) * height * 4, 0xff);
        std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
        input.read(reinterpret_cast<char*>(rgb.data()), rgb.size());
        if (!input)
            return false;
        for (size_t pixel = 0; pixel < static_cast<size_t>(width) * height; ++pixel)
        {
            rgbaPixels[pixel * 4 + 0] = rgb[pixel * 3 + 0];
            rgbaPixels[pixel * 4 + 1] = rgb[pixel * 3 + 1];
            rgbaPixels[pixel * 4 + 2] = rgb[pixel * 3 + 2];
        }
        return true;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    timeBeginPeriod(1);
    const bool cacheTest = std::wcsstr(GetCommandLineW(), L"--cache-test") != nullptr;
    const bool saveReference = std::wcsstr(GetCommandLineW(), L"--save-reference") != nullptr;
    if (cacheTest)
    {
        g_pendingWidth = 640;
        g_pendingHeight = 360;
    }

    WNDCLASSEX windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"DX12RaymarchingWindow";
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassEx(&windowClass);

    RECT rectangle{ 0, 0, static_cast<LONG>(g_pendingWidth), static_cast<LONG>(g_pendingHeight) };
    AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindow(windowClass.lpszClassName, L"DX12 Volumetric Raymarching",
        WS_OVERLAPPEDWINDOW, 100, 80, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
        nullptr, nullptr, instance, nullptr);
    if (!window)
        return 1;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    Renderer renderer;
    if (!renderer.Initialize(window, g_pendingWidth, g_pendingHeight))
    {
        std::wstring error(renderer.GetLastError().begin(), renderer.GetLastError().end());
        MessageBox(window, error.c_str(), L"Renderer initialization failed", MB_OK | MB_ICONERROR);
        ImGui::DestroyContext();
        DestroyWindow(window);
        UnregisterClass(windowClass.lpszClassName, instance);
        return 1;
    }

    if (!cacheTest)
    {
        ShowWindow(window, showCommand);
        UpdateWindow(window);
    }

    RenderSettings settings;
    std::array<float, 180> cpuHistory{};
    float smoothedCpuFrameMs = 0.0f;
    auto startTime = std::chrono::steady_clock::now();
    auto previousFrame = startTime;
    bool running = true;
    int exitCode = 0;
    int testPhase = 0;
    int testFrames = 0;
    int testTimingSamples = 0;
    double testTimingSum = 0.0;
    double naiveGpuMs = 0.0;
    double cachedGpuMs = 0.0;
    std::vector<uint8_t> naiveImage;
    std::vector<uint8_t> cachedImage;
    const std::filesystem::path testOutputDirectory =
        std::filesystem::current_path() / "cache-test-results";

    while (running)
    {
        MSG message{};
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessage(&message);
            if (message.message == WM_QUIT)
                running = false;
        }
        if (!running)
            break;

        if (g_resizePending)
        {
            renderer.Resize(g_pendingWidth, g_pendingHeight);
            g_resizePending = false;
        }
        if (renderer.GetWidth() == 0 || renderer.GetHeight() == 0)
        {
            Sleep(16);
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        float timeSeconds = std::chrono::duration<float>(now - startTime).count();
        float cpuFrameMs = std::chrono::duration<float, std::milli>(now - previousFrame).count();
        previousFrame = now;
        constexpr float timingSmoothing = 0.08f;
        smoothedCpuFrameMs = smoothedCpuFrameMs == 0.0f
            ? cpuFrameMs
            : smoothedCpuFrameMs + timingSmoothing * (cpuFrameMs - smoothedCpuFrameMs);
        ShiftHistory(cpuHistory, smoothedCpuFrameMs);

        renderer.BeginFrame();
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (cacheTest)
        {
            if (testFrames >= 10 && testTimingSamples < 24)
            {
                testTimingSum += renderer.GetStats().gpuRenderSampleMs;
                ++testTimingSamples;
            }

            settings.mode = testPhase == 0 ? RaymarchMode::Naive : RaymarchMode::Cached;
            ImGui::Render();
            renderer.Render(settings, ImGui::GetDrawData(), 0.0f);
            ++testFrames;

            if (testFrames >= 40)
            {
                std::vector<uint8_t>& image = testPhase == 0 ? naiveImage : cachedImage;
                if (!renderer.CaptureOutput(image))
                {
                    exitCode = 2;
                    running = false;
                    continue;
                }

                const double averageGpuMs = testTimingSamples > 0
                    ? testTimingSum / testTimingSamples
                    : 0.0;
                if (testPhase == 0)
                {
                    naiveGpuMs = averageGpuMs;
                    testPhase = 1;
                    settings.rebuildCache = true;
                    testFrames = 0;
                    testTimingSamples = 0;
                    testTimingSum = 0.0;
                }
                else
                {
                    cachedGpuMs = averageGpuMs;
                    std::filesystem::create_directories(testOutputDirectory);
                    WritePpm(testOutputDirectory / "naive.ppm",
                        renderer.GetWidth(), renderer.GetHeight(), naiveImage);
                    WritePpm(testOutputDirectory / "cached.ppm",
                        renderer.GetWidth(), renderer.GetHeight(), cachedImage);

                    const ImageDifference difference = CompareImages(naiveImage, cachedImage);
                    const double speedup = cachedGpuMs > 0.0 ? naiveGpuMs / cachedGpuMs : 0.0;

                    // Regression comparison: if `reference/naive.ppm` and
                    // `reference/cached.ppm` exist we report the MAE of the current
                    // run against those known-good goldens. Lets us detect drift in
                    // EITHER the naive path (bug in raymarcher) or the cached path
                    // (bug in the voxel linked transmittance cache) — the plain `mae`
                    // above only says "cached matches naive on this build", which
                    // wouldn't catch a regression where BOTH paths shifted together.
                    const std::filesystem::path referenceDirectory = testOutputDirectory / "reference";
                    if (saveReference)
                    {
                        std::filesystem::create_directories(referenceDirectory);
                        WritePpm(referenceDirectory / "naive.ppm",
                            renderer.GetWidth(), renderer.GetHeight(), naiveImage);
                        WritePpm(referenceDirectory / "cached.ppm",
                            renderer.GetWidth(), renderer.GetHeight(), cachedImage);
                    }
                    std::vector<uint8_t> referenceNaive, referenceCached;
                    const bool hasReferenceNaive = ReadPpmAsRgba(
                        referenceDirectory / "naive.ppm",
                        renderer.GetWidth(), renderer.GetHeight(), referenceNaive);
                    const bool hasReferenceCached = ReadPpmAsRgba(
                        referenceDirectory / "cached.ppm",
                        renderer.GetWidth(), renderer.GetHeight(), referenceCached);
                    ImageDifference naiveVsReference, cachedVsReference;
                    if (hasReferenceNaive)
                        naiveVsReference = CompareImages(referenceNaive, naiveImage);
                    if (hasReferenceCached)
                        cachedVsReference = CompareImages(referenceCached, cachedImage);

                    const RendererStats& testStats = renderer.GetStats();
                    const bool passed = difference.meanAbsolute <= 1.0 &&
                        cachedGpuMs <= naiveGpuMs * 0.95;
                    std::ofstream report(testOutputDirectory / "report.json");
                    report << std::fixed << std::setprecision(4)
                        << "{\n"
                        << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
                        << "  \"width\": " << renderer.GetWidth() << ",\n"
                        << "  \"height\": " << renderer.GetHeight() << ",\n"
                        << "  \"mae\": " << difference.meanAbsolute << ",\n"
                        << "  \"rms\": " << difference.rootMeanSquare << ",\n"
                        << "  \"maximum_error\": " << static_cast<uint32_t>(difference.maximum) << ",\n"
                        << "  \"naive_vs_reference_mae\": "
                        << (hasReferenceNaive ? naiveVsReference.meanAbsolute : -1.0) << ",\n"
                        << "  \"cached_vs_reference_mae\": "
                        << (hasReferenceCached ? cachedVsReference.meanAbsolute : -1.0) << ",\n"
                        << "  \"naive_gpu_ms\": " << naiveGpuMs << ",\n"
                        << "  \"cached_gpu_ms\": " << cachedGpuMs << ",\n"
                        << "  \"speedup\": " << speedup << ",\n"
                        << "  \"grid_resolution\": " << settings.gridResolution << ",\n"
                        << "  \"cache_memory_mib\": "
                        << testStats.cacheBytes / (1024.0 * 1024.0) << ",\n"
                        << "  \"occupied_cache_cells\": " << testStats.occupiedCells << ",\n"
                        << "  \"compact_residual_cells\": "
                        << testStats.compactCacheCells << ",\n"
                        << "  \"cache_build_ms\": " << testStats.lastCacheBuildMs << "\n"
                        << "}\n";
                    exitCode = passed ? 0 : 3;
                    running = false;
                }
            }
            continue;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_F1, false))
            settings.mode = RaymarchMode::Naive;
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false))
            settings.mode = RaymarchMode::Cached;

        if (!io.WantCaptureMouse)
        {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                settings.autoOrbit = false;
                settings.cameraYaw += io.MouseDelta.x * 0.006f;
                settings.cameraPitch = std::clamp(
                    settings.cameraPitch - io.MouseDelta.y * 0.006f, -1.553f, 1.553f);
            }
            if (io.MouseWheel != 0.0f)
            {
                const float zoomStep = std::max(0.1f, settings.cameraDistance * 0.12f);
                settings.cameraDistance = std::clamp(
                    settings.cameraDistance - io.MouseWheel * zoomStep, 0.0f, 20.0f);
            }
        }

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::SetNextWindowBgAlpha(1.0f);
        constexpr ImGuiWindowFlags sceneFlags = ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs;
        ImGui::Begin("Raymarched Scene", nullptr, sceneFlags);
        ImGui::Image(reinterpret_cast<ImTextureID>(renderer.GetOutputSrv().ptr), ImGui::GetContentRegionAvail());
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(390, 650), ImGuiCond_FirstUseEver);
        ImGui::Begin("Raymarching Controls");

        int mode = static_cast<int>(settings.mode);
        ImGui::TextUnformatted("Lighting method");
        ImGui::RadioButton("Naive nested shadow march", &mode, 0);
        ImGui::RadioButton("Voxel linked transmittance cache", &mode, 1);
        settings.mode = static_cast<RaymarchMode>(mode);
        ImGui::TextDisabled("F1: naive, F2: cached");

        {
            const RendererStats& stats = renderer.GetStats();
            ImGui::SeparatorText("Performance");
            ImGui::TextUnformatted("VSync");
            ImGui::SameLine();
            ImGui::RadioButton("Off", &settings.vsyncMode, 0); ImGui::SameLine();
            ImGui::RadioButton("60 FPS", &settings.vsyncMode, 1); ImGui::SameLine();
            ImGui::RadioButton("30 FPS", &settings.vsyncMode, 2);
            ImGui::Text("CPU active: %.2f ms  (no vsync wait)", stats.cpuActiveMs);
            ImGui::Text("CPU frame: %.2f ms (%.0f FPS)", smoothedCpuFrameMs,
                smoothedCpuFrameMs > 0.0f ? 1000.0f / smoothedCpuFrameMs : 0.0f);
            ImGui::Text("GPU scene: %.3f ms", stats.gpuRenderMs);
            ImGui::Text("GPU total: %.3f ms", stats.gpuTotalMs);
            ImGui::PlotLines("GPU scene ms", stats.gpuHistory.data(),
                static_cast<int>(stats.gpuHistory.size()), 0, nullptr, 0.0f, 25.0f, ImVec2(0, 55));
            ImGui::PlotLines("CPU frame ms", cpuHistory.data(),
                static_cast<int>(cpuHistory.size()), 0, nullptr, 0.0f, 33.0f, ImVec2(0, 55));
        }

        ImGui::SeparatorText("Volume and integration");
        bool cacheChanged = false;
        ImGui::SliderInt("Primary steps", &settings.primarySteps, 32, 320);
        ImGui::SliderFloat("Step scale", &settings.stepScale, 0.5f, 1.5f);
        ImGui::BeginDisabled(settings.mode != RaymarchMode::Naive);
        ImGui::SliderInt("Shadow steps", &settings.shadowSteps, 8, 160);
        ImGui::EndDisabled();

        if (ImGui::CollapsingHeader("Volume shape controls"))
        {
            cacheChanged |= ImGui::SliderFloat("Density gain", &settings.densityScale, 0.1f, 20.0f);
            cacheChanged |= ImGui::SliderFloat("Density threshold", &settings.densityThreshold, 0.0f, 0.95f);
            cacheChanged |= ImGui::SliderFloat("Density gamma", &settings.densityGamma, 0.1f, 3.0f);
            cacheChanged |= ImGui::SliderFloat("Extinction", &settings.extinction, 0.1f, 12.0f);
            ImGui::SliderFloat("Scattering", &settings.scattering, 0.0f, 4.0f);
            cacheChanged |= ImGui::SliderFloat("Shadow density gain", &settings.shadowDensityScale, 0.1f, 8.0f);
            ImGui::SliderFloat("Phase anisotropy", &settings.phaseAnisotropy, -0.85f, 0.85f);
            ImGui::SliderFloat("Direct light", &settings.directLightIntensity, 0.0f, 5.0f);
            ImGui::SliderFloat("Multiple scattering", &settings.multipleScattering, 0.0f, 3.0f);
            ImGui::SliderFloat("Powder / silver lining", &settings.powderStrength, 0.0f, 3.0f);
            ImGui::SliderFloat("Ambient intensity", &settings.ambientLight, 0.0f, 2.0f);
            ImGui::SliderFloat("Exposure", &settings.exposure, 0.1f, 8.0f);
            ImGui::SliderFloat("Contrast", &settings.contrast, 0.5f, 2.5f);
            ImGui::SliderFloat("Display gamma", &settings.displayGamma, 1.0f, 3.0f);
            ImGui::SliderFloat("Background intensity", &settings.backgroundIntensity, 0.1f, 3.0f);
            ImGui::ColorEdit3("Light color", &settings.lightColor.x);
            ImGui::ColorEdit3("Ambient color", &settings.ambientColor.x);
            ImGui::Checkbox("Surface-band normals", &settings.enableSurfaceNormals);
            if (ImGui::Button("Dense reference preset"))
            {
                settings.densityScale = 18.948f;
                settings.densityThreshold = 0.054f;
                settings.densityGamma = 0.8f;
                settings.extinction = 3.0f;
                settings.scattering = 0.557f;
                settings.shadowDensityScale = 1.093f;
                settings.phaseAnisotropy = 0.516f;
                settings.directLightIntensity = 1.007f;
                settings.multipleScattering = 0.452f;
                settings.powderStrength = 0.690f;
                settings.ambientLight = 0.155f;
                settings.lightColor = { 1.0f, 1.0f, 1.0f };
                settings.ambientColor = { 0.702f, 0.702f, 0.702f };
                settings.exposure = 1.25f;
                settings.contrast = 1.08f;
                settings.displayGamma = 2.2f;
                settings.backgroundIntensity = 0.987f;
                settings.primarySteps = 180;
                settings.shadowSteps = 55;
                settings.stepScale = 1.0f;
                cacheChanged = true;
            }
        }

        if (ImGui::CollapsingHeader("Transmittance cache", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const char* gridItems[] = { "32^3", "48^3", "64^3", "96^3", "128^3", "160^3", "192^3", "224^3", "256^3" };
            int gridItem = settings.gridResolution == 32 ? 0 :
                settings.gridResolution == 48 ? 1 :
                settings.gridResolution == 64 ? 2 :
                settings.gridResolution == 96 ? 3 :
                settings.gridResolution == 128 ? 4 :
                settings.gridResolution == 160 ? 5 :
                settings.gridResolution == 192 ? 6 :
                settings.gridResolution == 224 ? 7 : 8;
            if (ImGui::Combo("Grid", &gridItem, gridItems, IM_ARRAYSIZE(gridItems)))
            {
                constexpr int gridResolutions[] = { 32, 48, 64, 96, 128, 160, 192, 224, 256 };
                settings.gridResolution = gridResolutions[gridItem];
                cacheChanged = true;
            }
            cacheChanged |= ImGui::SliderFloat3("Direction to light", &settings.lightDirection.x, -1.0f, 1.0f);
            cacheChanged |= ImGui::Checkbox("Rotate light around up axis", &settings.rotateLight);
            ImGui::BeginDisabled(!settings.rotateLight);
            ImGui::SliderFloat("Light rotation speed", &settings.lightRotationSpeed, -2.0f, 2.0f, "%.3f rad/s");
            ImGui::EndDisabled();
            if (ImGui::Button("Rebuild cache"))
                settings.rebuildCache = true;
            ImGui::Checkbox("Green voxel wireframe", &settings.debugVoxels);
        }

        if (ImGui::CollapsingHeader("Optimisation techniques", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Checkbox("Use pre-baked normal LUT", &settings.useNormalLUT);
            if (settings.useNormalLUT)
            {
                const char* lutItems[] = { "128", "192", "256", "384", "512" };
                const int lutValues[] = { 128, 192, 256, 384, 512 };
                int lutIndex = 2;
                for (int i = 0; i < IM_ARRAYSIZE(lutValues); ++i)
                    if (settings.normalLUTResolution == lutValues[i]) { lutIndex = i; break; }
                if (ImGui::Combo("Normal LUT resolution", &lutIndex, lutItems, IM_ARRAYSIZE(lutItems)))
                    settings.normalLUTResolution = lutValues[lutIndex];
            }
            ImGui::BeginDisabled(settings.useNormalLUT || !settings.enableSurfaceNormals);
            ImGui::Checkbox("Reduced-tap normals (4-tap forward diff)", &settings.useReducedTapNormals);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Trades 2 depth taps for slight normal bias.\n"
                                  "Only active when 'Use pre-baked normal LUT' is off\n"
                                  "and 'Surface-band normals' is on (LUT path is\n"
                                  "already 1 tap, so reduced-tap can't help).");
            ImGui::EndDisabled();
            ImGui::Checkbox("Tile classification pre-pass", &settings.enableTileClassify);
            ImGui::BeginDisabled(!settings.enableTileClassify);
            ImGui::Checkbox("Debug: colour tiles by category", &settings.debugTiles);
            ImGui::EndDisabled();
            ImGui::Checkbox("Empty-space DDA skipping", &settings.useDistanceField);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("DDA (Digital Differential Analyser) empty-space skipping:\n"
                                  "at bake time we build a coarse Chebyshev distance field\n"
                                  "over the volume — each cell stores the distance (in cells)\n"
                                  "to the nearest non-empty voxel. On every march step the\n"
                                  "shader Load()s the DF at the current cell:\n"
                                  "  - if DF > 0, jump t forward by DF * cellSize (skip N cells\n"
                                  "    of empty space in one op, no density sampling).\n"
                                  "  - if DF == 0, take a normal density step.\n"
                                  "Wins where rays traverse a lot of empty box (halos above\n"
                                  "and beside the cloud). Costs one extra 3D texture Load()\n"
                                  "per step — a net loss if the shader is already texture-\n"
                                  "latency bound and most steps are inside dense material,\n"
                                  "where DF==0 forces the density step AND pays the DF tap.");
            ImGui::BeginDisabled(!settings.useDistanceField);
            ImGui::Checkbox("Debug: DDA cell-skip heatmap", &settings.debugDistanceField);
            ImGui::EndDisabled();
            ImGui::Checkbox("Debug: density-sample heatmap", &settings.showDensityHeatmap);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Overlays a false-colour count of DenseDensityAt calls\n"
                                  "per pixel. Blue=few, red=many. Sky pixels unchanged.\n"
                                  "Disables split-trace path (uses monolithic PSO).");
            ImGui::BeginDisabled(!settings.showDensityHeatmap);
            const char* heatmapModes[] = { "DenseVolume", "CachedDepth", "Sum (all)", "DistanceField" };
            ImGui::Combo("Heatmap counter", &settings.heatmapCounterMode,
                heatmapModes, IM_ARRAYSIZE(heatmapModes));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Which texture the count tracks:\n"
                                  "  DenseVolume:   DenseDensityAt samples (main march).\n"
                                  "  CachedDepth:   finite-diff normal + shadow-cache taps.\n"
                                  "                 (Reduced-tap only moves this counter.)\n"
                                  "  Sum (all):     dense + depth + df, unified cost view.\n"
                                  "  DistanceField: DDA cell-skip loads (empty-space DDA).\n"
                                  "                 Zero unless 'Empty-space DDA skipping'\n"
                                  "                 is enabled.");
            ImGui::DragInt("Heatmap max samples", &settings.heatmapMaxSamples, 4.0f, 8, 4096);
            ImGui::EndDisabled();
            ImGui::Checkbox("Interval-finding + shading split (A/B)", &settings.useSplitTrace);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Two-dispatch experiment: pass A does a thin DDA-only walk\n"
                    "to find first-hit t per pixel; pass B does the full lighting cascade\n"
                    "from there with no DDA/DF state (smaller register footprint, higher\n"
                    "occupancy). Cached mode only.");
            ImGui::BeginDisabled(!settings.useSplitTrace || !settings.enableTileClassify);
            ImGui::Checkbox("Tile-variant dispatch (indirect A/B/C)", &settings.useTileVariantDispatch);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Specialised pass B per tile category, dispatched via\n"
                    "ExecuteIndirect over per-category tile lists:\n"
                    "  - sky:      background-only writer\n"
                    "  - interior: no surface-band normal shading\n"
                    "  - surface:  full lighting cascade\n"
                    "Requires tile classifier + split A/B.");
            ImGui::EndDisabled();
            ImGui::TextDisabled("Lazy lighting: skip full lighting cascade for near-invisible samples.");
            ImGui::SliderFloat("Lazy lighting threshold", &settings.lightingSkipEpsilon,
                0.0f, 5e-3f, "%.5f", ImGuiSliderFlags_Logarithmic);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Skip full lighting cascade (light cache + normal LUT + phase)\n"
                    "when a sample's scattered contribution is below this threshold.\n"
                    "Transmittance is still updated so silhouettes stay correct.\n"
                    "0 disables the optimisation. Raise for more speed, lower for fidelity.");
        }

        if (cacheChanged)
            settings.rebuildCache = true;

        if (ImGui::CollapsingHeader("Camera"))
        {
            ImGui::Checkbox("Automatic orbit", &settings.autoOrbit);
            ImGui::BeginDisabled(settings.autoOrbit);
            ImGui::SliderAngle("Yaw", &settings.cameraYaw, -180.0f, 180.0f);
            ImGui::EndDisabled();
            ImGui::SliderAngle("Pitch", &settings.cameraPitch, -70.0f, 70.0f);
            ImGui::SliderFloat("Distance", &settings.cameraDistance, 0.0f, 20.0f);
            ImGui::TextDisabled("Scene: left-drag orbit, wheel dolly");
        }

        const RendererStats& stats = renderer.GetStats();
        ImGui::SeparatorText("Cache build");
        ImGui::Text("Column prefix build: %.3f ms", stats.gpuVoxelizeMs);
        ImGui::Text("Build total: %.3f ms", stats.lastCacheBuildMs);
        ImGui::Text("Cells: %u / %u occupied", stats.occupiedCells, stats.totalCells);
        ImGui::Text("Compact residual cells: %u", stats.compactCacheCells);
        ImGui::Text("Cache memory: %.2f MiB", stats.cacheBytes / (1024.0 * 1024.0));
        ImGui::Text("VDB dense grid: %u x %u x %u", stats.volumeWidth,
            stats.volumeHeight, stats.volumeDepth);
        ImGui::Text("VDB active bricks: %u", stats.activeBricks);
        ImGui::Text("VDB source active voxels: %llu",
            static_cast<unsigned long long>(stats.sourceActiveVoxels));
        ImGui::Text("Dense / sparse: %.2f / %.2f MiB",
            stats.denseVolumeBytes / (1024.0 * 1024.0),
            stats.sparseVolumeBytes / (1024.0 * 1024.0));
        ImGui::Text("Normal LUT: %.2f MiB",
            stats.normalLUTBytes / (1024.0 * 1024.0));
        ImGui::Text("View samples/pixel: %d", settings.primarySteps);
        if (settings.mode == RaymarchMode::Naive)
            ImGui::Text("Worst-case density evaluations/pixel: %d", settings.primarySteps * settings.shadowSteps);
        else
            ImGui::Text("Light cache lookups/pixel: up to %d", settings.primarySteps);
        ImGui::End();

        ImGui::Render();
        renderer.Render(settings, ImGui::GetDrawData(), timeSeconds);

        if (settings.vsyncMode > 0)
        {
            const float targetFrameMs = settings.vsyncMode == 1 ? 1000.0f / 60.0f
                : 1000.0f / 30.0f;
            using namespace std::chrono;
            const auto target = previousFrame + duration_cast<steady_clock::duration>(
                duration<float, std::milli>(targetFrameMs));
            for (;;)
            {
                const auto current = steady_clock::now();
                const auto remaining = duration_cast<duration<float, std::milli>>(
                    target - current).count();
                if (remaining <= 0.0f)
                    break;
                if (remaining > 2.0f)
                    Sleep(static_cast<DWORD>(remaining - 1.0f));
                else
                    std::this_thread::yield();
            }
        }
    }

    renderer.Shutdown();
    ImGui::DestroyContext();
    DestroyWindow(window);
    UnregisterClass(windowClass.lpszClassName, instance);
    timeEndPeriod(1);
    return exitCode;
}
