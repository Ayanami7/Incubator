#include "runtime/render/shadow.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/matrix.hpp>
#include <limits>

#include "runtime/core/error/error.h"
#include "runtime/render/frame_input.h"

namespace Incubator::Render
{
    namespace
    {
        glm::dvec3 validateAndNormalizeLightDirection(const ShadowSettings& settings, const glm::vec3& direction)
        {
            for (int axis = 0; axis < 3; ++axis)
                if (!std::isfinite(settings.center[axis]) || !std::isfinite(direction[axis]))
                    throw Error::Exception(Error::Code::CONFIG, "Shadow center and light direction must be finite");
            for (int axis = 0; axis < 2; ++axis)
                if (!std::isfinite(settings.halfExtent[axis]) || settings.halfExtent[axis] <= 0.0f)
                    throw Error::Exception(Error::Code::CONFIG, "Shadow half extents must be finite and positive");
            if (!std::isfinite(settings.nearPlane) || !std::isfinite(settings.farPlane) || settings.nearPlane <= 0.0f ||
                settings.farPlane <= settings.nearPlane)
                throw Error::Exception(Error::Code::CONFIG, "Shadow depth range requires finite 0 < near < far");
            if (!std::isfinite(settings.depthBias) || settings.depthBias < 0.0f)
                throw Error::Exception(Error::Code::CONFIG,
                                       "Shadow receiver depth bias must be finite and nonnegative");
            if (!std::isfinite(settings.normalBias) || settings.normalBias < 0.0f)
                throw Error::Exception(Error::Code::CONFIG,
                                       "Shadow receiver normal bias must be finite and nonnegative");
            if (!std::has_single_bit(settings.resolution) || settings.resolution < 512u || settings.resolution > 4096u)
                throw Error::Exception(Error::Code::CONFIG,
                                       "Shadow resolution must be a power of two in [512, 4096]");
            if (settings.cascadeCount == 0u || settings.cascadeCount > MAX_SHADOW_CASCADES)
                throw Error::Exception(Error::Code::CONFIG,
                                       "Shadow cascadeCount must be in [1, MAX_SHADOW_CASCADES]");
            if (!std::isfinite(settings.cascadeSplitLambda) || settings.cascadeSplitLambda < 0.0f ||
                settings.cascadeSplitLambda > 1.0f)
                throw Error::Exception(Error::Code::CONFIG,
                                       "Shadow cascadeSplitLambda must be finite in [0, 1]");
            if (!std::isfinite(settings.maxDistance) || settings.maxDistance <= 0.0f)
                throw Error::Exception(Error::Code::CONFIG, "Shadow maxDistance must be finite and positive");

            const double length = std::hypot(double(direction.x), double(direction.y), double(direction.z));
            if (length == 0.0)
                throw Error::Exception(Error::Code::CONFIG, "Shadow light direction must be nonzero");
            return glm::dvec3(direction) / length;
        }

        glm::dmat4 lightBasisMatrix(const glm::dvec3& forward)
        {
            const glm::dvec3 up = std::abs(forward.y) > 0.99 ? glm::dvec3(0.0, 0.0, 1.0) : glm::dvec3(0.0, 1.0, 0.0);
            return glm::lookAtRH(glm::dvec3(0.0), forward, up);
        }

        void validateFiniteMatrix(const glm::mat4& matrix)
        {
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 4; ++row)
                    if (!std::isfinite(matrix[column][row]))
                        throw Error::Exception(Error::Code::CONFIG,
                                               "Shadow view projection must be representable and finite");
        }

