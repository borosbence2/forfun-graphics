cmake_minimum_required(VERSION 3.22)
project(forfun_graphics LANGUAGES CXX)

# ============================================================================
# Project-wide settings
# ============================================================================
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(cmake/CPM.cmake)
include(ForfunShaders)  # provides add_shader()

# ============================================================================
# Dependencies (shared across forfun_core, helmet_demo, and any downstream app)
# ============================================================================
find_package(Vulkan 1.3 REQUIRED)

set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)

CPMAddPackage(
    NAME              glfw
    GITHUB_REPOSITORY glfw/glfw
    GIT_TAG           3.4
)

CPMAddPackage(
    NAME              vk-bootstrap
    GITHUB_REPOSITORY charles-lunarg/vk-bootstrap
    GIT_TAG           v1.3.302
)

CPMAddPackage(
    NAME              VulkanMemoryAllocator
    GITHUB_REPOSITORY GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
    GIT_TAG           v3.1.0
)

CPMAddPackage(
    NAME              glm
    GITHUB_REPOSITORY g-truc/glm
    GIT_TAG           1.0.1
)

CPMAddPackage(
    NAME              tinygltf
    GITHUB_REPOSITORY syoyo/tinygltf
    GIT_TAG           v2.9.5
    DOWNLOAD_ONLY     YES
)

CPMAddPackage(
    NAME              imgui
    GITHUB_REPOSITORY ocornut/imgui
    GIT_TAG           v1.90.9
)

# ============================================================================
# forfun_core — static rendering library
# ----------------------------------------------------------------------------
# Reusable engine pieces: Vulkan resource helpers, material system, IBL
# precompute, glTF loading, pipeline factories. Does NOT own a window, an
# instance/device, or a frame loop yet — those will be extracted from
# helmet_demo's main.cpp incrementally in Phase 0B as alpine-sun needs them.
# ============================================================================
add_library(forfun_core STATIC
    src/vk_helpers.cpp
    src/gltf_loader.cpp
    src/ibl.cpp
    src/pipelines.cpp
    src/material_package.cpp
    src/material.cpp
    src/material_instance.cpp
    src/device.cpp
    src/swapchain.cpp
    src/vma_impl.cpp
    src/tinygltf_impl.cpp
)

target_include_directories(forfun_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PRIVATE
        "${tinygltf_SOURCE_DIR}"
)

target_link_libraries(forfun_core
    PUBLIC
        Vulkan::Vulkan
        GPUOpen::VulkanMemoryAllocator
        glm::glm
    PRIVATE
        vk-bootstrap::vk-bootstrap   # device.cpp uses vkb::InstanceBuilder etc.
)

