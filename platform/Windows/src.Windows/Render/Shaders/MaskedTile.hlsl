// ===========================================================================
// MaskedTile.hlsl -- the game app's hex-grid tile mask
// ===========================================================================
//
// Draws a tile through an alpha mask: the piece texture is sampled normally,
// the mask is sampled by SCREEN position relative to the tile's rectangle, and
// anything outside the rectangle or under a low mask alpha is discarded. Its
// GLSL original is CRenderShaderMaskedTile::GetFragmentShaderSource() and its
// Metal twin is platform/MacOS/shaders/MTMaskedTile.metal; all three compute
// the same thing.
//
// ---------------------------------------------------------------------------
// THE ONE REAL DIFFERENCE FROM THE GLSL ORIGINAL: NO Y FLIP.
// ---------------------------------------------------------------------------
//
// The GLSL version ends its mask-UV maths with
//
//     maskUV.y = 1.0 - maskUV.y;   // OpenGL has Y=0 at bottom
//
// and it needs that because gl_FragCoord is measured from the BOTTOM-LEFT while
// the tile rectangle arrives in ImGui coordinates, which are top-left. D3D11's
// SV_Position is already top-left, exactly like Metal's [[position]], so the
// flip must NOT be carried across -- copying it would mirror every mask
// vertically, which draws a plausible-looking tile that is wrong in a way no
// compile step can catch. The Metal twin omits it for the same reason; compare
// its `maskUV` line, which is this one.
//
// The GLSL version also converts the tile's Y origin against the viewport
// height for that same bottom-left convention. Nothing here needs it: both the
// tile rectangle and SV_Position are top-left, so the subtraction is direct.
//
// ---------------------------------------------------------------------------
// A PIXEL SHADER ONLY, for the reason FlatColor.hlsl gives at length.
// ---------------------------------------------------------------------------
//
// The draw this replaces is one of ImGui's own, so ImGui's vertex shader, input
// layout and b0 projection buffer stay bound, and PS_INPUT below MUST match
// imgui_impl_dx11's vertex-shader output signature exactly -- semantics and
// order included: float4 SV_POSITION, float4 COLOR0, float2 TEXCOORD0.
//
// t0 AND s0 ARE ImGui's. imgui_impl_dx11 binds the draw command's texture to t0
// and its own sampler to s0 before the callback runs, so the piece texture and
// the sampler come for free; only the MASK at t1 has to be bound, which is what
// CRenderShaderMaskedTileD3D11::BindTo does. Sharing s0 rather than creating a
// second sampler state is deliberate: the mask is sampled with the same linear
// clamp filtering the GL and Metal versions use.
// ===========================================================================

cbuffer MaskedTileConstants : register(b0)
{
    // Both in PHYSICAL framebuffer pixels, y down -- the same space
    // SV_Position is measured in. 16 bytes exactly, one constant register.
    float2 uTilePos;
    float2 uTileSize;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};

Texture2D    pieceTexture : register(t0);   // ImGui's, the draw command's texture
Texture2D    maskTexture  : register(t1);   // ours, bound by the draw callback
SamplerState sampler0     : register(s0);   // ImGui's

float4 MaskedTilePS(PS_INPUT input) : SV_TARGET
{
    float4 piece = pieceTexture.Sample(sampler0, input.uv);

    // input.pos.xy after rasterisation is the framebuffer coordinate, which is
    // what gl_FragCoord gives the GLSL original and in.position the Metal one.
    float2 maskUV = (input.pos.xy - uTilePos) / uTileSize;

    float maskAlpha = maskTexture.Sample(sampler0, maskUV).a;

    if (maskUV.x < 0.0 || maskUV.x > 1.0 ||
        maskUV.y < 0.0 || maskUV.y > 1.0 ||
        maskAlpha < 0.5)
    {
        discard;
    }

    return input.col * piece;
}
