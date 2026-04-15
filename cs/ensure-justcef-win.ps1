param(
  [Parameter(Mandatory=$true)][string]$Url,
  [Parameter(Mandatory=$true)][string]$ZipPath,
  [Parameter(Mandatory=$true)][string]$ExtractDir,
  [Parameter(Mandatory=$true)][string]$Version
)

$ErrorActionPreference = "Stop"

$markerVersion = Join-Path $ExtractDir ".justcef.version"
$markerUrl     = Join-Path $ExtractDir ".justcef.url"

function Test-DirNonEmpty([string]$Path) {
  if (!(Test-Path -LiteralPath $Path -PathType Container)) { return $false }
  return @(Get-ChildItem -LiteralPath $Path -Force -ErrorAction SilentlyContinue).Count -gt 0
}

$zipBytes = [System.Text.Encoding]::UTF8.GetBytes($ZipPath.ToLowerInvariant())
$sha = [System.Security.Cryptography.SHA256]::Create()
$hash = ($sha.ComputeHash($zipBytes) | ForEach-Object { $_.ToString("x2") }) -join ""
$mutexName = "Global\JustCef_$hash"
$mutex = New-Object System.Threading.Mutex($false, $mutexName)

$null = $mutex.WaitOne()
try {
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ZipPath) | Out-Null
  New-Item -ItemType Directory -Force -Path $ExtractDir | Out-Null

  if ((Test-Path -LiteralPath $markerVersion) `
      -and ((Get-Content -LiteralPath $markerVersion -Raw).Trim() -eq $Version) `
      -and (Test-Path -LiteralPath $ZipPath) `
      -and (Test-DirNonEmpty $ExtractDir)) {
    exit 0
  }

  if (!(Test-Path -LiteralPath $ZipPath)) {
    Write-Host "JustCef: downloading $Url"
    $parent = Split-Path -Parent $ZipPath
    $tmp = Join-Path $parent ("{0}.tmp" -f [System.IO.Path]::GetRandomFileName())
    Invoke-WebRequest -Uri $Url -OutFile $tmp -UseBasicParsing
    Move-Item -LiteralPath $tmp -Destination $ZipPath -Force
  }

  if (Test-Path -LiteralPath $ExtractDir) {
    Remove-Item -LiteralPath $ExtractDir -Recurse -Force
  }
  New-Item -ItemType Directory -Force -Path $ExtractDir | Out-Null

  Expand-Archive -LiteralPath $ZipPath -DestinationPath $ExtractDir -Force

  $entries = Get-ChildItem -LiteralPath $ExtractDir -Force
  if ($entries.Count -eq 1 -and $entries[0].PSIsContainer) {
    $inner = $entries[0].FullName
    $tmpFlatten = "${ExtractDir}.flatten"
    if (Test-Path -LiteralPath $tmpFlatten) { Remove-Item -LiteralPath $tmpFlatten -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $tmpFlatten | Out-Null

    Get-ChildItem -LiteralPath $inner -Force | ForEach-Object {
      Move-Item -LiteralPath $_.FullName -Destination $tmpFlatten -Force
    }

    Remove-Item -LiteralPath $ExtractDir -Recurse -Force
    Move-Item -LiteralPath $tmpFlatten -Destination $ExtractDir -Force
  }

  Set-Content -LiteralPath $markerVersion -Value $Version -NoNewline
  Set-Content -LiteralPath $markerUrl -Value $Url -NoNewline
}
finally {
  $mutex.ReleaseMutex() | Out-Null
  $mutex.Dispose()
}