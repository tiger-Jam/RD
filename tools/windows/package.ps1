# BMjoy を配る形にまとめて zip にする（tools/release/make_release.sh から呼ぶ）。
#   -Build    ビルドのフォルダ（BMjoybody.exe がある所）
#   -Name     zip の中のフォルダの名前（zip の名前にもなる）
#   -OutDir   zip を置く所
#   -Assets   素材を取ってくる BMjoy の置き場（BMjoyfiles。スキン・サウンドセット・BGM・壁紙など）
#   -Notices  使っているもののライセンス（THIRD_PARTY_NOTICES.md）
# 2 つ作る：
#   <Name>.zip         はじめて入れる人向け。実行ファイル・DLL・公式の MOD と、素材の入った BMjoyfiles
#   <Name>-update.zip  自動の更新で入れ替える分。実行ファイル・DLL・公式の MOD・ライセンスだけ（フォルダを切らずに置く）
# 個人のもの（設定・曲データベース・記録・プレイヤー・リプレイ・ログ）は入れない。開発用のもの（.pdb・.lib・テストの exe）も入れない
param([string]$Build, [string]$Name, [string]$OutDir, [string]$Assets, [string]$Notices)
$ErrorActionPreference = "Stop"
$work = Join-Path $env:TEMP "bmjoy_package"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
$prog = Join-Path $work "program"
$stage = Join-Path $work $Name
New-Item -ItemType Directory -Force $prog, $stage | Out-Null

# 実行ファイルと DLL（SDL・FFmpeg）、公式の MOD、ライセンス
Copy-Item "$Build\bmjoy.exe", "$Build\BMjoybody.exe" $prog
Get-ChildItem $Build -Filter *.dll | Copy-Item -Destination $prog
# Visual C++ のランタイム（vcruntime140.dll・msvcp140.dll など）。入れていない PC でも動くように、実行ファイルの横に置く
$crt = Get-ChildItem "${env:ProgramFiles}\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT" -Directory -ErrorAction SilentlyContinue |
    Sort-Object FullName | Select-Object -Last 1
if ($crt) { Get-ChildItem $crt.FullName -Filter *.dll | Copy-Item -Destination $prog }
else { Write-Output "Visual C++ のランタイムが見つからない（入れずに作る）" }
Copy-Item -Recurse "$Build\mods" "$prog\mods"
Copy-Item $Notices $prog
# FFmpeg（LGPL 2.1）を入れるので、その全文も置く（vcpkg が入れたもの）
$ffl = Join-Path $env:USERPROFILE "vcpkg\installed\x64-windows\share\ffmpeg\copyright"
if (Get-ChildItem $prog -Filter "avcodec-*.dll") {
    New-Item -ItemType Directory -Force "$prog\licenses" | Out-Null
    Copy-Item $ffl "$prog\licenses\FFmpeg-LGPL-2.1.txt"
}

# 更新の zip（フォルダを切らない。実行ファイルのフォルダにそのまま広げる）
New-Item -ItemType Directory -Force $OutDir | Out-Null
$upd = Join-Path $OutDir "$Name-update.zip"
if (Test-Path $upd) { Remove-Item -Force $upd }
# zip は Windows に付いている tar で作る（PowerShell 5 の Compress-Archive は区切りが \ の zip を作ることがある）
tar.exe -a -cf $upd -C $prog .
if ($LASTEXITCODE -ne 0) { throw "更新の zip を作れない" }

# はじめて入れる人向け：プログラムと素材
Copy-Item -Recurse "$prog\*" $stage
$files = Join-Path $stage "BMjoyfiles"
New-Item -ItemType Directory -Force $files | Out-Null
$personal = @("Database", "Player", "Replay", "Log", "Mods", "Update")
Get-ChildItem $Assets -Directory | Where-Object { $personal -notcontains $_.Name } | ForEach-Object { Copy-Item -Recurse $_.FullName (Join-Path $files $_.Name) }
foreach ($f in "Config\config.txt", "Config\keyconfig.txt", "Config\client_id.txt") { Remove-Item -Force (Join-Path $files $f) -ErrorAction SilentlyContinue }
$full = Join-Path $OutDir "$Name.zip"
if (Test-Path $full) { Remove-Item -Force $full }
tar.exe -a -cf $full -C $work $Name
if ($LASTEXITCODE -ne 0) { throw "zip を作れない" }
Remove-Item -Recurse -Force $work
foreach ($z in $upd, $full) {
    $mb = [math]::Round((Get-Item $z).Length / 1MB, 1)
    $sha = (Get-FileHash $z -Algorithm SHA256).Hash.ToLower()
    Write-Output "ZIP_OK $z $mb MB $sha"
}
