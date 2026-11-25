#version 450

layout(set = 0, binding = 0) uniform sampler2D srcTexture;

layout(push_constant) uniform DownsampleParams
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

    // 13-tap 对称盒式降采样滤波，抑制高频走样并保持能量守恒。
    vec3 a = texture(srcTexture, uv + vec2(-2.0 * x,  2.0 * y)).rgb;
    vec3 b = texture(srcTexture, uv + vec2( 0.0,      2.0 * y)).rgb;
    vec3 c = texture(srcTexture, uv + vec2( 2.0 * x,  2.0 * y)).rgb;
    vec3 d = texture(srcTexture, uv + vec2(-1.0 * x,  1.0 * y)).rgb;
    vec3 e = texture(srcTexture, uv + vec2( 1.0 * x,  1.0 * y)).rgb;
    vec3 f = texture(srcTexture, uv + vec2(-2.0 * x,  0.0)).rgb;
    vec3 g = texture(srcTexture, uv).rgb;
    vec3 h = texture(srcTexture, uv + vec2( 2.0 * x,  0.0)).rgb;
    vec3 i = texture(srcTexture, uv + vec2(-1.0 * x, -1.0 * y)).rgb;
    vec3 j = texture(srcTexture, uv + vec2( 1.0 * x, -1.0 * y)).rgb;
    vec3 k = texture(srcTexture, uv + vec2(-2.0 * x, -2.0 * y)).rgb;
    vec3 l = texture(srcTexture, uv + vec2( 0.0,     -2.0 * y)).rgb;
    vec3 m = texture(srcTexture, uv + vec2( 2.0 * x, -2.0 * y)).rgb;

    vec3 downsample = g * 0.125;
    downsample += (a + c + k + m) * 0.03125;
    downsample += (b + f + h + l) * 0.0625;
    downsample += (d + e + i + j) * 0.125;
    fragmentColor = vec4(max(downsample, vec3(0.0)), 1.0);
}
