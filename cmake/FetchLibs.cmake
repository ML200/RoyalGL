
# ==============================================================================
# RoyalGL third-party dependencies.
#
# Everything is pulled in via FetchContent and pinned to an exact tag/commit so
# builds are reproducible. First configure needs network access; afterwards
# CMake caches the fetched sources under ${CMAKE_BINARY_DIR}/_deps.
# ==============================================================================

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ------------------------------------------------------------------------------
# GLFW - window / input / GL context creation
# ------------------------------------------------------------------------------
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)

# ------------------------------------------------------------------------------
# GLEW (Perlmint/glew-cmake fork) - OpenGL function loader
# Provides the `libglew_static` target (public include dirs + GLEW_STATIC define).
# ------------------------------------------------------------------------------
set(glew-cmake_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(glew-cmake_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ONLY_LIBS ON CACHE BOOL "" FORCE)
set(USE_GLU OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    glew
    GIT_REPOSITORY https://github.com/Perlmint/glew-cmake.git
    GIT_TAG        glew-cmake-2.3.1
    GIT_SHALLOW    TRUE
)

# ------------------------------------------------------------------------------
# GLM - math
# ------------------------------------------------------------------------------
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    TRUE
)

# ------------------------------------------------------------------------------
# Dear ImGui (docking branch) - UI. Ships no CMakeLists.txt, so it is wired up
# as a plain library target further down once its sources are available.
# ------------------------------------------------------------------------------
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.8-docking
    GIT_SHALLOW    TRUE
)

# ------------------------------------------------------------------------------
# cgltf - single-header glTF 2.0 / GLB loader
# ------------------------------------------------------------------------------
FetchContent_Declare(
    cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG        v1.15
    GIT_SHALLOW    TRUE
)

# ------------------------------------------------------------------------------
# stb (stb_image / stb_image_write) - texture loading & PNG export
# Pinned to a specific commit; stb has no tags.
# ------------------------------------------------------------------------------
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        31c1ad37456438565541f4919958214b6e762fb4
)

# ------------------------------------------------------------------------------
# tinybvh - single-header BVH build/traversal library (CPU-side build; GPU
# traversal is implemented by hand in shaders/pathtrace.comp using tinybvh's
# GPU-friendly node layout).
# Pinned to a specific commit; verified against tiny_bvh.h v1.7.1 API.
# ------------------------------------------------------------------------------
FetchContent_Declare(
    tinybvh
    GIT_REPOSITORY https://github.com/jbikker/tinybvh.git
    GIT_TAG        eea6b625f8fdbbd58fe9020b5475c228dec85e19
)

FetchContent_MakeAvailable(glfw glew glm imgui cgltf stb tinybvh)

# ------------------------------------------------------------------------------
# Dear ImGui target (core + GLFW/OpenGL3 backends). No upstream CMake support.
# ------------------------------------------------------------------------------
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw)
if (MSVC)
    target_compile_options(imgui PRIVATE /w) # third-party: silence warnings
endif ()

# ------------------------------------------------------------------------------
# cgltf header-only target
# ------------------------------------------------------------------------------
add_library(cgltf INTERFACE)
target_include_directories(cgltf INTERFACE ${cgltf_SOURCE_DIR})

# ------------------------------------------------------------------------------
# stb header-only target (stb_image.h, stb_image_write.h)
# ------------------------------------------------------------------------------
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${stb_SOURCE_DIR})

# ------------------------------------------------------------------------------
# tinybvh header-only target
# ------------------------------------------------------------------------------
add_library(tinybvh INTERFACE)
target_include_directories(tinybvh INTERFACE ${tinybvh_SOURCE_DIR})

# ------------------------------------------------------------------------------
# Intel Open Image Denoise - optional, fetched as a prebuilt Windows binary
# package (building from source needs ISPC + TBB, which we don't want to force
# on every contributor). If it can't be found, the renderer keeps working and
# the denoiser toggle in the UI simply stays disabled.
# ------------------------------------------------------------------------------
set(ROYALGL_HAS_OIDN OFF)
if (ROYALGL_ENABLE_OIDN)
    if (WIN32)
        FetchContent_Declare(
            oidn
            URL https://github.com/RenderKit/oidn/releases/download/v2.5.0/oidn-2.5.0.x64.windows.zip
        )
        FetchContent_MakeAvailable(oidn)
        list(APPEND CMAKE_PREFIX_PATH ${oidn_SOURCE_DIR})
        find_package(OpenImageDenoise 2.5 CONFIG QUIET)
        if (OpenImageDenoise_FOUND)
            set(ROYALGL_HAS_OIDN ON)
            # DLLs must sit next to the executable at runtime.
            file(GLOB ROYALGL_OIDN_DLLS ${oidn_SOURCE_DIR}/bin/*.dll)
        else ()
            message(WARNING "RoyalGL: OpenImageDenoise package not found after fetch; denoising will be disabled.")
        endif ()
    else ()
        find_package(OpenImageDenoise 2.5 CONFIG QUIET)
        if (OpenImageDenoise_FOUND)
            set(ROYALGL_HAS_OIDN ON)
        else ()
            message(STATUS "RoyalGL: OpenImageDenoise not found on this platform; denoising will be disabled. Install OIDN and set OpenImageDenoise_DIR to enable it.")
        endif ()
    endif ()
else ()
    message(STATUS "RoyalGL: OIDN integration disabled via ROYALGL_ENABLE_OIDN=OFF.")
endif ()
