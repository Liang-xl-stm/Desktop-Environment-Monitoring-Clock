Option Explicit
Dim shell, fso, folder, pythonw, pythonRoot, candidate, command
Set shell = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")
folder = fso.GetParentFolderName(WScript.ScriptFullName)
pythonRoot = shell.ExpandEnvironmentStrings("%LOCALAPPDATA%") & "\Programs\Python"
pythonw = pythonRoot & "\Python314\pythonw.exe"
If Not fso.FileExists(pythonw) And fso.FolderExists(pythonRoot) Then
    For Each candidate In fso.GetFolder(pythonRoot).SubFolders
        If fso.FileExists(candidate.Path & "\pythonw.exe") Then
            pythonw = candidate.Path & "\pythonw.exe"
            Exit For
        End If
    Next
End If
If Not fso.FileExists(pythonw) Then
    MsgBox "Python was not found. Install Python and Pillow, or launch picture_sender_gui.pyw with your Python.", 48, "Picture Sender"
    WScript.Quit 1
End If
shell.CurrentDirectory = folder
command = Chr(34) & pythonw & Chr(34) & " " & Chr(34) & folder & "\picture_sender_gui.pyw" & Chr(34)
shell.Run command, 0, False
