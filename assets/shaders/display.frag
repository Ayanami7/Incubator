#version 450

layout(set = 0, binding = 0) uniform sampler2D hdrScene;

layout(push_constant) uniform DisplayParams
{
    vec4 clearColor;
    uint toneMapping;
    float exposureScale;
} display;

layout(location = 0) out vec4 fragmentColor;

vec3 encodeSrgb(vec3 linearColor)
{
    linearColor = clamp(linearColor, 0.0, 1.0);
    return mix(1.055 * pow(linearColor, vec3(1.0 / 2.4)) - 0.055,
               12.92 * linearColor, lessThanEqual(linearColor, vec3(0.0031308)));
}

void main()
{
    vec4 hdr = texelFetch(hdrScene, ivec2(gl_FragCoord.xy), 0);
    if (hdr.a <= 0.0)
    {
        fragmentColor = display.clearColor;
        return;
    }

    // 半透明预乘像素（0 < alpha < 1）需先还原非预乘线性色再执行色调映射与 sRGB 编码，避免双重乘 Alpha 或压暗。
    if (hdr.a < 1.0)
    {
        vec3 linear = max(hdr.rgb / hdr.a, vec3(0.0));
        if (display.toneMapping == 1u)
        {
            vec3 exposed = linear * display.exposureScale;
            linear = exposed / (vec3(1.0) + exposed);
        }
        fragmentColor = vec4(mix(display.clearColor.rgb, encodeSrgb(linear), hdr.a), 1.0);
        return;
    }

    vec3 linear = max(hdr.rgb, vec3(0.0));
    if (display.toneMapping == 1u)
    {
        vec3 exposed = linear * display.exposureScale;
        linear = exposed / (vec3(1.0) + exposed);
    }
    fragmentColor = vec4(encodeSrgb(linear), 1.0);
}
