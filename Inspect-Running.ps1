param([int]$ProcessId = 0, [string]$ProcessName = 'Morrowind')
$ErrorActionPreference = 'Stop'

# Read-only: no injection, process writes, threads, UI input or game changes.
if (-not ('ModernizerReadOnlyMemory' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ModernizerReadOnlyMemory {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr OpenProcess(uint access, bool inherit, uint id);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool ReadProcessMemory(IntPtr process, IntPtr address,
        byte[] buffer, UIntPtr size, out UIntPtr read);
    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr handle);
    [DllImport("user32.dll")]
    public static extern bool GetClientRect(IntPtr window, out RECT rectangle);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr window, out RECT rectangle);
    [DllImport("user32.dll")]
    public static extern int GetWindowLongW(IntPtr window, int index);
    [DllImport("user32.dll")]
    public static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll")]
    public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("shcore.dll")]
    public static extern int GetProcessDpiAwareness(IntPtr process, out int awareness);
}
'@
}

# Process.Modules may enumerate only the native WOW64 support DLLs when a
# 64-bit PowerShell inspects this 32-bit game. Request both module lists.
if (-not ('ModernizerModuleSnapshot' -as [type])) {
    Add-Type @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class ModernizerModuleSnapshot {
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    private struct MODULEENTRY32 {
        public uint dwSize, th32ModuleID, th32ProcessID, GlblcntUsage, ProccntUsage;
        public IntPtr modBaseAddr;
        public uint modBaseSize;
        public IntPtr hModule;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=256)] public string szModule;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=260)] public string szExePath;
    }
    public sealed class Module {
        public string Name, Path;
        public long BaseAddress;
        public uint Size;
    }
    [DllImport("kernel32.dll", SetLastError=true)]
    private static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint processId);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Module32FirstW(IntPtr snapshot, ref MODULEENTRY32 module);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool Module32NextW(IntPtr snapshot, ref MODULEENTRY32 module);
    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr handle);
    public static Module[] Read(uint processId) {
        IntPtr snapshot = new IntPtr(-1);
        // A loader change can transiently yield ERROR_BAD_LENGTH.
        for (int attempt = 0; attempt < 8; ++attempt) {
            snapshot = CreateToolhelp32Snapshot(0x08 | 0x10, processId);
            if (snapshot != new IntPtr(-1)) break;
            int error = Marshal.GetLastWin32Error();
            if (error != 24 || attempt == 7) throw new Win32Exception(error, "Toolhelp module snapshot failed");
        }
        try {
            MODULEENTRY32 entry = new MODULEENTRY32();
            entry.dwSize = (uint)Marshal.SizeOf(typeof(MODULEENTRY32));
            if (!Module32FirstW(snapshot, ref entry))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Toolhelp first module failed");
            List<Module> modules = new List<Module>();
            HashSet<long> addresses = new HashSet<long>();
            for (;;) {
                long address = IntPtr.Size == 4 ? (long)unchecked((uint)entry.modBaseAddr.ToInt32()) : entry.modBaseAddr.ToInt64();
                // The main EXE can appear in both the native and WOW64 lists.
                if (addresses.Add(address)) modules.Add(new Module {
                    Name = entry.szModule, Path = entry.szExePath, Size = entry.modBaseSize, BaseAddress = address
                });
                entry.dwSize = (uint)Marshal.SizeOf(typeof(MODULEENTRY32));
                if (Module32NextW(snapshot, ref entry)) continue;
                int error = Marshal.GetLastWin32Error();
                if (error != 18) throw new Win32Exception(error, "Toolhelp next module failed");
                return modules.ToArray();
            }
        } finally { CloseHandle(snapshot); }
    }
}
'@
}

if ($ProcessId) {
    $targetProcess = Get-Process -Id $ProcessId
} else {
    $candidates = @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)
    if ($candidates.Count -ne 1) {
        throw "Expected one $ProcessName process; found $($candidates.Count). Pass -ProcessId explicitly."
    }
    $targetProcess = $candidates[0]
}

$processHandle = [ModernizerReadOnlyMemory]::OpenProcess(0x0410, $false, $targetProcess.Id)
if ($processHandle -eq [IntPtr]::Zero) {
    throw "OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ) failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
}

function Read-Remote([long]$Address, [int]$Length) {
    $buffer = [byte[]]::new($Length)
    $read = [UIntPtr]::Zero
    $ok = [ModernizerReadOnlyMemory]::ReadProcessMemory($processHandle,
        [IntPtr]::new($Address), $buffer, [UIntPtr]::new([uint64]$Length), [ref]$read)
    if (!$ok -or $read.ToUInt64() -ne $Length) {
        throw ('ReadProcessMemory at 0x{0:X} ({1} bytes) failed: {2}' -f $Address, $Length,
            [Runtime.InteropServices.Marshal]::GetLastWin32Error())
    }
    return ,$buffer
}

function Read-U32([long]$Address) {
    return [BitConverter]::ToUInt32((Read-Remote $Address 4), 0)
}

