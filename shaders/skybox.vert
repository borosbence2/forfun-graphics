#version 450

// Fullscreen triangle. Reconstruct world-space view direction at the far
// plane via inverse(viewProj), so the cubemap can be sampled in the frag
// shader without geometry.

layout(set = 0, binding = 0) uniform UBO {
    mat4 viewProj;
    mat4 invViewProj;
    mat4 lightViewProj;
    vec4 cameraPos;
} ubo;

layout(location = 0) out vec3 vDir;

void main() {
    const vec2 verts[3] = vec2[](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0)
    );
    vec2 p = verts[gl_VertexIndex];

    // z = 1 places the fragment exactly at the far plane.
    gl_Position = vec4(p, 1.0, 1.0);

    vec4 worldFar = ubo.invViewProj * vec4(p, 1.0, 1.0);
    worldFar /= worldFar.w;
    vDir = worldFar.xyz - ubo.cameraPos.xyz;
}
