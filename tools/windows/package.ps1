# BMjoy を配る形（LR2 と同じフォルダの構造）にまとめて zip にする。
#   -Build     ビルドのフォルダ（BMjoybody.exe がある所）
#   -Name      zip の中のフォルダの名前（zip の名前にもなる）
#   -OutDir    zip を置く所
#   -WithData  自分の設定・曲データベース・記録・難易度表・リプレイも入れる（自分の別の PC に移すとき）
# 開発用のもの（.pdb・.lib・テストの exe・ログ・スクリーンショット）は入れない
param([string]$Build, [string]$Name, [string]$OutDir, [switch]$WithData)
$ErrorActionPreference = "Stop"
$stage = Join-Path $env:TEMP "bmjoy_package\$Name"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

# 実行ファイルと FFmpeg の DLL、同梱の MOD、素材（LR2files）
Copy-Item "$Build\bmjoy.exe", "$Build\BMjoybody.exe" $stage
Get-ChildItem $Build -Filter *.dll | Copy-Item -Destination $stage
# Visual C++ のランタイム（vcruntime140.dll・msvcp140.dll など）。入れていない PC でも動くように、実行ファイルの横に置く
$crt = Get-ChildItem "${env:ProgramFiles}\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT" -Directory -ErrorAction SilentlyContinue |
    Sort-Object FullName | Select-Object -Last 1
if ($crt) { Get-ChildItem $crt.FullName -Filter *.dll | Copy-Item -Destination $stage }
else { Write-Output "Visual C++ のランタイムが見つからない（入れずに作る）" }
if (Test-Path "$Build\mods") { Copy-Item -Recurse "$Build\mods" "$stage\mods" }
# 使っているもののライセンス（リポジトリの THIRD_PARTY_NOTICES.md。ビルドのフォルダの 2 つ上にある）
$notices = Join-Path (Split-Path (Split-Path $Build)) "THIRD_PARTY_NOTICES.md"
if (Test-Path $notices) { Copy-Item $notices $stage } else { Write-Output "THIRD_PARTY_NOTICES.md が見つからない（入れずに作る）" }
Copy-Item -Recurse "$Build\LR2files" "$stage\LR2files"
# 素材のフォルダに入っている設定と記録は、-WithData のときだけ入れる（入れないときは空の状態で渡す）
foreach ($f in "Config\config.txt", "Config\keyconfig.txt") { Remove-Item -Force "$stage\LR2files\$f" -ErrorAction SilentlyContinue }
foreach ($d in "Database", "Replay", "Mods") { Remove-Item -Recurse -Force "$stage\LR2files\$d" -ErrorAction SilentlyContinue }

if ($WithData) {
    # 今の置き場（実行ファイルの横の LR2files）に設定があればそれを、無ければ前の置き場（%APPDATA%\BMjoy）から写す
    $src = Join-Path $Build "LR2files"
    if (Test-Path "$src\Config\config.txt") {
        foreach ($f in "Config\config.txt", "Config\keyconfig.txt") { if (Test-Path "$src\$f") { Copy-Item "$src\$f" "$stage\LR2files\$f" } }
        foreach ($d in "Database", "Replay", "Mods") { if (Test-Path "$src\$d") { Copy-Item -Recurse "$src\$d" "$stage\LR2files\$d" } }
    } else {
        $old = Join-Path $env:APPDATA "BMjoy"
        New-Item -ItemType Directory -Force "$stage\LR2files\Database", "$stage\LR2files\Replay" | Out-Null
        Copy-Item "$old\config.txt" "$stage\LR2files\Config\config.txt"
        if (Test-Path "$old\keyconfig.txt") { Copy-Item "$old\keyconfig.txt" "$stage\LR2files\Config\keyconfig.txt" }
        Copy-Item "$old\songs.db" "$stage\LR2files\Database\songs.db"
        if (Test-Path "$old\lr2_courses.txt") { Copy-Item "$old\lr2_courses.txt" "$stage\LR2files\Database\lr2_courses.txt" }
        if (Test-Path "$old\tables") { Copy-Item -Recurse "$old\tables" "$stage\LR2files\Database\Table" }
        if (Test-Path "$old\replay") { Get-ChildItem "$old\replay" | Copy-Item -Recurse -Destination "$stage\LR2files\Replay" }
        if (Test-Path "$old\inputlog") { Copy-Item -Recurse "$old\inputlog" "$stage\LR2files\Replay\InputLog" }
        if (Test-Path "$old\mods") { Copy-Item -Recurse "$old\mods" "$stage\LR2files\Mods" }
        if (Test-Path "$old\moddata") { New-Item -ItemType Directory -Force "$stage\LR2files\Mods" | Out-Null; Copy-Item -Recurse "$old\moddata" "$stage\LR2files\Mods\ModData" }
    }
    # 設定のパスのうち、ビルドのフォルダの中を指すものは相対にする（別の PC でもそのまま使える）
    $cfg = "$stage\LR2files\Config\config.txt"
    $text = [IO.File]::ReadAllText($cfg, [Text.Encoding]::UTF8)
    $prefix = (Resolve-Path $Build).Path.TrimEnd('\') + '\'
    $text = $text.Replace("lr2_root = $prefix`r`n", "lr2_root = .`r`n").Replace("lr2_root = $prefix`n", "lr2_root = .`n").Replace($prefix, "")
    [IO.File]::WriteAllText($cfg, $text, (New-Object Text.UTF8Encoding $false))
}

New-Item -ItemType Directory -Force $OutDir | Out-Null
$zip = Join-Path $OutDir "$Name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
Remove-Item -Recurse -Force (Split-Path $stage)
$mb = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Output "ZIP_OK $zip ($mb MB)"
