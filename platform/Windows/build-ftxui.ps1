param(
    [ValidateSet('Debug','Release')]
    [string]$Config = 'Release',
    [ValidateSet('x64','ARM64')]
    [string]$Platform = 'x64',
    [ValidateSet('Clang','MSVC')]
    [string]$Compiler = 'Clang'
)

$ErrorActionPreference = 'Stop'

function Require-Command([string]$name) {
    if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
        throw "Missing required tool '$name'. Run from 'Developer PowerShell for VS 2022' and ensure CMake is installed."
    }
}

Require-Command cmake
Require-Command lib

$repoRoot = (Resolve-Path "$PSScriptRoot\..\..").Path
$ftxuiSrc = Join-Path $repoRoot 'other\lib\ftxui'
$buildDir = Join-Path $ftxuiSrc "build-windows-$Platform"

if (-not (Test-Path $ftxuiSrc)) {
    throw "FTXUI submodule not found at $ftxuiSrc. Run: git submodule update --init --recursive"
}

$outDir = Join-Path $repoRoot "platform\Windows\libs\$Platform\$Config"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$cmakeToolset = if ($Compiler -eq 'Clang') { @('-T', 'ClangCL') } else { @() }

Write-Host "Configuring FTXUI ($Platform, $Config) in $buildDir"
cmake -S $ftxuiSrc -B $buildDir -G "Visual Studio 17 2022" -A $Platform @cmakeToolset `
    -DBUILD_SHARED_LIBS=OFF `
    -DFTXUI_BUILD_DOCS=OFF `
    -DFTXUI_BUILD_EXAMPLES=OFF `
    -DFTXUI_BUILD_MODULES=OFF `
    -DFTXUI_BUILD_TESTS=OFF `
    -DFTXUI_BUILD_TESTS_FUZZER=OFF `
    -DFTXUI_ENABLE_INSTALL=OFF `
    -DFTXUI_QUIET=ON

Write-Host "Building FTXUI libs ($Config)"
cmake --build $buildDir --config $Config --target screen dom component

# VS generator places libs under buildDir\{Config}\
$libs = @(
    (Join-Path $buildDir "$Config\ftxui-screen.lib"),
    (Join-Path $buildDir "$Config\ftxui-dom.lib"),
    (Join-Path $buildDir "$Config\ftxui-component.lib")
)

foreach ($p in $libs) {
    if (-not (Test-Path $p)) {
        throw "Expected library not found: $p"
    }
}

$outLib = Join-Path $outDir 'ftxui.lib'
Write-Host "Packing into $outLib"
if (Test-Path $outLib) { Remove-Item -Force $outLib }
lib /nologo /OUT:$outLib $libs

Write-Host "Done. Output: $outLib"
