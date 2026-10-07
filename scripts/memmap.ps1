<#
.SYNOPSIS
  Read-only map of a running process's committed memory: Image (per module), Mapped, Private.

.DESCRIPTION
  Walks the target's address space with VirtualQueryEx and sums COMMITTED memory by type.
  Nothing is written to the process and nothing is installed or downloaded; it only needs to
  open the process for query (PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, the same rights
  Task Manager uses), so run it as the same user that runs the game.

  Private memory is grouped by the size of the allocation it belongs to (every region sharing
  one AllocationBase is one VirtualAlloc reservation), by page protection, and the largest
  allocations are listed. Which module made a private allocation is not recorded by Windows, so
  it cannot be attributed to a caller from outside the process. Two hints do survive:
  write-combined pages (RW+WC) are almost always a GPU driver's mappings, and the driver's own
  system-RAM copies of textures show as large private allocations next to a big nvoglv64 /
  atio6axx image. Pair it with FrostMod's memdiag=1 log for the GL side.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\memmap.ps1 -ProcessId (Get-Process mxbikes).Id
.EXAMPLE
  .\memmap.ps1 -ProcessId 12345 -Top 25
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][int]$ProcessId,
    [int]$Top = 15
)
$ErrorActionPreference = 'Stop'

if (-not ('FrostMemMap.Walker' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace FrostMemMap {
    public class Region {
        public ulong Base, AllocBase, Size;
        public uint State, Protect, Type;
    }
    public static class Walker {
        [StructLayout(LayoutKind.Sequential)]
        struct MBI64 {
            public ulong BaseAddress, AllocationBase;
            public uint AllocationProtect, Align1;
            public ulong RegionSize;
            public uint State, Protect, Type, Align2;
        }
        [StructLayout(LayoutKind.Sequential)]
        public struct PMC_EX {
            public uint cb, PageFaultCount;
            public UIntPtr PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
                QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage;
        }
        [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern UIntPtr VirtualQueryEx(IntPtr h, UIntPtr addr, out MBI64 mbi, UIntPtr len);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, EntryPoint = "K32GetMappedFileNameW")]
        static extern uint GetMappedFileName(IntPtr h, UIntPtr addr, StringBuilder name, uint size);
        [DllImport("kernel32.dll", EntryPoint = "K32GetProcessMemoryInfo")]
        static extern bool GetProcessMemoryInfo(IntPtr h, out PMC_EX pmc, uint cb);

        public const uint MEM_COMMIT = 0x1000;
        static IntPtr Open(int pid) {
            IntPtr h = OpenProcess(0x0400 | 0x0010, false, pid);   // QUERY_INFORMATION | VM_READ
            if (h == IntPtr.Zero) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(),
                "OpenProcess(" + pid + ") failed");
            return h;
        }
        public static List<Region> Walk(int pid, Dictionary<ulong, string> names, out PMC_EX pmc) {
            var list = new List<Region>();
            IntPtr h = Open(pid);
            try {
                pmc = new PMC_EX(); pmc.cb = (uint)Marshal.SizeOf(typeof(PMC_EX));
                if (!GetProcessMemoryInfo(h, out pmc, pmc.cb)) pmc.cb = 0;
                ulong addr = 0;
                uint sz = (uint)Marshal.SizeOf(typeof(MBI64));
                var sb = new StringBuilder(1024);
                while (addr < 0x7FFFFFFF0000UL) {
                    MBI64 m;
                    if (VirtualQueryEx(h, new UIntPtr(addr), out m, new UIntPtr(sz)) == UIntPtr.Zero) break;
                    if (m.RegionSize == 0) break;
                    if (m.State == MEM_COMMIT) {
                        list.Add(new Region { Base = m.BaseAddress, AllocBase = m.AllocationBase, Size = m.RegionSize,
                                              State = m.State, Protect = m.Protect, Type = m.Type });
                        if (m.Type != 0x20000 && !names.ContainsKey(m.AllocationBase)) {   // image / mapped
                            sb.Length = 0;
                            uint n = GetMappedFileName(h, new UIntPtr(m.AllocationBase), sb, (uint)sb.Capacity);
                            names[m.AllocationBase] = n > 0 ? sb.ToString() : "";
                        }
                    }
                    ulong next = m.BaseAddress + m.RegionSize;
                    if (next <= addr) break;
                    addr = next;
                }
            } finally { CloseHandle(h); }
            return list;
        }
    }
}
'@
}

function MB([double]$b) { '{0,9:N1}' -f ($b / 1MB) }
function Leaf([string]$p) { if ($p) { ($p -split '\\')[-1] } else { '(no file / pagefile-backed)' } }