        bool isValidAabbForFitting(const Math::Aabb& bounds)
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                if (!std::isfinite(bounds.min[axis]) || !std::isfinite(bounds.max[axis]) ||
                    bounds.min[axis] > bounds.max[axis])
                    return false;
            }
            return bounds.max.x > bounds.min.x || bounds.max.y > bounds.min.y || bounds.max.z > bounds.min.z;
        }

        void transformAabbToLightSpace(const Math::Aabb& bounds, const glm::dmat4& lightBasis, glm::dvec3& outMin,
                                       glm::dvec3& outMax)
        {
            outMin = glm::dvec3(std::numeric_limits<double>::infinity());
            outMax = glm::dvec3(-std::numeric_limits<double>::infinity());
            for (int corner = 0; corner < 8; ++corner)
            {
                const glm::dvec3 point((corner & 1) ? bounds.max.x : bounds.min.x,
                                       (corner & 2) ? bounds.max.y : bounds.min.y,
                                       (corner & 4) ? bounds.max.z : bounds.min.z);
                const glm::dvec3 lightPos(lightBasis * glm::dvec4(point, 1.0));
                outMin = glm::min(outMin, lightPos);
                outMax = glm::max(outMax, lightPos);
            }
        }

        glm::mat4 buildOrthoFromLightSpaceBox(const glm::dmat4& lightBasis, double cx, double cy, double halfW,
                                              double halfH, double minZ, double maxZ)
        {
            const double depthMargin = std::max((maxZ - minZ) * 0.05, 0.5);
            const double nearPlane = depthMargin;
            const double farPlane = nearPlane + std::max(maxZ - minZ, 0.01) + depthMargin;
            // 从对齐后的光源空间原点反推世界眼点，并保持正交投影在 Y 轴翻转一次。
            const glm::dmat3 invRotation = glm::transpose(glm::dmat3(lightBasis));
            const glm::dvec3 eye = invRotation * glm::dvec3(cx, cy, maxZ + nearPlane);
            const glm::dmat4 view = glm::translate(lightBasis, -eye);
            auto projection = glm::orthoRH_ZO(-halfW, halfW, -halfH, halfH, nearPlane, farPlane);
            projection[1][1] *= -1.0;
            const glm::mat4 matrix(projection * view);
            validateFiniteMatrix(matrix);
            return matrix;
        }

        ShadowFrame initShadowFrame(const ShadowSettings& settings, const glm::mat4& firstMatrix)
        {
            ShadowFrame frame{};
            frame.viewProjection = firstMatrix;
            frame.depthBias = settings.depthBias;
            frame.normalBias = settings.normalBias;
            frame.resolution = settings.resolution;
            frame.cascadeCount = 1u;
            frame.maxDistance = settings.maxDistance;
            frame.cascadeViewProjections[0] = firstMatrix;
            frame.cascadeSplits[0] = settings.maxDistance;
            return frame;
        }
    }  // namespace

    std::optional<ShadowFrame> makeShadowFrame(const ShadowSettings& settings, const glm::vec3& direction)
    {
        if (!settings.enabled)
            return std::nullopt;

        const glm::dvec3 forward = validateAndNormalizeLightDirection(settings, direction);
        const glm::dmat4 lightBasis = lightBasisMatrix(forward);
        const auto eye =
            glm::dvec3(settings.center) - forward * ((double(settings.nearPlane) + double(settings.farPlane)) * 0.5);
        // 先构造朝向，再施加平移，避免大世界坐标下观察位置与目标点相减丢失方向。
        const auto view = glm::translate(lightBasis, -eye);
        auto projection = glm::orthoRH_ZO(-double(settings.halfExtent.x), double(settings.halfExtent.x),
                                          -double(settings.halfExtent.y), double(settings.halfExtent.y),
                                          double(settings.nearPlane), double(settings.farPlane));
        projection[1][1] *= -1.0;
        const glm::mat4 matrix(projection * view);
        validateFiniteMatrix(matrix);
        return initShadowFrame(settings, matrix);
    }

    std::optional<ShadowFrame> makeShadowFrameForBounds(const ShadowSettings& settings, const glm::vec3& direction,
                                                        const Math::Aabb& casterBounds)
    {
        if (!settings.enabled)
            return std::nullopt;

        const glm::dvec3 forward = validateAndNormalizeLightDirection(settings, direction);
        if (!isValidAabbForFitting(casterBounds))
            return makeShadowFrame(settings, direction);

        const glm::dmat4 lightBasis = lightBasisMatrix(forward);
        glm::dvec3 lsMin{};
        glm::dvec3 lsMax{};
        transformAabbToLightSpace(casterBounds, lightBasis, lsMin, lsMax);
        for (int axis = 0; axis < 3; ++axis)
            if (!std::isfinite(lsMin[axis]) || !std::isfinite(lsMax[axis]))
                return makeShadowFrame(settings, direction);

        const double width = std::max(lsMax.x - lsMin.x, 1e-2);
        const double height = std::max(lsMax.y - lsMin.y, 1e-2);
        const double texelX = width / double(settings.resolution);
        const double texelY = height / double(settings.resolution);
        const double cx = std::round((lsMin.x + lsMax.x) * 0.5 / texelX) * texelX;
        const double cy = std::round((lsMin.y + lsMax.y) * 0.5 / texelY) * texelY;
        const glm::mat4 matrix =
            buildOrthoFromLightSpaceBox(lightBasis, cx, cy, 0.5 * (width + texelX), 0.5 * (height + texelY), lsMin.z,
                                        lsMax.z);
        return initShadowFrame(settings, matrix);
    }

    std::optional<ShadowFrame> makeCascadedShadowFrame(const ShadowSettings& settings, const glm::vec3& direction,
                                                       const CameraMatrices& camera, float cameraNear, float cameraFar,
                                                       const std::optional<Math::Aabb>& casterBounds)
    {
        if (!settings.enabled)
            return std::nullopt;

        const glm::dvec3 forward = validateAndNormalizeLightDirection(settings, direction);
        if (!std::isfinite(cameraNear) || !std::isfinite(cameraFar) || cameraNear <= 0.0f || cameraFar <= cameraNear)
            throw Error::Exception(Error::Code::CONFIG, "Cascaded shadow requires finite 0 < cameraNear < cameraFar");

        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                if (!std::isfinite(camera.view[column][row]) || !std::isfinite(camera.projection[column][row]))
                    throw Error::Exception(Error::Code::CONFIG, "Camera matrices for cascaded shadow must be finite");

        const glm::dmat4 viewProj = glm::dmat4(camera.projection) * glm::dmat4(camera.view);
        const double det = glm::determinant(viewProj);
        if (!std::isfinite(det) || std::abs(det) <= 1e-18)
            throw Error::Exception(Error::Code::CONFIG, "Camera matrices for cascaded shadow must be invertible");
        const glm::dmat4 invViewProj = glm::inverse(viewProj);

        std::array<glm::dvec3, 4> nearCorners{};
        std::array<glm::dvec3, 4> farCorners{};
        constexpr std::array<std::array<double, 2>, 4> ndcXY{{{-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0}}};
        for (size_t i = 0; i < 4; ++i)
        {
            const glm::dvec4 nearH = invViewProj * glm::dvec4(ndcXY[i][0], ndcXY[i][1], 0.0, 1.0);
            const glm::dvec4 farH = invViewProj * glm::dvec4(ndcXY[i][0], ndcXY[i][1], 1.0, 1.0);
            if (!std::isfinite(nearH.w) || std::abs(nearH.w) <= 1e-12 || !std::isfinite(farH.w) ||
                std::abs(farH.w) <= 1e-12)
                throw Error::Exception(Error::Code::CONFIG, "Camera frustum corners are degenerate");
            nearCorners[i] = glm::dvec3(nearH) / nearH.w;
            farCorners[i] = glm::dvec3(farH) / farH.w;
        }

        const double nearClip = double(cameraNear);
        const double farClip = std::max(std::min(double(cameraFar), double(settings.maxDistance)),
                                        nearClip + 1e-3 * double(settings.cascadeCount));
        const double depthSpan = double(cameraFar) - nearClip;
        const double lambda = double(settings.cascadeSplitLambda);
        const glm::dmat4 lightBasis = lightBasisMatrix(forward);

        bool hasCasterLightBounds = false;
        glm::dvec3 casterLsMin{};
        glm::dvec3 casterLsMax{};
        if (casterBounds && isValidAabbForFitting(*casterBounds))
        {
            transformAabbToLightSpace(*casterBounds, lightBasis, casterLsMin, casterLsMax);
            hasCasterLightBounds = std::isfinite(casterLsMin.z) && std::isfinite(casterLsMax.z);
        }

        ShadowFrame frame{};
        frame.depthBias = settings.depthBias;
        frame.normalBias = settings.normalBias;
        frame.resolution = settings.resolution;
        frame.cascadeCount = settings.cascadeCount;
        frame.maxDistance = settings.maxDistance;

        double prevSplit = nearClip;
        for (uint32_t cascade = 0; cascade < settings.cascadeCount; ++cascade)
        {
            const double p = double(cascade + 1u) / double(settings.cascadeCount);
            const double logSplit = nearClip * std::pow(farClip / nearClip, p);
            const double uniSplit = nearClip + (farClip - nearClip) * p;
            const double split = lambda * logSplit + (1.0 - lambda) * uniSplit;

            const double t0 = std::clamp((prevSplit - nearClip) / depthSpan, 0.0, 1.0);
            const double t1 = std::clamp((split - nearClip) / depthSpan, 0.0, 1.0);

            std::array<glm::dvec3, 8> corners{};
            glm::dvec3 center(0.0);
            for (size_t i = 0; i < 4; ++i)
            {
                const glm::dvec3 ray = farCorners[i] - nearCorners[i];
                corners[i] = nearCorners[i] + ray * t0;
                corners[4 + i] = nearCorners[i] + ray * t1;
                center += corners[i] + corners[4 + i];
            }
            center /= 8.0;

            double radius = 0.01;
            for (const auto& corner : corners)
                radius = std::max(radius, glm::length(corner - center));

            // 在光源正交平面执行纹素对齐，确保相机旋转或平移时阴影纹素网稳定不抖动。
            const glm::dvec3 centerLs(lightBasis * glm::dvec4(center, 1.0));
            const double texelSize = (2.0 * radius) / double(settings.resolution);
            const double snappedCx = std::floor(centerLs.x / texelSize) * texelSize;
            const double snappedCy = std::floor(centerLs.y / texelSize) * texelSize;

            double minZ = centerLs.z - radius;
            double maxZ = centerLs.z + radius;
            if (hasCasterLightBounds)
            {
                minZ = std::min(minZ, casterLsMin.z);
                maxZ = std::max(maxZ, casterLsMax.z);
            }
            else
            {
                // 未提供投射物边界时，沿光线反方向拉长近平面以覆盖视锥外遮挡体。
                maxZ += std::max(radius * 2.0, 30.0);
                minZ -= radius;
            }

            frame.cascadeViewProjections[cascade] =
                buildOrthoFromLightSpaceBox(lightBasis, snappedCx, snappedCy, radius, radius, minZ, maxZ);
            frame.cascadeSplits[cascade] = float(split);
            prevSplit = split;
        }
        frame.viewProjection = frame.cascadeViewProjections[0];
        return frame;
    }
}  // namespace Incubator::Render
