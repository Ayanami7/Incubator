#version 450

layout(set = 1, binding = 0) uniform sampler2D colorTexture;
layout(set = 1, binding = 1, std140) uniform MaterialData
{
    vec4 color;
} material;
layout(location = 0) in vec2 uv;
layout(location = 1) in vec3 worldNormal;
layout(location = 0) out vec4 fragmentColor;

void main()
{
    vec3 linearColor = clamp(texture(colorTexture, uv).rgb * material.color.rgb, 0.0, 1.0);
    vec3 encoded = mix(1.055 * pow(linearColor, vec3(1.0 / 2.4)) - 0.055,
                       12.92 * linearColor, lessThanEqual(linearColor, vec3(0.0031308)));
    fragmentColor = vec4(encoded, 1.0);
}
