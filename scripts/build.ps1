# Cross-compile .nro using native Windows clang + ld.lld + a local devkitA64 sysroot.
# (The aarch64-none-elf-gcc in this sysroot is a Linux ELF binary and cannot run
#  on Windows; this script follows the proven clang/lld route.)
# NOTE: keep this file pure ASCII - Windows PowerShell 5.1 misparses UTF-8
# scripts without BOM when they contain non-ASCII comments.
$ErrorActionPreference = 'Stop'

# switch-tools exes (nacptool/elf2nro) need msys64 runtime DLLs.
# Set MSYS2_BIN to your msys64\usr\bin directory, or keep the default install path.
$msysBin = if ($env:MSYS2_BIN) { $env:MSYS2_BIN } else { 'C:\msys64\usr\bin' }
if (Test-Path $msysBin) { $env:PATH = "$msysBin;$env:PATH" }

$repo = Split-Path -Parent $PSScriptRoot
# DSH_SWITCH_DEPS must point at the folder that contains both
# .tools\devkita64-sysroot (a devkitA64 sysroot) and .tools\switch-tools-win.
$fw = $env:DSH_SWITCH_DEPS
if (-not $fw) { throw 'DSH_SWITCH_DEPS is not set; point it at the folder containing .tools\devkita64-sysroot and .tools\switch-tools-win' }
$toolRoot    = "$fw\.tools\devkita64-sysroot\opt\devkitpro"
$switchTools = "$fw\.tools\switch-tools-win\bin"
$llvm        = if ($env:LLVM_BIN) { $env:LLVM_BIN } else { 'C:\Program Files\LLVM\bin' }
$build       = Join-Path $repo 'build'
$portlib     = Join-Path $toolRoot 'portlibs\switch\lib'

$gccVer = Get-ChildItem (Join-Path $toolRoot 'devkitA64\lib\gcc\aarch64-none-elf') -Directory |
          Sort-Object Name -Descending | Select-Object -First 1
$gccPic    = Join-Path $gccVer.FullName 'pic'
$newlibPic = Join-Path $toolRoot 'devkitA64\aarch64-none-elf\lib\pic'

New-Item -ItemType Directory -Force -Path $build | Out-Null

# ---- Static render guards (fail the build on known stretch/full-target bugs) ----
$appSrc = [System.IO.File]::ReadAllText((Join-Path $repo 'source\app.c'))

# Guard 1: RenderCopy with NULL destination stretches the texture to the whole
# render target (caused the "giant text" bug). Dest rect must always be explicit.
if ($appSrc -match 'SDL_RenderCopy\([^;]*,\s*NULL\s*,\s*NULL\s*\)') {
    throw 'guard: SDL_RenderCopy with NULL destination (stretches to full render target) found in app.c'
}

# Guard 2: RenderFillRect with NULL rect fills the whole target silently.
if ($appSrc -match 'SDL_RenderFillRect\([^;]*,\s*NULL\s*\)') {
    throw 'guard: SDL_RenderFillRect with NULL rect found in app.c'
}

# Guard 3: every render-target switch must be reset (leak detector).
$rtTotal = ([regex]::Matches($appSrc, 'SDL_SetRenderTarget\(g_ren,')).Count
$rtResets = ([regex]::Matches($appSrc, 'SDL_SetRenderTarget\(g_ren,\s*NULL')).Count
$rtSets = $rtTotal - $rtResets
if ($rtSets -ne $rtResets) {
    throw "guard: unbalanced SDL_SetRenderTarget in app.c ($rtSets sets vs $rtResets resets)"
}

