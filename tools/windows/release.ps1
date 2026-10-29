# 公開版をビルドして zip にする（tools/release/make_release.sh から呼ぶ）。
#   -Ver     版（年.月.日.番号）
#   -Root    作業の置き場（<Root>\<Ver>\src に出典のコメントを外したソースが広げてある）
#   -Assets  素材を取ってくる RD の置き場（RDfiles）
# 開発用の口を切り（RD_DEV=OFF）、版を入れてビルドし、テストを通してから package.ps1 で zip にする
param([string]$Ver, [string]$Root, [string]$Assets)
$ErrorActionPreference = "Stop"
$src = Join-Path $Root "$Ver\src"
$build = Join-Path $src "build"
$out = Join-Path $Root "$Ver\out"
Push-Location $src
try {
    cmd /c "tools\windows\release_build.cmd $Ver"
    if ($LASTEXITCODE -ne 0) { throw "ビルドできない" }
    $bin = Join-Path $build "RelWithDebInfo"
    # 動画の BGA のための FFmpeg（vcpkg のもの）を実行ファイルの横に置く
    $ff = Join-Path $env:USERPROFILE "vcpkg\installed\x64-windows\bin"
    Get-ChildItem $ff -Filter "av*.dll" | Copy-Item -Destination $bin
    Get-ChildItem $ff -Filter "sw*.dll" | Copy-Item -Destination $bin
    $t = & "$bin\rd_tests.exe" | Select-Object -Last 1
    Write-Output "TESTS $t"
    if ($t -notmatch "失敗0件") { throw "テストが通らない" }
    & powershell -ExecutionPolicy Bypass -File (Join-Path $src "tools\windows\package.ps1") -Build $bin -Name "RD-$Ver" -OutDir $out -Assets $Assets -Notices (Join-Path $src "THIRD_PARTY_NOTICES.md")
    if ($LASTEXITCODE -ne 0) { throw "zip を作れない" }
    # 落ちたときのダンプを読むための .pdb（配らない。Mac に持って帰って版ごとに残す）
    Copy-Item "$bin\RDbody.pdb", "$bin\rd.pdb" $out
    Write-Output "RELEASE_OK $out"
} finally {
    Pop-Location
}
