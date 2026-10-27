# Сборка LEDCAST Console для Windows x64 в desktop\dist\.
#
#   .\desktop\package-windows.ps1
#
# NDI runtime загружается динамически и кладётся рядом с exe, чтобы отдельная
# установка NDI Tools не требовалась. Путь к Processing.NDI.Lib.x64.dll берётся
# из NDI_RUNTIME_DLL, установленного NDI SDK/Runtime или Resolume.
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$version = (Select-String -Path Cargo.toml -Pattern '^version = "(.*)"$' | Select-Object -First 1).Matches.Groups[1].Value

$env:RUSTFLAGS = "-C target-feature=+crt-static"
cargo build --release --locked

$dist = Join-Path $PSScriptRoot "dist"
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Path $dist | Out-Null

$exe = Join-Path $dist "LEDCAST-Console.exe"
Copy-Item "target\release\ledcast-console.exe" $exe

$ndiCandidates = @(
    $env:NDI_RUNTIME_DLL,
    $(if ($env:NDI_RUNTIME_DIR_V6) { Join-Path $env:NDI_RUNTIME_DIR_V6 "Processing.NDI.Lib.x64.dll" }),
    "C:\Program Files\NDI\NDI 6 SDK\Bin\x64\Processing.NDI.Lib.x64.dll",
    "C:\Program Files\Resolume Arena\Processing.NDI.Lib.x64.dll",
    "C:\Program Files\Resolume Avenue\Processing.NDI.Lib.x64.dll"
) | Where-Object { $_ -and (Test-Path $_) }
if (-not $ndiCandidates) {
    throw "Processing.NDI.Lib.x64.dll not found. Set NDI_RUNTIME_DLL to the official NDI runtime path."
}
$ndi = Join-Path $dist "Processing.NDI.Lib.x64.dll"
Copy-Item @($ndiCandidates)[0] $ndi

$readme = Join-Path $dist "README.txt"
Copy-Item "package\windows\README.txt" $readme

$archive = Join-Path $dist "LEDCAST-Console-$version-Windows-x64.zip"
Compress-Archive -Path $exe, $ndi, $readme -DestinationPath $archive

$hash = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLower()
# Перевод строки — LF: файл склеивается с остальными суммами релиза, а shasum
# на macOS с CRLF не находит архив по имени.
[IO.File]::WriteAllText((Join-Path $dist "SHA256SUMS"), "$hash  $(Split-Path $archive -Leaf)`n")
Write-Output $archive
