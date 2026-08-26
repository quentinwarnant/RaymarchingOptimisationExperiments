#include "VolumeData.h"

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    const std::filesystem::path path = argc > 1
        ? std::filesystem::path(argv[1])
        : std::filesystem::path("Assets") / "Altocumlus.vdb";
    VolumeData volume;
    std::string error;
    if (!LoadVdbVolume(path, volume, error))
    {
        std::cerr << error << '\n';
        return 1;
    }

    const uint64_t denseBytes = volume.denseHalf.size() * sizeof(uint16_t);
    const uint64_t sparseBytes = volume.pageTable.size() * sizeof(uint32_t) +
        volume.brickPool.size() * sizeof(float);
    std::cout << "grid=" << volume.gridName << '\n'
        << "dense_dimensions=" << volume.width << 'x' << volume.height << 'x'
        << volume.depth << '\n'
        << "source_active_voxels=" << volume.sourceActiveVoxels << '\n'
        << "source_max_density=" << volume.sourceMaximumDensity << '\n'
        << "normalization_density_p99=" << volume.normalizationDensity << '\n'
        << "page_dimensions=" << volume.pageWidth << 'x' << volume.pageHeight << 'x'
        << volume.pageDepth << '\n'
        << "active_bricks=" << volume.ActiveBrickCount() << '\n'
        << "dense_bytes=" << denseBytes << '\n'
        << "sparse_bytes=" << sparseBytes << '\n'
        << "bounds_min=" << volume.boundsMin.x << ',' << volume.boundsMin.y << ','
        << volume.boundsMin.z << '\n'
        << "bounds_max=" << volume.boundsMax.x << ',' << volume.boundsMax.y << ','
        << volume.boundsMax.z << '\n';
    return 0;
}
