// SPDX-License-Identifier: GPL-3.0-or-later
// The source texture is temporarily NEAREST. Key original 8-bit texels before
// interpolation so transparent magenta contributes neither RGB nor coverage.
vec4 gccKeyedTexel(vec2 pixel) {
    vec2 bounded = clamp(pixel, vec2(0.0), gccTextureSize - vec2(1.0));
    vec3 c = texture2D(texture, (bounded + vec2(0.5)) / gccTextureSize).rgb;
    float a = keyAlpha(c);
    return vec4(c * a, a);
}
vec4 gccExactKeyBilinear(vec2 uv) {
    vec2 pixel = uv * gccTextureSize - vec2(0.5);
    vec2 base = floor(pixel);
    vec2 weight = fract(pixel);
    vec4 bottom = mix(gccKeyedTexel(base), gccKeyedTexel(base + vec2(1.0, 0.0)), weight.x);
    vec4 top = mix(gccKeyedTexel(base + vec2(0.0, 1.0)), gccKeyedTexel(base + vec2(1.0, 1.0)), weight.x);
    return mix(bottom, top, weight.y);
}
