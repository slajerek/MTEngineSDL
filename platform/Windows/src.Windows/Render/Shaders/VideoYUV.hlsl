// ===========================================================================
// VideoYUV.hlsl -- YUV -> RGB video conversion, D3D11 (S-6 Task A4)
// ===========================================================================
//
// THIS IS THE THIRD COPY OF THE HDR TRANSFER MATHS. The other two are:
//   * platform/MacOS/src.MacOS/Render/CVideoYUVShaderMetal.mm  (MSL)
//   * src/Engine/Video/CVideoYUVShader.cpp                     (GLSL)
// and the C++ original they are all generated from is
//   * src/Engine/Video/CVideoTransferFunctions.h               ("THE ONE COPY")
//
// ALL FOUR MUST BE UPDATED TOGETHER. A shader cannot include a C++ header --
// this source becomes bytecode -- which is exactly why the agreement TEST
// exists: `hdr_shader_agrees_with_transfer_header` runs a ramp of code values
// through the REAL compiled shader on the GPU, reads it back, and compares
// against those functions evaluated on the CPU. A drifted constant fails there
// with a named value rather than showing up as an unexplained tolerance miss in
// a whole-frame comparison. Two copies have already drifted once in this
// programme's history; this is the third, so the test matters more, not less.
//
// WHY 203: SDR reference white is 203 nits (ITU-R BT.2408) and it is the anchor
// for the whole float pipeline. Output of the HDR branch below is "1.0 == SDR
// reference white", so 2.0 genuinely means twice as bright as paper white.
//
// TRANSCRIPTION NOTES -- where D3D differs from Metal, and where it must not:
//   * Metal REQUIRES every declared texture slot to be bound even on branches
//     that never execute, which is why the MSL path binds 1x1 dummies. D3D11
//     does not: an unbound SRV reads as zero. The backend binds dummies anyway,
//     so the two behave identically rather than merely both "working".
//   * D3D11 clip space is Y-up and its texture origin is top-left, the same as
//     Metal's, so the ONE uv flip below is transcribed unchanged. GL needs a
//     second inversion because its FBO is bottom-up; copying BOTH of GL's
//     inversions here would cancel out and put the video upside-down again.
//   * **D3D11's DEFAULT RASTERIZER STATE CULLS BACK FACES**
//     (CullMode = D3D11_CULL_BACK, FrontCounterClockwise = FALSE; that is also
//     what RSSetState(NULL) binds). Metal's default is MTLCullModeNone and
//     GL's GL_CULL_FACE is off, so NEITHER sibling ever had to think about
//     winding -- and on D3D11 **BOTH callers make uTransform.w POSITIVE, so
//     BOTH strips are BACK-facing and BOTH would be culled** by the default
//     state. Render() passes a pixel rect (h > 0); RenderToTarget() passes
//     METAL'S (-1, -1, 2, 2), never GL's (-1, 1, 2, -2) -- GL's negative
//     height exists solely because a GL FBO is bottom-up, and copying it here
//     is the SECOND inversion the bullet above forbids: it renders every
//     offscreen target vertically mirrored.
//     Render() survives today only because it runs inside an ImGui draw
//     callback where imgui_impl_dx11's own CULL_NONE state is still bound;
//     RenderToTarget() opens its own pass and has no such cover. So:
//     **BOTH D3D11 DRAW PATHS MUST BIND A CULL_NONE RASTERIZER STATE
//     EXPLICITLY** and never rely on RSSetState(NULL). The symptom is video
//     that vanishes with no error and no debug-layer message.
//     (A zero-initialised D3D11_RASTERIZER_DESC is NOT that default either --
//     CullMode = 0 is not a valid enum value; the defaults come from
//     CD3D11_RASTERIZER_DESC(D3D11_DEFAULT).)
//   * **DEPTH TESTING MUST BE OFF.** Both vertex shaders write z = 0, and
//     D3D11's default depth-stencil state is DepthEnable = TRUE with
//     COMPARISON_LESS. It happens to be harmless while no DSV is bound -- but
//     imgui_impl_dx11 creates a DepthEnable = FALSE state and RESTORES the
//     previous one, so "no DSV bound" is a property of the current backend,
//     not a guarantee. Bind an explicit DepthEnable = FALSE state.
// ===========================================================================

