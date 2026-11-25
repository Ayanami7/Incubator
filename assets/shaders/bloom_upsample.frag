#version 450

layout(set = 0, binding = 0) uniform sampler2D srcTexture;

layout(push_constant) uniform UpsampleParams
{
    vec2 srcTexelSize;
    vec2 dstSize;
} params;

layout(location = 0) out vec4 fragmentColor;

void main()
{
    vec2 uv = gl_FragCoord.xy / max(params.dstSize, vec2(1.0));
    float x = params.srcTexelSize.x;
    float y = params.srcTexelSize.y;

    // 9-tap Tent 升采样滤波，配合硬件加性混合累加至高一级纹理。
    vec3 a = texture(srcTexture, uv + vec2(-x,  y)).rgb;
    vec3 b = texture(srcTexture, uv + vec2(0.0, y)).rgb;
    vec3 c = texture(srcTexture, uv + vec2( x,  y)).rgb;
    vec3 d = texture(srcTexture, uv + vec2(-x, 0.0)).rgb;
    vec3 e = texture(srcTexture, uv).rgb;
    vec3 f = texture(srcTexture, uv + vec2( x, 0.0)).rgb;
    vec3 g = texture(srcTexture, uv + vec2(-x, -y)).rgb;
    vec3 h = texture(srcTexture, uv + vec2(0.0, -y)).rgb;
    vec3 i = texture(srcTexture, uv + vec2( x, -y)).rgb;

    vec3 upsample = (e * 4.0 + (b + d + f + h) * 2.0 + (a + c + g + i)) / 16.0;
    fragmentColor = vec4(max(upsample, vec3(0.0)), 1.0);
}
