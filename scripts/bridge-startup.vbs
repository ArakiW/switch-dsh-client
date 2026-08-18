' bridge-startup.vbs — 启动 dsh-bridge(隐藏窗口,适合开机自启)
' 用法: wscript bridge-startup.vbs
Set WshShell = CreateObject("WScript.Shell")
Dim cmd
cmd = Chr(34) & "C:\Program Files\nodejs\node.exe" & Chr(34) & " " & Chr(34) & "C:\Users\L\Documents\Codex\switch-dsh-client\bridge\dsh-bridge.js" & Chr(34) & " --host 0.0.0.0 --port 8765"
WshShell.Run cmd, 0, False
Set WshShell = Nothing
