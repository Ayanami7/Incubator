#version 450

layout(set = 0, binding = 3) uniform samplerCube iblPrefiltered;

layout(location = 0) in vec3 worldDirection;
layout(location = 0) out vec4 fragmentColor;

void main()
{
    vec3 dir = dot(worldDirection, worldDirection) > 0.0 ? normalize(worldDirection) : vec3(0.0, 0.0, -1.0);
    vec3 envColor = textureLod(iblPrefiltered, dir, 0.0).rgb;
    fragmentColor = vec4(max(envColor, vec3(0.0)), 1.0);
}
