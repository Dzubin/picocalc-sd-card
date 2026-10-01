# Verify-only pass for stresstest.ps1: rebuilds the expected content of each file from the
# same seeds and compares it with what is on the drive now (run after an eject and
# mount again, so Windows reads from the card and not from its cache).
$drive = 'D'                                  # the drive letter Windows gave the PicoCalc
$log = Join-Path $PSScriptRoot 'stressverify.log'
$dir = "${drive}:\sdtest2"
Remove-Item $log -ErrorAction SilentlyContinue
function Log($m) { Add-Content $log ("{0:HH:mm:ss}  {1}" -f (Get-Date), $m) }

$sha = [Security.Cryptography.SHA256]::Create()
$rng = New-Object Random 4242
$sizes = @(0, 1, 511, 512, 513, 4095, 4096, 4097, 4194304)
for ($i = 0; $i -lt 150; $i++) { $sizes += $rng.Next(1, 524288) }
for ($i = 0; $i -lt 41; $i++)  { $sizes += $rng.Next(524288, 1572864) }

$sw = [Diagnostics.Stopwatch]::StartNew()
$bad = 0; $checked = 0; $bytes = 0
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $name = "{0}\f{1:D3}.bin" -f $dir, $i
    $b = New-Object byte[] $sizes[$i]
    (New-Object Random (9000 + $i)).NextBytes($b)
    $want = [BitConverter]::ToString($sha.ComputeHash($b))
    try { $got = [BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($name))) } catch { $got = 'READ ERROR ' + $_.Exception.Message }
    $checked++; $bytes += $sizes[$i]
    if ($got -ne $want) { $bad++; Log ("MISMATCH " + $name + "  " + $got) }
}
Log ("verified {0} files, {1:N1} MB, {2} bad, {3:N1}s" -f $checked, ($bytes / 1MB), $bad, $sw.Elapsed.TotalSeconds)
if ($bad -eq 0) { Log "PASS" } else { Log "FAIL" }
