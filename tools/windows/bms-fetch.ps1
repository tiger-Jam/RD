# 難易度表の、持っていない譜面を取ってくる（RD の曲データベースと難易度表の手元の版で探し、bms-get で 1 譜面ずつ取る）。
#   -Table         難易度表の名前（一部でよい）・記号・URL。省くと RD に登録した表を全部
#   -Level         レベル（何度でも。例 -Level 5,6）。省くと全部のレベル
#   -Limit         取る数の上限（0 なら全部）
#   -Out           置き場（既定 E:\BMSFolder\Download。bms-get は <置き場>\packages\<md5>\ に置く）
#   -DryRun        取らずに、持っていない譜面の一覧だけを出す
#   -UpdateTables  先に難易度表を取り直す（毎日の自動の収集で使う）
#   -Rescan        取り終わったら、RD が閉じるのを待って曲データベースを読み直す
#   -RetryDays     取れなかった譜面は、この日数のあいだ取りに行かない（既定 7。0 なら毎回）
#   -TableList     難易度表の一覧（JSON。各要素の url）。毎回取り直し、登録した表に足して使う（選曲画面には出さない）。空なら登録した表だけ
# RD が開いているあいだは取らない（始める前は閉じるまで待ち、途中で開いたら取っている分も一時停止して、閉じたら続ける）
#   -Jobs          同時に取る数（既定 4。配布元は譜面ごとにばらばらなので並べて取る。同じ曲の譜面は 1 つずつ）
# 同じ曲の別の譜面を持っていれば（BMS SEARCH の曲ごとの譜面の一覧で調べる）、その曲のフォルダを本体として bms-get に渡す。
# 差分の音源が本体にそろうなら、差分だけを本体のフォルダに入れ、本体は取らない（本体を二重に持たない）
param([string]$Table = "", [string[]]$Level = @(), [int]$Limit = 0,
      [string]$Out = "E:\BMSFolder\Download", [switch]$DryRun, [switch]$UpdateTables, [switch]$Rescan, [int]$RetryDays = 7, [int]$Jobs = 4,
      [string]$TableList = "https://script.google.com/macros/s/AKfycbzaQbcI9UZDcDlSHHl2NHilhmePrNrwxRdOFkmIXsfnbfksKKmAB3V65WZ8jPWU-7E/exec?table=tablelist")
$ErrorActionPreference = "Stop"
# PowerShell 5 は JSON の配列を 1 個のまとまりで返すことがあるので、入れ子をほどいて 1 件ずつにする
function Items($x) { foreach ($a in $x) { if ($a -is [array]) { Items $a } else { $a } } }
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$rd = "C:\Users\mmzkq\BMjoy-beta20261022\build\RelWithDebInfo\RDbody.exe"
# 改造版の bms-get（repo の tools/windows/bms-get-src を Tools\bms-get-new でビルドしてここへ写す）
$bmsget = "C:\Users\mmzkq\Tools\bms-get-bin\bms-get.exe"
$work = Join-Path $env:TEMP "bms-fetch"
New-Item -ItemType Directory -Force $work | Out-Null
$utf8 = New-Object Text.UTF8Encoding $false

# 2 つ同時に動かない（毎日の自動の収集が、前の日の分を取り終わっていないときなど）
try { $lock = [IO.File]::Open((Join-Path $work "fetch.lock"), "OpenOrCreate", "ReadWrite", "None") }
catch { Write-Output "RESULT ほかの収集が動いているので止めた"; exit 0 }

