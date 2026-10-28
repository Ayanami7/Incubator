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

layout(set = 1, binding = 0) uniform sampler2D colorTexture;
layout(set = 1, binding = 1, std140) uniform MaterialData
{
    vec4 colorShading;
} material;
layout(location = 0) in vec2 uv;
layout(location = 1) in vec3 worldNormal;
layout(location = 0) out vec4 fragmentColor;

void main()
{
    vec3 linearColor = texture(colorTexture, uv).rgb * material.colorShading.rgb;
    if (material.colorShading.w == 1.0)
    {
        float lengthSquared = dot(worldNormal, worldNormal);
        float diffuse = 0.0;
        if (lengthSquared > 0.0)
            diffuse = max(dot(worldNormal * inversesqrt(lengthSquared), -frame.lightDirectionIntensity.xyz), 0.0);
        linearColor *= vec3(frame.lightColorAmbient.w) +
                       frame.lightColorAmbient.rgb * frame.lightDirectionIntensity.w * diffuse;
    }
    linearColor = clamp(linearColor, 0.0, 1.0);
    vec3 encoded = mix(1.055 * pow(linearColor, vec3(1.0 / 2.4)) - 0.055,
                       12.92 * linearColor, lessThanEqual(linearColor, vec3(0.0031308)));
    fragmentColor = vec4(encoded, 1.0);
}
