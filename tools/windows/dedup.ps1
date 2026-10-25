# 同じ曲のフォルダが複数あるとき、残す方を 1 つにする（plan.json は songs.db から作った計画）。
#   譜面がばらけている組は、残す方に無いファイルを写してから移す。名前が同じで中身の大きさが違うファイルがある組は触らない
#   移したフォルダは E:\BMSFolder\_重複\ に元の形で置き、moves.tsv に記録する（dedup-undo.ps1 で戻せる）
param([string]$Plan = "$env:TEMP\bms-fetch\plan.json", [switch]$Run)
$ErrorActionPreference = "Stop"
# PowerShell 5 は JSON の配列を 1 個のまとまりで返すことがあるので、入れ子をほどいて 1 件ずつにする
function Items($x) { foreach ($a in $x) { if ($a -is [array]) { Items $a } else { $a } } }
[Console]::OutputEncoding = [Text.Encoding]::UTF8
# 音のファイルの、拡張子を除いた相対パス（小文字）
function Stem([string]$rel) { $rel.Substring(0, $rel.Length - [IO.Path]::GetExtension($rel).Length).ToLower() }
$base = "E:\BMSFolder"; $trash = "E:\BMSFolder\_重複"
$groups = [IO.File]::ReadAllText($Plan, [Text.Encoding]::UTF8) | ConvertFrom-Json
$logDir = "C:\Users\mmzkq\Tools\dedup-log"; New-Item -ItemType Directory -Force $logDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$moves = if ($Run) { New-Object IO.StreamWriter("$logDir\moves-$stamp.tsv", $false, (New-Object Text.UTF8Encoding $false)) }
$report = New-Object IO.StreamWriter("$logDir\report-$stamp.txt", $false, (New-Object Text.UTF8Encoding $false))
$nMove = 0; $nCopy = 0; $bCopy = 0; $nConflict = 0; $nMissing = 0; $nFail = 0; $nOther = 0
$i = 0
foreach ($g in (Items $groups)) {
    $i++; if ($i % 200 -eq 0) { "{0} 組目まで見た" -f $i; $report.Flush() }
    $others = @(Items $g.others | Where-Object { [IO.Directory]::Exists($_) })
    if (-not [IO.Directory]::Exists($g.keep)) { $nMissing++; $report.WriteLine("残す方が無い`t$($g.keep)"); continue }
    if ($others.Count -eq 0) { continue }
    $copies = New-Object Collections.Generic.List[object]; $seen = @{}
    if ($g.merge) {
        $skip = @{}; foreach ($p in (Items $g.skipcharts)) { $skip[$p] = 1 }
        $conflict = $false
        # 音のファイルの名前が半分以上同じものだけを同じ曲とみなす（同じ譜面のファイルが別の曲に紛れていることがある）
        $keepSounds = @{}
        foreach ($f in [IO.Directory]::EnumerateFiles($g.keep, "*", [IO.SearchOption]::AllDirectories)) {
            if ($f -match '\.(wav|ogg|flac|mp3)$') { $keepSounds[(Stem $f.Substring($g.keep.Length + 1))] = 1 }
        }
        $same = @()
        foreach ($o in $others) {
            $all = 0; $hit = 0
            foreach ($f in [IO.Directory]::EnumerateFiles($o, "*", [IO.SearchOption]::AllDirectories)) {
                if ($f -match '\.(wav|ogg|flac|mp3)$') { $all++; if ($keepSounds.ContainsKey((Stem $f.Substring($o.Length + 1)))) { $hit++ } }
            }
            if ($all -gt 0 -and $hit * 2 -ge $all) { $same += $o } else { $nOther++; $report.WriteLine("別の曲かもしれないので触らない`t$o`t（音 $hit/$all が同じ名前）") }
        }
        $others = $same
        foreach ($o in $others) {
            foreach ($f in [IO.Directory]::EnumerateFiles($o, "*", [IO.SearchOption]::AllDirectories)) {
                if ($skip.ContainsKey($f)) { continue }
                if ($f -match '\\__MACOSX\\|\\\._[^\\]*$|\\\.DS_Store$') { continue }
                $dst = Join-Path $g.keep $f.Substring($o.Length + 1)
                # 残す方に拡張子違いの同じ音がある（wav と ogg など。BMS は拡張子を読み替えて鳴らす）
                if ($f -match '\.(wav|ogg|flac|mp3)$' -and $keepSounds.ContainsKey((Stem $f.Substring($o.Length + 1)))) { continue }
                if ([IO.File]::Exists($dst)) {
                    # 譜面は中身で比べる（大きさが同じで中身の違う別の版がある）
                    $differ = if ($f -match '\.(bms|bme|bml|pms|bmson)$') { (Get-FileHash -Algorithm MD5 -LiteralPath $f).Hash -ne (Get-FileHash -Algorithm MD5 -LiteralPath $dst).Hash }
                              else { (New-Object IO.FileInfo $f).Length -ne (New-Object IO.FileInfo $dst).Length }
                    if ($differ) { $conflict = $true; $report.WriteLine("名前がぶつかる`t$f`t$dst") }
                    continue
                }
                if ($seen.ContainsKey($dst)) { continue }
                $seen[$dst] = 1; $copies.Add(@($f, $dst))
            }
        }
        if ($conflict) { $nConflict++; $report.WriteLine("触らない組`t$($g.keep)"); continue }
    }
    foreach ($c in $copies) {
        $nCopy++; $bCopy += (New-Object IO.FileInfo $c[0]).Length
        $report.WriteLine("写す`t$($c[0])`t$($c[1])")
        if ($Run) { [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($c[1])) | Out-Null; [IO.File]::Copy($c[0], $c[1], $false) }
    }
    foreach ($o in $others) {
        $dst = Join-Path $trash $o.Substring($base.Length + 1)
        $k = 2; while ([IO.Directory]::Exists($dst) -or [IO.File]::Exists($dst)) { $dst = (Join-Path $trash $o.Substring($base.Length + 1)) + " ($k)"; $k++ }
        $nMove++
        $report.WriteLine("移す`t$o`t$dst")
        if ($Run) {
            try { [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($dst)) | Out-Null; [IO.Directory]::Move($o, $dst); $moves.WriteLine("$o`t$dst"); $moves.Flush() }
            catch { $nFail++; $report.WriteLine("移せない`t$o`t$($_.Exception.Message)") }
        }
    }
}
$report.Close(); if ($moves) { $moves.Close() }
"{0} 移す {1} フォルダ／写す {2} ファイル（{3:N1} MB）／名前がぶつかって触らない組 {4}／残す方が無い {5}／別の曲かもしれず触らない {7}／失敗 {6}" -f $(if ($Run) { "実行：" } else { "試し：" }), $nMove, $nCopy, ($bCopy / 1MB), $nConflict, $nMissing, $nFail, $nOther
"記録：$logDir\report-$stamp.txt"
