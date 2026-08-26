#include "VolumeData.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/tools/Interpolation.h>
#include <DirectXPackedVector.h>

namespace
{
    constexpr uint32_t MaximumDenseDimension = 256;
    constexpr float ActiveThreshold = 1e-4f;

    uint32_t DivideRoundUp(uint32_t value, uint32_t divisor)
    {
        return (value + divisor - 1) / divisor;
    }

    size_t Flatten(uint32_t x, uint32_t y, uint32_t z, uint32_t width, uint32_t height)
    {
        return x + static_cast<size_t>(width) * (y + static_cast<size_t>(height) * z);
    }
}

bool LoadVdbVolume(const std::filesystem::path& path, VolumeData& result, std::string& error)
{
    try
    {
        openvdb::initialize();
        openvdb::io::File file(path.string());
        file.open();
        openvdb::GridPtrVecPtr grids = file.getGrids();
        file.close();

        openvdb::FloatGrid::Ptr densityGrid;
        for (const openvdb::GridBase::Ptr& baseGrid : *grids)
        {
            if (!baseGrid->isType<openvdb::FloatGrid>())
                continue;
            openvdb::FloatGrid::Ptr candidate = openvdb::gridPtrCast<openvdb::FloatGrid>(baseGrid);
            if (!densityGrid || candidate->getName() == "density")
                densityGrid = candidate;
            if (candidate->getName() == "density")
                break;
        }

        if (!densityGrid)
        {
            error = "The VDB contains no FloatGrid density volume.";
            return false;
        }

        if (densityGrid->activeVoxelCount() == 0)
        {
            error = "The VDB density grid has no active voxels.";
            return false;
        }
        const openvdb::CoordBBox activeBounds = densityGrid->evalActiveVoxelBoundingBox();

        const openvdb::Coord sourceDimensions = activeBounds.dim();
        const uint32_t maximumSourceDimension = static_cast<uint32_t>(
            std::max({ sourceDimensions.x(), sourceDimensions.y(), sourceDimensions.z() }));
        const float scale = std::min(1.0f,
            static_cast<float>(MaximumDenseDimension) / maximumSourceDimension);

        result.width = std::max(2u, static_cast<uint32_t>(std::ceil(sourceDimensions.x() * scale)));
        result.height = std::max(2u, static_cast<uint32_t>(std::ceil(sourceDimensions.y() * scale)));
        result.depth = std::max(2u, static_cast<uint32_t>(std::ceil(sourceDimensions.z() * scale)));
        result.gridName = densityGrid->getName();
        result.sourceActiveVoxels = densityGrid->activeVoxelCount();

        float maximumDensity = 0.0f;
        for (auto iterator = densityGrid->cbeginValueOn(); iterator; ++iterator)
            maximumDensity = std::max(maximumDensity, std::max(0.0f, *iterator));
        result.sourceMaximumDensity = maximumDensity;

        const uint64_t denseCount = static_cast<uint64_t>(result.width) * result.height * result.depth;
        result.denseHalf.resize(static_cast<size_t>(denseCount));
        std::vector<float> denseFloat(static_cast<size_t>(denseCount));

        const openvdb::FloatGrid::ConstAccessor accessor = densityGrid->getConstAccessor();
        openvdb::tools::GridSampler<openvdb::FloatGrid::ConstAccessor,
            openvdb::tools::BoxSampler> sampler(accessor, densityGrid->transform());

        const openvdb::Vec3d sourceMinimum(
            activeBounds.min().x(), activeBounds.min().y(), activeBounds.min().z());
        const openvdb::Vec3d sourceExtent(
            std::max(1, sourceDimensions.x() - 1),
            std::max(1, sourceDimensions.y() - 1),
            std::max(1, sourceDimensions.z() - 1));

        for (uint32_t z = 0; z < result.depth; ++z)
        {
            const double normalizedZ = static_cast<double>(z) / (result.depth - 1);
            for (uint32_t y = 0; y < result.height; ++y)
            {
                const double normalizedY = static_cast<double>(y) / (result.height - 1);
                for (uint32_t x = 0; x < result.width; ++x)
                {
                    const double normalizedX = static_cast<double>(x) / (result.width - 1);
                    const openvdb::Vec3d indexPosition = sourceMinimum +
                        openvdb::Vec3d(normalizedX * sourceExtent.x(),
                            normalizedY * sourceExtent.y(), normalizedZ * sourceExtent.z());
                    const float density = std::max(0.0f, sampler.isSample(indexPosition));
                    const size_t index = Flatten(x, y, z, result.width, result.height);
                    denseFloat[index] = density;
                }
            }
        }

        std::vector<float> nonzeroDensities;
        nonzeroDensities.reserve(denseFloat.size() / 4);
        for (float density : denseFloat)
        {
            if (density > ActiveThreshold)
                nonzeroDensities.push_back(density);
        }
        if (nonzeroDensities.empty())
        {
            error = "The resampled VDB density grid contains no nonzero samples.";
            return false;
        }
        const size_t percentileIndex = std::min(nonzeroDensities.size() - 1,
            static_cast<size_t>(nonzeroDensities.size() * 0.99));
        std::nth_element(nonzeroDensities.begin(),
            nonzeroDensities.begin() + percentileIndex, nonzeroDensities.end());
        result.normalizationDensity = std::max(nonzeroDensities[percentileIndex], 1e-4f);
        const float normalization = 1.0f / result.normalizationDensity;
        for (size_t index = 0; index < denseFloat.size(); ++index)
        {
            const float exposedDensity =
                std::clamp(denseFloat[index] * normalization, 0.0f, 1.0f);
            denseFloat[index] = exposedDensity;
            result.denseHalf[index] =
                DirectX::PackedVector::XMConvertFloatToHalf(exposedDensity);
        }

        result.pageWidth = DivideRoundUp(result.width, VolumeData::BrickSize);
        result.pageHeight = DivideRoundUp(result.height, VolumeData::BrickSize);
        result.pageDepth = DivideRoundUp(result.depth, VolumeData::BrickSize);
        result.pageTable.assign(static_cast<size_t>(result.pageWidth) *
            result.pageHeight * result.pageDepth, VolumeData::EmptyBrick);

        constexpr uint32_t valuesPerBrick =
            VolumeData::BrickSize * VolumeData::BrickSize * VolumeData::BrickSize;
        for (uint32_t brickZ = 0; brickZ < result.pageDepth; ++brickZ)
        {
            for (uint32_t brickY = 0; brickY < result.pageHeight; ++brickY)
            {
                for (uint32_t brickX = 0; brickX < result.pageWidth; ++brickX)
                {
                    bool active = false;
                    for (uint32_t localZ = 0; localZ < VolumeData::BrickSize && !active; ++localZ)
                    {
                        const uint32_t z = brickZ * VolumeData::BrickSize + localZ;
                        if (z >= result.depth)
                            continue;
                        for (uint32_t localY = 0; localY < VolumeData::BrickSize && !active; ++localY)
                        {
                            const uint32_t y = brickY * VolumeData::BrickSize + localY;
                            if (y >= result.height)
                                continue;
                            for (uint32_t localX = 0; localX < VolumeData::BrickSize; ++localX)
                            {
                                const uint32_t x = brickX * VolumeData::BrickSize + localX;
                                if (x < result.width &&
                                    denseFloat[Flatten(x, y, z, result.width, result.height)] > ActiveThreshold)
                                {
                                    active = true;
                                    break;
                                }
                            }
                        }
                    }

                    if (!active)
                        continue;

                    const uint32_t brickIndex = result.ActiveBrickCount();
                    result.pageTable[Flatten(brickX, brickY, brickZ,
                        result.pageWidth, result.pageHeight)] = brickIndex;
                    const size_t baseOffset = result.brickPool.size();
                    result.brickPool.resize(baseOffset + valuesPerBrick, 0.0f);
                    for (uint32_t localZ = 0; localZ < VolumeData::BrickSize; ++localZ)
                    {
                        const uint32_t z = brickZ * VolumeData::BrickSize + localZ;
                        for (uint32_t localY = 0; localY < VolumeData::BrickSize; ++localY)
                        {
                            const uint32_t y = brickY * VolumeData::BrickSize + localY;
                            for (uint32_t localX = 0; localX < VolumeData::BrickSize; ++localX)
                            {
                                const uint32_t x = brickX * VolumeData::BrickSize + localX;
                                const uint32_t localIndex = localX + VolumeData::BrickSize *
                                    (localY + VolumeData::BrickSize * localZ);
                                if (x < result.width && y < result.height && z < result.depth)
                                    result.brickPool[baseOffset + localIndex] =
                                        denseFloat[Flatten(x, y, z, result.width, result.height)];
                            }
                        }
                    }
                }
            }
        }

        const float maximumWorldExtent = 2.4f;
        const float largestDimension = static_cast<float>(
            std::max({ result.width, result.height, result.depth }));
        const DirectX::XMFLOAT3 extent{
            maximumWorldExtent * result.width / largestDimension,
            maximumWorldExtent * result.height / largestDimension,
            maximumWorldExtent * result.depth / largestDimension
        };
        result.boundsMin = { -extent.x * 0.5f, -extent.y * 0.5f, -extent.z * 0.5f };
        result.boundsMax = { extent.x * 0.5f, extent.y * 0.5f, extent.z * 0.5f };
        return true;
    }
    catch (const std::exception& exception)
    {
        error = std::string("OpenVDB load failed: ") + exception.what();
        return false;
    }
}
