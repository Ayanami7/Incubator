// 各着色器共享的帧级 Uniform 布局（1536 字节 std140，与 C++ VulkanFrameUniform 严格一致）。
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
