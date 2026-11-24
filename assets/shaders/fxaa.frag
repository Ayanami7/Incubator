#version 450

layout(set = 0, binding = 0) uniform sampler2D ldrScene;

layout(location = 0) out vec4 fragmentColor;

// 固定质量 LDR FXAA 参数：输入为已执行色调映射与 sRGB 编码的 RGBA8 UNORM 图像。
const float FXAA_EDGE_THRESHOLD = 0.0625;
const float FXAA_EDGE_THRESHOLD_MIN = 0.0312;
const float FXAA_SUBPIX_QUALITY = 0.75;
const int FXAA_SEARCH_STEPS = 12;

float rgbLuma(vec3 rgb)
{
    return dot(rgb, vec3(0.299, 0.587, 0.114));
}

float searchStepMultiplier(int index)
{
    if (index < 4)
        return 1.0;
    if (index == 4)
        return 1.5;
    if (index < 9)
        return 2.0;
    if (index == 9)
        return 4.0;
    return 8.0;
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    ivec2 size = textureSize(ldrScene, 0);
    vec2 rcpFrame = 1.0 / vec2(size);
    vec2 uv = gl_FragCoord.xy * rcpFrame;

    vec4 center = texelFetch(ldrScene, pixel, 0);
    float lumaM = rgbLuma(center.rgb);

    float lumaUp = rgbLuma(texture(ldrScene, uv + vec2(0.0, -rcpFrame.y)).rgb);
    float lumaDown = rgbLuma(texture(ldrScene, uv + vec2(0.0, rcpFrame.y)).rgb);
    float lumaLeft = rgbLuma(texture(ldrScene, uv + vec2(-rcpFrame.x, 0.0)).rgb);
    float lumaRight = rgbLuma(texture(ldrScene, uv + vec2(rcpFrame.x, 0.0)).rgb);

    float lumaMin = min(lumaM, min(min(lumaUp, lumaDown), min(lumaLeft, lumaRight)));
    float lumaMax = max(lumaM, max(max(lumaUp, lumaDown), max(lumaLeft, lumaRight)));
    float lumaRange = lumaMax - lumaMin;

    // 远离高反差边缘的纯色区域直接输出精确中心像素。
    if (lumaRange < max(FXAA_EDGE_THRESHOLD_MIN, lumaMax * FXAA_EDGE_THRESHOLD))
    {
        fragmentColor = center;
        return;
    }

    float lumaUpLeft = rgbLuma(texture(ldrScene, uv + vec2(-rcpFrame.x, -rcpFrame.y)).rgb);
    float lumaUpRight = rgbLuma(texture(ldrScene, uv + vec2(rcpFrame.x, -rcpFrame.y)).rgb);
    float lumaDownLeft = rgbLuma(texture(ldrScene, uv + vec2(-rcpFrame.x, rcpFrame.y)).rgb);
    float lumaDownRight = rgbLuma(texture(ldrScene, uv + vec2(rcpFrame.x, rcpFrame.y)).rgb);

    float lumaUpDown = lumaUp + lumaDown;
    float lumaLeftRight = lumaLeft + lumaRight;
    float lumaLeftCorners = lumaUpLeft + lumaDownLeft;
    float lumaRightCorners = lumaUpRight + lumaDownRight;
    float lumaUpCorners = lumaUpLeft + lumaUpRight;
    float lumaDownCorners = lumaDownLeft + lumaDownRight;

    float edgeHorz = abs(-2.0 * lumaLeft + lumaLeftCorners) +
                     abs(-2.0 * lumaM + lumaUpDown) * 2.0 +
                     abs(-2.0 * lumaRight + lumaRightCorners);
    float edgeVert = abs(-2.0 * lumaUp + lumaUpCorners) +
                     abs(-2.0 * lumaM + lumaLeftRight) * 2.0 +
                     abs(-2.0 * lumaDown + lumaDownCorners);
    bool isHorizontal = (edgeHorz >= edgeVert);

    float subpixNSWE = lumaUpDown + lumaLeftRight;
    float subpixCorners = lumaLeftCorners + lumaRightCorners;
    float subpixA = subpixNSWE * 2.0 + subpixCorners;
    float subpixB = (subpixA * (1.0 / 12.0)) - lumaM;
    float subpixC = clamp(abs(subpixB) / lumaRange, 0.0, 1.0);
    float subpixD = (-2.0 * subpixC + 3.0) * (subpixC * subpixC);
    float subpixOffset = subpixD * subpixD * FXAA_SUBPIX_QUALITY;

    float luma1 = isHorizontal ? lumaUp : lumaLeft;
    float luma2 = isHorizontal ? lumaDown : lumaRight;
    float gradient1 = luma1 - lumaM;
    float gradient2 = luma2 - lumaM;
    bool is1Steepest = abs(gradient1) >= abs(gradient2);
    float gradientScaled = 0.25 * max(abs(gradient1), abs(gradient2));

    vec2 stepDir = isHorizontal
                       ? vec2(0.0, is1Steepest ? -rcpFrame.y : rcpFrame.y)
                       : vec2(is1Steepest ? -rcpFrame.x : rcpFrame.x, 0.0);
    float lumaLocalAverage = 0.5 * ((is1Steepest ? luma1 : luma2) + lumaM);

    vec2 currentUv = uv + 0.5 * stepDir;
    vec2 edgeStep = isHorizontal ? vec2(rcpFrame.x, 0.0) : vec2(0.0, rcpFrame.y);

    vec2 uv1 = currentUv - edgeStep;
    vec2 uv2 = currentUv + edgeStep;
    float lumaEnd1 = rgbLuma(texture(ldrScene, uv1).rgb) - lumaLocalAverage;
    float lumaEnd2 = rgbLuma(texture(ldrScene, uv2).rgb) - lumaLocalAverage;
    bool reached1 = abs(lumaEnd1) >= gradientScaled;
    bool reached2 = abs(lumaEnd2) >= gradientScaled;
    bool reachedBoth = reached1 && reached2;

    if (!reached1)
        uv1 -= edgeStep;
    if (!reached2)
        uv2 += edgeStep;

    if (!reachedBoth)
    {
        for (int i = 2; i < FXAA_SEARCH_STEPS; ++i)
        {
            float stepScale = searchStepMultiplier(i);
            if (!reached1)
            {
                lumaEnd1 = rgbLuma(texture(ldrScene, uv1).rgb) - lumaLocalAverage;
                reached1 = abs(lumaEnd1) >= gradientScaled;
            }
            if (!reached2)
            {
                lumaEnd2 = rgbLuma(texture(ldrScene, uv2).rgb) - lumaLocalAverage;
                reached2 = abs(lumaEnd2) >= gradientScaled;
            }
            reachedBoth = reached1 && reached2;
            if (reachedBoth)
                break;
            if (!reached1)
                uv1 -= edgeStep * stepScale;
            if (!reached2)
                uv2 += edgeStep * stepScale;
        }
    }

    float distance1 = isHorizontal ? (uv.x - uv1.x) : (uv.y - uv1.y);
    float distance2 = isHorizontal ? (uv2.x - uv.x) : (uv2.y - uv.y);
    bool isDirection1 = distance1 < distance2;
    float distanceFinal = min(distance1, distance2);
    float edgeThickness = distance1 + distance2;
    float pixelOffset = -distanceFinal / edgeThickness + 0.5;

    bool isLumaCenterSmaller = lumaM < lumaLocalAverage;
    bool correctVariation = (((isDirection1 ? lumaEnd1 : lumaEnd2) < 0.0) != isLumaCenterSmaller);
    float finalOffset = correctVariation ? pixelOffset : 0.0;
    finalOffset = max(finalOffset, subpixOffset);

    vec3 finalRgb = texture(ldrScene, uv + stepDir * finalOffset).rgb;
    fragmentColor = vec4(finalRgb, center.a);
}
