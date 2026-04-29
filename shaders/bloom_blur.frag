#version 450

layout(set = 0, binding = 0) uniform sampler2D imageTex;

layout(push_constant) uniform BlurParams {
    vec2 direction;
} params;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 texel = 1.0 / vec2(textureSize(imageTex, 0));
    vec2 stepUv = params.direction * texel;

    vec3 result = texture(imageTex, vUV).rgb * 0.227027;
    result += texture(imageTex, vUV + stepUv * 1.384615).rgb * 0.316216;
    result += texture(imageTex, vUV - stepUv * 1.384615).rgb * 0.316216;
    result += texture(imageTex, vUV + stepUv * 3.230769).rgb * 0.070270;
    result += texture(imageTex, vUV - stepUv * 3.230769).rgb * 0.070270;

    outColor = vec4(result, 1.0);
}
