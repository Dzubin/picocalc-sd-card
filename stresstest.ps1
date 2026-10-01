# Stress test for the SD drive: writes about 200 files of mixed sizes with known
# content, flushes the volume cache, then reads every file back (unbuffered where
# Windows allows) and compares SHA-256. Run detached; progress goes to stresstest.log.
$drive = 'D'                                  # the drive letter Windows gave the PicoCalc
$log  = Join-Path $PSScriptRoot 'stresstest.log'
$dir  = "${drive}:\sdtest2"
Remove-Item $log -ErrorAction SilentlyContinue
function Log($m) { Add-Content $log ("{0:HH:mm:ss}  {1}" -f (Get-Date), $m) }

$sha  = [Security.Cryptography.SHA256]::Create()
$rng  = New-Object Random 4242
$sizes = @(0, 1, 511, 512, 513, 4095, 4096, 4097, 4194304)
for ($i = 0; $i -lt 150; $i++) { $sizes += $rng.Next(1, 524288) }
for ($i = 0; $i -lt 41; $i++)  { $sizes += $rng.Next(524288, 1572864) }

$hashes = @{}
$sw = [Diagnostics.Stopwatch]::StartNew()
try {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    Log ("writing {0} files" -f $sizes.Count)
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $b = New-Object byte[] $sizes[$i]
        (New-Object Random (9000 + $i)).NextBytes($b)
        $name = "{0}\f{1:D3}.bin" -f $dir, $i
        [IO.File]::WriteAllBytes($name, $b)
        $hashes[$name] = [BitConverter]::ToString($sha.ComputeHash($b))
    }
    Log ("write phase done in {0:N1}s" -f $sw.Elapsed.TotalSeconds)
    Write-VolumeCache $drive
    Log "volume cache flushed"
} catch { Log ("WRITE FAILED: " + $_.Exception.Message); exit 1 }

$bad = 0; $checked = 0
foreach ($name in ($hashes.Keys | Sort-Object)) {
    try {
        $fs = New-Object IO.FileStream($name, 'Open', 'Read', 'None', 4096, ([IO.FileOptions]0x20000000))
        $buf = New-Object byte[] $fs.Length
        $off = 0
        while ($off -lt $buf.Length) { $n = $fs.Read($buf, $off, $buf.Length - $off); if ($n -le 0) { break }; $off += $n }
        $fs.Dispose()
        $h = [BitConverter]::ToString($sha.ComputeHash($buf))
    } catch {
        # unbuffered reads need aligned sizes on some systems: fall back to a normal read
        try { $h = [BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($name))) } catch { $h = 'READ ERROR' }
    }
    $checked++
    if ($h -ne $hashes[$name]) { $bad++; Log ("MISMATCH " + $name) }
}
Log ("verify done: {0} files checked, {1} bad, total {2:N1}s" -f $checked, $bad, $sw.Elapsed.TotalSeconds)
if ($bad -eq 0) { Log "PASS" } else { Log "FAIL" }
