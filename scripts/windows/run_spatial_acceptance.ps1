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
        throw "RAMMap64.exe is required for the cold-cache acceptance cases."
    }
    & $RamMapPath -Ew | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "RAMMap cache clear returned exit code $LASTEXITCODE"
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

function Parse-BenchmarkOutput {
    param(
        [string]$Stdout,
        [string]$Stderr,
        [string]$Scale,
        [string]$IoMode,
        [string]$CacheState,
        [double]$PeakRssMb,
        [string]$LogPath
    )

    $details = @{}
    $lastCase = $null
    $summaryStarted = $false
    $summaryRows = New-Object System.Collections.Generic.List[object]

    foreach ($line in ($Stdout -split "`r?`n")) {
        if ($line -match '^--- .* Summary \(median of ([0-9]+)\) ---$') {
            $summaryStarted = $true
            continue
        }

        if (!$summaryStarted -and $line -match '^(coverage_[^ ]+)\s+([0-9]+)\s+([0-9.]+)%\s+(\S+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)$') {
            $lastCase = $Matches[1]
            $details[$lastCase] = @{
                candidates = [long]$Matches[2]
                candidate_ratio_pct = [double]$Matches[3]
                execution_path = $Matches[4]
                last_fast_wall_ms = [double]$Matches[5]
                last_gdal_wall_ms = [double]$Matches[6]
                candidate_lookup_ms = [double]$Matches[7]
                geometry_scan_ms = [double]$Matches[8]
                blob_lookup_ms = [double]$Matches[9]
                bbox_filter_ms = [double]$Matches[10]
                exact_filter_ms = [double]$Matches[11]
                invalid_geometries = -1
                result_count = -1
            }
            continue
        }

        if (!$summaryStarted -and $null -ne $lastCase -and
            $line -match '^\s+funnel: candidate=([0-9]+) rejected=([0-9]+) contained=([0-9]+) exact=([0-9]+) invalid=([0-9]+) result=([0-9]+)') {
            $details[$lastCase].invalid_geometries = [long]$Matches[5]
            $details[$lastCase].result_count = [long]$Matches[6]
            continue
        }

        if ($summaryStarted -and $line -match '^(coverage_[^ ]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)$') {
            $caseName = $Matches[1]
            if (!$details.ContainsKey($caseName)) {
                throw "Missing detailed benchmark row for $caseName"
            }
            $detail = $details[$caseName]
            $summaryRows.Add([pscustomobject]@{
                scale = $Scale
                io_mode = $IoMode
                cache_state = $CacheState
                cache_clear_scope = if ($CacheState -eq "cold") { "before benchmark process" } else { "warm-up then steady-state" }
                coverage = $caseName
                trials = 20
                execution_path = $detail.execution_path
                candidates = $detail.candidates
                candidate_ratio_pct = $detail.candidate_ratio_pct
                fast_median_ms = [double]$Matches[2]
                gdal_median_ms = [double]$Matches[3]
                fast_p95_ms = [double]$Matches[4]
                gdal_p95_ms = [double]$Matches[5]
                fast_gdal_ratio = [double]$Matches[6]
                geometry_scan_ms_last = $detail.geometry_scan_ms
                query_wall_total_ms_last = $detail.last_fast_wall_ms
                invalid_geometries_last = $detail.invalid_geometries
                result_count_last = $detail.result_count
                peak_rss_mb = $PeakRssMb
                log = $LogPath
            })
        }
    }

    if ($summaryRows.Count -ne 5) {
        throw "Expected five coverage rows, found $($summaryRows.Count) in $LogPath"
    }

    $batchReads = 0L
    $readBytes = 0L
    $maxAsyncDepth = 0
    foreach ($line in ($Stderr -split "`r?`n")) {
        if ($line -match 'batch_reads=([0-9]+) bytes=([0-9]+)(?: async_depth=([0-9]+))?') {
            $batchReads += [long]$Matches[1]
            $readBytes += [long]$Matches[2]
            if ($Matches[3]) {
                $maxAsyncDepth = [math]::Max($maxAsyncDepth, [int]$Matches[3])
            }
        }
    }
    foreach ($row in $summaryRows) {
        Add-Member -InputObject $row -NotePropertyName batch_read_calls -NotePropertyValue $batchReads
        Add-Member -InputObject $row -NotePropertyName batch_read_bytes -NotePropertyValue $readBytes
        Add-Member -InputObject $row -NotePropertyName max_async_depth -NotePropertyValue $maxAsyncDepth
    }
    return $summaryRows
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
    $env:FAST_GDB_BENCHMARK_TRIALS = "20"
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

    if ($process.ExitCode -ne 0) {
        throw "Acceptance case failed: $baseName (exit $($process.ExitCode))"
    }

    $peakMb = [math]::Round($process.PeakWorkingSet64 / 1MB, 2)
    return Parse-BenchmarkOutput -Stdout $stdout -Stderr $stderr `
        -Scale $Scale -IoMode $IoMode -CacheState $CacheState `
        -PeakRssMb $peakMb -LogPath $combinedPath
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
            $rows = Invoke-Benchmark -Runner $runner -Scale $dataSet.Scale `
                -GdbPath $dataSet.Path -IoMode $ioMode -CacheState $cacheState
            foreach ($row in $rows) { $results.Add($row) }
        }
    }
}

$csvPath = Join-Path $OutputDir "matrix.csv"
$results | Export-Csv -NoTypeInformation -Path $csvPath
$results | Format-Table scale, io_mode, cache_state, coverage, fast_median_ms, fast_p95_ms, gdal_median_ms, fast_gdal_ratio, invalid_geometries_last, peak_rss_mb -AutoSize
Write-Host "Windows Release acceptance matrix complete: $csvPath"
