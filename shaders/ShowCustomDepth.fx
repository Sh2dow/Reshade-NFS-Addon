// Minimal debug effect to visualize the injected CUSTOMDEPTH texture.
// Put this file in your ReShade "Shaders" folder (or add this repo path to effect search paths).

#include "ReShade.fxh"

texture BackBufferTex : COLOR;
sampler sColor { Texture = BackBufferTex; };

// Bind from addon via runtime->update_texture_bindings("CUSTOMDEPTH", ...).
texture CustomDepthTex : CUSTOMDEPTH;
sampler sDepth { Texture = CustomDepthTex; };

// ---- Haze controls ----
uniform float HazeStart    = 0.6;   // where haze begins (compressed depth)
uniform float HazeEnd      = 0.95;  // full haze
uniform float HazeStrength = 0.35;  // blend amount
uniform float HazeBias     = 2.0;
uniform float3 HazeColor   = float3(0.88, 0.8, 0.4);

float4 VS_Fullscreen(uint id : SV_VertexID, out float2 uv : TEXCOORD) : SV_Position
{
    uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 PS_RawDepth_CompressWhite_Haze(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    float d = tex2D(sDepth, uv).r;

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

float4 PS_UsableDepth(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float3 scene = tex2D(sColor, uv).rgb;

    float d = tex2D(sDepth, uv).r;

    // NFS depth is reversed? If near looks white, keep this ON.
    d = 1.0 - d;

    // Now extract detail from the far range
    // Far = foggy, Near = clear
    float fog = smoothstep(0.30, 0.05, d);

    // Shape
    fog = pow(fog, 1.3);

    scene = lerp(scene, HazeColor, fog * HazeStrength);

    return float4(scene, 1);
}

technique DebugCustomDepth
{
    pass
    {
        VertexShader = VS_Fullscreen;
        PixelShader  = PS_UsableDepth;
    }
}
