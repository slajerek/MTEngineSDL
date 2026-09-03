<#
.SYNOPSIS
  Compile every HLSL shader to bytecode and embed it in a committed header.

.DESCRIPTION
  The Windows mirror of tools/embed-metal-shaders.sh, and it exists for the same
  reason: the apps must be SINGLE EXECUTABLES. c64d embeds its fonts, icons and
  ROMs into src/Embedded/*.h precisely to get that, so a .cso sitting next to the
  binary is not an option -- and no other backend looks for shader source at
  runtime. Precompiling also moves shader syntax errors from a user's first
  launch to our build.

  Unlike the Metal path there is NO runtime source-compilation fallback. Metal
  keeps one because newLibraryWithSource is a two-line call; the D3D equivalent
  is D3DCompile from d3dcompiler_47.dll, and adding a second way to produce a
  shader is a second thing that can disagree with the first. imgui_impl_dx11
  compiles its OWN two shaders that way, which is upstream's business; ours are
  bytecode only.

.PARAMETER Check
  Do not compile. Verify every committed header is up to date with its .hlsl and
  is not a placeholder. This is what the engine's pre-build step runs, and it is
  the whole reason this script has a mode at all -- see "WHAT ENFORCES WHAT".

.NOTES
  WHAT ENFORCES WHAT -- read this before trusting a generated header's comment.

  The Metal side of this pipeline emits headers that say "The build FAILS if
  this file is stale", and as of S-6 NOTHING RUNS ITS --check: not the Xcode
  project, not a script, not CI. S-4 recorded that gap and it is still open. The
  HLSL side deliberately does not inherit the claim: the check here IS wired
  into MTEngineSDL.vcxproj as a pre-build step, and the headers say exactly what
  runs it. If that pre-build step is ever removed, the sentence in the generated
  headers has to change with it.

  STALENESS IS A CONTENT HASH, NOT A TIMESTAMP. git does not preserve mtimes, so
  a fresh clone or a branch switch would fail or pass depending on checkout
  order -- and "regenerate because the mtime moved" trains people to regenerate
  without looking at what changed.

.EXAMPLE
  powershell tools/embed-hlsl-shaders.ps1          # regenerate the headers
  powershell tools/embed-hlsl-shaders.ps1 -Check   # fail if any is stale
#>
[CmdletBinding()]
param(
    [switch]$Check
)

$ErrorActionPreference = 'Stop'

$repo      = Split-Path -Parent $PSScriptRoot
$shaderDir = Join-Path $repo 'platform\Windows\src.Windows\Render\Shaders'
$outDir    = Join-Path $repo 'platform\Windows\src.Windows\Render\Generated'

# ONE ROW PER COMPILED SHADER, not per file: Resolve.hlsl and VideoYUV.hlsl each
# hold a vertex AND a pixel entry point, and each entry point is a separate
# blob. The generated header is named after the ENTRY, so the backend includes
# exactly what it binds.
# SHADER MODEL 4.0, NOT 5.0, AND THAT IS A DELIBERATE FLOOR.
#
# Nothing in these three files needs SM5: SV_VertexID, the templated
# Texture2D<float4>/Texture3D<float4> object syntax, the .Sample() method form
# and dynamic indexing of a constant array all exist in SM4. imgui_impl_dx11
# compiles its own shaders at vs_4_0/ps_4_0 for exactly this reason -- so it
# still runs on a D3D_FEATURE_LEVEL_10_x device.
#
# TO BE PRECISE ABOUT WHAT THIS DOES AND DOES NOT BUY TODAY: the backend asks
# D3D11CreateDevice for { 11_1, 11_0 } ONLY, so a 10_x adapter is refused
# outright and the factory falls back to OpenGL long before any CreatePixelShader
# runs. The "ps_5_0 fails on a 10_x device" failure therefore cannot happen as
# the tree stands -- an earlier version of this comment claimed it could, which
# would have talked the next maintainer out of ever wanting SM5.
#
# The real reason is smaller and still sufficient: 4_0 is the LOWEST profile
# that expresses everything these three files do, so choosing it forecloses
# nothing -- adding 10_1/10_0 to the device's feature-level list later becomes a
# one-line change rather than a shader rewrite. If something here ever genuinely
# needs SM5 (a Texture2DArray plane layout, heavy [unroll] in the LUT path),
# raise the profile: the device floor already permits it.
$shaders = @(
    @{ File = 'Resolve.hlsl';   Entry = 'ResolveVS';   Profile = 'vs_4_0'; Name = 'ResolveVS'   },
    @{ File = 'Resolve.hlsl';   Entry = 'ResolvePS';   Profile = 'ps_4_0'; Name = 'ResolvePS'   },
    @{ File = 'FlatColor.hlsl'; Entry = 'FlatColorPS'; Profile = 'ps_4_0'; Name = 'FlatColorPS' },
    @{ File = 'VideoYUV.hlsl';  Entry = 'YuvVS';       Profile = 'vs_4_0'; Name = 'VideoYuvVS'  },
    @{ File = 'VideoYUV.hlsl';  Entry = 'YuvPS';       Profile = 'ps_4_0'; Name = 'VideoYuvPS'  },
    @{ File = 'MaskedTile.hlsl';Entry = 'MaskedTilePS';Profile = 'ps_4_0'; Name = 'MaskedTilePS'}
)

function Get-SourceSha256([string]$path) {
    # CR BYTES ARE STRIPPED BEFORE HASHING, and that is not fussiness.
    #
    # Git for Windows sets core.autocrlf=true by DEFAULT, so a Windows clone
    # gets CRLF working-tree copies of these .hlsl files while every macOS and
    # Linux clone gets LF. Hashing the raw bytes would therefore make a CLEAN
    # WINDOWS CLONE fail this gate on all five shaders -- or, worse, someone
    # would regenerate on Windows and commit CRLF digests that break everyone
    # else. The check would be reliably wrong in one direction or the other.
    #
    # The usual answer is a repo-root .gitattributes pinning `*.hlsl text
    # eol=lf`. THAT IS NOT AVAILABLE HERE: this repo's .gitignore deliberately
    # ignores .gitattributes, and quietly overriding a standing decision to make
    # a build script tidier is the wrong trade. Normalising in the hash instead
    # is self-contained, and it is the SAME normalisation on both sides -- the
    # generator and -Check call this one function -- so the two can never
    # disagree about what a source's digest is.
    #
    # An earlier version of this comment claimed a .gitattributes existed. It
    # did not (A4 review round 2).
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $lf = $bytes | Where-Object { $_ -ne 13 }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha.ComputeHash([byte[]]$lf))).Replace("-", "").ToLowerInvariant()
    }
    finally { $sha.Dispose() }
}

function Find-ShaderCompiler {
    # fxc.exe first: it ships with every Windows SDK and targets Shader Model
    # 4.x and 5.x directly -- and 4.0 is all this stage uses (see the $shaders
    # table). dxc is the modern one but defaults to SM 6.x and DXIL, which
    # D3D11 cannot consume at all.
    $cmd = Get-Command fxc.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $roots = @("${env:ProgramFiles(x86)}\Windows Kits\10\bin", "${env:ProgramFiles}\Windows Kits\10\bin")
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        $found = Get-ChildItem -Path $root -Filter fxc.exe -Recurse -ErrorAction SilentlyContinue |
                 Where-Object { $_.FullName -match '\\x64\\' } |
                 Sort-Object FullName -Descending |
                 Select-Object -First 1
        if ($found) { return $found.FullName }
    }
    return $null
}

# NOTE ON THE COMMITTED PLACEHOLDERS: they were HAND-AUTHORED on macOS, not
# produced by this function -- there is no PowerShell there either. This
# generator only ever emits REAL bytecode, and the first successful run replaces
# all five headers wholesale. Do not add a placeholder mode here to "match":
# a code path that runs once and never again is a code path nobody maintains,
# and the committed headers say plainly at the top that they are hand-authored.
function Write-BytecodeHeader([string]$name, [string]$srcFile, [string]$entry,
                              [string]$profile, [string]$sha, [byte[]]$blob,
                              [string]$outPath) {
    $sb = [System.Text.StringBuilder]::new()
    [void]$sb.AppendLine("// GENERATED FILE -- DO NOT EDIT.")
    [void]$sb.AppendLine("//")
    [void]$sb.AppendLine("// Produced by tools/embed-hlsl-shaders.ps1 from $srcFile, entry point")
    [void]$sb.AppendLine("// $entry, profile $profile. Regenerate with:")
    [void]$sb.AppendLine("//")
    [void]$sb.AppendLine("//     powershell tools/embed-hlsl-shaders.ps1")
    [void]$sb.AppendLine("//")
    [void]$sb.AppendLine("// Committed on purpose: it lets a checkout build without a shader compiler")
    [void]$sb.AppendLine("// on PATH, and it puts shader changes in the diff where a reviewer can see")
    [void]$sb.AppendLine("// them.")
    [void]$sb.AppendLine("//")
    [void]$sb.AppendLine("// WHAT ENFORCES FRESHNESS, precisely: MTEngineSDL.vcxproj runs")
    [void]$sb.AppendLine("// embed-hlsl-shaders.ps1 -Check as a PRE-BUILD step, and that step FAILS")
    [void]$sb.AppendLine("// the build when k${name}SourceSha256 below disagrees with the")
    [void]$sb.AppendLine("// .hlsl on disk. A HASH, not a timestamp: git does not preserve mtimes.")
    [void]$sb.AppendLine("// (The Metal side of this pipeline emits the same sentence and NOTHING runs")
    [void]$sb.AppendLine("// its check -- S-4 recorded that and it is still open. This side does not")
    [void]$sb.AppendLine("// inherit the claim without the step.)")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("#ifndef _${name}_Bytecode_h_")
    [void]$sb.AppendLine("#define _${name}_Bytecode_h_")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("// sha256 of the .hlsl source this header was generated from.")
    [void]$sb.AppendLine("// ``inline constexpr``, not ``static const``: the sha256 is read by the build")
    [void]$sb.AppendLine("// script, never by C++, and a namespace-scope ``static const`` in a header is")
    [void]$sb.AppendLine("// an unreferenced internal-linkage variable in every translation unit that")
    [void]$sb.AppendLine("// includes it -- one -Wunused-variable warning per include.")
    [void]$sb.AppendLine("inline constexpr const char *k${name}SourceSha256 = `"$sha`";")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("static const unsigned char k${name}BytecodeData[] = {")
    for ($i = 0; $i -lt $blob.Length; $i += 16) {
        $chunk = $blob[$i..([Math]::Min($i + 15, $blob.Length - 1))]
        [void]$sb.AppendLine("`t" + (($chunk | ForEach-Object { '0x{0:x2},' -f $_ }) -join ' '))
    }
    [void]$sb.AppendLine("};")
    [void]$sb.AppendLine("static const unsigned long k${name}BytecodeLength = $($blob.Length);")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("#endif")

    # LF line endings and no BOM, to match every other source file in the tree
    # and to keep the diff of a regeneration readable.
    [System.IO.File]::WriteAllText($outPath, ($sb.ToString() -replace "`r`n", "`n"),
                                   (New-Object System.Text.UTF8Encoding $false))
}

if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }

$status = 0
$placeholders = 0

if ($Check) {
    foreach ($s in $shaders) {
        $src  = Join-Path $shaderDir $s.File
        $out  = Join-Path $outDir "$($s.Name)Bytecode.h"
        if (-not (Test-Path $out)) {
            # Write-Host, NOT Write-Error. With $ErrorActionPreference = 'Stop'
            # a Write-Error is a TERMINATING error, so `$status = 1; continue`
            # never ran, `exit $status` was unreachable, and only the FIRST
            # stale shader was ever reported. The build still failed -- an
            # unhandled terminating error makes powershell.exe exit 1 -- but by
            # accident, and with one shader named instead of all of them.
            Write-Host "STALE: $out does not exist -- run tools/embed-hlsl-shaders.ps1"
            $status = 1; continue
        }
        $text = Get-Content -Raw -Path $out

        # THE HASH COMPARISON COMES FIRST, BEFORE the placeholder branch.
        #
        # The first version tested for a placeholder and `continue`d -- so with
        # all five headers still placeholders (their state until the first
        # Windows build), the hash was NEVER compared, $status stayed 0, and the
        # script printed "generated headers are consistent with their .hlsl
        # sources". Editing a shader on the VM would have passed the very gate
        # the commit called "real". Order matters more than the check.
        $want = Get-SourceSha256 $src
        # -match, not -notmatch: PowerShell populates $Matches on a SUCCESSFUL
        # -match, and relying on it after an inverted operator is exactly the
        # kind of subtlety that works until it does not.
        if (-not ($text -match "k$($s.Name)SourceSha256 = `"([0-9a-f]+)`"")) {
            Write-Host "STALE: $out has no recorded source hash"
            $status = 1; continue
        }
        if ($Matches[1] -ne $want) {
            Write-Host ("STALE: $out was generated from a different $($s.File) " +
                        "(want $want, have $($Matches[1])) -- run tools/embed-hlsl-shaders.ps1 and commit the result")
            $status = 1; continue
        }
        # And that the header still describes the ENTRY POINT and PROFILE the
        # table asks for. The source hash cannot see a profile change: when
        # every profile moved from 5_0 to 4_0, nothing here would have noticed.
        if ($text -notmatch [regex]::Escape("$($s.Entry), profile $($s.Profile).")) {
            Write-Host "STALE: $out is not for entry $($s.Entry) profile $($s.Profile)"
            $status = 1; continue
        }

        if ($text -match "k$($s.Name)IsPlaceholder") {
            # A WARNING, NOT AN ERROR, and the distinction is deliberate.
            #
            # An OpenGL-only Windows build -- which is every Windows build until
            # S-6's Subphase B turns MT_RENDER_BACKEND_D3D11 on -- compiles none
            # of the D3D sources and does not care that the bytecode is a stub.
            # Failing here would break a build that is working perfectly.
            #
            # The case that MUST NOT slip through is a D3D build shipping stubs,
            # and only the BACKEND knows whether D3D is enabled. Be precise
            # about how much of that it covers: CRenderBackendD3D11::IsAvailable()
            # refuses on a placeholder RESOLVE blob -- that is what stops the
            # backend starting and makes the factory fall back to OpenGL -- while
            # a placeholder FlatColor or VideoYUV blob is refused later, by its
            # own shader class, and surfaces as a failed shader probe or as
            # video that never draws. Both are loud; neither is silent. This
            # check says its piece and gets out of the way.
            Write-Warning ("PLACEHOLDER: $out carries no real bytecode (authored on a machine with " +
                           "no HLSL compiler). Run tools/embed-hlsl-shaders.ps1 -- it needs fxc.exe " +
                           "from the Windows SDK -- and commit the result before enabling the D3D11 backend.")
            $placeholders++
            continue
        }
    }
    # NEVER PRINT AN UNQUALIFIED CONSISTENCY CLAIM WHILE PLACEHOLDERS EXIST.
    # "consistent with their .hlsl sources" read as "these are the shaders", and
    # they are not.
    if ($status -eq 0) {
        if ($placeholders -gt 0) {
            Write-Host ("embed-hlsl-shaders: $placeholders of $($shaders.Count) header(s) are still " +
                        "PLACEHOLDERS; the rest match their .hlsl sources")
        } else {
            Write-Host "embed-hlsl-shaders: generated headers are consistent with their .hlsl sources"
        }
    }
    exit $status
}

$fxc = Find-ShaderCompiler
if (-not $fxc) {
    Write-Error ("embed-hlsl-shaders: fxc.exe not found. Install a Windows SDK, or run with -Check " +
                 "to verify the committed headers without compiling.")
    exit 1
}
Write-Host "embed-hlsl-shaders: using $fxc"

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ([System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    foreach ($s in $shaders) {
        $src = Join-Path $shaderDir $s.File
        $cso = Join-Path $tmp "$($s.Name).cso"
        $out = Join-Path $outDir "$($s.Name)Bytecode.h"

        # /Zpc: COLUMN-MAJOR is fxc's default for matrices and nothing here uses
        # one, but pinning it means a future matrix uniform cannot silently
        # change convention. /O3 and no /Gfa: the Metal path deliberately does
        # not pass -ffast-math, because these ports are judged against their
        # GLSL originals pixel for pixel and relaxed floating point is exactly
        # the kind of difference that shows up as a small, unattributable colour
        # shift. Same reasoning here.
        # RELAX $ErrorActionPreference AROUND THE NATIVE CALL, and this is the
        # single most likely first-contact failure on the VM.
        #
        # Windows PowerShell 5.1 wraps a native command's STDERR into
        # ErrorRecords, and under 'Stop' that is a TERMINATING NativeCommandError.
        # fxc writes WARNINGS to stderr (unused variables, X3206, ...), so the
        # script would die on the first warning -- inside the loop, before
        # $LASTEXITCODE was ever examined, with a message that does not name the
        # shader. A clean compile with one warning would read as a broken script.
        $prevEA = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        & $fxc /nologo /T $s.Profile /E $s.Entry /O3 /Zpc /Fo $cso $src 2>&1 |
            ForEach-Object { Write-Host $_ }
        $rc = $LASTEXITCODE
        $ErrorActionPreference = $prevEA
        if ($rc -ne 0) {
            Write-Host "embed-hlsl-shaders: fxc FAILED for $($s.File):$($s.Entry) (exit $rc)"
            exit 1
        }

        $blob = [System.IO.File]::ReadAllBytes($cso)
        $sha  = Get-SourceSha256 $src
        Write-BytecodeHeader $s.Name $s.File $s.Entry $s.Profile $sha $blob $out
        Write-Host ("{0,-34} {1,7} bytes  sha {2}" -f "$($s.Name)Bytecode.h", $blob.Length, $sha.Substring(0,12))
    }
}
finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