function Read-Ascii([long]$Address, [int]$Limit = 256) {
    $bytes = Read-Remote $Address $Limit
    $end = [Array]::IndexOf($bytes, [byte]0)
    if ($end -lt 0) { throw 'Unterminated remote import name.' }
    return [Text.Encoding]::ASCII.GetString($bytes, 0, $end)
}

try {
    # Keep this inspector's Win32 rectangle queries in physical pixels too.
    [void][ModernizerReadOnlyMemory]::SetThreadDpiAwarenessContext([IntPtr]::new(-4))
    $modules = @([ModernizerModuleSnapshot]::Read([uint32]$targetProcess.Id) | ForEach-Object {
        [pscustomobject]@{
            name = $_.Name
            baseAddress = $_.BaseAddress
            size = $_.Size
            path = $_.Path
        }
    })
    $game = $modules | Where-Object { $_.name -ieq ($targetProcess.ProcessName + '.exe') } | Select-Object -First 1
    if (!$game) { throw 'The game executable is missing from the Toolhelp module snapshot.' }
    $modernizer = $modules | Where-Object { $_.name -ieq 'modernizer.dll' } | Select-Object -First 1
    $interesting = @('CreateFileA', 'CreateFileW', 'ReadFile', 'SetFilePointer', 'CloseHandle', 'ExitProcess',
        'RegQueryValueExA', 'CreateWindowExA', 'SetWindowPos', 'Direct3DCreate8', 'GetProcAddress')
    $imports = [System.Collections.Generic.List[object]]::new()

    foreach ($module in ($modules | Where-Object { $_ -eq $game -or $_.name -ieq 'msvcrt.dll' -or $_.name -ieq 'binkw32.dll' })) {
        $baseAddress = [long]$module.baseAddress
        $peOffset = Read-U32 ($baseAddress + 0x3C)
        if ($peOffset -gt 0x100000) { throw 'Invalid loaded PE offset.' }
        $pe = Read-Remote ($baseAddress + $peOffset) 160
        if ([BitConverter]::ToUInt32($pe, 0) -ne 0x4550 -or
            [BitConverter]::ToUInt16($pe, 24) -ne 0x10B) { throw 'Expected loaded PE32 image.' }
        $importRva = [BitConverter]::ToUInt32($pe, 24 + 104)
        if (!$importRva) { continue }
        for ($descriptorIndex = 0; $descriptorIndex -lt 256; ++$descriptorIndex) {
            $descriptor = Read-Remote ($baseAddress + $importRva + 20 * $descriptorIndex) 20
            $nameRva = [BitConverter]::ToUInt32($descriptor, 12)
            if (!$nameRva) { break }
            $originalRva = [BitConverter]::ToUInt32($descriptor, 0)
            $targetRva = [BitConverter]::ToUInt32($descriptor, 16)
            if (!$originalRva) { continue }
            $importDll = Read-Ascii ($baseAddress + $nameRva)
            for ($thunkIndex = 0; $thunkIndex -lt 16384; ++$thunkIndex) {
                $namePointer = Read-U32 ($baseAddress + $originalRva + 4 * $thunkIndex)
                if (!$namePointer) { break }
                if ($namePointer -ge 0x80000000L) { continue }
                $symbol = Read-Ascii ($baseAddress + $namePointer + 2)
                if ($symbol -notin $interesting) { continue }
                $iatAddress = $baseAddress + $targetRva + 4 * $thunkIndex
                $target = [long](Read-U32 $iatAddress)
                $isHooked = $modernizer -and $target -ge $modernizer.baseAddress -and
                    $target -lt ($modernizer.baseAddress + $modernizer.size)
                $imports.Add([pscustomobject]@{
                    module = $module.name
                    importedFrom = $importDll
                    function = $symbol
                    iatAddress = ('0x{0:X8}' -f $iatAddress)
                    targetAddress = ('0x{0:X8}' -f $target)
                    expectedHook = (($symbol -notin @('ExitProcess', 'RegQueryValueExA', 'CreateWindowExA', 'SetWindowPos', 'Direct3DCreate8', 'GetProcAddress')) -or $module -eq $game)
                    pointsIntoModernizer = [bool]$isHooked
                })
            }
        }
    }

    $branch = Read-Remote ($game.baseAddress + 0x2A0A4) 6
    $resolutionPatched = (@($branch | Where-Object { $_ -ne 0x90 }).Count -eq 0)
    $sortCall = Read-Remote ($game.baseAddress + 0x29DEB) 5
    $sortTarget = ([long]$game.baseAddress + 0x29DF0 + [BitConverter]::ToInt32($sortCall, 1)) -band 0xFFFFFFFFL
    $sortHooked = $sortCall[0] -eq 0xE8 -and $modernizer -and
        $sortTarget -ge $modernizer.baseAddress -and $sortTarget -lt ($modernizer.baseAddress + $modernizer.size)
    $thresholds = foreach ($rva in @(0x866C5, 0x866DB, 0x866E8, 0x866F5)) {
        $instruction = Read-Remote ($game.baseAddress + $rva) 6
        [pscustomobject]@{
            address = ('0x{0:X8}' -f ($game.baseAddress + $rva))
            bytes = (($instruction | ForEach-Object { $_.ToString('X2') }) -join ' ')
            threshold = [BitConverter]::ToInt32($instruction, 2)
        }
    }
    $world = [long](Read-U32 ($game.baseAddress + 0x3C67DC))
    $options = [long](Read-U32 ($game.baseAddress + 0x3C6CDC))
    $display = $null
    if ($world -and $options) {
        $mainWindow = [long](Read-U32 ($world + 0xC8))
        $rendererWindow = [long](Read-U32 ($world + 0xCC))
        $renderer = [long](Read-U32 ($world + 0x30))
        $backbufferInfo = if ($renderer) { [long](Read-U32 ($renderer + 0x544)) } else { 0 }
        $d3dFactory = [long](Read-U32 ($game.baseAddress + 0x3DD8C4))
        $d3dObject = [long](Read-U32 ($game.baseAddress + 0x3DD8C0))
        $d3dTable = if ($d3dObject) { [long](Read-U32 $d3dObject) } else { 0 }
        $deviceCreator = if ($d3dTable) { [long](Read-U32 ($d3dTable + 15 * 4)) } else { 0 }
        $creatorHooked = $modernizer -and $deviceCreator -ge $modernizer.baseAddress -and
            $deviceCreator -lt ($modernizer.baseAddress + $modernizer.size)
        $awareness = 0
        $dpiResult = [ModernizerReadOnlyMemory]::GetProcessDpiAwareness($processHandle, [ref]$awareness)
        $client = [ModernizerReadOnlyMemory+RECT]::new()
        $outer = [ModernizerReadOnlyMemory+RECT]::new()
        [void][ModernizerReadOnlyMemory]::GetClientRect([IntPtr]::new($mainWindow), [ref]$client)
        [void][ModernizerReadOnlyMemory]::GetWindowRect([IntPtr]::new($mainWindow), [ref]$outer)
        $display = [pscustomobject]@{
            mainWindow = ('0x{0:X8}' -f $mainWindow)
            rendererWindow = ('0x{0:X8}' -f $rendererWindow)
            mainWindowStyle = ('0x{0:X8}' -f [uint32]([long][ModernizerReadOnlyMemory]::GetWindowLongW([IntPtr]::new($mainWindow), -16) -band 0xFFFFFFFFL))
            mainWindowExtendedStyle = ('0x{0:X8}' -f [uint32]([long][ModernizerReadOnlyMemory]::GetWindowLongW([IntPtr]::new($mainWindow), -20) -band 0xFFFFFFFFL))
            clientWidth = $client.Right - $client.Left
            clientHeight = $client.Bottom - $client.Top
            outerRectangle = $outer
            dpi = [ModernizerReadOnlyMemory]::GetDpiForWindow([IntPtr]::new($mainWindow))
            dpiAwarenessResult = $dpiResult
            dpiAwareness = $awareness
            gameFullscreenFlag = (Read-Remote ($options + 0x1C) 1)[0]
            gameOptionsWidth = Read-U32 ($options + 8)
            gameOptionsHeight = Read-U32 ($options + 12)
            logicalWidth = Read-U32 ($world + 0x78)
            logicalHeight = Read-U32 ($world + 0x7C)
            backbufferWidth = if ($backbufferInfo) { Read-U32 $backbufferInfo } else { 0 }
            backbufferHeight = if ($backbufferInfo) { Read-U32 ($backbufferInfo + 4) } else { 0 }
            dynamicD3dFactoryAddress = ('0x{0:X8}' -f $d3dFactory)
            d3dObjectAddress = ('0x{0:X8}' -f $d3dObject)
            createDeviceTargetAddress = ('0x{0:X8}' -f $deviceCreator)
            createDeviceHooked = [bool]$creatorHooked
        }
    }
    [pscustomobject]@{
        processId = $targetProcess.Id
        processName = $targetProcess.ProcessName
        executable = $game.path
        observedUtc = [DateTime]::UtcNow.ToString('o')
        inspectorBitness = [IntPtr]::Size * 8
        moduleEnumeration = 'Toolhelp32: SNAPMODULE | SNAPMODULE32'
        modernizerLoaded = [bool]$modernizer
        resolutionBranchAddress = ('0x{0:X8}' -f ($game.baseAddress + 0x2A0A4))
        resolutionBytes = (($branch | ForEach-Object { $_.ToString('X2') }) -join ' ')
        resolutionPatched = $resolutionPatched
        resolutionSortCallBytes = (($sortCall | ForEach-Object { $_.ToString('X2') }) -join ' ')
        resolutionSortTargetAddress = ('0x{0:X8}' -f $sortTarget)
        resolutionSortHooked = [bool]$sortHooked
        display = $display
        experimentalThresholds = @($thresholds)
        imports = @($imports.ToArray())
        modules = @($modules | ForEach-Object {
            [pscustomobject]@{ name = $_.name; baseAddress = ('0x{0:X8}' -f $_.baseAddress); size = $_.size; path = $_.path }
        })
    } | ConvertTo-Json -Depth 8
} finally {
    [void][ModernizerReadOnlyMemory]::CloseHandle($processHandle)
}
