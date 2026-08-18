# Pack romfs/espeak-ng-data/ into assets/espeak-ng-data.bin (a simple archive)
# so the Windows build can embed it via objcopy (this elf2nro's romfs is broken).
# Format (little-endian):
#   u32 magic   = 0x4B505345 ("ESPK")
#   u32 count
#   per file:
#     u32 name_len
#     u8[name_len] name   (relative path, '/' separators, no trailing NUL)
#     u32 data_len
#     u8[data_len] data
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$src  = Join-Path $repo 'romfs\espeak-ng-data'
$dst  = Join-Path $repo 'assets\espeak-ng-data.bin'

$files = Get-ChildItem $src -Recurse -File | Sort-Object FullName

$ms = New-Object System.IO.MemoryStream
$w  = New-Object System.IO.BinaryWriter($ms)

$w.Write([uint32]0x4B505345)  # magic "ESPK"
$w.Write([uint32]$files.Count)

foreach ($f in $files) {
    $rel = $f.FullName.Substring($src.Length + 1).Replace('\', '/')
    $nameBytes = [System.Text.Encoding]::UTF8.GetBytes($rel)
    $data = [System.IO.File]::ReadAllBytes($f.FullName)
    $w.Write([uint32]$nameBytes.Length)
    $w.Write($nameBytes)
    $w.Write([uint32]$data.Length)
    $w.Write($data)
}
$w.Flush()
[System.IO.File]::WriteAllBytes($dst, $ms.ToArray())
$w.Dispose()
$ms.Dispose()

"packed {0} files -> {1} ({2:N0} bytes)" -f $files.Count, $dst, (Get-Item $dst).Length
