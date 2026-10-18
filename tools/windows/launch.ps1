param([string]$Exe, [string]$ArgLine = "", [string]$WorkDir, [string]$EnvList = "")
# ログイン中のユーザーのデスクトップ（対話セッション）で起動する。SSHのセッションから直接起動すると画面が出ないため
$name = "BMjoyRun"
$envset = ""
foreach ($kv in ($EnvList -split ";")) { if ($kv) { $envset += "set `"$kv`"&& " } }
$arg = "/c ${envset}cd /d `"$WorkDir`" && `"$Exe`" $ArgLine > `"$WorkDir\last_run.log`" 2>&1"
# conhost --headless で動かすと、コマンドの黒い窓を出さない（ログはファイルに出す）
$action = New-ScheduledTaskAction -Execute "conhost.exe" -Argument "--headless cmd.exe $arg"
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName $name -Action $action -Principal $principal -Force | Out-Null
Start-ScheduledTask -TaskName $name
"started: $Exe $ArgLine"
