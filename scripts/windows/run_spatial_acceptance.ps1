[CmdletBinding()]
param(
    [string]$BuildDir = "build-windows",
    [string]$BaselineBuildDir = "",
    [string]$DataRoot = "test_data/spatial_matrix/windows",
    [string]$OutputDir = "artifacts/windows-spatial-acceptance",
    [switch]$Generate,
    [string]$RamMapPath = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Resolve-Executable {
    param([string]$Root, [string]$Name)
    $candidates = @(
        (Join-Path $Root "bin/$Name.exe"),
        (Join-Path $Root "bin/Release/$Name.exe"),
        (Join-Path $Root "Release/$Name.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { return (Resolve-Path $candidate).Path }
    }
    throw "Cannot find $Name under $Root"
}

function Ensure-DataSet {
    param([string]$Generator, [string]$Path, [long]$Count)
    if (!$Generate) {
        if (!(Test-Path $Path)) { throw "Benchmark data not found: $Path" }
        return
    }
    New-Item -ItemType Directory -Force -Path (Split-Path $Path -Parent) | Out-Null
    & $Generator --count $Count --geometry point --distribution uniform --output $Path
    if ($LASTEXITCODE -ne 0) { throw "Data generation failed for $Path" }
}

function Assert-ObservedIoPath {
    param([string]$IoMode, [string]$Stderr, [string]$Reference)
    if ($Reference -ne "current") { return }

    $mmapSuccess = $Stderr -match 'fast-gdb windows mmap: success'
    $fallbackSync = $Stderr -match 'mode=fallback-sync'
    $fallbackOverlapped = $Stderr -match 'mode=fallback-overlapped'
    $overlappedObserved = $Stderr -match 'overlapped_batches=([1-9][0-9]*)'
    $parallelObserved = $Stderr -match 'async_depth=([2-9][0-9]*)'

    switch ($IoMode) {
        "mmap" {
            if (!$mmapSuccess) { throw "mmap case did not observe a successful MapViewOfFile path" }
            if ($fallbackSync -or $fallbackOverlapped) {
                throw "mmap case unexpectedly entered a fallback geometry path"
            }
        }
        "fallback-sync" {
            if ($mmapSuccess) { throw "fallback-sync unexpectedly mapped the table" }
            if (!$fallbackSync) { throw "fallback-sync path was not observed" }
            if ($overlappedObserved -or $parallelObserved) {
                throw "fallback-sync used OVERLAPPED or parallel I/O"
            }
        }
        "fallback-overlapped" {
            if ($mmapSuccess) { throw "fallback-overlapped unexpectedly mapped the table" }
            if (!$fallbackOverlapped) { throw "fallback-overlapped path was not observed" }
            if (!$overlappedObserved) { throw "fallback-overlapped issued no OVERLAPPED batches" }
            if (!$parallelObserved) { throw "fallback-overlapped never reached async depth >= 2" }
        }
        default { throw "Unknown I/O mode: $IoMode" }
    }
}

function Parse-BenchmarkOutput {
    param(
        [string]$Stdout,
        [string]$Stderr,
        [string]$Scale,
        [string]$IoMode,
        [string]$CacheState,
        [string]$Coverage,
        [string]$Reference,
        [double]$PeakRssMb,
        [string]$LogPath
    )

    $detail = $null
    $summary = $null
    $trials = 0
    $lastCase = $null
    $summaryStarted = $false

    foreach ($line in ($Stdout -split "`r?`n")) {
        if ($line -match '^--- .* Summary \(median of ([0-9]+)\) ---$') {
            $summaryStarted = $true
            $trials = [int]$Matches[1]
            continue
        }
        if (!$summaryStarted -and $line -match '^(coverage_[^ ]+)\s+([0-9]+)\s+([0-9.]+)%\s+(\S+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)$') {
            $lastCase = $Matches[1]
            $detail = @{
                coverage = $Matches[1]
                candidates = [long]$Matches[2]
                candidate_ratio_pct = [double]$Matches[3]
                execution_path = $Matches[4]
                query_wall_total_ms_last = [double]$Matches[5]
                gdal_wall_total_ms_last = [double]$Matches[6]
                candidate_lookup_ms_last = [double]$Matches[7]
                geometry_scan_ms_last = [double]$Matches[8]
                blob_lookup_ms_last = [double]$Matches[9]
                bbox_filter_ms_last = [double]$Matches[10]
                exact_filter_ms_last = [double]$Matches[11]
                invalid_geometries_last = -1
                result_count_last = -1
            }
            continue
        }
        if (!$summaryStarted -and $null -ne $lastCase -and
            $line -match '^\s+funnel: candidate=([0-9]+) rejected=([0-9]+) contained=([0-9]+) exact=([0-9]+) invalid=([0-9]+) result=([0-9]+)') {
            $detail.invalid_geometries_last = [long]$Matches[5]
            $detail.result_count_last = [long]$Matches[6]
            continue
        }
        if ($summaryStarted -and $line -match '^(coverage_[^ ]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)$') {
            $summary = @{
                coverage = $Matches[1]
                fast_median_ms = [double]$Matches[2]
                gdal_median_ms = [double]$Matches[3]
                fast_p95_ms = [double]$Matches[4]
                gdal_p95_ms = [double]$Matches[5]
                fast_gdal_ratio = [double]$Matches[6]
            }
        }
    }

    if ($null -eq $detail -or $null -eq $summary) {
        throw "Missing detailed or summary benchmark row in $LogPath"
    }
    if ($detail.coverage -ne $Coverage -or $summary.coverage -ne $Coverage) {
        throw "Expected $Coverage but parsed $($detail.coverage)/$($summary.coverage)"
    }
    if ($detail.invalid_geometries_last -ne 0) {
        throw "Invalid geometries were observed for $Scale/$IoMode/$CacheState/$Coverage"
    }

    $batchReads = 0L
    $readBytes = 0L
    $exactReads = 0L
    $exactBytes = 0L
    $overlappedBatches = 0L
    $maxAsyncDepth = 0
    foreach ($line in ($Stderr -split "`r?`n")) {
        if ($line -match 'batch_reads=([0-9]+) bytes=([0-9]+) exact_reads=([0-9]+) exact_bytes=([0-9]+) overlapped_batches=([0-9]+) async_depth=([0-9]+) failed=(true|false)') {
            if ($Matches[7] -eq "true") { throw "I/O trace reported a failed optimized path" }
            $batchReads += [long]$Matches[1]
            $readBytes += [long]$Matches[2]
            $exactReads += [long]$Matches[3]
            $exactBytes += [long]$Matches[4]
            $overlappedBatches += [long]$Matches[5]
            $maxAsyncDepth = [math]::Max($maxAsyncDepth, [int]$Matches[6])
        }
    }

    Assert-ObservedIoPath -IoMode $IoMode -Stderr $Stderr -Reference $Reference

    return [pscustomobject]@{
        reference = $Reference
        scale = $Scale
        io_mode = $IoMode
        cache_state = $CacheState
        cache_clear_scope = if ($CacheState -eq "cold") { "before every fast-gdb and GDAL sample" } else { "warm-up then steady-state" }
        coverage = $Coverage
        trials = $trials
        execution_path = $detail.execution_path
        candidates = $detail.candidates
        candidate_ratio_pct = $detail.candidate_ratio_pct
        fast_median_ms = $summary.fast_median_ms
        gdal_median_ms = $summary.gdal_median_ms
        fast_p95_ms = $summary.fast_p95_ms
        gdal_p95_ms = $summary.gdal_p95_ms
        fast_gdal_ratio = $summary.fast_gdal_ratio
        geometry_scan_ms_last = $detail.geometry_scan_ms_last
        query_wall_total_ms_last = $detail.query_wall_total_ms_last
        invalid_geometries_last = $detail.invalid_geometries_last
        result_count_last = $detail.result_count_last
        peak_rss_mb = $PeakRssMb
        batch_read_calls = $batchReads
        batch_read_bytes = $readBytes
        exact_read_calls = $exactReads
        exact_read_bytes = $exactBytes
        overlapped_batch_calls = $overlappedBatches
        max_async_depth = $maxAsyncDepth
        log = $LogPath
    }
}

function Invoke-Benchmark {
    param(
        [string]$Runner,
        [string]$Scale,
        [string]$GdbPath,
        [string]$IoMode,
        [string]$CacheState,
        [string]$Coverage,
        [string]$Reference
    )

    $env:FAST_GDB_RUN_SPATIAL_BENCHMARKS = "1"
    $env:FAST_GDB_BENCHMARK_PATH = (Resolve-Path $GdbPath).Path
    $env:FAST_GDB_BENCHMARK_LABEL = "$Reference / $Scale / $IoMode / $CacheState / $Coverage"
    $env:FAST_GDB_BENCHMARK_CASE = $Coverage
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
        if ([string]::IsNullOrWhiteSpace($RamMapPath) -or !(Test-Path $RamMapPath)) {
            throw "RAMMap64.exe is required for strict cold-cache cases"
        }
        $resolvedRamMap = (Resolve-Path $RamMapPath).Path
        $env:FAST_GDB_BENCHMARK_MODE = "fresh-open"
        $env:FAST_GDB_BENCHMARK_STRICT_COLD = "1"
        $env:FAST_GDB_BENCHMARK_CACHE_CLEAR_COMMAND = "`"$resolvedRamMap`" -Ew"
    } else {
        Remove-Item Env:FAST_GDB_BENCHMARK_MODE -ErrorAction SilentlyContinue
        Remove-Item Env:FAST_GDB_BENCHMARK_STRICT_COLD -ErrorAction SilentlyContinue
        Remove-Item Env:FAST_GDB_BENCHMARK_CACHE_CLEAR_COMMAND -ErrorAction SilentlyContinue
    }

    $baseName = "$Reference-$Scale-$IoMode-$CacheState-$Coverage"
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
        -Coverage $Coverage -Reference $Reference `
        -PeakRssMb $peakMb -LogPath $combinedPath
}

function Assert-BaselineRegression {
    param([System.Collections.Generic.List[object]]$Rows)
    $currentRows = @($Rows | Where-Object { $_.reference -eq "current" -and $_.io_mode -eq "fallback-sync" })
    $baselineRows = @($Rows | Where-Object { $_.reference -eq "main" })
    foreach ($current in $currentRows) {
        $baseline = $baselineRows | Where-Object {
            $_.scale -eq $current.scale -and
            $_.cache_state -eq $current.cache_state -and
            $_.coverage -eq $current.coverage
        } | Select-Object -First 1
        if ($null -eq $baseline) { throw "Missing main baseline for $($current.scale)/$($current.cache_state)/$($current.coverage)" }
        $limit = $baseline.fast_median_ms * 1.05
        if ($current.fast_median_ms -gt $limit) {
            throw "fallback-sync regressed >5% for $($current.scale)/$($current.cache_state)/$($current.coverage): current=$($current.fast_median_ms) baseline=$($baseline.fast_median_ms)"
        }
    }
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$runner = Resolve-Executable -Root $BuildDir -Name "gdb_tutorial_test_runner"
$generator = Resolve-Executable -Root $BuildDir -Name "generate_large_gdb"
$baselineRunner = if ([string]::IsNullOrWhiteSpace($BaselineBuildDir)) { $null } else {
    Resolve-Executable -Root $BaselineBuildDir -Name "gdb_tutorial_test_runner"
}

$dataSets = @(
    @{ Scale = "1m"; Count = 1000000L; Path = (Join-Path $DataRoot "point_1m/point_1m.gdb") },
    @{ Scale = "10m"; Count = 10000000L; Path = (Join-Path $DataRoot "point_10m/point_10m.gdb") }
)
foreach ($dataSet in $dataSets) {
    Ensure-DataSet -Generator $generator -Path $dataSet.Path -Count $dataSet.Count
}

$coverages = @("coverage_01pct", "coverage_10pct", "coverage_30pct", "coverage_80pct", "coverage_full")
$ioModes = @("mmap", "fallback-sync", "fallback-overlapped")
$cacheStates = @("cold", "warm")
$results = New-Object System.Collections.Generic.List[object]

foreach ($dataSet in $dataSets) {
    foreach ($ioMode in $ioModes) {
        foreach ($cacheState in $cacheStates) {
            foreach ($coverage in $coverages) {
                $results.Add((Invoke-Benchmark -Runner $runner -Scale $dataSet.Scale `
                    -GdbPath $dataSet.Path -IoMode $ioMode -CacheState $cacheState `
                    -Coverage $coverage -Reference "current"))
            }
        }
    }
}

if ($null -ne $baselineRunner) {
    foreach ($dataSet in $dataSets) {
        foreach ($cacheState in $cacheStates) {
            foreach ($coverage in $coverages) {
                $results.Add((Invoke-Benchmark -Runner $baselineRunner -Scale $dataSet.Scale `
                    -GdbPath $dataSet.Path -IoMode "fallback-sync" -CacheState $cacheState `
                    -Coverage $coverage -Reference "main"))
            }
        }
    }
    Assert-BaselineRegression -Rows $results
}

$csvPath = Join-Path $OutputDir "matrix.csv"
$results | Export-Csv -NoTypeInformation -Path $csvPath
$results | Format-Table reference, scale, io_mode, cache_state, coverage, fast_median_ms, fast_p95_ms, gdal_median_ms, fast_gdal_ratio, invalid_geometries_last, peak_rss_mb -AutoSize
Write-Host "Windows Release acceptance matrix complete: $csvPath"
