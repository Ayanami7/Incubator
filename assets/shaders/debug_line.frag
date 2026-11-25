#version 450

layout(location = 0) in vec4 lineColor;
layout(location = 0) out vec4 fragmentColor;

void main()
{
    fragmentColor = vec4(max(lineColor.rgb, vec3(0.0)), clamp(lineColor.a, 0.0, 1.0));
}
