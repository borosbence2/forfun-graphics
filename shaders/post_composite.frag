#version 450

layout(set = 0, binding = 0) uniform sampler2D hdrScene;
layout(set = 0, binding = 1) uniform sampler2D bloomTex;

layout(push_constant) uniform CompositeParams {
    float exposure;
    float bloomStrength;
} params;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 acesTonemap(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture(hdrScene, vUV).rgb * params.exposure;
    vec3 bloom = texture(bloomTex, vUV).rgb;
    vec3 color = hdr + bloom * params.bloomStrength;
    outColor = vec4(acesTonemap(color), 1.0);
}
