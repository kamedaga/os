$ErrorActionPreference = "Stop"

Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class Native {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr VirtualAlloc(IntPtr lpAddress, UIntPtr dwSize, UInt32 flAllocationType, UInt32 flProtect);

    [DllImport("kernel32.dll")]
    public static extern IntPtr GetCurrentThread();

    [DllImport("kernel32.dll")]
    public static extern UIntPtr SetThreadAffinityMask(IntPtr hThread, UIntPtr dwThreadAffinityMask);

    [DllImport("kernel32.dll")]
    public static extern bool SetThreadPriority(IntPtr hThread, int nPriority);
}

public delegate ulong CycleFn();
"@

$MEM_COMMIT = 0x1000
$MEM_RESERVE = 0x2000
$PAGE_EXECUTE_READWRITE = 0x40
$THREAD_PRIORITY_HIGHEST = 2

[Native]::SetThreadAffinityMask([Native]::GetCurrentThread(), [UIntPtr]::new(1)) | Out-Null
[Native]::SetThreadPriority([Native]::GetCurrentThread(), $THREAD_PRIORITY_HIGHEST) | Out-Null

function New-RdtscFunction([int]$NopCount) {
    [byte[]]$prefix = @(
        0x53,                         # push rbx
        0x31, 0xC0,                   # xor eax,eax
        0x0F, 0xA2,                   # cpuid
        0x0F, 0x31,                   # rdtsc
        0x48, 0xC1, 0xE2, 0x20,       # shl rdx,32
        0x48, 0x09, 0xD0,             # or rax,rdx
        0x49, 0x89, 0xC0              # mov r8,rax
    )
    [byte[]]$suffix = @(
        0x0F, 0x01, 0xF9,             # rdtscp
        0x48, 0xC1, 0xE2, 0x20,       # shl rdx,32
        0x48, 0x09, 0xD0,             # or rax,rdx
        0x49, 0x89, 0xC1,             # mov r9,rax
        0x31, 0xC0,                   # xor eax,eax
        0x0F, 0xA2,                   # cpuid
        0x4C, 0x89, 0xC8,             # mov rax,r9
        0x4C, 0x29, 0xC0,             # sub rax,r8
        0x5B,                         # pop rbx
        0xC3                          # ret
    )

    [byte[]]$code = New-Object byte[] ($prefix.Length + $NopCount + $suffix.Length)
    [Array]::Copy($prefix, 0, $code, 0, $prefix.Length)
    for ($i = 0; $i -lt $NopCount; $i++) {
        $code[$prefix.Length + $i] = 0x90
    }
    [Array]::Copy($suffix, 0, $code, $prefix.Length + $NopCount, $suffix.Length)

    $ptr = [Native]::VirtualAlloc([IntPtr]::Zero, [UIntPtr]::new([uint64]$code.Length), $MEM_COMMIT -bor $MEM_RESERVE, $PAGE_EXECUTE_READWRITE)
    if ($ptr -eq [IntPtr]::Zero) {
        throw "VirtualAlloc failed"
    }
    [Runtime.InteropServices.Marshal]::Copy($code, 0, $ptr, $code.Length)
    return [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($ptr, [CycleFn])
}

function Get-Stats($Name, [UInt64[]]$Values) {
    [Array]::Sort($Values)
    $n = $Values.Length
    $drop = [int]($n / 20)
    [UInt64]$sum = 0
    for ($i = $drop; $i -lt ($n - $drop); $i++) {
        $sum += $Values[$i]
    }
    $kept = $n - 2 * $drop
    [Console]::WriteLine(("{0,-12} min={1,5} p50={2,5} p90={3,5} p99={4,5} trimmed_mean={5:N2}" -f `
        $Name, $Values[0], $Values[[int]($n / 2)], $Values[[int]($n * 90 / 100)], $Values[[int]($n * 99 / 100)], ([double]$sum / $kept)))
    return [pscustomobject]@{
        Min = $Values[0]
        P50 = $Values[[int]($n / 2)]
        P90 = $Values[[int]($n * 90 / 100)]
        P99 = $Values[[int]($n * 99 / 100)]
        TrimmedMean = [double]$sum / $kept
    }
}

$samples = 200000
if ($args.Count -gt 0) {
    $samples = [Math]::Max(1000, [int]$args[0])
}

$emptyFn = New-RdtscFunction 0
$nop500Fn = New-RdtscFunction 500
$nop5000Fn = New-RdtscFunction 5000
$nop16000Fn = New-RdtscFunction 16000

for ($i = 0; $i -lt 10000; $i++) {
    [void]$emptyFn.Invoke()
    [void]$nop500Fn.Invoke()
    [void]$nop5000Fn.Invoke()
    [void]$nop16000Fn.Invoke()
}

[UInt64[]]$empty = New-Object UInt64[] $samples
[UInt64[]]$nop500 = New-Object UInt64[] $samples
[UInt64[]]$nop5000 = New-Object UInt64[] $samples
[UInt64[]]$nop16000 = New-Object UInt64[] $samples

for ($i = 0; $i -lt $samples; $i++) {
    $empty[$i] = $emptyFn.Invoke()
}
for ($i = 0; $i -lt $samples; $i++) {
    $nop500[$i] = $nop500Fn.Invoke()
}
for ($i = 0; $i -lt $samples; $i++) {
    $nop5000[$i] = $nop5000Fn.Invoke()
}
for ($i = 0; $i -lt $samples; $i++) {
    $nop16000[$i] = $nop16000Fn.Invoke()
}

"samples=$samples, 1-byte nop blocks"
$emptyS = Get-Stats "empty" $empty
$nop500S = Get-Stats "nop500" $nop500
$nop5000S = Get-Stats "nop5000" $nop5000
$nop16000S = Get-Stats "nop16000" $nop16000
"estimate cycles per 500 nops, using trimmed_mean-empty:"
"  nop500:   {0:N2}" -f ($nop500S.TrimmedMean - $emptyS.TrimmedMean)
"  nop5000:  {0:N2}" -f (($nop5000S.TrimmedMean - $emptyS.TrimmedMean) / 10.0)
"  nop16000: {0:N2}" -f (($nop16000S.TrimmedMean - $emptyS.TrimmedMean) / 32.0)
