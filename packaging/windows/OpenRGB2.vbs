Option Explicit

Dim files, shell, root, launcher, command
Set files = CreateObject("Scripting.FileSystemObject")
Set shell = CreateObject("WScript.Shell")
root = files.GetParentFolderName(WScript.ScriptFullName)
launcher = files.BuildPath(files.BuildPath(root, "scripts"), "start-openrgb2-gui.ps1")

If Not files.FileExists(launcher) Then
    MsgBox "OpenRGB2 startup file is missing:" & vbCrLf & launcher, vbCritical, "OpenRGB2 startup"
    WScript.Quit 1
End If

command = "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File " & Chr(34) & launcher & Chr(34)
On Error Resume Next
shell.Run command, 0, False
If Err.Number <> 0 Then
    MsgBox "OpenRGB2 could not launch:" & vbCrLf & Err.Description, vbCritical, "OpenRGB2 startup"
    WScript.Quit 1
End If
On Error GoTo 0
