# dedup.ps1 で移したフォルダを元の場所へ戻す（moves-*.tsv を指定）。写したファイルはそのまま残る
param([Parameter(Mandatory = $true)][string]$Moves)
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$n = 0
foreach ($l in [IO.File]::ReadAllLines($Moves, [Text.Encoding]::UTF8)) {
    $src, $dst = $l -split "`t"
    if ([IO.Directory]::Exists($dst) -and -not [IO.Directory]::Exists($src)) { [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($src)) | Out-Null; [IO.Directory]::Move($dst, $src); $n++ }
}
"戻した $n フォルダ"
