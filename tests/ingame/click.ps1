# Test helper: focus the game window and left-click at screen coordinates (x, y).
param([int]$X, [int]$Y)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class In {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
}
"@
$p = Get-Process gma_wonderlands -ErrorAction Stop
[In]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 400
[In]::SetCursorPos($X, $Y) | Out-Null
Start-Sleep -Milliseconds 150
[In]::mouse_event(0x2, 0, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 80
[In]::mouse_event(0x4, 0, 0, 0, [UIntPtr]::Zero)
