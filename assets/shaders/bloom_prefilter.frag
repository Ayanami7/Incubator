#version 450

layout(set = 0, binding = 0) uniform sampler2D hdrScene;

layout(push_constant) uniform PrefilterParams
{
    float threshold;
    float knee;
    vec2 srcTexelSize;
} params;

layout(location = 0) out vec4 fragmentColor;

vec3 sampleHdrChecked(vec2 uv)
{
    vec4 s = texture(hdrScene, uv);
    if (s.a <= 0.0)
        return vec3(0.0);
    // 与 display.frag 对称还原半透明预乘像素的非预乘线性色，同时夹紧极值避免半精度溢出与萤火虫闪烁。
    vec3 rgb = s.a < 1.0 ? (s.rgb / s.a) : s.rgb;
    return clamp(rgb, vec3(0.0), vec3(65000.0));
}

float karisWeight(vec3 c)
{
    float peak = max(c.r, max(c.g, c.b));
    return 1.0 / (1.0 + peak);
}

void main()
{
    vec2 dstSize = vec2(max(textureSize(hdrScene, 0) / 2, ivec2(1)));
    vec2 uv = gl_FragCoord.xy / dstSize;
    vec2 d = params.srcTexelSize * 0.5;

    vec3 c0 = sampleHdrChecked(uv + vec2(-d.x, -d.y));
    vec3 c1 = sampleHdrChecked(uv + vec2( d.x, -d.y));
    vec3 c2 = sampleHdrChecked(uv + vec2(-d.x,  d.y));
    vec3 c3 = sampleHdrChecked(uv + vec2( d.x,  d.y));

    float w0 = karisWeight(c0);
    float w1 = karisWeight(c1);
    float w2 = karisWeight(c2);
    float w3 = karisWeight(c3);
    vec3 color = (c0 * w0 + c1 * w1 + c2 * w2 + c3 * w3) / max(w0 + w1 + w2 + w3, 1e-5);

    float brightness = max(color.r, max(color.g, color.b));
    float soft = brightness - params.threshold + params.knee;
    soft = clamp(soft, 0.0, 2.0 * params.knee);
    soft = (soft * soft) / (4.0 * params.knee + 1e-5);
    float contribution = max(soft, brightness - params.threshold) / max(brightness, 1e-5);
    fragmentColor = vec4(color * max(contribution, 0.0), 1.0);
}
