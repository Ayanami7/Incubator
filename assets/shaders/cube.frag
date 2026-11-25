#version 450

layout(constant_id = 0) const bool LINEAR_HDR_OUTPUT = false;

struct GpuLocalLight
{
    vec4 positionRange;
    vec4 colorIntensity;
    vec4 directionType;
    vec4 coneParams;
};

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
    uvec4 localLightMeta;
    GpuLocalLight localLights[16];
} frame;

layout(set = 0, binding = 1) uniform sampler2DArray shadowDepth;
layout(set = 0, binding = 2) uniform samplerCube iblIrradiance;
layout(set = 0, binding = 3) uniform samplerCube iblPrefiltered;
layout(set = 0, binding = 4) uniform sampler2D iblBrdfLut;
layout(set = 1, binding = 0) uniform sampler2D colorTexture;
layout(set = 1, binding = 1, std140) uniform MaterialData
{
    vec4 colorShading;
    vec4 surfaceParams;
    vec4 emissiveAlpha;
    vec4 alphaParams;
} material;
layout(set = 1, binding = 2) uniform sampler2D normalTexture;
layout(set = 1, binding = 3) uniform sampler2D metallicRoughnessTexture;
layout(set = 1, binding = 4) uniform sampler2D emissiveTexture;
layout(location = 0) in vec2 uv;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) in vec3 worldPosition;
layout(location = 3) in vec4 worldTangent;
layout(location = 0) out vec4 fragmentColor;

float shadowVisibility(vec3 geomNormal)
{
    if (frame.shadowParams.y == 0.0)
        return 1.0;

    float viewDepth = -(frame.view * vec4(worldPosition, 1.0)).z;
    float maxDist = frame.shadowMeta.x;
    if (maxDist > 0.0 && viewDepth >= maxDist)
        return 1.0;

    uint cascadeCount = max(uint(frame.shadowParams.w + 0.5), 1u);
    uint layer = cascadeCount - 1u;
    for (uint i = 0u; i < cascadeCount; ++i)
    {
        if (viewDepth <= frame.cascadeSplits[i])
        {
            layer = i;
            break;
        }
    }

    vec3 samplePos = worldPosition + geomNormal * frame.shadowParams.z;
    vec4 clip = frame.cascadeViewProjections[layer] * vec4(samplePos, 1.0);
    if (clip.w <= 0.0)
        return 1.0;
    vec3 ndc = clip.xyz / clip.w;
    if (any(lessThan(ndc, vec3(-1.0, -1.0, 0.0))) || any(greaterThan(ndc, vec3(1.0))))
        return 1.0;
    vec2 shadowUv = ndc.xy * 0.5 + 0.5;

    // 阴影图仅含基础 mip；使用 3×3 整数 texelFetch 配合连续亚像素 Tent 权重，消除导数依赖与阶梯跳变。
    ivec2 shadowSize = textureSize(shadowDepth, 0).xy;
    vec2 texelPos = shadowUv * vec2(shadowSize) - 0.5;
    ivec2 baseTexel = ivec2(round(texelPos));
    vec2 delta = clamp(texelPos - vec2(baseTexel), vec2(-0.5), vec2(0.5));
    float wx[3] = float[3](0.5 - delta.x, 1.0, 0.5 + delta.x);
    float wy[3] = float[3](0.5 - delta.y, 1.0, 0.5 + delta.y);
    ivec2 maxTexel = shadowSize - 1;
    float receiverDepth = ndc.z - frame.shadowParams.x;

    float visibility = 0.0;
    for (int dy = -1; dy <= 1; ++dy)
    {
        float weightY = wy[dy + 1];
        for (int dx = -1; dx <= 1; ++dx)
        {
            ivec2 coord = clamp(baseTexel + ivec2(dx, dy), ivec2(0), maxTexel);
            float stored = texelFetch(shadowDepth, ivec3(coord, int(layer)), 0).r;
            visibility += (receiverDepth <= stored ? 1.0 : 0.0) * (wx[dx + 1] * weightY);
        }
    }
    visibility *= 0.25;

    if (maxDist > 0.0)
    {
        float fadeStart = maxDist * 0.9;
        if (viewDepth > fadeStart)
        {
            float fade = clamp((viewDepth - fadeStart) / max(maxDist - fadeStart, 1e-4), 0.0, 1.0);
            visibility = mix(visibility, 1.0, fade);
        }
    }
    return visibility;
}

