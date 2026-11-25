#version 450

struct GpuLocalLight
{
    vec4 positionRange;
    vec4 colorIntensity;
    vec4 directionType;
    vec4 coneParams;
};

layout(set = 0, binding = 0, std140) uniform FrameData
{
    mat4 view;
    mat4 projection;
    vec4 lightDirectionIntensity;
    vec4 lightColorAmbient;
    mat4 cascadeViewProjections[4];
    vec4 cascadeSplits;
    vec4 shadowParams;
    vec4 shadowMeta;
    vec4 cameraPosition;
    vec4 iblParams;
    uvec4 localLightMeta;
    GpuLocalLight localLights[16];
} frame;

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
