#version 450

layout(set = 1, binding = 0) uniform sampler2D colorTexture;
layout(set = 1, binding = 1, std140) uniform MaterialData
{
    vec4 colorShading;
    vec4 surfaceParams;
    vec4 emissiveAlpha;
    vec4 alphaParams;
} material;

layout(location = 0) in vec2 uv;

void main()
{
    if (texture(colorTexture, uv).a * material.emissiveAlpha.a < material.alphaParams.x)
        discard;
}