// 按 glTF KHR_lights_punctual 窗口平滑与聚光灯内外锥角平滑计算单盏局部光源的入射方向与辐射度。
bool evalLocalLight(uint index, out vec3 L, out vec3 radiance)
{
    GpuLocalLight light = frame.localLights[index];
    vec3 toLight = light.positionRange.xyz - worldPosition;
    float dist2 = dot(toLight, toLight);
    float range = light.positionRange.w;
    if (range <= 0.0 || dist2 >= range * range)
        return false;
    float dist = sqrt(dist2);
    L = toLight / max(dist, 1e-5);
    float ratio = dist / range;
    float ratio2 = ratio * ratio;
    float window = clamp(1.0 - ratio2 * ratio2, 0.0, 1.0);
    float atten = (window * window) / max(dist2, 1e-4);
    if (light.directionType.w > 0.5)
    {
        float cosTheta = dot(-L, light.directionType.xyz);
        atten *= smoothstep(light.coneParams.y, light.coneParams.x, cosTheta);
    }
    if (atten <= 0.0)
        return false;
    radiance = light.colorIntensity.rgb * (light.colorIntensity.a * atten);
    return true;
}

// 统一的 Cook-Torrance GGX PBR 直接光求值，供方向光与局部光源复用。
vec3 evalPbrDirect(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 baseColor, float roughness, float metallic, vec3 F0)
{
    const float PI = 3.14159265359;
    float NdotL = dot(N, L);
    float NdotV = dot(N, V);
    if (NdotL <= 0.0 || NdotV <= 0.0)
        return vec3(0.0);
    vec3 halfVec = L + V;
    float halfLen2 = dot(halfVec, halfVec);
    vec3 H = halfLen2 > 1e-10 ? halfVec * inversesqrt(halfLen2) : N;
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denom = NdotH * NdotH * (alpha2 - 1.0) + 1.0;
    float D = alpha2 / (PI * denom * denom);

    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float gV = NdotV / (NdotV * (1.0 - k) + k);
    float gL = NdotL / (NdotL * (1.0 - k) + k);
    float G = gV * gL;

    vec3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
    vec3 spec = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-4);
    vec3 kD = (1.0 - F) * (1.0 - metallic);
    return (kD * baseColor / PI + spec) * radiance * NdotL;
}

