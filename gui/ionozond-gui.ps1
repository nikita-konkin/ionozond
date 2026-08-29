<#
.SYNOPSIS
    Launch the reconstructed dsChirp (or the original) in a browser window.

.DESCRIPTION
    Runs the app inside the Linux dev container on a virtual display, and
    publishes that display over noVNC. You drive it in a normal browser --
    nothing to install on Windows beyond Docker Desktop.

.PARAMETER Mode
    app       the reconstruction                          (default)
    original  the shipped binary, for side-by-side checks
    both      both at once on one desktop
    viewer    standalone capture viewer

.PARAMETER Data
    Archive directory to mount read-only at /data.
    Default: F:\MyData\ND\lfs

.PARAMETER Capture
    For -Mode viewer: the .lfs file to open, as a path inside /data,
    e.g. "cyprus1_20191023_071510.lfs".

.EXAMPLE
    .\ionozond-gui.ps1
    .\ionozond-gui.ps1 -Mode both
    .\ionozond-gui.ps1 -Mode viewer -Capture cyprus1_20191023_071510.lfs
    .\ionozond-gui.ps1 -Data F:\MyData\ND\lfs\ionozond_data2
#>
[CmdletBinding()]
param(
    [ValidateSet('app', 'original', 'both', 'viewer')]
    [string]$Mode = 'app',

    [string]$Data = 'F:\MyData\ND\lfs',

    [string]$Capture = '',

    [string]$Project = 'N:\ds_shirp_revers_eng\ionozond',

    # Directory holding bin\dsChirp, the shipped binary. Not part of this
    # repository; only -Mode original and -Mode both need it.
    [string]$Original = 'N:\ds_shirp_revers_eng\dsChirp',

    [int]$Port = 6080,

    [string]$Geometry = '1600x1000'
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Project)) { throw "Project directory not found: $Project" }
if (-not (Test-Path $Data))    { throw "Data directory not found: $Data" }

if ($Mode -eq 'original' -or $Mode -eq 'both') {
    if (-not (Test-Path "$Original\bin\dsChirp")) {
        throw "-Mode $Mode runs the shipped binary, but no bin\dsChirp under: $Original"
    }
}

# Is the image there?
$img = docker images -q ionozond-dev 2>$null
if (-not $img) {
    Write-Host "Building the dev image (first run only, a few minutes)..." -ForegroundColor Yellow
    docker build -t ionozond-dev -f "$Project\docker\Dockerfile" "$Project\docker"
    if ($LASTEXITCODE -ne 0) { throw "docker build failed" }
}

$args = @($Mode)
if ($Mode -eq 'viewer') {
    if (-not $Capture) { throw "-Mode viewer needs -Capture <file.lfs>" }
    $args += "/data/$Capture"
}

Write-Host ""
Write-Host "  mode      $Mode"          -ForegroundColor Cyan
Write-Host "  data      $Data"          -ForegroundColor Cyan
Write-Host "  browser   http://localhost:$Port/vnc.html" -ForegroundColor Green
Write-Host ""
Write-Host "  Starting... the page will be ready once you see the banner below." -ForegroundColor Gray
Write-Host "  Press Ctrl-C here to shut it down." -ForegroundColor Gray
Write-Host ""

# Open the browser once the server has had a moment to come up.
Start-Job -ScriptBlock {
    param($p)
    Start-Sleep -Seconds 12
    Start-Process "http://localhost:$p/vnc.html?autoconnect=true&resize=scale"
} -ArgumentList $Port | Out-Null

# run-gui.sh reads the source from /work/ionozond and the shipped binary from
# /work/dsChirp, so mount each where it expects to find it.
$mounts = @(
    '-v', "${Project}:/work/ionozond",
    '-v', "${Data}:/data:ro"
)
if (Test-Path "$Original\bin\dsChirp") {
    $mounts += @('-v', "${Original}:/work/dsChirp:ro")
}

docker run --rm -it `
    -m 6g `
    -p "${Port}:6080" `
    @mounts `
    -e "GEOMETRY=$Geometry" `
    ionozond-dev bash /work/ionozond/gui/run-gui.sh @args
