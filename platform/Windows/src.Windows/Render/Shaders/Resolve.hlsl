// ===========================================================================
// Resolve.hlsl -- the present-time offscreen -> swapchain pass (S-6 Task A4)
// ===========================================================================
//
// WHAT THIS PASS IS FOR. Everything MTEngineSDL draws is sRGB-ENCODED: textures
// are RGBA8Unorm rather than the _sRGB variants, so sampling returns them
// undecoded, ImGui's own pixel shader is `col * texel`, and nothing converts
// anywhere. macOS can simply declare that truth to the compositor
// (kCGColorSpaceExtendedSRGB). DXGI has no such colour space: its float HDR
// path is scRGB -- DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 -- and the G10 in
// that name means gamma 1.0, i.e. LINEAR. So Windows cannot dodge the question,
// and this one full-screen pass is the answer: the whole application keeps
// rendering exactly as it does today, into an offscreen R16G16B16A16_FLOAT
// target, and this shader converts once at present time.
//
// THE C++ TWIN IS `MTSurfaceEncoding::ResolveToSurface`
// (src/Engine/Core/Render/MT_SurfaceEncoding.h). A shader cannot include a C++
// header -- this source is compiled to bytecode by fxc/dxc -- so the two are
// transcriptions of each other and MUST BE CHANGED TOGETHER. The C++ side is
// unit-tested by CTestSurfaceEncoding on macOS, which is the only place any of
// this arithmetic can be proven before Windows; that is why the decode lives
// there in a form a test can reach, and why this file's job is to be a
// faithful copy rather than a clever one.
//
// ---------------------------------------------------------------------------
// THE TWO THINGS THIS FILE MUST NOT GET WRONG
// ---------------------------------------------------------------------------
//
// 1. ON AN SDR SWAPCHAIN THIS PASS IS THE EXACT IDENTITY. An R8G8B8A8_UNORM /
//    G22 swapchain wants the values exactly as the pipeline wrote them --
//    sRGB-encoded -- so `uSwapchainIsLinear == 0` returns the sample UNCHANGED.
//    It is NOT "the same maths at scale 1.0": SrgbExtendedDecode(0.5) is 0.214,
//    so decoding here would display mid-grey as 0.21, a ~73-LSB error across
//    the entire UI on every non-HDR Windows machine -- which is S-4's
//    washed-out surface bug inverted onto the DEFAULT path. The first draft of
//    the S-6 plan specified exactly that and review round 2 caught it.
//
// 2. THE WHITE-LEVEL SCALE IS A MULTIPLY.
//    SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT is "the value of SDR white in the
//    SDL_COLORSPACE_SRGB_LINEAR colorspace" -- the scRGB value SDR white
//    BECOMES, not a divisor. With Windows 11's common 200-nit SDR white it
//    reads 2.5 (200/80), every SDR window is composited with its white at 2.5,
//    and for our UI to sit beside them at the same brightness OUR 1.0 must be
//    emitted as 2.5. Dividing puts the whole app at 32 nits next to 200-nit SDR
//    windows AND makes it get darker as the user turns the SDR-brightness
//    slider UP.
//
//    THE IDENTITY IS THE SHADER'S; THE PASS NEEDS THREE MORE THINGS, none of
//    which this file can enforce and all of which silently break it:
//      (a) an offscreen format that round-trips n/255 exactly AND carries
//          alpha. R16G16B16A16_FLOAT does both (11 significant bits per
//          channel). R11G11B10_FLOAT does NEITHER, and it is the tempting
//          bandwidth "economy": 6/6/5 mantissa bits give ~6 significant bits,
//          so 1/255 quantises to 2^-8 -- 0.39% off, NOT zero, but enough to
//          move mid-greys by an LSB or two across the whole UI while this file
//          went on promising "bit for bit" -- and it has NO ALPHA CHANNEL at
//          all, so `return src` cannot round-trip src.a and ImGui's SRC_ALPHA
//          blending into the offscreen has nowhere to write.
//      (b) a REPLACE blend state, not the SRC_ALPHA / INV_SRC_ALPHA blend
//          imgui_impl_dx11 leaves bound, and not depth testing (both vertex
//          shaders write z = 0 and D3D11's default is DepthEnable = TRUE with
//          COMPARISON_LESS).
//      (c) a NON-_SRGB typed RTV on the swapchain. An _SRGB view re-encodes on
//          write, which would reproduce S-4's washed-out bug on the very
//          default path this identity exists to protect.
//
//    ON CULLING: this triangle comes out CLOCKWISE in screen space and is
//    therefore FRONT-facing, so it survives D3D11's default CULL_BACK -- by
//    accident, not by design. VideoYUV.hlsl's quad is not so lucky. The
//    backend binds a CULL_NONE state here anyway; if the winding above is ever
//    touched, that is what keeps this from becoming the same bug.
//
// THE SAME SHADER AND THE SAME PASS RUN IN BOTH MODES, deliberately: there is
// then no HDR-only code path to rot unnoticed while everybody develops on SDR.
//
// 203 nits does not appear here. Our 1.0 is "SDR reference white"; how many
// nits the display shows for it is the OS's decision, and SDL hands that answer
// over already in scRGB units. 203 (BT.2408) is the PQ/HLG anchor and belongs
// to the video transfer path alone.
// ===========================================================================

