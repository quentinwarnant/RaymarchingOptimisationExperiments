#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <DirectXMath.h>

struct VolumeData
{
    static constexpr uint32_t BrickSize = 8;
    static constexpr uint32_t EmptyBrick = 0xffffffffu;

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 0;
    uint32_t pageWidth = 0;
    uint32_t pageHeight = 0;
    uint32_t pageDepth = 0;
    uint64_t sourceActiveVoxels = 0;
    float sourceMaximumDensity = 0.0f;
    float normalizationDensity = 1.0f;
    DirectX::XMFLOAT3 boundsMin{};
    DirectX::XMFLOAT3 boundsMax{};
    std::string gridName;
    std::vector<uint16_t> denseHalf;
    std::vector<uint32_t> pageTable;
    std::vector<float> brickPool;

    uint32_t ActiveBrickCount() const
    {
        return static_cast<uint32_t>(brickPool.size() / (BrickSize * BrickSize * BrickSize));
    }
};

bool LoadVdbVolume(const std::filesystem::path& path, VolumeData& result, std::string& error);
