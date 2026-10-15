#version 450

layout(set = 0, binding = 1) uniform sampler2D colorTexture;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragmentColor;

void main()
{
    vec3 linearColor = clamp(texture(colorTexture, uv).rgb, 0.0, 1.0);
    vec3 encoded = mix(1.055 * pow(linearColor, vec3(1.0 / 2.4)) - 0.055,
                       12.92 * linearColor, lessThanEqual(linearColor, vec3(0.0031308)));
    fragmentColor = vec4(encoded, 1.0);
}