cbuffer ResolveConstants : register(b0)
{
    // SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, verbatim, re-read EVERY FRAME.
    // "Poll, never latch" -- SDL updates it dynamically and raises
    // SDL_EVENT_WINDOW_HDR_STATE_CHANGED, and a value latched at startup is
    // simply wrong the moment the user moves the slider or drags the window to
    // another monitor.
    float uSdrWhiteScRgb;

    // A function of the SWAPCHAIN FORMAT and nothing else -- never of whether
    // SetColorSpace1() returned S_OK. DXGI treats every R16G16B16A16_FLOAT back
    // buffer as scRGB (G10) whether or not that call was made, so keying this
    // on an API return code would, on any failure, write ENCODED values into a
    // LINEAR buffer: S-4's washed-out bug on the exact path this stage exists
    // for.
    int uSwapchainIsLinear;

    float2 _padResolve;   // cbuffers pack in 16-byte registers
};

Texture2D<float4> uOffscreen    : register(t0);
SamplerState      uPointSampler : register(s0);

struct VSOut
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

// A full-screen TRIANGLE, not a quad, and no vertex buffer: the three
// positions come from SV_VertexID, so this pass needs no ID3D11InputLayout and
// no geometry of its own. Draw(3, 0).
//
// vid 0 -> uv (0,0), 1 -> (2,0), 2 -> (0,2); the triangle overhangs the
// viewport and is clipped, which is one fewer edge than a two-triangle quad
// has down its diagonal.
//
// uv (0,0) maps to NDC (-1, +1) -- top-left in both D3D texture space (origin
// top-left) and D3D clip space (Y up). Getting this flip wrong is a vertically
// mirrored UI, which a "not all zero" pixel check happily accepts.
VSOut ResolveVS(uint vid : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.uv = uv;
    o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// The IEC 61966-2-1 sRGB curve, SIGN-SYMMETRIC and CONTINUED past 1.0 rather
// than clamped there.
//
// Transcribed from SrgbExtendedDecode in src/Engine/Core/MT_SrgbCurve.h,
// constant for constant. The continuation above 1.0 is what carries HDR
// highlights; the sign symmetry is what stops a wide-gamut colour that lands
// slightly negative in sRGB primaries being clipped to zero, which would
// quietly throw away the gamut a float pipeline exists to preserve.
float SrgbExtendedDecode(float v)
{
    float a = abs(v);
    float d = (a <= 0.04045) ? (a / 12.92) : pow((a + 0.055) / 1.055, 2.4);
    return (v < 0.0) ? -d : d;
}

float4 ResolvePS(VSOut i) : SV_TARGET
{
    // POINT sampling, and the offscreen target is created at exactly the
    // swapchain's size: this is a 1:1 blit, so any filtering would be a way to
    // lose the bit-exactness the SDR arm below promises.
    float4 src = uOffscreen.Sample(uPointSampler, i.uv);

    if (uSwapchainIsLinear == 0)
    {
        // SDR swapchain: EXACT identity, alpha included. See note 1 above.
        return src;
    }

    // scRGB: decode the extended-sRGB curve, then scale so our 1.0 lands on the
    // display's SDR white. MULTIPLY. See note 2 above.
    float3 lin = float3(SrgbExtendedDecode(src.r),
                        SrgbExtendedDecode(src.g),
                        SrgbExtendedDecode(src.b)) * uSdrWhiteScRgb;

    // Alpha is not a colour channel and gets neither the transfer nor the
    // scale. The swapchain is opaque anyway; passing it through keeps this the
    // identity on the SDR arm and keeps the two arms differing in exactly one
    // thing.
    return float4(lin, src.a);
}