$base = @(
    '--target=aarch64-none-elf','-D__SWITCH__','-D_REENTRANT',
    '-march=armv8-a+crc+crypto','-mcpu=cortex-a57','-mtp=tpidrro_el0',
    '-O2','-g','-fPIE','-ffunction-sections','-fdata-sections','-fno-strict-aliasing',
    '-Wall','-Wextra',
    '-isystem',(Join-Path $toolRoot 'libnx\include'),
    '-isystem',(Join-Path $toolRoot 'devkitA64\aarch64-none-elf\include'),
    '-isystem',(Join-Path $toolRoot 'portlibs\switch\include'),
    '-isystem',(Join-Path $toolRoot 'portlibs\switch\include\SDL2'),
    '-isystem',(Join-Path $toolRoot 'portlibs\switch\include\freetype2')
)
$common = $base + @('-I',(Join-Path $repo 'source'), '-I',(Join-Path $repo 'libs\cjson'), '-I',(Join-Path $repo 'third_party\espeak-ng\src\include'))

# espeak-ng(本地 TTS)的独立 include 集:不放 source/,避免与项目 config.h 冲突
$espeakRoot = Join-Path $repo 'third_party\espeak-ng'
$espeakCommon = $base + @(
    '-DHAVE_CONFIG_H',
    '-I',(Join-Path $espeakRoot 'switch-compat'),
    '-I',(Join-Path $espeakRoot 'src\include'),
    '-I',(Join-Path $espeakRoot 'src\include\compat'),
    '-I',(Join-Path $espeakRoot 'src\libespeak-ng'),
    '-I',(Join-Path $espeakRoot 'src\ucd-tools\src\include'),
    '-I',$espeakRoot
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
    @('source\tts.c','tts.o'),
    @('libs\cjson\cJSON.c','cJSON.o')
)

# espeak-ng 库源文件(33 个:6 ucd + 27 libespeak-ng 基础源)
$espeakLib = @('common','compiledata','compiledict','dictionary','encoding','error','espeak_api','ieee80','intonation','langopts','mnemonics','numbers','readclause','phoneme','phonemelist','setlengths','soundicon','spect','speech','ssml','synthdata','synthesize','translate','translateword','tr_languages','voices','wavegen')
foreach ($f in $espeakLib) { $sources += ,@("third_party\espeak-ng\src\libespeak-ng\$f.c", "espeak_$f.o") }
$espeakUcd = @('case','categories','ctype','proplist','scripts','tostring')
foreach ($f in $espeakUcd) { $sources += ,@("third_party\espeak-ng\src\ucd-tools\src\$f.c", "espeak_ucd_$f.o") }

foreach ($entry in $sources) {
    $object = Join-Path $build $entry[1]
    $flags = if ($entry[0] -like 'third_party\espeak-ng\*') { $espeakCommon } else { $common }
    Write-Host "compiling $($entry[0])"
    & (Join-Path $llvm 'clang.exe') @flags -c (Join-Path $repo $entry[0]) -o $object
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $($entry[0])" }
    $objects += $object
}

Push-Location $build
try {
    & (Join-Path $llvm 'llvm-ar.exe') x (Join-Path $toolRoot 'libnx\lib\libnx.a') switch_crt0.o
    if ($LASTEXITCODE -ne 0) { throw 'extract switch_crt0.o failed' }
} finally { Pop-Location }

# Embed assets into rodata via objcopy (this elf2nro's romfs support is
# unreliable; the objcopy route has been verified on real hardware).
$assets = @(
    @('assets\NotoSansCJKsc-Regular.otf','noto_font.o'),
    @('assets\cacert.pem','cacert.o'),
    @('assets\logo.png','logo.o')
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
    (Join-Path $portlib 'libSDL2_image.a'),
    (Join-Path $portlib 'libfreetype.a'),
    (Join-Path $portlib 'libharfbuzz.a'),
    (Join-Path $portlib 'libfribidi.a'),
    (Join-Path $portlib 'libpng16.a'),
    (Join-Path $portlib 'libjpeg.a'),
    (Join-Path $portlib 'libwebp.a'),
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
& (Join-Path $switchTools 'elf2nro.exe') $elf $nro "--nacp=$nacp" "--icon=$(Join-Path $repo 'icon.jpg')" "--romfsdir=$(Join-Path $repo 'romfs')"
if ($LASTEXITCODE -ne 0) { throw 'nro failed' }

Get-Item $nro | Select-Object FullName, Length
