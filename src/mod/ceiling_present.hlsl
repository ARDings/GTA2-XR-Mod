// GTA2Ceiling final D3D9 presentation, compiled for ps_2_0.
// The caller binds the resolved A8R8G8B8 texture with POINT/CLAMP sampling,
// disables blending, dithering and sRGB conversion, and uses a pixel-aligned
// full-size quad on a NON-multisampled swapchain. Never filter the keyed RGB.
sampler2D resolvedScene : register(s0);
// x = 0: ordinary opaque copy; x = 1: coverage-to-exact-key presentation.
float4 presentationMode : register(c0);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 source = tex2D(resolvedScene, uv);
    if (presentationMode.x < 0.5)
        return float4(source.rgb, 1.0);

    // A8 resolve stores coverage in alpha and coverage-weighted RGB. The
    // smallest representable nonzero alpha must remain covered; this is NOT
    // a 50% coverage threshold. RGB keying can express only binary coverage.
    float3 rgb = saturate(source.rgb / max(source.a, 1.0 / 255.0));

    // Reserve only RGB values that would round to the host's exact #FF00FF
    // key after UNORM8 conversion. Other purple artwork stays unchanged.
    if (rgb.r >= 254.5 / 255.0 && rgb.g <= 0.5 / 255.0 && rgb.b >= 254.5 / 255.0)
        rgb.g = 1.0 / 255.0;

    if (source.a <= 0.5 / 255.0)
        rgb = float3(1.0, 0.0, 1.0);
    return float4(rgb, 1.0);
}