cbuffer YuvConstants : register(b0)
{
    float4 uTransform;        // x, y, w, h in NDC
    int    uRotation;         // 0/90/180/270, applied as a UV transform
    int    uMode;             // EYUVShaderMode ordinal: 0=3-plane, 1=NV12, 2=P10
    int    uColorSpace;       // normalized VPX_CS_*: 1=601, 2=709, 5=2020ncl
    int    uFullRange;
    int    uHasAlpha;
    int    uUseLut;
    float  uAlpha;
    float  uLutScale;         // (N-1)/N
    float  uLutOffset;        // 1/(2N)
    // --- S-5 Phase 5: HDR transfer ---------------------------------------
    int    uColorTrc;         // 16 = PQ, 18 = HLG, anything else = SDR
    int    uFloatTarget;      // 1: keep above-white. 0: tone-map into 0..1
    int    uSurfaceLinear;
    int    uSurfaceP3;
    float  uToneMapHeadroom;
    float2 _padYuv;
    // 80 bytes. The C++ struct in CVideoYUVShaderD3D11.cpp MUST mirror this
    // field for field AND in ORDER -- it is a raw byte copy into the constant
    // buffer, so a mismatch silently misreads every field after it rather than
    // failing to compile. Same trap the MSL twin documents.
};

Texture2D<float4> texY   : register(t0);
Texture2D<float4> texU   : register(t1);   // NV12: the interleaved U,V plane
Texture2D<float4> texV   : register(t2);
Texture2D<float4> texA   : register(t3);
Texture3D<float4> texLut : register(t4);
SamplerState      samp   : register(s0);

struct VSOut
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

// ---------------------------------------------------------------------------
// HDR transfer maths -- GENERATED FROM CVideoTransferFunctions.h
// ---------------------------------------------------------------------------

// PQ (SMPTE ST 2084) EOTF: encoded 0..1 -> absolute luminance, 1.0 == 10000 nits.
float PqEotf(float e)
{
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float p = pow(max(e, 0.0), 1.0 / m2);
    float num = p - c1;
    float den = c2 - c3 * p;
    if (num <= 0.0 || den <= 0.0) return 0.0;
    return pow(num / den, 1.0 / m1);
}

// HLG (ARIB STD-B67) inverse OETF: encoded 0..1 -> SCENE linear.
float HlgInverseOetf(float e)
{
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    e = max(e, 0.0);
    return (e <= 0.5) ? (e * e) / 3.0 : (exp((e - c) / a) + b) / 12.0;
}

// The IEC sRGB curve, sign-symmetric and CONTINUED past 1.0 rather than
// clamped -- that continuation is how an extended-sRGB surface defines a value
// of 2.0 as "twice reference white". Mirrors SrgbExtendedEncode in
// src/Engine/Core/MT_SrgbCurve.h.
float SrgbExtendedEncode(float v)
{
    float a = abs(v);
    float e = (a <= 0.0031308) ? (a * 12.92) : (1.055 * pow(a, 1.0 / 2.4) - 0.055);
    return (v < 0.0) ? -e : e;
}

// Extended Reinhard, normalised so `headroom` maps exactly to 1.0. At headroom
// 1.0 it is EXACTLY the identity on 0..1, which is the property the SDR
// regression rests on. Same curve as CImageData::ConvertRGBA16FToRGBA8, which
// is what the POSTER goes through when the resident format is 8-bit.
float ToneMapReinhard(float v, float headroom)
{
    float h = max(headroom, 1.0);
    v = max(v, 0.0);
    float t = v * (1.0 + v / (h * h)) / (1.0 + v);
    return min(t, 1.0);
}

// ---------------------------------------------------------------------------

// Fullscreen quad as a TRIANGLE STRIP of 4 vertices, positions from
// SV_VertexID -- no vertex buffer and no input layout, matching the MSL path's
// kQuadPos. Draw(4, 0) with D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP.
// File scope and `static const`, so fxc puts it in the shader's immediate
// constant buffer and indexes it directly. A mutable local array that is then
// dynamically indexed forces an indexable temp plus eight movs per vertex --
// four vertices a frame, so the cost is nil, but the MSL twin declares its
// kQuadPos in the `constant` address space and this is the same thing said in
// HLSL.
static const float2 kQuadPos[4] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1) };

VSOut YuvVS(uint vid : SV_VertexID)
{
    float2 aPos = kQuadPos[vid];

    VSOut o;
    float2 pos = aPos * uTransform.zw + uTransform.xy;
    o.position = float4(pos, 0.0, 1.0);

    // V IS FLIPPED RELATIVE TO POSITION, exactly as the GL and Metal paths do
    // it. The GL quad carries TWO attributes and they are not the same: aPos
    // runs (0,0)..(1,1) while aTexCoord is (aPos.x, 1 - aPos.y). Porting
    // `uv = aPos` drops that flip and plays every video upside-down -- which is
    // what happened on Metal the first time.
    //
    // ONE flip, not two: see the header note.
    //
    // Rotation is derived from the FLIPPED uv, matching both siblings: rot90 is
    // a COUNTER-clockwise turn of the coded pixels, so u_src = 1 - v_out,
    // v_src = u_out (and symmetrically for 180/270).
    float2 uv = float2(aPos.x, 1.0 - aPos.y);
    if (uRotation == 90)       o.uv = float2(1.0 - uv.y, uv.x);
    else if (uRotation == 180) o.uv = float2(1.0 - uv.x, 1.0 - uv.y);
    else if (uRotation == 270) o.uv = float2(uv.y, 1.0 - uv.x);
    else                       o.uv = uv;
    return o;
}

