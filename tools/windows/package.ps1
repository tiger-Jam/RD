# RD を配る形にまとめて zip にする（tools/release/make_release.sh から呼ぶ）。
#   -Build    ビルドのフォルダ（RDbody.exe がある所）
#   -Name     zip の中のフォルダの名前（zip の名前にもなる）
#   -OutDir   zip を置く所
#   -Assets   素材を取ってくる RD の置き場（RDfiles。スキン・サウンドセット・BGM・壁紙など）
#   -Notices  使っているもののライセンス（THIRD_PARTY_NOTICES.md）
# 2 つ作る：
#   <Name>.zip         はじめて入れる人向け。実行ファイル・DLL・公式の MOD と、素材の入った RDfiles
#   <Name>-update.zip  自動の更新で入れ替える分。実行ファイル・DLL・公式の MOD・ライセンスだけ（フォルダを切らずに置く）
# 個人のもの（設定・曲データベース・記録・プレイヤー・リプレイ・ログ）は入れない。開発用のもの（.pdb・.lib・テストの exe）も入れない
param([string]$Build, [string]$Name, [string]$OutDir, [string]$Assets, [string]$Notices)
$ErrorActionPreference = "Stop"
$work = Join-Path $env:TEMP "rd_package"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
$prog = Join-Path $work "program"
$stage = Join-Path $work $Name
New-Item -ItemType Directory -Force $prog, $stage | Out-Null

# 実行ファイルと DLL（SDL・FFmpeg）、公式の MOD、ライセンス
Copy-Item "$Build\RD.exe", "$Build\RDbody.exe" $prog
Get-ChildItem $Build -Filter *.dll | Copy-Item -Destination $prog
# Visual C++ のランタイム（vcruntime140.dll・msvcp140.dll など）。入れていない PC でも動くように、実行ファイルの横に置く
$crt = Get-ChildItem "${env:ProgramFiles}\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT" -Directory -ErrorAction SilentlyContinue |
    Sort-Object FullName | Select-Object -Last 1
if ($crt) { Get-ChildItem $crt.FullName -Filter *.dll | Copy-Item -Destination $prog }
else { Write-Output "Visual C++ のランタイムが見つからない（入れずに作る）" }
Copy-Item -Recurse "$Build\mods" "$prog\mods"
# 公開版にはまだ入れない MOD（推定難易度のフォルダ・判定の記録・判定のずれのバー）。更新で、入れてある人の分も消える
foreach ($m in @("level-folders", "judge-log", "judge-bar")) { Remove-Item -Recurse -Force "$prog\mods\$m" -ErrorAction SilentlyContinue }
Copy-Item -Recurse "$Build\skinpatch" "$prog\skinpatch"  # RD で足したスキンの表示（スキンのファイルは書き換えない）
Copy-Item $Notices $prog
Copy-Item (Join-Path (Split-Path $Notices) "説明書.html") $prog  # 説明書（ソースのいちばん上にある）
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
# zip は 1 ファイルずつ、名前を UTF-8、区切りを / にして書く
# （Windows の tar は日本語の名前を ? にし、PowerShell 5 の ZipFile.CreateFromDirectory は区切りを \ にしてしまう）
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
function New-Zip([string]$dir, [string]$zip, [string]$prefix) {
    $fs = [IO.File]::Open($zip, [IO.FileMode]::Create)
    $za = New-Object IO.Compression.ZipArchive($fs, [IO.Compression.ZipArchiveMode]::Create, $false, [Text.Encoding]::UTF8)
    try {
        $root = (Resolve-Path $dir).Path.TrimEnd('\')
        Get-ChildItem $root -Recurse -File | ForEach-Object {
            $rel = $prefix + $_.FullName.Substring($root.Length + 1).Replace('\', '/')
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $_.FullName, $rel, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally {
        $za.Dispose()
        $fs.Dispose()
    }
}
New-Zip $prog $upd ""

# はじめて入れる人向け：プログラムと素材
Copy-Item -Recurse "$prog\*" $stage
$files = Join-Path $stage "RDfiles"
New-Item -ItemType Directory -Force $files | Out-Null
$personal = @("Database", "Player", "Replay", "Log", "Mods", "Update")
Get-ChildItem $Assets -Directory | Where-Object { $personal -notcontains $_.Name } | ForEach-Object { Copy-Item -Recurse $_.FullName (Join-Path $files $_.Name) }
# 設定の置き場に残っている個人のもの：RD の設定とその控え、LR2 の設定（曲フォルダの場所などが入っている）
Get-ChildItem (Join-Path $files "Config") -File | Where-Object { $_.Name -like "config.txt*" -or $_.Name -in @("keyconfig.txt", "client_id.txt", "config.xml", "config.xmh", "openlr2-config.xml") } | Remove-Item -Force
$full = Join-Path $OutDir "$Name.zip"
if (Test-Path $full) { Remove-Item -Force $full }
New-Zip $stage $full "$Name/"
Remove-Item -Recurse -Force $work
foreach ($z in $upd, $full) {
    $mb = [math]::Round((Get-Item $z).Length / 1MB, 1)
    $sha = (Get-FileHash $z -Algorithm SHA256).Hash.ToLower()
    Write-Output "ZIP_OK $z $mb MB $sha"
}
