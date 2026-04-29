cmake_minimum_required(VERSION 3.22)
project(forfun_graphics LANGUAGES CXX)

set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(cmake/CPM.cmake)

find_package(Vulkan 1.3 REQUIRED)

set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)

CPMAddPackage(
    NAME        glfw
    GITHUB_REPOSITORY glfw/glfw
    GIT_TAG     3.4
)

CPMAddPackage(
    NAME        vk-bootstrap
    GITHUB_REPOSITORY charles-lunarg/vk-bootstrap
    GIT_TAG     v1.3.302
)

CPMAddPackage(
    NAME        VulkanMemoryAllocator
    GITHUB_REPOSITORY GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
    GIT_TAG     v3.1.0
)

CPMAddPackage(
    NAME        glm
    GITHUB_REPOSITORY g-truc/glm
    GIT_TAG     1.0.1
)

CPMAddPackage(
    NAME        tinygltf
    GITHUB_REPOSITORY syoyo/tinygltf
    GIT_TAG     v2.9.5
    DOWNLOAD_ONLY YES
)

CPMAddPackage(
    NAME        imgui
    GITHUB_REPOSITORY ocornut/imgui
    GIT_TAG     v1.90.9
)

# ---- One-shot asset download: DamagedHelmet.glb ----
set(HELMET_MODEL_DIR  "${CMAKE_SOURCE_DIR}/assets/DamagedHelmet")
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

# ---- One-shot asset download: HDR environment (PolyHaven CC0) ----
set(ENV_HDR_DIR  "${CMAKE_SOURCE_DIR}/assets/env")
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

# ---- Generated config header: paths resolved at configure time ----
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/config.h.in"
    "${CMAKE_BINARY_DIR}/generated/config.h"
    @ONLY
)

add_executable(forfun_graphics
    src/main.cpp
    src/vk_helpers.cpp
    src/gltf_loader.cpp
    src/ibl.cpp
    src/pipelines.cpp
    src/material_package.cpp
    src/material.cpp
    src/material_instance.cpp
    src/vma_impl.cpp
    src/tinygltf_impl.cpp
)

# ---- Material package round-trip test (M8.materials.1) ----
add_executable(material_package_test
    tools/material_package_test.cpp
    src/material_package.cpp
)
target_include_directories(material_package_test PRIVATE src)
if(MSVC)
    target_compile_options(material_package_test PRIVATE /W4 /permissive-)
endif()

# ---- Material compiler (M8.materials.2+) ----
add_executable(forfun_matc
    tools/matc/main.cpp
    tools/matc/mat_parser.cpp
    tools/matc/codegen.cpp
    tools/matc/spirv_compiler.cpp
    src/material_package.cpp
)
target_include_directories(forfun_matc PRIVATE
    src
    tools/matc
    ${tinygltf_SOURCE_DIR}
)
# Bake the glslangValidator path so forfun_matc can invoke it at runtime.
# Use cmake_path to normalise to forward slashes (safe in cmd.exe on Windows).
cmake_path(CONVERT "${GLSLANG_VALIDATOR}" TO_CMAKE_PATH_LIST _matc_glslang_path NORMALIZE)
target_compile_definitions(forfun_matc PRIVATE
    GLSLANG_VALIDATOR_PATH="${_matc_glslang_path}"
)
if(MSVC)
    target_compile_options(forfun_matc PRIVATE /W4 /permissive-)
endif()

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

target_link_libraries(forfun_graphics PRIVATE
    Vulkan::Vulkan
    glfw
    vk-bootstrap::vk-bootstrap
    GPUOpen::VulkanMemoryAllocator
    glm::glm
    imgui
)

target_include_directories(forfun_graphics PRIVATE
    ${tinygltf_SOURCE_DIR}
    "${CMAKE_BINARY_DIR}/generated"
)

# Bake the matc path so the runtime can invoke it for hot-reload.
# We use a generator expression for the target file, and add a build dependency.
target_compile_definitions(forfun_graphics PRIVATE
    MATC_EXE_PATH="$<TARGET_FILE:forfun_matc>"
)
add_dependencies(forfun_graphics forfun_matc)

