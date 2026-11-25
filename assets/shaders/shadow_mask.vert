#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame_data.glsl"

layout(push_constant) uniform ObjectData
{
    mat4 model;
    uvec4 cascadeParams;
} object;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUv;
layout(location = 0) out vec2 uv;

void main()
{
    gl_Position = frame.cascadeViewProjections[object.cascadeParams.x] * object.model * vec4(inPosition, 1.0);
    uv = inUv;
}
