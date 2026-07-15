[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CurrentBuildDir,
    [Parameter(Mandatory = $true)]
    [string]$BaselineBuildDir,
    [string]$DataRoot = "test_data/spatial_matrix/windows",
    [string]$OutputDir = "artifacts/windows-spatial-main-ab",
    [Parameter(Mandatory = $true)]
    [string]$CacheClearCommand
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Resolve-Runner {
    param([string]$Root)
    $candidates = @(
        (Join-Path $Root "bin/gdb_tutorial_test_runner.exe"),
        (Join-Path $Root "bin/Release/gdb_tutorial_test_runner.exe"),
        (Join-Path $Root "Release/gdb_tutorial_test_runner.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { return (Resolve-Path $candidate).Path }
    }
    throw "Cannot find gdb_tutorial_test_runner under $Root"
}

function Parse-Summary {
    param(
        [string]$Output,
        [string]$Reference,
        [string]$Scale,
        [string]$CacheState,
        [string]$Coverage,
        [string]$LogPath
    )

    $summaryMatches = [regex]::Matches(
        $Output,
        '(?m)^(coverage_[^ ]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)$')
    if ($summaryMatches.Count -ne 1) {
        throw "Expected one summary row for $Reference/$Scale/$CacheState/$Coverage, found $($summaryMatches.Count)"
    }
    $summary = $summaryMatches[0]
    if ($summary.Groups[1].Value -ne $Coverage) {
        throw "Expected $Coverage, parsed $($summary.Groups[1].Value)"
    }

    $funnelMatches = [regex]::Matches(
        $Output,
        '(?m)^\s+funnel:.*invalid=([0-9]+) result=([0-9]+)')
    if ($funnelMatches.Count -ne 1) {
        throw "Expected one funnel row for $Reference/$Scale/$CacheState/$Coverage"
    }
    $invalid = [long]$funnelMatches[0].Groups[1].Value
    if ($invalid -ne 0) {
        throw "Invalid geometries observed for $Reference/$Scale/$CacheState/$Coverage"
    }

    return [pscustomobject]@{
        reference = $Reference
        scale = $Scale
        cache_state = $CacheState
        coverage = $Coverage
        trials = 20
        fast_median_ms = [double]$summary.Groups[2].Value
        gdal_median_ms = [double]$summary.Groups[3].Value
        fast_p95_ms = [double]$summary.Groups[4].Value
        gdal_p95_ms = [double]$summary.Groups[5].Value
        fast_gdal_ratio = [double]$summary.Groups[6].Value
        invalid_geometries = $invalid
        result_count = [long]$funnelMatches[0].Groups[2].Value
        log = $LogPath
    }
}

function Invoke-AbCase {
    param(
        [string]$Runner,
        [string]$Reference,
        [string]$Scale,
        [string]$GdbPath,
        [string]$CacheState,
        [string]$Coverage,
        [string]$ResolvedCacheCommand
    )

    $env:FAST_GDB_RUN_SPATIAL_BENCHMARKS = "1"
    $env:FAST_GDB_BENCHMARK_PATH = (Resolve-Path $GdbPath).Path
    $env:FAST_GDB_BENCHMARK_LABEL = "$Reference / $Scale / fallback-sync / $CacheState / $Coverage"
    $env:FAST_GDB_BENCHMARK_CASE = $Coverage
    $env:FAST_GDB_BENCHMARK_TRIALS = "20"
    $env:FAST_GDB_WINDOWS_MMAP = "0"
    $env:FAST_GDB_WINDOWS_ASYNC_IO = "0"
    $env:FAST_GDB_SPATIAL_PROFILE = "1"
    Remove-Item Env:FAST_GDB_WINDOWS_IO_TRACE -ErrorAction SilentlyContinue

    if ($CacheState -eq "cold") {
        $env:FAST_GDB_BENCHMARK_MODE = "fresh-open"
        $env:FAST_GDB_BENCHMARK_STRICT_COLD = "1"
        $env:FAST_GDB_BENCHMARK_CACHE_CLEAR_COMMAND = "`"$ResolvedCacheCommand`""
        $env:FAST_GDB_TABLX_CACHE = "0"
    } else {
        Remove-Item Env:FAST_GDB_BENCHMARK_MODE -ErrorAction SilentlyContinue
        Remove-Item Env:FAST_GDB_BENCHMARK_STRICT_COLD -ErrorAction SilentlyContinue
        Remove-Item Env:FAST_GDB_BENCHMARK_CACHE_CLEAR_COMMAND -ErrorAction SilentlyContinue
        Remove-Item Env:FAST_GDB_TABLX_CACHE -ErrorAction SilentlyContinue
    }

    $baseName = "$Reference-$Scale-$CacheState-$Coverage"
    $stdoutPath = Join-Path $OutputDir "$baseName.stdout.log"
    $stderrPath = Join-Path $OutputDir "$baseName.stderr.log"
    $logPath = Join-Path $OutputDir "$baseName.log"
    $process = Start-Process -FilePath $Runner `
        -ArgumentList "--gtest_filter=SpatialDensityBenchmark.DensityMatrixConfigured" `
        -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath

    $stdout = if (Test-Path $stdoutPath) { Get-Content $stdoutPath -Raw } else { "" }
    $stderr = if (Test-Path $stderrPath) { Get-Content $stderrPath -Raw } else { "" }
    $combined = $stdout + [Environment]::NewLine + $stderr
    $combined | Set-Content $logPath
    Write-Host $combined
    if ($process.ExitCode -ne 0) {
        throw "A/B benchmark failed: $baseName (exit $($process.ExitCode))"
    }
    return Parse-Summary -Output $stdout -Reference $Reference `
        -Scale $Scale -CacheState $CacheState -Coverage $Coverage `
        -LogPath $logPath
}

if (!(Test-Path $CacheClearCommand)) {
    throw "Cache-clear command does not exist: $CacheClearCommand"
}
$cacheCommand = (Resolve-Path $CacheClearCommand).Path
$currentRunner = Resolve-Runner $CurrentBuildDir
$baselineRunner = Resolve-Runner $BaselineBuildDir
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$dataSets = @(
    @{ Scale = "1m"; Path = (Join-Path $DataRoot "point_1m/point_1m.gdb") },
    @{ Scale = "10m"; Path = (Join-Path $DataRoot "point_10m/point_10m.gdb") }
)
$coverages = @("coverage_01pct", "coverage_10pct", "coverage_30pct", "coverage_80pct", "coverage_full")
$cacheStates = @("cold", "warm")
$rows = New-Object System.Collections.Generic.List[object]
$caseIndex = 0

foreach ($dataSet in $dataSets) {
    if (!(Test-Path $dataSet.Path)) { throw "Benchmark data not found: $($dataSet.Path)" }
    foreach ($cacheState in $cacheStates) {
        foreach ($coverage in $coverages) {
            $order = if (($caseIndex % 2) -eq 0) {
                @(
                    @{ Name = "current"; Runner = $currentRunner },
                    @{ Name = "main"; Runner = $baselineRunner }
                )
            } else {
                @(
                    @{ Name = "main"; Runner = $baselineRunner },
                    @{ Name = "current"; Runner = $currentRunner }
                )
            }
            foreach ($entry in $order) {
                $rows.Add((Invoke-AbCase -Runner $entry.Runner `
                    -Reference $entry.Name -Scale $dataSet.Scale `
                    -GdbPath $dataSet.Path -CacheState $cacheState `
                    -Coverage $coverage -ResolvedCacheCommand $cacheCommand))
            }
            ++$caseIndex
        }
    }
}

foreach ($current in @($rows | Where-Object { $_.reference -eq "current" })) {
    $baseline = $rows | Where-Object {
        $_.reference -eq "main" -and
        $_.scale -eq $current.scale -and
        $_.cache_state -eq $current.cache_state -and
        $_.coverage -eq $current.coverage
    } | Select-Object -First 1
    if ($null -eq $baseline) {
        throw "Missing baseline row for $($current.scale)/$($current.cache_state)/$($current.coverage)"
    }
    if ($current.fast_median_ms -gt $baseline.fast_median_ms) {
        throw "Windows fallback regressed for $($current.scale)/$($current.cache_state)/$($current.coverage): current=$($current.fast_median_ms)ms main=$($baseline.fast_median_ms)ms"
    }
}

$csvPath = Join-Path $OutputDir "windows-fallback-main-ab.csv"
$rows | Export-Csv -NoTypeInformation -Path $csvPath
Write-Host "Windows fallback current-versus-main A/B passed: $csvPath"
