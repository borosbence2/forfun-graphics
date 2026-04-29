#version 450

layout(set = 0, binding = 0) uniform sampler2D hdrScene;

layout(push_constant) uniform ExtractParams {
    float threshold;
} params;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 color = texture(hdrScene, vUV).rgb;
    float brightness = max(max(color.r, color.g), color.b);
    vec3 bloom = brightness > params.threshold ? color : vec3(0.0);
    outColor = vec4(bloom, 1.0);
}
