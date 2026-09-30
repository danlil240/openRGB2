param([string]$out)
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;using System.Runtime.InteropServices;
public class W {[DllImport("user32.dll")] public static extern IntPtr FindWindow(string c,string n);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
public struct RECT{public int Left,Top,Right,Bottom;}}
"@
$h=[W]::FindWindow($null,"OpenRGB2")
if($h -eq [IntPtr]::Zero){ # try title match
  $p=Get-Process openrgb2-ui | Where-Object {$_.MainWindowTitle -like '*OpenRGB2*'} | Select-Object -First 1
  if($p){$h=$p.MainWindowHandle}
}
if($h -eq [IntPtr]::Zero){Write-Output "no window";exit 1}
$r=New-Object W+RECT;[W]::GetWindowRect($h,[ref]$r)|Out-Null
[W]::SetForegroundWindow($h)|Out-Null;Start-Sleep -Milliseconds 400
$rw=$r.Right-$r.Left;$rh=$r.Bottom-$r.Top
$bmp=New-Object System.Drawing.Bitmap $rw,$rh
$g=[System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.Left,$r.Top,0,0,$bmp.Size)
$bmp.Save($out,[System.Drawing.Imaging.ImageFormat]::Png)
Write-Output "saved $out ${rw}x${rh}"
