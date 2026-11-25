#version 450

layout(set = 0, binding = 0) uniform sampler2D hdrScene;
layout(set = 0, binding = 1) uniform sampler2D bloomTexture;

layout(push_constant) uniform DisplayParams
{
    vec4 clearColor;
    uint toneMapping;
    float exposureScale;
    float bloomIntensity;
    float padding;
} display;

layout(location = 0) out vec4 fragmentColor;

vec3 encodeSrgb(vec3 linearColor)
{
    linearColor = clamp(linearColor, 0.0, 1.0);
    return mix(1.055 * pow(linearColor, vec3(1.0 / 2.4)) - 0.055,
               12.92 * linearColor, lessThanEqual(linearColor, vec3(0.0031308)));
}

vec3 decodeSrgb(vec3 srgb)
{
    srgb = clamp(srgb, 0.0, 1.0);
    return mix(pow((srgb + 0.055) / 1.055, vec3(2.4)),
               srgb / 12.92, lessThanEqual(srgb, vec3(0.04045)));
}

vec3 acesFilmic(vec3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 applyToneMapping(vec3 linearColor)
{
    if (display.toneMapping == 1u)
    {
        vec3 exposed = linearColor * display.exposureScale;
        return exposed / (vec3(1.0) + exposed);
    }
    if (display.toneMapping == 2u)
        return acesFilmic(linearColor * display.exposureScale);
    return linearColor;
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    ivec2 size = textureSize(hdrScene, 0);
    vec2 uv = gl_FragCoord.xy / vec2(max(size, ivec2(1)));
    vec4 hdr = texelFetch(hdrScene, pixel, 0);
    vec3 bloom = max(texture(bloomTexture, uv).rgb, vec3(0.0)) * display.bloomIntensity;
    bool hasBloom = any(greaterThan(bloom, vec3(1e-6)));

    if (hdr.a <= 0.0)
    {
        if (!hasBloom)
        {
            fragmentColor = display.clearColor;
            return;
        }
        vec3 bgLinear = decodeSrgb(display.clearColor.rgb) + bloom;
        fragmentColor = vec4(encodeSrgb(applyToneMapping(bgLinear)), display.clearColor.a);
        return;
    }

    // 半透明预乘像素（0 < alpha < 1）需先还原非预乘线性色并叠加 Bloom 再做色调映射，避免双重乘 Alpha 或边缘黑边。
    if (hdr.a < 1.0)
    {
        vec3 fgLinear = max(hdr.rgb / hdr.a, vec3(0.0)) + bloom;
        vec3 fgSrgb = encodeSrgb(applyToneMapping(fgLinear));
        vec3 bgSrgb = hasBloom ? encodeSrgb(applyToneMapping(decodeSrgb(display.clearColor.rgb) + bloom))
                               : display.clearColor.rgb;
        fragmentColor = vec4(mix(bgSrgb, fgSrgb, hdr.a), 1.0);
        return;
    }

    vec3 linear = max(hdr.rgb, vec3(0.0)) + bloom;
    fragmentColor = vec4(encodeSrgb(applyToneMapping(linear)), 1.0);
}
