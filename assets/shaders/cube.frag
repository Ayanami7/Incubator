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

layout(set = 0, binding = 1) uniform sampler2D shadowDepth;
layout(set = 1, binding = 0) uniform sampler2D colorTexture;
layout(set = 1, binding = 1, std140) uniform MaterialData
{
    vec4 colorShading;
} material;
layout(location = 0) in vec2 uv;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) in vec3 worldPosition;
layout(location = 0) out vec4 fragmentColor;

float shadowVisibility()
{
    if (frame.shadowParams.y == 0.0)
        return 1.0;
    vec4 clip = frame.shadowViewProjection * vec4(worldPosition, 1.0);
    if (clip.w <= 0.0)
        return 1.0;
    vec3 ndc = clip.xyz / clip.w;
    if (any(lessThan(ndc, vec3(-1.0, -1.0, 0.0))) || any(greaterThan(ndc, vec3(1.0))))
        return 1.0;
    vec2 shadowUv = ndc.xy * 0.5 + 0.5;
    // 阴影图仅有基础层，显式读取以避免范围分支中的隐式导数依赖。
    float storedDepth = textureLod(shadowDepth, shadowUv, 0.0).r;
    return ndc.z - frame.shadowParams.x <= storedDepth ? 1.0 : 0.0;
}

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
                       frame.lightColorAmbient.rgb * frame.lightDirectionIntensity.w * diffuse * shadowVisibility();
    }
    linearColor = clamp(linearColor, 0.0, 1.0);
    vec3 encoded = mix(1.055 * pow(linearColor, vec3(1.0 / 2.4)) - 0.055,
                       12.92 * linearColor, lessThanEqual(linearColor, vec3(0.0031308)));
    fragmentColor = vec4(encoded, 1.0);
}
