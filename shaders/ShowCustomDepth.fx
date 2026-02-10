// Minimal debug effect to visualize the injected CUSTOMDEPTH texture.
// Put this file in your ReShade "Shaders" folder (or add this repo path to effect search paths).

#include "ReShade.fxh"

// Bind from addon via runtime->update_texture_bindings("CUSTOMDEPTH", ...).
texture2D DepthTex : CUSTOMDEPTH;
sampler2D sDepthTex { Texture = DepthTex; };

// ---- Haze controls ----
uniform float HazeStart    = 0.6;   // where haze begins (compressed depth)
uniform float HazeEnd      = 0.95;  // full haze
uniform float HazeStrength = 0.35;  // blend amount
uniform float HazeBias     = 2.0;
uniform float3 HazeColor   = float3(0.88, 0.8, 0.4);

// Heat map helper
float3 DepthHeat(float t)
{
    return saturate(float3(
        abs(t * 2.0 - 1.0),        // R → peaks near/far
        1.0 - abs(t * 2.0 - 1.0),  // G → peaks in the middle
        1.0 - t                   // B → far
    ));
}

float4 PS_DepthHeat_Haze(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    float d = saturate(tex2D(sDepthTex, uv).r);
    float dc = 1.0 - pow(1.0 - d, 0.35);

    float3 heat = DepthHeat(dc);
    float heatContrast = dot(heat, float3(0.25, 0.5, 0.25));

    float3 heatSafe = lerp(dc.xxx, heatContrast.xxx, 0.35);

    // haze
    float haze = smoothstep(HazeStart, HazeEnd, dc);
    haze = pow(haze, HazeBias);

    float3 col = lerp(heatSafe, HazeColor, haze * HazeStrength);

    return float4(col, 1.0);
}

float4 PS_RawDepth_CompressWhite_Haze(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    float d = tex2D(sDepthTex, uv).r;

    // Guard against garbage
    d = saturate(d);

    // Compress upper range only (no amplification)
    float dc = 1.0 - pow(1.0 - d, 0.35);

    // Haze factor from compressed depth
    float haze = smoothstep(HazeStart, HazeEnd, dc);

    // Additive haze blend
    float3 col = lerp(dc.xxx, HazeColor, haze * HazeStrength);

    return float4(col, 1.0);
}

technique ShowCustomDepth
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader  = PS_RawDepth_CompressWhite_Haze;
    }
}