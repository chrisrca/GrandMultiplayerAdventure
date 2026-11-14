# Test helper: focus the game window and tap a virtual-key code (e.g. 0x1B = Esc).
param([int]$Vk)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class K {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
}
"@
$p = Get-Process gma_wonderlands -ErrorAction Stop
[K]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 300
$sc = [byte][K]::MapVirtualKey($Vk, 0)
[K]::keybd_event([byte]$Vk, $sc, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 80
[K]::keybd_event([byte]$Vk, $sc, 2, [UIntPtr]::Zero)
