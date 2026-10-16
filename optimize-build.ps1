<#
.SYNOPSIS
    Build Optimization Script for ESP32-C6 Matter Controller
.DESCRIPTION
    Configures ccache (20GB), sets environment variables, and adds Windows Defender
    exclusions to prevent real-time I/O lock contention on multi-core CPUs.
#>

param(
    [switch]$SkipDefender
)

$ErrorActionPreference = "Continue"

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "  ESP32-C6 Matter Controller - Build Performance Optimizer " -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

# 1. Locate and configure ccache
$ccacheCandidates = @(
    "C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64\ccache.exe",
    (Get-Command ccache.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source)
)

$ccacheExe = $ccacheCandidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1

if ($ccacheExe) {
    Write-Host "[+] Found ccache: $ccacheExe" -ForegroundColor Green
    & $ccacheExe -M 20G
    Write-Host "[+] Set ccache cache size limit to 20 GB." -ForegroundColor Green
    & $ccacheExe -s
} else {
    Write-Host "[-] ccache not found in expected Espressif path." -ForegroundColor Yellow
}

# 2. Set IDF_CCACHE_ENABLE environment variable
[Environment]::SetEnvironmentVariable("IDF_CCACHE_ENABLE", "1", "User")
$env:IDF_CCACHE_ENABLE = "1"
Write-Host "[+] Enabled IDF_CCACHE_ENABLE=1 in environment." -ForegroundColor Green

# 3. Windows Defender exclusions
if (-not $SkipDefender) {
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    
    $projectDir = $PSScriptRoot
    $espressifDir = "C:\Espressif"
    $processes = @("ccache.exe", "ninja.exe", "riscv32-esp-elf-gcc.exe", "riscv32-esp-elf-g++.exe", "cmake.exe")

    if ($isAdmin) {
        Write-Host "[+] Running as Administrator. Applying Windows Defender exclusions..." -ForegroundColor Cyan
        try {
            Add-MpPreference -ExclusionPath $projectDir, $espressifDir -ErrorAction Stop
            Write-Host "[+] Added folder exclusions: $projectDir, $espressifDir" -ForegroundColor Green
            
            foreach ($proc in $processes) {
                Add-MpPreference -ExclusionProcess $proc -ErrorAction Stop
            }
            Write-Host "[+] Added process exclusions: $($processes -join ', ')" -ForegroundColor Green
        } catch {
            Write-Host "[-] Failed to set Defender preferences: $_" -ForegroundColor Red
        }
    } else {
        Write-Host ""
        Write-Host "==========================================================" -ForegroundColor Yellow
        Write-Host " [NOTICE] To bypass Windows Defender scanning and boost " -ForegroundColor Yellow
        Write-Host " compilation speeds by up to 5x, run this script as Admin," -ForegroundColor Yellow
        Write-Host " or run the following command in an Administrator PowerShell:" -ForegroundColor Yellow
        Write-Host "==========================================================" -ForegroundColor Yellow
        Write-Host "Add-MpPreference -ExclusionPath '$projectDir', '$espressifDir'" -ForegroundColor White
        Write-Host "Add-MpPreference -ExclusionProcess $($processes -join ', ')" -ForegroundColor White
        Write-Host ""
    }
}

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "  Optimization Complete! You can now run 'idf.py build'   " -ForegroundColor Green
Write-Host "==========================================================" -ForegroundColor Cyan
