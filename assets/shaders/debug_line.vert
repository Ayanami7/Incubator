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

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 lineColor;

void main()
{
    gl_Position = frame.projection * frame.view * vec4(inPosition, 1.0);
    lineColor = inColor;
}
