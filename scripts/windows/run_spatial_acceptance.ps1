[CmdletBinding()]
param(
    [string]$BuildDir = "build-windows",
    [string]$DataRoot = "test_data/spatial_matrix/windows",
    [string]$OutputDir = "artifacts/windows-spatial-acceptance",
    [switch]$Generate,
    [string]$RamMapPath = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Resolve-Executable {
    param([string]$Name)
    $candidates = @(
        (Join-Path $BuildDir "bin/$Name.exe"),
        (Join-Path $BuildDir "bin/Release/$Name.exe"),
        (Join-Path $BuildDir "Release/$Name.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { return (Resolve-Path $candidate).Path }
    }
    throw "Cannot find $Name under $BuildDir"
}

function Clear-WindowsFileCache {
    if ([string]::IsNullOrWhiteSpace($RamMapPath) -or !(Test-Path $RamMapPath)) {
        Write-Warning "RAMMap is unavailable; cold-cache run records fresh-open process state only."
        return
    }
    & $RamMapPath -Ew | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "RAMMap cache clear returned exit code $LASTEXITCODE"
    }
    Start-Sleep -Seconds 2
}

function Ensure-DataSet {
    param(
        [string]$Generator,
        [string]$Path,
        [long]$Count
    )
    if (!$Generate) {
        if (!(Test-Path $Path)) { throw "Benchmark data not found: $Path" }
        return
    }
    New-Item -ItemType Directory -Force -Path (Split-Path $Path -Parent) | Out-Null
    & $Generator --count $Count --geometry point --distribution uniform --output $Path
    if ($LASTEXITCODE -ne 0) { throw "Data generation failed for $Path" }
}

function Invoke-Benchmark {
    param(
        [string]$Runner,
        [string]$Scale,
        [string]$GdbPath,
        [string]$IoMode,
        [string]$CacheState
    )

    $env:FAST_GDB_RUN_SPATIAL_BENCHMARKS = "1"
    $env:FAST_GDB_BENCHMARK_PATH = (Resolve-Path $GdbPath).Path
    $env:FAST_GDB_BENCHMARK_LABEL = "$Scale point / $IoMode / $CacheState"
    $env:FAST_GDB_WINDOWS_IO_TRACE = "1"
    $env:FAST_GDB_WINDOWS_BATCH_MB = "4"
    $env:FAST_GDB_WINDOWS_SPARSE_WINDOW_MB = "1"
    $env:FAST_GDB_WINDOWS_ASYNC_DEPTH = "4"

    switch ($IoMode) {
        "mmap" {
            $env:FAST_GDB_WINDOWS_MMAP = "1"
            $env:FAST_GDB_WINDOWS_ASYNC_IO = "0"
        }
        "fallback-sync" {
            $env:FAST_GDB_WINDOWS_MMAP = "0"
            $env:FAST_GDB_WINDOWS_ASYNC_IO = "0"
        }
        "fallback-overlapped" {
            $env:FAST_GDB_WINDOWS_MMAP = "0"
            $env:FAST_GDB_WINDOWS_ASYNC_IO = "1"
        }
        default { throw "Unknown I/O mode: $IoMode" }
    }

    if ($CacheState -eq "cold") {
        Clear-WindowsFileCache
        $env:FAST_GDB_BENCHMARK_MODE = "fresh-open"
    } else {
        Remove-Item Env:FAST_GDB_BENCHMARK_MODE -ErrorAction SilentlyContinue
    }

    $baseName = "$Scale-$IoMode-$CacheState"
    $stdoutPath = Join-Path $OutputDir "$baseName.stdout.log"
    $stderrPath = Join-Path $OutputDir "$baseName.stderr.log"
    $combinedPath = Join-Path $OutputDir "$baseName.log"

    $process = Start-Process -FilePath $Runner `
        -ArgumentList "--gtest_filter=SpatialDensityBenchmark.DensityMatrixConfigured" `
        -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath

    $stdout = if (Test-Path $stdoutPath) { Get-Content $stdoutPath -Raw } else { "" }
    $stderr = if (Test-Path $stderrPath) { Get-Content $stderrPath -Raw } else { "" }
    ($stdout + [Environment]::NewLine + $stderr) | Set-Content $combinedPath
    Write-Host $stdout
    if ($stderr) { Write-Host $stderr }

    $peakMb = [math]::Round($process.PeakWorkingSet64 / 1MB, 2)
    [pscustomobject]@{
        scale = $Scale
        io_mode = $IoMode
        cache_state = $CacheState
        benchmark_mode = if ($CacheState -eq "cold") { "fresh-open" } else { "steady-state" }
        exit_code = $process.ExitCode
        peak_rss_mb = $peakMb
        log = $combinedPath
    }

    if ($process.ExitCode -ne 0) {
        throw "Acceptance case failed: $baseName (exit $($process.ExitCode))"
    }
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$runner = Resolve-Executable "gdb_tutorial_test_runner"
$generator = Resolve-Executable "generate_large_gdb"

$dataSets = @(
    @{ Scale = "1m"; Count = 1000000L; Path = (Join-Path $DataRoot "point_1m/point_1m.gdb") },
    @{ Scale = "10m"; Count = 10000000L; Path = (Join-Path $DataRoot "point_10m/point_10m.gdb") }
)

foreach ($dataSet in $dataSets) {
    Ensure-DataSet -Generator $generator -Path $dataSet.Path -Count $dataSet.Count
}

$results = New-Object System.Collections.Generic.List[object]
$ioModes = @("mmap", "fallback-sync", "fallback-overlapped")
$cacheStates = @("cold", "warm")

foreach ($dataSet in $dataSets) {
    foreach ($ioMode in $ioModes) {
        foreach ($cacheState in $cacheStates) {
            $result = Invoke-Benchmark -Runner $runner -Scale $dataSet.Scale `
                -GdbPath $dataSet.Path -IoMode $ioMode -CacheState $cacheState
            $results.Add($result)
        }
    }
}

$csvPath = Join-Path $OutputDir "matrix.csv"
$results | Export-Csv -NoTypeInformation -Path $csvPath
$results | Format-Table -AutoSize
Write-Host "Windows Release acceptance matrix complete: $csvPath"
