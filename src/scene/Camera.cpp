#include "scene/Camera.h"

#include <algorithm>
#include <cmath>

namespace RoyalGL
{
    glm::vec3 Camera::Forward() const
    {
        return glm::normalize(target - position);
    }

    glm::vec3 Camera::Right() const
    {
        return glm::normalize(glm::cross(Forward(), worldUp));
    }

    glm::vec3 Camera::Up() const
    {
        return glm::normalize(glm::cross(Right(), Forward()));
    }

    void Camera::Look(float dYawDegrees, float dPitchDegrees)
    {
        glm::vec3 offset = target - position;
        float distance = std::max(glm::length(offset), 1e-4f);

        float yaw = std::atan2(offset.x, offset.z);
        float pitch = std::asin(std::clamp(offset.y / distance, -1.0f, 1.0f));

        yaw += glm::radians(dYawDegrees);
        pitch += glm::radians(dPitchDegrees);
        pitch = std::clamp(pitch, glm::radians(-89.0f), glm::radians(89.0f));

        offset.x = distance * std::cos(pitch) * std::sin(yaw);
        offset.y = distance * std::sin(pitch);
        offset.z = distance * std::cos(pitch) * std::cos(yaw);

        target = position + offset;
    }

    void Camera::Dolly(float dScroll)
    {
        glm::vec3 offset = position - target;
        float radius = glm::length(offset);
        float newRadius = std::max(radius * (1.0f - dScroll * 0.1f), 0.05f);

        glm::vec3 direction = radius > 1e-4f ? offset / radius : offset;
        position = target + direction * newRadius;
    }

    void Camera::Move(const glm::vec3& localDelta)
    {
        glm::vec3 delta = Right() * localDelta.x + worldUp * localDelta.y + Forward() * localDelta.z;
        position += delta;
        target += delta;
    }

    bool Camera::operator==(const Camera& other) const
    {
        return position == other.position
            && target == other.target
            && worldUp == other.worldUp
            && verticalFovDegrees == other.verticalFovDegrees;
    }
}
