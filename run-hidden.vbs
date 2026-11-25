' Starts iPodStats with no window. Used by the Windows Startup shortcut.
Set sh = CreateObject("WScript.Shell")
dir = CreateObject("Scripting.FileSystemObject").GetParentFolderName(WScript.ScriptFullName)
sh.CurrentDirectory = dir
sh.Run """" & dir & "\iPodStats.exe""", 0, False
' Now-playing helper (needs Windows PowerShell 5.1 for WinRT). It exits by itself if already running.
sh.Run "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File """ & dir & "\media.ps1""", 0, False
