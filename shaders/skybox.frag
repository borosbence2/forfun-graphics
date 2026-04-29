#version 450

layout(set = 0, binding = 1) uniform samplerCube envCube;

layout(location = 0) in  vec3 vDir;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 color = texture(envCube, normalize(vDir)).rgb;
    outColor = vec4(color, 1.0);
}
