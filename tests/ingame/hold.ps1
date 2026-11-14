# Test helper: focus the game and hold a sequence of keys.
#   hold.ps1 -Seq "0x25:1500,0x27:1500"     (virtual-key:milliseconds, ...)
# Refuses to send input unless the game window really is in the foreground.
param([string]$Seq)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class KH {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
}
"@
$p = Get-Process gma_wonderlands -ErrorAction Stop
[KH]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 300
if ([KH]::GetForegroundWindow() -ne $p.MainWindowHandle) { throw "game is not in the foreground; not sending input" }
foreach ($step in $Seq.Split(',')) {
    $vk, $ms = $step.Split(':')
    $vk = [Convert]::ToInt32($vk, 16)
    $sc = [byte][KH]::MapVirtualKey($vk, 0)
    if ($vk -ne 0) { [KH]::keybd_event([byte]$vk, $sc, 0, [UIntPtr]::Zero) }
    Start-Sleep -Milliseconds ([int]$ms)
    if ($vk -ne 0) { [KH]::keybd_event([byte]$vk, $sc, 2, [UIntPtr]::Zero) }
}
