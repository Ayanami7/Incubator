#version 450
#extension GL_GOOGLE_include_directive : require

#include "frame_data.glsl"

layout(push_constant) uniform ObjectData
{
    mat4 model;
    uvec4 cascadeParams;
} object;

layout(location = 0) in vec3 inPosition;

void main()
{
    gl_Position = frame.cascadeViewProjections[object.cascadeParams.x] * object.model * vec4(inPosition, 1.0);
}
