#version 450

layout(set = 0, binding = 0, std140) uniform FrameData
{
    mat4 view;
    mat4 projection;
    vec4 lightDirectionIntensity;
    vec4 lightColorAmbient;
    mat4 shadowViewProjection;
    vec4 shadowParams;
    vec4 cameraPosition;
} frame;

layout(push_constant) uniform ObjectData
{
    mat4 model;
    mat3 normalMatrix;
    vec4 tangentParams;
} object;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec4 inTangent;
layout(location = 0) out vec2 uv;
layout(location = 1) out vec3 worldNormal;
layout(location = 2) out vec3 worldPosition;
layout(location = 3) out vec4 worldTangent;

void main()
{
    vec4 world = object.model * vec4(inPosition, 1.0);
    gl_Position = frame.projection * frame.view * world;
    worldPosition = world.xyz;
    uv = inUv;
    vec3 normal = object.normalMatrix * inNormal;
    float scale = max(max(abs(normal.x), abs(normal.y)), abs(normal.z));
    worldNormal = scale > 0.0 ? normalize(normal / scale) : vec3(0.0);

    vec3 tangent = mat3(object.model) * inTangent.xyz;
    float tangentScale = max(max(abs(tangent.x), abs(tangent.y)), abs(tangent.z));
    float handedness = inTangent.w * object.tangentParams.x;
    worldTangent = (tangentScale > 0.0 && handedness != 0.0)
                       ? vec4(normalize(tangent / tangentScale), handedness < 0.0 ? -1.0 : 1.0)
                       : vec4(0.0);
}
