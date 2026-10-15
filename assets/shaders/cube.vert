#version 450

layout(set = 0, binding = 0, std140) uniform CameraData
{
    mat4 view;
    mat4 projection;
} camera;

layout(push_constant) uniform ObjectData
{
    mat4 model;
} object;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUv;
layout(location = 0) out vec2 uv;

void main()
{
    gl_Position = camera.projection * camera.view * object.model * vec4(inPosition, 1.0);
    uv = inUv;
}
