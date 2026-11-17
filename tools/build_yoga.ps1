#requires -Version 7
<#
Builds ns-autoverify for one commit on the Yoga (devkitA64 + libnx from the unpacked devkitPro there), booked in the
Yoga build ledger, at nice 19 under a memory and time cap. Brings back the Stage 1 SD layout (NO boot2.flag), the
elf/nso/npdm/map, the build log and host-test log, SHA256SUMS.txt and build.json, then releases the booking and
deletes the remote lane. Never installs or changes packages on the Yoga.

  /usr/bin/pwsh -NoLogo -NoProfile -NonInteractive -File tools/build_yoga.ps1 -Rev <commit> -Out <dir>
#>
param(
    [Parameter(Mandatory)][string]$Rev,
    [Parameter(Mandatory)][string]$Out,
    [string]$Repo = (Split-Path $PSScriptRoot -Parent),
    [string]$BuildHost = 'memento-claw-yoga-ubuntu',
    [string]$DevkitPro = '/home/intpa/smash-replay-export-campaign/grok-nro/private/devkitpro/root/opt/devkitpro',
    [string]$Lane = 'ns-autoverify',
    [int]$ClaimWaitSeconds = 1800
)
$ErrorActionPreference = 'Stop'
$ssh = @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=15')
function Say($m) { Write-Host "[build_yoga $(Get-Date -AsUTC -Format 'HH:mm:ss')Z] $m" }
$tid = '4200000000004156'
$full = git -C $Repo rev-parse $Rev
$short = $full.Substring(0, 8)
New-Item -ItemType Directory -Force $Out | Out-Null
$tar = Join-Path ([IO.Path]::GetTempPath()) "ns-autoverify-$short.tar"
git -C $Repo archive --format=tar -o $tar $full

# Book the host; if it is busy, wait in <=60 s slices (bounded), never queue-jump.
$deadline = (Get-Date).AddSeconds($ClaimWaitSeconds)
$bid = $null
while (-not $bid) {
    $claim = & ssh @ssh $BuildHost "/usr/bin/python3 -I ~/build-hosts/bin/ledger.py claim --host yoga --owner ns-autoverify --note 'ns-autoverify $short'" 2>&1
    if ($LASTEXITCODE -eq 0) {
        $last = (@($claim) | Where-Object { $_ -and $_.Trim() } | Select-Object -Last 1).Trim()
        $bid = if ($last -match '^[0-9a-f]{6,}$') { $last } else { ([regex]::Match(($claim -join ' '), 'bid=(\S+)')).Groups[1].Value }
        if (-not $bid) { throw "no booking id in the claim output: $claim" }
        break
    }
    if ((Get-Date) -gt $deadline) { throw "ledger claim still refused after $ClaimWaitSeconds s: $claim" }
    Say "Yoga busy, waiting 60 s: $(@($claim) -join ' ')"
    Start-Sleep -Seconds 60
}
Say "claimed bid=$bid"
try {
    $remote = "build-hosts/lanes/$Lane"
    & ssh @ssh $BuildHost "Remove-Item -Recurse -Force ~/$remote -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force ~/$remote/src | Out-Null"
    & scp -q @ssh $tar "${BuildHost}:$remote/src.tar"
    $script = @"
Set-Location ~/$remote/src
tar -xmf ../src.tar
`$env:DEVKITPRO = '$DevkitPro'
systemd-run --user --scope -q -p MemoryMax=2G /usr/bin/timeout 600 nice -n 19 ionice -c 3 make test *> ../test.log
`$t = `$LASTEXITCODE
"exit=`$t" | Add-Content ../test.log
systemd-run --user --scope -q -p MemoryMax=2G /usr/bin/timeout 900 nice -n 19 ionice -c 3 make -j2 all *> ../build.log
`$code = `$LASTEXITCODE
"exit=`$code" | Add-Content ../build.log
if (`$t -ne 0) { exit 90 }
exit `$code
"@
    $b64 = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($script))
    & ssh @ssh $BuildHost "/usr/bin/pwsh -NoLogo -NoProfile -NonInteractive -EncodedCommand $b64"
    $code = $LASTEXITCODE
    & scp -q @ssh "${BuildHost}:$remote/build.log" (Join-Path $Out 'build.log')
    & scp -q @ssh "${BuildHost}:$remote/test.log" (Join-Path $Out 'test.log')
    if ($code -ne 0) { throw "remote build failed (exit $code; 90 = host tests); see $Out/build.log and test.log" }
    & scp -q -r @ssh "${BuildHost}:$remote/src/out/stage1" (Join-Path $Out 'stage1')
    foreach ($f in 'ns-autoverify.elf', 'ns-autoverify.nso', 'ns-autoverify.npdm', 'ns-autoverify.map') {
        & scp -q @ssh "${BuildHost}:$remote/src/out/$f" (Join-Path $Out $f)
    }
    if (Test-Path (Join-Path $Out "stage1/atmosphere/contents/$tid/flags")) { throw 'stage1 tree contains flags/: refusing' }
    $sums = Get-ChildItem $Out -Recurse -File | Where-Object Name -notin 'SHA256SUMS.txt', 'build.json' | Sort-Object FullName | ForEach-Object {
        "$((Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower())  $([IO.Path]::GetRelativePath($Out, $_.FullName))"
    }
    $sums | Set-Content (Join-Path $Out 'SHA256SUMS.txt')
    $nsp = Get-Item (Join-Path $Out "stage1/atmosphere/contents/$tid/exefs.nsp")
    $tbx = Get-Item (Join-Path $Out "stage1/atmosphere/contents/$tid/toolbox.json")
    $gcc = & ssh @ssh $BuildHost "$DevkitPro/devkitA64/bin/aarch64-none-elf-gcc --version" 2>&1 | Select-Object -First 1
    [ordered]@{ rev = $full; host = $BuildHost; devkitpro = $DevkitPro; gcc = "$gcc"; booking = $bid
        nsp_bytes = $nsp.Length; nsp_sha256 = (Get-FileHash $nsp.FullName -Algorithm SHA256).Hash.ToLower()
        toolbox_sha256 = (Get-FileHash $tbx.FullName -Algorithm SHA256).Hash.ToLower()
        boot2_flag_present = $false; time = (Get-Date -AsUTC -Format o) } |
        ConvertTo-Json | Set-Content (Join-Path $Out 'build.json')
    Say "ok: exefs.nsp $($nsp.Length) B"
} finally {
    & ssh @ssh $BuildHost "/usr/bin/python3 -I ~/build-hosts/bin/ledger.py release --host yoga --owner ns-autoverify --bid $bid --note 'ns-autoverify $short done'" | Out-Null
    & ssh @ssh $BuildHost "Remove-Item -Recurse -Force ~/build-hosts/lanes/$Lane -ErrorAction SilentlyContinue; Test-Path ~/build-hosts/lanes/$Lane"
    Remove-Item $tar -ErrorAction SilentlyContinue
    Say "released bid=$bid, lane cleaned"
}
