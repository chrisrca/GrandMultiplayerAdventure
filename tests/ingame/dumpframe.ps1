# Test helper: restore/focus the game, ask the mod for a frame dump, save it as PNG.
param([string]$Out)
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class WD { [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c); [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h); }
"@
$G = "C:\Program Files (x86)\Steam\steamapps\common\GMA Wonderlands"
$p = Get-Process gma_wonderlands -ErrorAction Stop
if ([WD]::IsIconic($p.MainWindowHandle)) { [WD]::ShowWindow($p.MainWindowHandle, 9) | Out-Null; [WD]::SetForegroundWindow($p.MainWindowHandle) | Out-Null; Start-Sleep 4 }
Remove-Item "$G\gma_mp_shot.bmp" -ErrorAction SilentlyContinue
New-Item "$G\gma_mp_shot.req" -ItemType File -Force | Out-Null
for ($i = 0; $i -lt 30 -and -not (Test-Path "$G\gma_mp_shot.bmp"); $i++) { Start-Sleep -Milliseconds 200 }
Start-Sleep -Milliseconds 500
Add-Type -AssemblyName System.Drawing
$b = [System.Drawing.Image]::FromFile("$G\gma_mp_shot.bmp")
$b.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$b.Dispose()
Remove-Item "$G\gma_mp_shot.bmp" -ErrorAction SilentlyContinue
