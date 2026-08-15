# Cross-compile .nro using native Windows clang + ld.lld + local devkitA64 sysroot.
# (The aarch64-none-elf-gcc in this sysroot is a Linux ELF binary and cannot run
#  on Windows; this script follows the clang/lld route proven by F1RaceWatch.)
# NOTE: keep this file pure ASCII - Windows PowerShell 5.1 misparses UTF-8
# scripts without BOM when they contain non-ASCII comments.
$ErrorActionPreference = 'Stop'

# switch-tools exes (nacptool/elf2nro) need msys64 runtime DLLs
$env:PATH = "C:\msys64\usr\bin;$env:PATH"

$repo        = Split-Path -Parent $PSScriptRoot
$fw          = 'C:\Users\USER\Documents\Codex\example\f1-race-watch-windows'
$toolRoot    = "$fw\.tools\devkita64-sysroot\opt\devkitpro"
$switchTools = "$fw\.tools\switch-tools-win\bin"
$llvm        = 'C:\Program Files\LLVM\bin'
$build       = Join-Path $repo 'build'
$portlib     = Join-Path $toolRoot 'portlibs\switch\lib'

$gccVer = Get-ChildItem (Join-Path $toolRoot 'devkitA64\lib\gcc\aarch64-none-elf') -Directory |
          Sort-Object Name -Descending | Select-Object -First 1
$gccPic    = Join-Path $gccVer.FullName 'pic'
$newlibPic = Join-Path $toolRoot 'devkitA64\aarch64-none-elf\lib\pic'

New-Item -ItemType Directory -Force -Path $build | Out-Null

$common = @(
    '--target=aarch64-none-elf','-D__SWITCH__','-D_REENTRANT',
    '-march=armv8-a+crc+crypto','-mcpu=cortex-a57','-mtp=tpidrro_el0',
    '-O2','-g','-fPIE','-ffunction-sections','-fdata-sections','-fno-strict-aliasing',
    '-Wall','-Wextra',
    '-isystem',(Join-Path $toolRoot 'libnx\include'),
    '-isystem',(Join-Path $toolRoot 'devkitA64\aarch64-none-elf\include'),
    '-isystem',(Join-Path $toolRoot 'portlibs\switch\include'),
    '-isystem',(Join-Path $toolRoot 'portlibs\switch\include\SDL2'),
    '-isystem',(Join-Path $toolRoot 'portlibs\switch\include\freetype2'),
    '-I',(Join-Path $repo 'source'),
    '-I',(Join-Path $repo 'libs\cjson')
)

$objects = @()
$sources = @(
    @('source\main.c','main.o'),
    @('source\app.c','app.o'),
    @('source\config.c','config.o'),
    @('source\textinput.c','textinput.o'),
    @('source\util.c','util.o'),
    @('source\net.c','net.o'),
    @('source\backend.c','backend.o'),
    @('source\backend_harness.c','backend_harness.o'),
    @('source\backend_deepseek.c','backend_deepseek.o'),
    @('libs\cjson\cJSON.c','cJSON.o')
)
foreach ($entry in $sources) {
    $object = Join-Path $build $entry[1]
    Write-Host "compiling $($entry[0])"
    & (Join-Path $llvm 'clang.exe') @common -c (Join-Path $repo $entry[0]) -o $object
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $($entry[0])" }
    $objects += $object
}

Push-Location $build
try {
    & (Join-Path $llvm 'llvm-ar.exe') x (Join-Path $toolRoot 'libnx\lib\libnx.a') switch_crt0.o
    if ($LASTEXITCODE -ne 0) { throw 'extract switch_crt0.o failed' }
} finally { Pop-Location }

# Embed assets into rodata via objcopy (this elf2nro's romfs support is
# unreliable; F1RaceWatch proved the objcopy route on real hardware).
$assets = @(
    @('assets\NotoSansCJKsc-Regular.otf','noto_font.o'),
    @('assets\cacert.pem','cacert.o')
)
foreach ($entry in $assets) {
    $src = Join-Path $repo $entry[0]
    $dst = Join-Path $build $entry[1]
    Push-Location (Split-Path $src)
    try {
        & (Join-Path $llvm 'llvm-objcopy.exe') --input-target=binary --output-target=elf64-littleaarch64 --binary-architecture=aarch64 (Split-Path $src -Leaf) $dst
        if ($LASTEXITCODE -ne 0) { throw "objcopy failed: $($entry[0])" }
    } finally { Pop-Location }
    $objects += $dst
}

$elf = Join-Path $build 'switch-dsh-client.elf'
$link = @(
    '-T',(Join-Path $repo 'linker\switch-lld.ld'),'-pie','--no-dynamic-linker','--gc-sections',
    '-z','text','-z','now','--build-id=sha1','-u','main',
    (Join-Path $build 'switch_crt0.o'),(Join-Path $gccPic 'crti.o'),(Join-Path $gccPic 'crtbegin.o'),
    $objects,
    '--start-group',
    (Join-Path $portlib 'libSDL2_ttf.a'),
    (Join-Path $portlib 'libSDL2.a'),
    (Join-Path $portlib 'libSDL2_gfx.a'),
    (Join-Path $portlib 'libfreetype.a'),
    (Join-Path $portlib 'libharfbuzz.a'),
    (Join-Path $portlib 'libfribidi.a'),
    (Join-Path $portlib 'libpng16.a'),
    (Join-Path $portlib 'libbz2.a'),
    (Join-Path $portlib 'libz.a'),
    (Join-Path $portlib 'libcurl.a'),
    (Join-Path $portlib 'libEGL.a'),
    (Join-Path $portlib 'libGLESv2.a'),
    (Join-Path $portlib 'libglapi.a'),
    (Join-Path $portlib 'libdrm_nouveau.a'),
    (Join-Path $toolRoot 'libnx\lib\libnx.a'),
    (Join-Path $newlibPic 'libc.a'),(Join-Path $newlibPic 'libm.a'),(Join-Path $newlibPic 'libsysbase.a'),
    (Join-Path $newlibPic 'libstdc++.a'),(Join-Path $newlibPic 'libsupc++.a'),(Join-Path $gccPic 'libgcc.a'),
    '--end-group',
    (Join-Path $gccPic 'crtend.o'),(Join-Path $gccPic 'crtn.o'),
    '-o',$elf
)
Write-Host 'linking'
& (Join-Path $llvm 'ld.lld.exe') @link
if ($LASTEXITCODE -ne 0) { throw 'link failed' }

$nacp = Join-Path $build 'switch-dsh-client.nacp'
$nro  = Join-Path $repo 'switch-dsh-client.nro'
& (Join-Path $switchTools 'nacptool.exe') --create 'DSH Switch Client' 'switch-dsh-client' '0.1.0' $nacp
if ($LASTEXITCODE -ne 0) { throw 'nacp failed' }
& (Join-Path $switchTools 'elf2nro.exe') $elf $nro "--nacp=$nacp" "--icon=$(Join-Path $toolRoot 'libnx\default_icon.jpg')"
if ($LASTEXITCODE -ne 0) { throw 'nro failed' }

Get-Item $nro | Select-Object FullName, Length