# 遊んでいるか（設定アプリか本体が開いている）。譜面の配信（--serve）と曲データベースの読み直し（--rescan）は遊んでいないものとする
function RD-Open {
    [bool](Get-CimInstance Win32_Process -Filter "Name='rd.exe' OR Name='RDbody.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -notmatch '--serve|--rescan' })
}
# 取っている途中のプロセス（cmd とその下の bms-get）を一時停止・再開する（遊んでいるあいだ HDD とネットを使わないように）
Add-Type -Namespace BmsFetch -Name Proc -MemberDefinition @'
[DllImport("ntdll.dll")] public static extern int NtSuspendProcess(IntPtr h);
[DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr h);
'@
function Set-JobPaused($j, [bool]$pause) {
    $ids = @($j.proc.Id) + @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($j.proc.Id)" -ErrorAction SilentlyContinue | ForEach-Object { $_.ProcessId })
    foreach ($id in $ids) {
        try {
            $p = [Diagnostics.Process]::GetProcessById($id)
            if ($pause) { [void][BmsFetch.Proc]::NtSuspendProcess($p.Handle) } else { [void][BmsFetch.Proc]::NtResumeProcess($p.Handle) }
        } catch { }
    }
}
$logDir = "C:\Users\mmzkq\Tools\fetch-log"
New-Item -ItemType Directory -Force $logDir | Out-Null
$log = Join-Path $logDir ("fetch-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".log")
function Say([string]$s) { Write-Output $s; [IO.File]::AppendAllText($log, (Get-Date -Format "HH:mm:ss ") + "$s`r`n", $utf8) }
if (RD-Open) {
    Say "RD が開いているので、閉じるまで待つ"
    while (RD-Open) { Start-Sleep -Seconds 60 }
}

# 難易度表の一覧から、集める表の URL を作る（取れなければ前の回の一覧を使う）
$tablesFile = "C:\Users\mmzkq\Tools\collect-tables.txt"
$tablesArg = ""
if ($TableList) {
    try {
        $urls = @(Items (Invoke-RestMethod -UseBasicParsing -TimeoutSec 60 $TableList) | ForEach-Object { "$($_.url)".Trim() } | Where-Object { $_ })
        if ($urls.Count -gt 0) { [IO.File]::WriteAllLines($tablesFile, $urls, $utf8); Say "難易度表の一覧：$($urls.Count) 個" }
    } catch { Say "難易度表の一覧を取れないので、前の回の一覧を使う：$($_.Exception.Message)" }
    if (Test-Path $tablesFile) { $tablesArg = "--tables-file `"$tablesFile`"" }
}

if ($UpdateTables) {
    cmd /c "`"$rd`" --update-tables $tablesArg > `"$work\tables.txt`" 2>&1"
    Get-Content -Encoding UTF8 "$work\tables.txt" | ForEach-Object { Say $_ }
}

# 持っていない譜面の一覧（md5、レベル、曲名、アーティスト、表の名前。タブ区切り）。いくつもの表に載っている譜面は 1 回だけ
$levelArgs = ($Level | ForEach-Object { "--level `"$_`"" }) -join " "
$list = Join-Path $work "missing.tsv"
$which = if ($Table) { $Table } else { "*" }
# 持っている譜面も含めて出す（行の 8 つ目が 1 なら持っている）。全部の表のときは、表ごとの譜面の一覧（Excel の表）も作り直す
cmd /c "`"$rd`" --list-charts `"$which`" $levelArgs $tablesArg > `"$list`" 2> `"$work\list.err`""
Get-Content -Encoding UTF8 "$work\list.err" | ForEach-Object { Say $_ }
if (-not $Table -and -not $DryRun -and $Level.Count -eq 0) {
    $sheet = Join-Path ([Environment]::GetFolderPath("Desktop")) "BMS譜面一覧.xlsx"
    # 作れなくても収集は続ける（エラー出力があると、ErrorActionPreference が Stop なので止まってしまう）
    try {
        $r = & py -3 "C:\Users\mmzkq\Tools\chart-sheet.py" $list "C:\Users\mmzkq\Tools\bms-get-data\mirror-index.tsv" $sheet 2>&1
        $r | ForEach-Object { Say "$_" }
    } catch { Say "譜面の一覧を作れなかった：$_" }
}
# 表に書いてある本体と差分の URL は、いくつもの表のぶんをまとめて bms-get に渡す
$seen = @{}
$rows = New-Object Collections.Generic.List[object]
foreach ($line in [IO.File]::ReadLines($list, [Text.Encoding]::UTF8)) {
    if ($line -notmatch "^[0-9a-f]{32}\t") { continue }
    $f = $line -split "`t"
    if ($f.Count -gt 7 -and $f[7] -eq "1") { continue }  # 持っている
    $main = if ($f.Count -gt 5) { $f[5].Trim() } else { "" }
    $diff = if ($f.Count -gt 6) { $f[6].Trim() } else { "" }
    if (-not $seen.ContainsKey($f[0])) {
        $r = [pscustomobject]@{ md5 = $f[0]; level = $f[1]; title = $f[2]; artist = $f[3]; table = $f[4]
                                mains = New-Object Collections.Generic.List[string]; diffs = New-Object Collections.Generic.List[string] }
        $seen[$f[0]] = $r; $rows.Add($r)
    }
    $r = $seen[$f[0]]
    if ($main -match "^https?://" -and -not $r.mains.Contains($main)) { $r.mains.Add($main) }
    if ($diff -match "^https?://" -and -not $r.diffs.Contains($diff)) { $r.diffs.Add($diff) }
}
$rows = $rows.ToArray()

# 前に取れなかった譜面（md5 と最後に試した日）。RetryDays のあいだは飛ばす
$failedFile = Join-Path $logDir "failed.tsv"
$failed = @{}
# bms-get が新しくなったら、前に取れなかった譜面も取り直す（取れる所が増えているかもしれない）
$verFile = Join-Path $logDir "bms-get.ver"
$ver = (Get-Item $bmsget).LastWriteTimeUtc.Ticks.ToString() + "/" + (Get-Item $bmsget).Length
if ((Test-Path $verFile) -and ((Get-Content $verFile -Raw).Trim() -eq $ver) -and (Test-Path $failedFile)) {
    foreach ($line in [IO.File]::ReadLines($failedFile, [Text.Encoding]::UTF8)) { $a = $line -split "`t"; if ($a.Count -ge 2) { $failed[$a[0]] = $a[1] } }
} elseif (Test-Path $failedFile) {
    Say "bms-get が新しくなったので、前に取れなかった譜面も取り直す"
}
if (-not $DryRun) { [IO.File]::WriteAllText($verFile, $ver) }
# 落としておいた差分のミラーの索引（譜面の md5 → 手元の zip。tools/windows/mirror-index.py で作る）
# 索引が新しくなったら、そこにある譜面は前に取れなかったものも取り直す
$mirrorFile = "C:\Users\mmzkq\Tools\bms-get-data\mirror-index.tsv"
$mirror = @{}
if (Test-Path $mirrorFile) {
    foreach ($line in [IO.File]::ReadLines($mirrorFile, [Text.Encoding]::UTF8)) {
        $a = $line -split "`t"
        if ($a.Count -ge 2) { if (-not $mirror.ContainsKey($a[0])) { $mirror[$a[0]] = New-Object Collections.Generic.List[string] }; $mirror[$a[0]].Add($a[1]) }
    }
    $mirrorDate = (Get-Item $mirrorFile).LastWriteTime.ToString("yyyy-MM-dd")
    foreach ($k in @($failed.Keys)) { if ($mirror.ContainsKey($k) -and $failed[$k] -le $mirrorDate) { $failed.Remove($k) } }
}
$cut = (Get-Date).AddDays(-$RetryDays).ToString("yyyy-MM-dd")
$waiting = @($rows | Where-Object { $failed.ContainsKey($_.md5) -and $failed[$_.md5] -gt $cut }).Count
$rows = @($rows | Where-Object { -not ($failed.ContainsKey($_.md5) -and $failed[$_.md5] -gt $cut) })
if ($Limit -gt 0 -and $rows.Count -gt $Limit) { $rows = $rows[0..($Limit - 1)] }
if ($DryRun) {
    $rows | ForEach-Object { "{0}`t{1}`t{2}`t{3}" -f $_.level, $_.title, $_.artist, $_.table }
    Write-Output "RESULT 一覧だけ：$($rows.Count) 件（前に取れず、日を置いている $waiting 件は除く）"
    exit 0
}

# 要望スレ（したらばの BMSクレクレ板と発狂BMS板。消えたスレは Internet Archive から）の索引。週に 1 回作り直す。
# bms-get は、ほかで見つからなかった譜面を、曲名とアーティストでここから探す
$threads = "C:\Users\mmzkq\Tools\bms-get-data\threads.jsonl"
if (-not (Test-Path $threads) -or ((Get-Date) - (Get-Item $threads).LastWriteTime).TotalDays -ge 7) {
    New-Item -ItemType Directory -Force (Split-Path $threads) | Out-Null
    cmd /c "`"$bmsget`" threads `"$threads`" > `"$work\threads.log`" 2>&1"
    Get-Content -Encoding UTF8 "$work\threads.log" | Select-Object -Last 1 | ForEach-Object { Say $_ }
}

# 持っている譜面の md5 → フォルダ（本体のフォルダを探すのに使う）
$ownedFile = Join-Path $work "owned.tsv"
cmd /c "`"$rd`" --list-owned > `"$ownedFile`""
$owned = @{}
foreach ($line in [IO.File]::ReadLines($ownedFile, [Text.Encoding]::UTF8)) {
    $t = $line.IndexOf("`t")
    if ($t -eq 32) { $owned[$line.Substring(0, 32)] = $line.Substring(33) }
}
# BMS SEARCH での曲の id と、同じ曲の別の譜面のうち持っているもののフォルダ（分からなければ空）
function Find-Song([string]$md5) {
    $r = @{ id = ""; body = "" }
    try {
        $p = Invoke-RestMethod -UseBasicParsing -TimeoutSec 20 "https://api.bmssearch.net/v1/patterns/$md5"
        if (-not $p.bms.id) { return $r }
        $r.id = "$($p.bms.id)"
        $all = Invoke-RestMethod -UseBasicParsing -TimeoutSec 20 "https://api.bmssearch.net/v1/bmses/$($p.bms.id)/patterns"
        foreach ($q in (Items $all)) {
            $h = "$($q.file.hashMd5)".ToLower()
            if ($h -and $owned.ContainsKey($h) -and (Test-Path -LiteralPath $owned[$h])) { $r.body = $owned[$h]; break }
        }
    } catch { }
    return $r
}
function Save-Failed {
    $w = New-Object IO.StreamWriter($failedFile, $false, $utf8)
    foreach ($k in $failed.Keys) { $w.WriteLine("$k`t$($failed[$k])") }
    $w.Close()
}

New-Item -ItemType Directory -Force $Out | Out-Null
$ok = 0; $intoBody = 0; $ng = 0
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$today = Get-Date -Format "yyyy-MM-dd"
# この回で取った本体のフォルダ（曲の id → フォルダ）。同じ曲の次の譜面は、差分だけをそこへ入れる
$bodyById = @{}
$running = New-Object Collections.Generic.List[object]
$deferred = New-Object Collections.Generic.List[object]
$freeSlots = New-Object Collections.Generic.Queue[int]
for ($k = 0; $k -lt $Jobs; $k++) { $freeSlots.Enqueue($k) }
$next = 0; $done = 0; $paused = $false

function Finish($j) {
    $script:done++
    $r = $j.row
    $how = ""
    if (Test-Path (Join-Path $Out "placed\$($r.md5).txt")) {
        $script:ok++; $script:intoBody++; $how = "本体のフォルダへ差分だけ"; $failed.Remove($r.md5)
    } elseif (Test-Path (Join-Path $Out "packages\$($r.md5)")) {
        $script:ok++; $how = "取れた"; $failed.Remove($r.md5)
        if ($j.id -and -not $bodyById.ContainsKey($j.id)) { $bodyById[$j.id] = Join-Path $Out "packages\$($r.md5)" }
    } elseif ($j.wasPaused) {
        # 一時停止しているあいだに通信が切れたのかもしれない。取れないことにせず、あとでもう一度取る
        $how = "止めていたあいだに途切れたので、あとで取り直す"; $script:deferred.Add($j.item); $script:done--
    } else { $script:ng++; $how = "取れない"; $failed[$r.md5] = $today }
    Say ("[{0}/{1}] {2} {3} {4}（{5}）" -f $script:done, $rows.Count, $r.level, $r.title, $how, $r.table)
    if ($script:done % 20 -eq 0) { Save-Failed }
    # 長い回でも途中で遊べるように、1000 譜面ごとに曲データベースを読み直す（RD が開いていれば今回は飛ばす）
    if ($Rescan -and $script:done % 1000 -eq 0 -and -not (RD-Open)) { Rescan-Now }
}
function Rescan-Now {
    cmd /c "`"$rd`" --rescan > `"$work\rescan.txt`" 2>&1"
    Get-Content -Encoding UTF8 "$work\rescan.txt" | ForEach-Object { Say $_ }
}

while ($next -lt $rows.Count -or $deferred.Count -gt 0 -or $running.Count -gt 0) {
    # 終わったもの・10 分を過ぎたもの（応答の無い配布元）を片付ける
    for ($k = $running.Count - 1; $k -ge 0; $k--) {
        $j = $running[$k]
        # 一時停止していた時間は数えない
        if (-not $j.proc.HasExited -and ((Get-Date) - $j.start).TotalMinutes - $j.pausedMin -ge 10) {
            try { Start-Process taskkill -ArgumentList "/T /F /PID $($j.proc.Id)" -NoNewWindow -Wait } catch { }
        }
        if ($j.proc.HasExited) { $running.RemoveAt($k); $freeSlots.Enqueue($j.slot); Finish $j }
    }
    # 空いた分だけ始める。同じ曲の譜面が取っている最中なら、後に回す
    # RD が開いたら、取っている分も一時停止して、閉じるまで新しく取り始めない
    if (RD-Open) {
        if (-not $paused) {
            Say "RD が開いたので、取っている $($running.Count) 件を止めて、閉じるまで待つ"
            foreach ($j in $running) { Set-JobPaused $j $true; $j.wasPaused = $true }
            $pausedAt = Get-Date
            $paused = $true
        }
        Start-Sleep -Seconds 5
        continue
    } elseif ($paused) {
        $m = ((Get-Date) - $pausedAt).TotalMinutes
        foreach ($j in $running) { $j.pausedMin += $m; Set-JobPaused $j $false }
        Say "RD が閉じたので続ける"
        $paused = $false
    }
    while ($freeSlots.Count -gt 0) {
        $busy = @{}; foreach ($j in $running) { if ($j.id) { $busy[$j.id] = 1 } }
        $item = $null
        for ($k = 0; $k -lt $deferred.Count; $k++) {
            if (-not $busy.ContainsKey($deferred[$k].id)) { $item = $deferred[$k]; $deferred.RemoveAt($k); break }
        }
        if (-not $item) {
            if ($next -ge $rows.Count) { break }
            $r = $rows[$next]; $next++
            $song = Find-Song $r.md5
            $item = [pscustomobject]@{ row = $r; id = $song.id; body = $song.body }
            if ($item.id -and $busy.ContainsKey($item.id)) { $deferred.Add($item); continue }
        }
        $body = if ($item.body) { $item.body } elseif ($item.id -and $bodyById.ContainsKey($item.id)) { $bodyById[$item.id] } else { "" }
        $bodyArg = if ($body) { "--body-dir `"$body`"" } else { "" }
        # cmd に渡すので、引用符は取り除く（URL と曲名に入っていても意味は変わらない）
        $q = { param($v) "`"" + ($v -replace '"', '') + "`"" }
        $mirrorUrls = if ($mirror.ContainsKey($item.row.md5)) { @($mirror[$item.row.md5] | ForEach-Object { "file:///" + ($_ -replace '\\', '/') }) } else { @() }
        $urlArgs = (@($mirrorUrls | ForEach-Object { "--diff-url " + (& $q $_) }) + @($item.row.mains | ForEach-Object { "--main-url " + (& $q $_) }) + @($item.row.diffs | ForEach-Object { "--diff-url " + (& $q $_) })) -join " "
        # 「-」で始まる曲名を引数の名前と取り違えないよう、= でつなぐ
        $hintArg = if ($item.row.title) { "--hint=" + (& $q $item.row.title) } else { "" }
        if ($item.row.artist) { $hintArg += " --artist=" + (& $q $item.row.artist) }
        if (Test-Path $threads) { $hintArg += " --threads `"$threads`"" }
        $slot = $freeSlots.Dequeue()
        $getLog = Join-Path $logDir "bms-get-$stamp-$slot.log"
        # bms-get は進み具合をエラー出力に書くので、cmd 越しにログへ流す（PowerShell に直接受けさせるとエラー扱いで止まる）
        $p = Start-Process cmd -ArgumentList "/c set RUST_LOG=bms_get=info&& `"$bmsget`" -o `"$Out`" $bodyArg md5 $($item.row.md5) $urlArgs $hintArg >> `"$getLog`" 2>&1" -NoNewWindow -PassThru
        $running.Add([pscustomobject]@{ proc = $p; row = $item.row; id = $item.id; slot = $slot; start = Get-Date; item = $item; wasPaused = $false; pausedMin = 0.0 })
    }
    Start-Sleep -Milliseconds 500
}
Save-Failed
Say "RESULT 取れた $ok 件（うち本体のフォルダへ差分だけ入れた $intoBody 件）、取れなかった $ng 件、前に取れず日を置いている $waiting 件（ログ：$log）"

if ($Rescan -and $ok -gt 0) {
    # RD が開いているあいだは曲データベースを書かない（閉じるまで待つ）
    while (RD-Open) { Start-Sleep -Seconds 60 }
    Rescan-Now
}
$lock.Close()
