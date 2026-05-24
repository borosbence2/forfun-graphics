# ForfunShaders.cmake
#
# Compiles GLSL shaders to SPIR-V and embeds them as C arrays in headers under
# <listfile-binary-dir>/generated/shaders/. Adds those headers to TARGET's
# sources so build dependencies are wired correctly, and adds the output
# directory to TARGET's PRIVATE include path so consumers can `#include
# "<shader>.h"` directly.
#
# Usage:
#   include(ForfunShaders)
#   add_shader(<target> <absolute-shader-path> <c-variable-name>)
#
# The shader path must be absolute; pass ${CMAKE_CURRENT_SOURCE_DIR}/path/to.frag.

if(NOT DEFINED GLSLANG_VALIDATOR)
    find_program(GLSLANG_VALIDATOR
        NAMES glslangValidator
        HINTS $ENV{VULKAN_SDK}/Bin $ENV{VULKAN_SDK}/bin
        REQUIRED
    )
endif()

function(add_shader TARGET SHADER_FILE VAR_NAME)
    set(_outdir "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders")
    file(MAKE_DIRECTORY "${_outdir}")
    get_filename_component(_shader_name "${SHADER_FILE}" NAME)
    set(_out_header "${_outdir}/${_shader_name}.h")
    add_custom_command(
        OUTPUT  "${_out_header}"
        COMMAND "${GLSLANG_VALIDATOR}" -V "${SHADER_FILE}"
                --vn ${VAR_NAME} -o "${_out_header}"
        DEPENDS "${SHADER_FILE}"
        COMMENT "Compiling shader ${_shader_name} -> ${_shader_name}.h"
        VERBATIM
    )
    target_sources(${TARGET} PRIVATE "${_out_header}")
    target_include_directories(${TARGET} PRIVATE "${_outdir}")
endfunction()
