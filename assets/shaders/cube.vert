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
    mat3 normalMatrix;
} object;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec3 inNormal;
layout(location = 0) out vec2 uv;
layout(location = 1) out vec3 worldNormal;
layout(location = 2) out vec3 worldPosition;

void main()
{
    vec4 world = object.model * vec4(inPosition, 1.0);
    gl_Position = frame.projection * frame.view * world;
    worldPosition = world.xyz;
    uv = inUv;
    vec3 normal = object.normalMatrix * inNormal;
    float scale = max(max(abs(normal.x), abs(normal.y)), abs(normal.z));
    worldNormal = scale > 0.0 ? normalize(normal / scale) : vec3(0.0);
}
