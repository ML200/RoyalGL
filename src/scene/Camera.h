#pragma once

#include <glm/glm.hpp>

namespace RoyalGL
{
    // Free-fly camera. Stores position/target directly (rather than
    // yaw/pitch/distance) so it composes trivially with glTF camera nodes;
    // Forward/Right/Up are derived on demand.
    class Camera
    {
    public:
        glm::vec3 position{0.0f, 1.0f, 4.0f};
        glm::vec3 target{0.0f, 0.0f, 0.0f};
        glm::vec3 worldUp{0.0f, 1.0f, 0.0f};
        float verticalFovDegrees = 45.0f;

        glm::vec3 Forward() const;
        glm::vec3 Right() const;
        glm::vec3 Up() const;

        // Mouse-look: rotates the view direction about the (fixed) position.
        void Look(float dYawDegrees, float dPitchDegrees);
        // Scroll-wheel zoom: moves position toward/away from target.
        void Dolly(float dScroll);
        // WASD-style fly movement, in the camera's local axes:
        // x = right, y = world-up, z = forward.
        void Move(const glm::vec3& localDelta);

        bool operator==(const Camera& other) const;
        bool operator!=(const Camera& other) const { return !(*this == other); }
    };
}
