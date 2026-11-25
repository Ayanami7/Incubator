#version 450

layout(set = 0, binding = 0, std140) uniform FrameData
{
    mat4 view;
    mat4 projection;
    vec4 lightDirectionIntensity;
    vec4 lightColorAmbient;
    mat4 shadowViewProjection;
    vec4 shadowParams;
} frame;

layout(push_constant) uniform ObjectData
{
    mat4 model;
} object;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUv;
layout(location = 0) out vec2 uv;

void main()
{
    gl_Position = frame.shadowViewProjection * object.model * vec4(inPosition, 1.0);
    uv = inUv;
}