$proc = Get-Process -Id $ProcessId
$names = New-Object 'System.Collections.Generic.Dictionary[uint64,string]'
$pmc = New-Object FrostMemMap.Walker+PMC_EX
$regions = [FrostMemMap.Walker]::Walk($ProcessId, $names, [ref]$pmc)

$MEM_PRIVATE = 0x20000; $MEM_MAPPED = 0x40000; $MEM_IMAGE = 0x1000000
$byType = @{ Private = [uint64]0; Mapped = [uint64]0; Image = [uint64]0 }
$cnt    = @{ Private = 0; Mapped = 0; Image = 0 }
$image  = @{}; $mapped = @{}; $alloc = @{}; $prot = @{}
foreach ($r in $regions) {
    switch ($r.Type) {
        $MEM_IMAGE   { $byType.Image += $r.Size; $cnt.Image++
                       $k = Leaf $names[$r.AllocBase]; $image[$k] = [uint64]$image[$k] + $r.Size }
        $MEM_MAPPED  { $byType.Mapped += $r.Size; $cnt.Mapped++
                       $k = Leaf $names[$r.AllocBase]; $mapped[$k] = [uint64]$mapped[$k] + $r.Size }
        $MEM_PRIVATE { $byType.Private += $r.Size; $cnt.Private++
                       $alloc[$r.AllocBase] = [uint64]$alloc[$r.AllocBase] + $r.Size
                       $p = $r.Protect
                       $k = switch ($p -band 0xFF) { 0x04 { 'RW' } 0x02 { 'R' } 0x20 { 'RX' } 0x40 { 'RWX' } 0x01 { 'NOACCESS' } default { '0x{0:X}' -f ($p -band 0xFF) } }
                       if ($p -band 0x400) { $k += '+WC (write-combined: GPU driver)' }
                       elseif ($p -band 0x200) { $k += '+NOCACHE' }
                       if ($p -band 0x100) { $k += '+GUARD' }
                       $prot[$k] = [uint64]$prot[$k] + $r.Size }
    }
}
$total = $byType.Private + $byType.Mapped + $byType.Image

"{0} (PID {1}) - committed {2} MB in {3} regions" -f $proc.ProcessName, $ProcessId, (MB $total).Trim(), $regions.Count
if ($pmc.cb) {
    "  GetProcessMemoryInfo: PrivateUsage {0} MB, WorkingSet {1} MB, PagefileUsage {2} MB" -f `
        (MB $pmc.PrivateUsage.ToUInt64()).Trim(), (MB $pmc.WorkingSetSize.ToUInt64()).Trim(), (MB $pmc.PagefileUsage.ToUInt64()).Trim()
}
""
"Type       Committed MB  Regions"
foreach ($t in 'Private', 'Mapped', 'Image') { "{0,-8} {1}  {2,7}" -f $t, (MB $byType[$t]), $cnt[$t] }

""
"Image by module (top $Top)"
$image.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First $Top |
    ForEach-Object { "  {0} MB  {1}" -f (MB $_.Value), $_.Key }

""
"Mapped by file (top $Top)"
$mapped.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First $Top |
    ForEach-Object { "  {0} MB  {1}" -f (MB $_.Value), $_.Key }

""
"Private by allocation size (committed bytes of one AllocationBase)"
$buckets = @(
    @{ n = '< 64 KB';       lo = 0;     hi = 64KB },
    @{ n = '64 KB - 1 MB';  lo = 64KB;  hi = 1MB },
    @{ n = '1 - 16 MB';     lo = 1MB;   hi = 16MB },
    @{ n = '16 - 64 MB';    lo = 16MB;  hi = 64MB },
    @{ n = '64 - 256 MB';   lo = 64MB;  hi = 256MB },
    @{ n = '>= 256 MB';     lo = 256MB; hi = [double]::MaxValue })
"  {0,-14} {1,7} {2} MB" -f 'size', 'allocs', '   '
foreach ($b in $buckets) {
    $sel = @($alloc.Values | Where-Object { $_ -ge $b.lo -and $_ -lt $b.hi })
    $sum = ($sel | Measure-Object -Sum).Sum; if (-not $sum) { $sum = 0 }
    "  {0,-14} {1,7} {2} MB" -f $b.n, $sel.Count, (MB $sum)
}

""
"Private by protection"
$prot.GetEnumerator() | Sort-Object Value -Descending |
    ForEach-Object { "  {0} MB  {1}" -f (MB $_.Value), $_.Key }

""
"Largest private allocations (top $Top)"
$alloc.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First $Top |
    ForEach-Object { "  {0} MB  @ 0x{1:X12}" -f (MB $_.Value), $_.Key }
