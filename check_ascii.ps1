# check_ascii.ps1
# Scan source files for any non-ASCII content (Chinese, full-width chars, invalid bytes, etc.).
# Usage: powershell -ExecutionPolicy Bypass -File check_ascii.ps1 [-Path <dir>] [-Pattern <glob>]
# Defaults: -Path = <script_dir>\detTest\Screen, -Pattern = *.cpp,*.h,*.hpp
# Exit code 0 = all pure ASCII, 1 = found non-ASCII content.

param(
    [string]$Path = "",
    [string]$Pattern = "*.cpp,*.h,*.hpp"
)

$ErrorActionPreference = "Stop"

if (-not $Path) {
    $Path = Join-Path $PSScriptRoot "detTest\Screen"
}
if (-not (Test-Path $Path -PathType Container)) {
    Write-Host "ERROR: directory not found: $Path" -ForegroundColor Red
    exit 1
}

$patterns = $Pattern -split "," | ForEach-Object { $_.Trim() } | Where-Object { $_ }
$files = Get-ChildItem -Path $Path -Recurse -File -Include $patterns
if (-not $files) {
    Write-Host "No matching files under: $Path"
    exit 0
}

$totalFiles = 0
$badFiles = 0
$badLines = 0

foreach ($f in $files) {
    $totalFiles++

    # Read raw bytes, strip a UTF-8 BOM if present.
    $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
    $offset = 0
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        $offset = 3
    }
    # Is there any non-ASCII byte in the file body?
    $hasNonAscii = $false
    for ($i = $offset; $i -lt $bytes.Length; $i++) {
        if ($bytes[$i] -gt 127) { $hasNonAscii = $true; break }
    }
    if (-not $hasNonAscii) { continue }

    $badFiles++

    # Decode UTF-8 (invalid bytes become U+FFFD, still non-ASCII) and check line by line.
    $text = [System.Text.Encoding]::UTF8.GetString($bytes, $offset, $bytes.Length - $offset)
    $lines = $text -split "`r?`n", -1
    $lineNo = 0
    $shown = 0
    foreach ($ln in $lines) {
        $lineNo++
        $bad = $false
        foreach ($ch in $ln.ToCharArray()) {
            if ([int]$ch -gt 127) { $bad = $true; break }
        }
        if ($bad) {
            if ($shown -eq 0) {
                Write-Host "FILE: $($f.FullName)" -ForegroundColor Yellow
            }
            $shown++
            $badLines++
            Write-Host ("{0,5}: {1}" -f $lineNo, $ln)
        }
    }
    if ($shown -eq 0) {
        # Non-ASCII bytes existed but could not be attributed to a line (e.g. in a BOM region).
        Write-Host "FILE (unattributable non-ASCII bytes): $($f.FullName)" -ForegroundColor Yellow
    }
}

Write-Host ""
Write-Host "Scanned: $totalFiles file(s) | Files with non-ASCII: $badFiles | Lines affected: $badLines"
if ($badFiles -eq 0) {
    Write-Host "RESULT: ALL FILES ARE PURE ASCII" -ForegroundColor Green
    exit 0
} else {
    Write-Host "RESULT: $badFiles file(s) still contain non-ASCII content" -ForegroundColor Red
    exit 1
}
