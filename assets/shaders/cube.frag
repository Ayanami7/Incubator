#version 450

layout(set = 0, binding = 1) uniform sampler2D checkerTexture;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragmentColor;

void main()
{
    fragmentColor = texture(checkerTexture, uv);
}
