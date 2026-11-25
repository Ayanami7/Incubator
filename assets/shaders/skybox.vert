#version 450

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
} frame;

layout(location = 0) out vec3 worldDirection;

void main()
{
    vec2 pos = vec2((gl_VertexIndex == 1) ? 3.0 : -1.0, (gl_VertexIndex == 2) ? 3.0 : -1.0);
    gl_Position = vec4(pos, 1.0, 1.0);

    vec4 viewPos = inverse(frame.projection) * vec4(pos, 1.0, 1.0);
    vec3 viewDir = abs(viewPos.w) > 1e-6 ? (viewPos.xyz / viewPos.w) : viewPos.xyz;
    worldDirection = inverse(mat3(frame.view)) * viewDir;
}