float4 YuvPS(VSOut input) : SV_TARGET
{
    float y, uu, vv;

    if (uMode == 2)
    {
        // YUV420P10: REQUIRES DXGI_FORMAT_R16_UNORM planes holding the raw
        // 10-bit code word (0..1023) LSB-aligned. Sampling a UNORM normalises
        // by /65535, so scale back by 65535/1023 to recover the
        // 10-bit-normalised [0,1] sample.
        //
        // R16_UINT IS NOT A SUBSTITUTE, and it is the natural mistake when you
        // have a uint16_t* in hand: Sample() is illegal on a UINT SRV (it needs
        // Load()), and "fixing" that by switching to Load() makes k10 wrong by
        // a factor of 65535. NV12's chroma plane likewise REQUIRES R8G8_UNORM
        // (.r = U, .g = V), not R8G8_UINT.
        const float k10 = 65535.0 / 1023.0;
        y  = texY.Sample(samp, input.uv).r * k10;
        uu = texU.Sample(samp, input.uv).r * k10;
        vv = texV.Sample(samp, input.uv).r * k10;
    }
    else if (uMode == 1)
    {
        // NV12: texU is the interleaved U,V plane (R8G8_UNORM).
        y = texY.Sample(samp, input.uv).r;
        float2 uv2 = texU.Sample(samp, input.uv).rg;
        uu = uv2.r;
        vv = uv2.g;
    }
    else
    {
        y  = texY.Sample(samp, input.uv).r;
        uu = texU.Sample(samp, input.uv).r;
        vv = texV.Sample(samp, input.uv).r;
    }

    float yNorm, uNorm, vNorm;
    if (uFullRange != 0)
    {
        yNorm = y;
        uNorm = uu - 0.5;
        vNorm = vv - 0.5;
    }
    else
    {
        yNorm = (y  - 16.0/255.0)  * (255.0/219.0);
        uNorm = (uu - 128.0/255.0) * (255.0/224.0);
        vNorm = (vv - 128.0/255.0) * (255.0/224.0);
    }

    float3 rgb;
    if (uColorSpace == 5)
    {
        // BT.2020 non-constant luminance
        rgb.r = yNorm + 1.4746 * vNorm;
        rgb.g = yNorm - 0.16455 * uNorm - 0.57135 * vNorm;
        rgb.b = yNorm + 1.8814 * uNorm;
    }
    else if (uColorSpace == 2)
    {
        // BT.709
        rgb.r = yNorm + 1.5748 * vNorm;
        rgb.g = yNorm - 0.1873 * uNorm - 0.4681 * vNorm;
        rgb.b = yNorm + 1.8556 * uNorm;
    }
    else
    {
        // BT.601
        rgb.r = yNorm + 1.402 * vNorm;
        rgb.g = yNorm - 0.344136 * uNorm - 0.714136 * vNorm;
        rgb.b = yNorm + 1.772 * uNorm;
    }

    // --- S-5 Phase 5: the HDR transfer -----------------------------------
    //
    // For an SDR clip this whole block is skipped and the original
    // clamp-then-LUT runs exactly as before -- byte-identical, which is what
    // keeps every existing clip unchanged.
    if (uColorTrc == 16 || uColorTrc == 18)
    {
        // 1. EOTF -> LINEAR, 1.0 == SDR reference white (203 nit, BT.2408).
        float3 lin;
        if (uColorTrc == 16)
        {
            const float kPqScale = 10000.0 / 203.0;
            lin = float3(PqEotf(rgb.r), PqEotf(rgb.g), PqEotf(rgb.b)) * kPqScale;
        }
        else
        {
            const float kHlgScale = 1000.0 / 203.0;
            lin = float3(HlgInverseOetf(rgb.r), HlgInverseOetf(rgb.g), HlgInverseOetf(rgb.b));
            // HLG's display OOTF, driven by BT.2020 luma, applied only once all
            // three channels exist. PQ has no OOTF, which is why one transfer
            // can be right while the other is wrong.
            float ys = 0.2627 * lin.r + 0.6780 * lin.g + 0.0593 * lin.b;
            float gain = (ys > 0.0) ? pow(ys, 0.2) : 0.0;
            lin = lin * gain * kHlgScale;
        }

        // 2. BT.2020 -> sRGB primaries, linear, D65 (pure primaries change).
        //    Rows sum to 1.0 so neutral stays neutral -- that is the property
        //    that breaks first if this is ever mistyped.
        float3 srgbLin;
        srgbLin.r =  1.6605 * lin.r - 0.5876 * lin.g - 0.0728 * lin.b;
        srgbLin.g = -0.1246 * lin.r + 1.1329 * lin.g - 0.0083 * lin.b;
        srgbLin.b = -0.0182 * lin.r - 0.1006 * lin.g + 1.1187 * lin.b;

        // 3. PRIMARIES, for BOTH arms. The surface may be Display P3 rather
        //    than sRGB, and the POSTER applies this stage in both arms too, so
        //    a shader that applied P3 only on the float arm would put playback
        //    in sRGB primaries while the poster sat in P3 on the gate-closed
        //    path. Order matters and matches the poster: primaries FIRST, then
        //    the tone-map.
        //
        //    FULL PRECISION, copied from PC_kLinearSrgbToLinearP3. Rows sum to
        //    1.0, so neutral stays neutral.
        if (uSurfaceP3 != 0)
        {
            float3 p3;
            p3.r = 0.8224621 * srgbLin.r + 0.1775380 * srgbLin.g + 0.0000000 * srgbLin.b;
            p3.g = 0.0331941 * srgbLin.r + 0.9668058 * srgbLin.g + 0.0000000 * srgbLin.b;
            p3.b = 0.0170827 * srgbLin.r + 0.0723974 * srgbLin.g + 0.9105199 * srgbLin.b;
            srgbLin = p3;
        }

        if (uFloatTarget != 0)
        {
            // 4a. GATE OPEN. Finish in the surface's own space, keeping values
            //     above 1.0. This mirrors PC_EncodeForSurface, which is what
            //     the POSTER goes through -- matching it IS the acceptance
            //     criterion.
            //
            //     NOTE FOR D3D11: uSurfaceLinear comes from
            //     CRenderBackend::GetSurfaceIsLinearColorSpace(), which this
            //     backend answers FALSE -- on an SDR and on an scRGB swapchain
            //     alike. That query is not "is the swapchain linear", it is
            //     "what encoding must the values we WRITE be in", and what we
            //     write is the sRGB-encoded offscreen target that Resolve.hlsl
            //     decodes afterwards. Answer true and this branch writes linear
            //     light, the resolve decodes it a SECOND time, and HDR video
            //     comes out crushed on exactly the content the stage exists for
            //     -- while the UI looks fine.
            if (uSurfaceLinear != 0)
            {
                rgb = srgbLin;              // extended LINEAR surface: as-is
            }
            else
            {
                rgb = float3(SrgbExtendedEncode(srgbLin.r),
                             SrgbExtendedEncode(srgbLin.g),
                             SrgbExtendedEncode(srgbLin.b));
            }
        }
        else
        {
            // 4b. GATE CLOSED -- and this is NOT a consolation prize. It is the
            //     arm every user without headroom sees, which is most users
            //     most of the time. Tone-map in LINEAR, then encode to plain
            //     sRGB.
            rgb = float3(SrgbExtendedEncode(ToneMapReinhard(srgbLin.r, uToneMapHeadroom)),
                         SrgbExtendedEncode(ToneMapReinhard(srgbLin.g, uToneMapHeadroom)),
                         SrgbExtendedEncode(ToneMapReinhard(srgbLin.b, uToneMapHeadroom)));
        }
    }
    else
    {
        // THE SDR PATH, UNCHANGED. The clamp stays here and only here --
        // removing it wholesale would change every existing clip, and it is the
        // above-white killer only on the HDR branch, which no longer runs
        // through it.
        rgb = clamp(rgb, 0.0, 1.0);
    }

    // CM-E display transform, over ENCODED R'G'B'. No LUT bound must be
    // bit-identical to the pre-CM-E path, which is why uUseLut gates it rather
    // than binding an identity lattice.
    //
    // For HDR clips uUseLut is ALREADY 0 and stays that way: an SDR
    // source->display transform over PQ/HLG values would be confidently wrong,
    // and the poster lane skips its equivalent for the same reason.
    if (uUseLut != 0)
    {
        rgb = texLut.Sample(samp, rgb * uLutScale + uLutOffset).rgb;
    }

    float a = uAlpha;
    if (uHasAlpha != 0)
    {
        a *= texA.Sample(samp, input.uv).r;
    }
    return float4(rgb, a);
}
