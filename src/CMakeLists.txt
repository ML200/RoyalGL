set(ROYALGL_SOURCES
    main.cpp

    core/Window.cpp
    core/Application.cpp

    gfx/Shader.cpp
    gfx/Buffer.cpp
    gfx/Texture.cpp
    gfx/FullscreenPass.cpp

    scene/Camera.cpp
    scene/Scene.cpp
    scene/GLTFLoader.cpp

    bvh/BVHBuilder.cpp

    optics/LensSystem.cpp

    pathtracer/PathTracer.cpp
    pathtracer/LightTree.cpp

    denoise/Denoiser.cpp

    io/ImageExport.cpp

    ui/UILayer.cpp
)

set(ROYALGL_HEADERS
    core/Types.h
    core/Log.h
    core/Window.h
    core/Application.h

    gfx/GLCheck.h
    gfx/GPUTypes.h
    gfx/Shader.h
    gfx/Buffer.h
    gfx/Texture.h
    gfx/FullscreenPass.h

    scene/Camera.h
    scene/Material.h
    scene/Mesh.h
    scene/Scene.h
    scene/GLTFLoader.h

    bvh/BVHBuilder.h

    optics/Glass.h
    optics/LensSystem.h

    pathtracer/RenderSettings.h
    pathtracer/PathTracer.h
    pathtracer/LightTree.h

    denoise/Denoiser.h

    io/ImageExport.h

    ui/UILayer.h
)

add_executable(RoyalGL ${ROYALGL_SOURCES} ${ROYALGL_HEADERS})

target_include_directories(RoyalGL PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(RoyalGL PRIVATE
    glfw
    libglew_static
    glm::glm
    imgui
    cgltf
    stb
    tinybvh
)

target_compile_definitions(RoyalGL PRIVATE
    GLFW_INCLUDE_NONE
    ROYALGL_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders/"
    ROYALGL_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/"
)

if (ROYALGL_HAS_OIDN)
    target_compile_definitions(RoyalGL PRIVATE ROYALGL_HAS_OIDN=1)
    target_link_libraries(RoyalGL PRIVATE OpenImageDenoise)
    if (ROYALGL_OIDN_DLLS)
        add_custom_command(TARGET RoyalGL POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ROYALGL_OIDN_DLLS} $<TARGET_FILE_DIR:RoyalGL>
            COMMENT "RoyalGL: copying OpenImageDenoise runtime DLLs"
        )
    endif ()
endif ()

if (MSVC)
    target_compile_options(RoyalGL PRIVATE /W4 /permissive-)
    set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT RoyalGL)
else ()
    target_compile_options(RoyalGL PRIVATE -Wall -Wextra)
endif ()

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND WIN32)
    # MinGW builds dynamically link the compiler's own runtime DLLs
    # (libstdc++/libgcc_s/libwinpthread). Without a copy sitting next to the
    # executable, Windows' DLL search can pick up a mismatched version from
    # some other MinGW/MSYS install on PATH (e.g. Git for Windows' bundled
    # mingw64), which fails at process startup with
    # STATUS_ENTRYPOINT_NOT_FOUND. Ship the exact matching DLLs from the
    # compiler's own directory instead of relying on PATH.
    get_filename_component(ROYALGL_MINGW_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
    foreach (RUNTIME_DLL libstdc++-6.dll libgcc_s_seh-1.dll libgcc_s_dw2-1.dll libwinpthread-1.dll)
        if (EXISTS ${ROYALGL_MINGW_BIN_DIR}/${RUNTIME_DLL})
            add_custom_command(TARGET RoyalGL POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ROYALGL_MINGW_BIN_DIR}/${RUNTIME_DLL} $<TARGET_FILE_DIR:RoyalGL>
                COMMENT "RoyalGL: copying MinGW runtime ${RUNTIME_DLL}"
            )
        endif ()
    endforeach ()
endif ()

# Shaders and assets are loaded from disk at runtime (not embedded), so keep a
# copy next to the executable and re-sync it on every build.
add_custom_command(TARGET RoyalGL POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/shaders $<TARGET_FILE_DIR:RoyalGL>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:RoyalGL>/assets
    COMMENT "RoyalGL: syncing shaders/ and assets/ next to the executable"
)

foreach (SRC ${ROYALGL_SOURCES} ${ROYALGL_HEADERS})
    get_filename_component(SRC_DIR ${SRC} DIRECTORY)
    string(REPLACE "/" "\\" GROUP_NAME "${SRC_DIR}")
    source_group("${GROUP_NAME}" FILES ${SRC})
endforeach ()