void main()
{
    vec4 baseSample = texture(colorTexture, uv);
    float outAlpha = baseSample.a * material.emissiveAlpha.a;
    if (material.alphaParams.y == 1.0 && outAlpha < material.alphaParams.x)
        discard;
    vec3 linearColor = baseSample.rgb * material.colorShading.rgb;
    vec3 N0 = material.alphaParams.z != 0.0 && !gl_FrontFacing ? -worldNormal : worldNormal;
    uint localCount = min(frame.localLightMeta.x, 16u);
    if (material.colorShading.w == 1.0)
    {
        float lengthSquared = dot(N0, N0);
        vec3 geomNormal = lengthSquared > 0.0 ? (N0 * inversesqrt(lengthSquared)) : vec3(0.0);
        float diffuse = lengthSquared > 0.0 ? max(dot(geomNormal, -frame.lightDirectionIntensity.xyz), 0.0) : 0.0;
        vec3 direct = frame.lightColorAmbient.rgb * frame.lightDirectionIntensity.w * diffuse *
                      shadowVisibility(geomNormal);
        if (lengthSquared > 0.0)
        {
            for (uint i = 0u; i < localCount; ++i)
            {
                vec3 localDir;
                vec3 radiance;
                if (evalLocalLight(i, localDir, radiance))
                    direct += radiance * max(dot(geomNormal, localDir), 0.0);
            }
        }
        linearColor *= vec3(frame.lightColorAmbient.w) + direct;
    }
    else if (material.colorShading.w == 2.0)
    {
        const float PI = 3.14159265359;
        vec3 baseColor = linearColor;
        vec4 mr = texture(metallicRoughnessTexture, uv);
        uint modeBits = uint(material.surfaceParams.w + 0.5);
        bool fixedRoughness = (modeBits & 1u) != 0u;
        bool fixedMetallic = (modeBits & 2u) != 0u;
        float rawRoughness = fixedRoughness ? material.surfaceParams.y : (mr.g * material.surfaceParams.y);
        float rawMetallic = fixedMetallic ? material.surfaceParams.z : (mr.b * material.surfaceParams.z);
        float roughness = clamp(rawRoughness, 0.04, 1.0);
        float metallic = clamp(rawMetallic, 0.0, 1.0);
        vec3 ambient = baseColor * (1.0 - metallic) * frame.lightColorAmbient.w;

        float normalLen2 = dot(N0, N0);
        if (normalLen2 <= 0.0)
        {
            linearColor = frame.iblParams.x > 0.0 ? vec3(0.0) : ambient;
        }
        else
        {
            N0 *= inversesqrt(normalLen2);
            vec3 N = N0;
            if (material.surfaceParams.x > 0.0 && abs(worldTangent.w) > 0.0 &&
                dot(worldTangent.xyz, worldTangent.xyz) > 0.0)
            {
                vec3 T0 = worldTangent.xyz - N0 * dot(N0, worldTangent.xyz);
                float t0Len2 = dot(T0, T0);
                if (t0Len2 > 1e-10)
                {
                    vec3 T = T0 * inversesqrt(t0Len2);
                    vec3 B = (worldTangent.w < 0.0 ? -1.0 : 1.0) * cross(N0, T);
                    vec3 nTs = texture(normalTexture, uv).xyz * 2.0 - 1.0;
                    nTs.xy *= material.surfaceParams.x;
                    vec3 perturbed = T * nTs.x + B * nTs.y + N0 * nTs.z;
                    float pLen2 = dot(perturbed, perturbed);
                    if (pLen2 > 0.0)
                        N = perturbed * inversesqrt(pLen2);
                }
            }

            vec3 toCamera = frame.cameraPosition.xyz - worldPosition;
            float toCameraLen2 = dot(toCamera, toCamera);
            vec3 V;
            if (toCameraLen2 > 1e-10)
            {
                V = toCamera * inversesqrt(toCameraLen2);
            }
            else
            {
                vec3 vFallback = vec3(frame.view[0][2], frame.view[1][2], frame.view[2][2]);
                float vfLen2 = dot(vFallback, vFallback);
                V = vfLen2 > 0.0 ? vFallback * inversesqrt(vfLen2) : vec3(0.0, 0.0, 1.0);
            }

            vec3 F0 = mix(vec3(0.04), baseColor, metallic);
            if (frame.iblParams.x > 0.0)
            {
                float NdotVClamped = clamp(dot(N, V), 0.0, 1.0);
                vec3 F_ibl = F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - NdotVClamped, 5.0);
                vec3 kD_ibl = (1.0 - F_ibl) * (1.0 - metallic);
                vec3 irradiance = texture(iblIrradiance, N).rgb;
                vec3 diffuseIbl = kD_ibl * irradiance * baseColor;
                vec3 R = reflect(-V, N);
                vec3 prefiltered = textureLod(iblPrefiltered, R, roughness * frame.iblParams.y).rgb;
                vec2 brdf = texture(iblBrdfLut, vec2(NdotVClamped, roughness)).rg;
                vec3 specularIbl = prefiltered * (F0 * brdf.x + brdf.y);
                ambient = diffuseIbl + specularIbl;
            }

            vec3 dirRadiance = frame.lightColorAmbient.rgb * (PI * frame.lightDirectionIntensity.w) *
                               shadowVisibility(N0);
            vec3 direct = evalPbrDirect(N, V, -frame.lightDirectionIntensity.xyz, dirRadiance, baseColor, roughness,
                                        metallic, F0);
            for (uint i = 0u; i < localCount; ++i)
            {
                vec3 localDir;
                vec3 radiance;
                if (evalLocalLight(i, localDir, radiance))
                    direct += evalPbrDirect(N, V, localDir, radiance, baseColor, roughness, metallic, F0);
            }
            linearColor = ambient + direct;
        }
    }
    if (material.colorShading.w == 1.0 || material.colorShading.w == 2.0)
        linearColor += texture(emissiveTexture, uv).rgb * material.emissiveAlpha.rgb;
    float finalAlpha = material.alphaParams.y == 2.0 ? clamp(outAlpha, 0.0, 1.0) : 1.0;
    if (LINEAR_HDR_OUTPUT)
    {
        fragmentColor = vec4(max(linearColor, vec3(0.0)), finalAlpha);
        return;
    }
    linearColor = clamp(linearColor, 0.0, 1.0);
    vec3 encoded = mix(1.055 * pow(linearColor, vec3(1.0 / 2.4)) - 0.055,
                       12.92 * linearColor, lessThanEqual(linearColor, vec3(0.0031308)));
    fragmentColor = vec4(encoded, finalAlpha);
}