# Compile the helmet material package as part of the build so a fresh checkout
# can run the renderer without invoking forfun_matc manually.
set(HELMET_MAT_PATH  "${CMAKE_SOURCE_DIR}/materials/damaged_helmet.mat")
set(HELMET_FPKG_PATH "${CMAKE_SOURCE_DIR}/materials/damaged_helmet.fpkg")
add_custom_command(
    OUTPUT  ${HELMET_FPKG_PATH}
    COMMAND $<TARGET_FILE:forfun_matc> ${HELMET_MAT_PATH} -o ${HELMET_FPKG_PATH}
    DEPENDS forfun_matc ${HELMET_MAT_PATH}
    COMMENT "Compiling material package ${HELMET_MAT_PATH} -> damaged_helmet.fpkg"
    VERBATIM
)
add_custom_target(helmet_fpkg DEPENDS ${HELMET_FPKG_PATH})
add_dependencies(forfun_graphics helmet_fpkg)

# ---- Shader compilation: GLSL -> SPIR-V -> C header embedded via #include ----
find_program(GLSLANG_VALIDATOR
    NAMES glslangValidator
    HINTS $ENV{VULKAN_SDK}/Bin $ENV{VULKAN_SDK}/bin
    REQUIRED
)

set(SHADER_HEADER_DIR "${CMAKE_BINARY_DIR}/generated/shaders")
file(MAKE_DIRECTORY ${SHADER_HEADER_DIR})

function(add_shader TARGET SHADER_FILE VAR_NAME)
    get_filename_component(SHADER_NAME ${SHADER_FILE} NAME)
    set(OUT_HEADER "${SHADER_HEADER_DIR}/${SHADER_NAME}.h")
    add_custom_command(
        OUTPUT  ${OUT_HEADER}
        COMMAND ${GLSLANG_VALIDATOR} -V "${CMAKE_SOURCE_DIR}/${SHADER_FILE}"
                --vn ${VAR_NAME} -o ${OUT_HEADER}
        DEPENDS "${CMAKE_SOURCE_DIR}/${SHADER_FILE}"
        COMMENT "Compiling shader ${SHADER_FILE} -> ${SHADER_NAME}.h"
        VERBATIM
    )
    target_sources(${TARGET} PRIVATE ${OUT_HEADER})
endfunction()

add_shader(forfun_graphics shaders/post.vert            post_vert_spv)
add_shader(forfun_graphics shaders/bloom_extract.frag   bloom_extract_frag_spv)
add_shader(forfun_graphics shaders/bloom_blur.frag      bloom_blur_frag_spv)
add_shader(forfun_graphics shaders/post_composite.frag  post_composite_frag_spv)
add_shader(forfun_graphics shaders/skybox.vert          skybox_vert_spv)
add_shader(forfun_graphics shaders/skybox.frag          skybox_frag_spv)
add_shader(forfun_graphics shaders/equirect_to_cube.comp    equirect_to_cube_spv)
add_shader(forfun_graphics shaders/prefilter_env.comp       prefilter_env_spv)
add_shader(forfun_graphics shaders/brdf_lut.comp            brdf_lut_spv)
add_shader(forfun_graphics shaders/sh_project.comp          sh_project_spv)

target_include_directories(forfun_graphics PRIVATE ${SHADER_HEADER_DIR})

if(MSVC)
    target_compile_options(forfun_graphics PRIVATE
        /W4 /permissive-
        # Silence VMA's noisy /W4 warnings (third-party header)
        /wd4100  # unreferenced formal parameter
        /wd4127  # conditional expression is constant
        /wd4189  # local variable initialized but not referenced
        /wd4324  # structure padded due to alignment specifier
        /wd4505  # unreferenced function with internal linkage removed
    )
else()
    target_compile_options(forfun_graphics PRIVATE -Wall -Wextra -Wpedantic)
endif()
