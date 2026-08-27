// ===========================================================================
// FlatColor.hlsl -- the shader-plumbing proof (S-6 Task A4)
// ===========================================================================
//
// Fills its quad with one constant colour, ignoring the texture entirely.
//
// WHY A SHADER THAT DOES NOTHING IS IN SCOPE FOR THIS STAGE. It proves the
// shader PLUMBING -- compile, pixel-shader creation, ImGui draw-callback
// dispatch, live-context access, constant-buffer binding -- independently of
// any real shader's maths, so a failure in the video converter is a shader bug
// rather than an infrastructure bug. And it is not optional:
// src/Engine/Tests/MT_ShaderProbe.cpp maps a NULL CreateFlatColorShader() to
// SHADER_PROBE_UNSUPPORTED, and the ImGui test `render_backend_shader_probe`
// asserts SHADER_PROBE_READY on EVERY backend by design -- "present on EVERY
// backend deliberately", CRenderBackend.h at CreateFlatColorShader -- so a
// NULL there makes the Windows suite unreachable. It is also the smallest
// HLSL in the stage.
//
// Its Metal twin is platform/MacOS/shaders/MTFlatColor.metal and its OpenGL
// twin is CRenderShaderFlatColorOpenGL4; the test that exercises it asserts on
// all of them, because imgui_test_engine has NO Skipped status (the statuses
// are Unknown/Success/Queued/Running/Error/Suspended) and a test that returns
// early on one backend is counted as PASSED there -- a permanent false green.
//
// ---------------------------------------------------------------------------
// A PIXEL SHADER ONLY, AND THAT IS A DESIGN DECISION, NOT AN OMISSION.
// ---------------------------------------------------------------------------
//
// The draw this replaces is one of ImGui's own: the callback swaps the pixel
// shader and lets ImGui's vertex shader, input layout and b0 projection buffer
// stay bound. Writing a custom vertex shader would oblige its own
// ID3D11InputLayout built against THAT bytecode -- an input layout is validated
// against the vertex shader signature it was created with -- for no gain
// whatever, since the geometry is ImGui's ImDrawVert stream either way. So
// PS-only is the smallest correct form.
//
// PS_INPUT below therefore MUST match imgui_impl_dx11's own vertex-shader
// output signature exactly, semantics and order included (see its `vertexShader`
// string): float4 SV_POSITION, float4 COLOR0, float2 TEXCOORD0. If a future
// ImGui upgrade changes it, this file changes with it -- and nothing will tell
// you, because a mismatched PS input is a compile error at OUR build time only
// if the shader is compiled against the same signature, which it is not.
// ===========================================================================

cbuffer FlatColorConstants : register(b0)
{
    float4 uFlatColor;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};

// The vertex colour and the texture are both DELIBERATELY unused: the point is
// a colour that depends on nothing except the constant buffer, so a wrong
// result can only be the plumbing.
//
// Note the constant buffer is bound to b0 of the PIXEL stage. ImGui's b0 is on
// the VERTEX stage (its projection matrix), so the two do not collide --
// PSSetConstantBuffers and VSSetConstantBuffers are separate binding tables.
float4 FlatColorPS(PS_INPUT input) : SV_TARGET
{
    return uFlatColor;
}
