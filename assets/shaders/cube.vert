#version 450

layout(set = 0, binding = 0, std140) uniform TransformData
{
    mat4 model;
    mat4 view;
    mat4 projection;
} transforms;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUv;
layout(location = 0) out vec2 uv;

void main()
{
    gl_Position = transforms.projection * transforms.view * transforms.model * vec4(inPosition, 1.0);
    uv = inUv;
}
