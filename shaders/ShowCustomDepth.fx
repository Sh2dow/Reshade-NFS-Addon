// Minimal debug effect to visualize the injected CUSTOMDEPTH texture.
// Put this file in your ReShade "Shaders" folder (or add this repo path to effect search paths).

#include "ReShade.fxh"

// Bind from addon via runtime->update_texture_bindings("CUSTOMDEPTH", ...).
texture2D DepthTex : CUSTOMDEPTH;
sampler2D sDepthTex { Texture = DepthTex; };

float4 PS_ShowDepth(float4 pos : SV_Position, float2 uv : TexCoord) : SV_Target
{
    float d = tex2D(sDepthTex, uv).r;

    // Grayscale inverted depth
    d = saturate(1.0 - d);
    d = pow(d, 0.7);
    return float4(d, d, d, 1.0);
}

technique ShowCustomDepth
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader  = PS_ShowDepth;
    }
}
