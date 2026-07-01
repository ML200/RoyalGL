#pragma once

#include <filesystem>
#include "scene/Scene.h"

namespace RoyalGL
{
    class GLTFLoader
    {
    public:
        // Loads a .gltf/.glb file, flattening the scene graph into
        // world-space triangles in `outScene`. Returns false (leaving
        // `outScene` untouched) on failure; check the log for the reason.
        static bool Load(const std::filesystem::path& path, Scene& outScene);
    };
}