# IBL precompute shaders are embedded into the library (ibl.cpp #includes them).
add_shader(forfun_core
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/equirect_to_cube.comp" equirect_to_cube_spv)
add_shader(forfun_core
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/prefilter_env.comp"    prefilter_env_spv)
add_shader(forfun_core
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/brdf_lut.comp"         brdf_lut_spv)
add_shader(forfun_core
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sh_project.comp"       sh_project_spv)

if(MSVC)
    target_compile_options(forfun_core PRIVATE
        /W4 /permissive-
        # Silence VMA's noisy /W4 warnings (third-party header)
        /wd4100  # unreferenced formal parameter
        /wd4127  # conditional expression is constant
        /wd4189  # local variable initialized but not referenced
        /wd4324  # structure padded due to alignment specifier
        /wd4505  # unreferenced function with internal linkage removed
    )
else()
    target_compile_options(forfun_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# An alias lets downstream code link as forfun::core regardless of layout.
add_library(forfun::core ALIAS forfun_core)

# ============================================================================
# helmet_demo — original PBR demo, downstream of forfun_core
# ----------------------------------------------------------------------------
# Owns the window, GLFW + ImGui setup, the swapchain, and the render loop. All
# of this is the work that Phase 0B will gradually pull into forfun_core.
# ============================================================================

# ---- Asset downloads (demo-only) -------------------------------------------
set(HELMET_MODEL_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/assets/DamagedHelmet")
set(HELMET_MODEL_PATH "${HELMET_MODEL_DIR}/DamagedHelmet.glb")
if(NOT EXISTS "${HELMET_MODEL_PATH}")
    file(MAKE_DIRECTORY "${HELMET_MODEL_DIR}")
    message(STATUS "Downloading DamagedHelmet.glb (Khronos sample asset)...")
    file(DOWNLOAD
        "https://github.com/KhronosGroup/glTF-Sample-Models/raw/main/2.0/DamagedHelmet/glTF-Binary/DamagedHelmet.glb"
        "${HELMET_MODEL_PATH}"
        SHOW_PROGRESS
        STATUS HELMET_DL_STATUS
    )
    list(GET HELMET_DL_STATUS 0 HELMET_DL_CODE)
    if(NOT HELMET_DL_CODE EQUAL 0)
        list(GET HELMET_DL_STATUS 1 HELMET_DL_MESSAGE)
        message(FATAL_ERROR "Failed to download DamagedHelmet.glb: ${HELMET_DL_MESSAGE}")
    endif()
endif()

set(ENV_HDR_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/assets/env")
set(ENV_HDR_PATH "${ENV_HDR_DIR}/environment.hdr")
if(NOT EXISTS "${ENV_HDR_PATH}")
    file(MAKE_DIRECTORY "${ENV_HDR_DIR}")
    message(STATUS "Downloading HDR environment (PolyHaven CC0)...")
    file(DOWNLOAD
        "https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/1k/symmetrical_garden_02_1k.hdr"
        "${ENV_HDR_PATH}"
        SHOW_PROGRESS
        STATUS ENV_DL_STATUS
    )
    list(GET ENV_DL_STATUS 0 ENV_DL_CODE)
    if(NOT ENV_DL_CODE EQUAL 0)
        list(GET ENV_DL_STATUS 1 ENV_DL_MESSAGE)
        file(REMOVE "${ENV_HDR_PATH}")
        message(FATAL_ERROR
            "Failed to download HDR environment: ${ENV_DL_MESSAGE}\n"
            "Place any equirect HDR file at ${ENV_HDR_PATH} and reconfigure.")
    endif()
endif()

# ---- Generated config header: paths resolved at configure time -------------
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/config.h"
    @ONLY
)

# ---- ImGui static lib (demo-only UI) ---------------------------------------
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC
    Vulkan::Vulkan
    glfw
)

# ---- helmet_demo executable ------------------------------------------------
add_executable(helmet_demo src/main.cpp)

target_link_libraries(helmet_demo PRIVATE
    forfun_core
    imgui
    glfw
    vk-bootstrap::vk-bootstrap
)

target_include_directories(helmet_demo PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

# Demo's own shaders (skybox + post-processing pipeline).
add_shader(helmet_demo "${CMAKE_CURRENT_SOURCE_DIR}/shaders/post.vert"           post_vert_spv)
add_shader(helmet_demo "${CMAKE_CURRENT_SOURCE_DIR}/shaders/bloom_extract.frag"  bloom_extract_frag_spv)
add_shader(helmet_demo "${CMAKE_CURRENT_SOURCE_DIR}/shaders/bloom_blur.frag"     bloom_blur_frag_spv)
add_shader(helmet_demo "${CMAKE_CURRENT_SOURCE_DIR}/shaders/post_composite.frag" post_composite_frag_spv)
add_shader(helmet_demo "${CMAKE_CURRENT_SOURCE_DIR}/shaders/skybox.vert"         skybox_vert_spv)
add_shader(helmet_demo "${CMAKE_CURRENT_SOURCE_DIR}/shaders/skybox.frag"         skybox_frag_spv)

# Bake the matc path so the runtime can invoke it for hot-reload.
target_compile_definitions(helmet_demo PRIVATE
    MATC_EXE_PATH="$<TARGET_FILE:forfun_matc>"
)
add_dependencies(helmet_demo forfun_matc)

if(MSVC)
    target_compile_options(helmet_demo PRIVATE
        /W4 /permissive-
        /wd4100 /wd4127 /wd4189 /wd4324 /wd4505
    )
endif()

# ---- Helmet material package compiled as part of the demo build ------------
set(HELMET_MAT_PATH  "${CMAKE_CURRENT_SOURCE_DIR}/materials/damaged_helmet.mat")
set(HELMET_FPKG_PATH "${CMAKE_CURRENT_SOURCE_DIR}/materials/damaged_helmet.fpkg")
add_custom_command(
    OUTPUT  ${HELMET_FPKG_PATH}
    COMMAND $<TARGET_FILE:forfun_matc> ${HELMET_MAT_PATH} -o ${HELMET_FPKG_PATH}
    DEPENDS forfun_matc ${HELMET_MAT_PATH}
    COMMENT "Compiling material package ${HELMET_MAT_PATH} -> damaged_helmet.fpkg"
    VERBATIM
)
add_custom_target(helmet_fpkg DEPENDS ${HELMET_FPKG_PATH})
add_dependencies(helmet_demo helmet_fpkg)

# ============================================================================
# Tools
# ============================================================================

# ---- Material package round-trip test --------------------------------------
add_executable(material_package_test
    tools/material_package_test.cpp
    src/material_package.cpp
)
target_include_directories(material_package_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
if(MSVC)
    target_compile_options(material_package_test PRIVATE /W4 /permissive-)
endif()

# ---- Material compiler -----------------------------------------------------
add_executable(forfun_matc
    tools/matc/main.cpp
    tools/matc/mat_parser.cpp
    tools/matc/codegen.cpp
    tools/matc/spirv_compiler.cpp
    src/material_package.cpp
)
target_include_directories(forfun_matc PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/matc"
    "${tinygltf_SOURCE_DIR}"
)
cmake_path(CONVERT "${GLSLANG_VALIDATOR}" TO_CMAKE_PATH_LIST _matc_glslang_path NORMALIZE)
target_compile_definitions(forfun_matc PRIVATE
    GLSLANG_VALIDATOR_PATH="${_matc_glslang_path}"
)
if(MSVC)
    target_compile_options(forfun_matc PRIVATE /W4 /permissive-)
endif()
