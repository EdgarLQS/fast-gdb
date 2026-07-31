<#
.SYNOPSIS
    fast-gdb 测试数据一键生成脚本
.DESCRIPTION
    自动检测 ArcGIS Pro arcpy 环境，依次生成 acceptance_metadata.gdb 和 testcurve.gdb，
    验证输出，并清理开发期遗留的 fix_*.py 补丁脚本。
.PARAMETER PythonPath
    手动指定 ArcGIS Pro 的 python.exe 路径。不指定时自动从注册表检测。
.PARAMETER SkipCleanup
    指定此开关跳过 fix_*.py 清理步骤。
.PARAMETER SkipVerify
    指定此开关跳过生成后的验证步骤。
.EXAMPLE
    # 自动检测 arcpy 并生成
    .\generate_test_data.ps1

    # 手动指定 Python 路径
    .\generate_test_data.ps1 -PythonPath "D:\software\arcgis\install\arcpro352\bin\Python\envs\arcgispro-py3\python.exe"

    # 跳过清理和验证
    .\generate_test_data.ps1 -SkipCleanup -SkipVerify
#>

param(
    [string]$PythonPath = "",
    [switch]$SkipCleanup,
    [switch]$SkipVerify
)

# ── 项目路径 ──
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Resolve-Path "$ScriptDir\..\.."
$TestDataDir = "$ProjectRoot\test_data"
$GdbDir = "$TestDataDir\gdb"

Write-Host ("=" * 60)
Write-Host "  fast-gdb 测试数据生成器"
Write-Host "  项目根目录: $ProjectRoot"
Write-Host ("=" * 60)
Write-Host ""

# ── 步骤 1: 检测 arcpy Python 路径 ──
Write-Host "【步骤 1】检测 ArcGIS Pro Python 环境" -ForegroundColor Cyan

if (-not $PythonPath) {
    # 从注册表自动检测
    $regPath = "HKLM:\SOFTWARE\ESRI\ArcGISPro"
    if (Test-Path $regPath) {
        $props = Get-ItemProperty $regPath
        $condaRoot = $props.PythonCondaRoot
        $condaEnv = $props.PythonCondaEnv
        if ($condaRoot -and $condaEnv) {
            $PythonPath = "$condaRoot\envs\$condaEnv\python.exe"
        }
    }
}

if (-not $PythonPath -or -not (Test-Path $PythonPath)) {
    Write-Host "  [FAIL] 未找到 ArcGIS Pro Python，请通过 -PythonPath 参数指定" -ForegroundColor Red
    Write-Host "  示例: .\generate_test_data.ps1 -PythonPath ""D:\path\to\python.exe""" -ForegroundColor Yellow
    exit 1
}

Write-Host "  [OK] Python 路径: $PythonPath" -ForegroundColor Green
Write-Host ""

# ── 步骤 2: 生成 acceptance_metadata.gdb ──
Write-Host "【步骤 2】生成 acceptance_metadata.gdb" -ForegroundColor Cyan
$Script1 = "$ScriptDir\..\python\generate_acceptance_metadata.py"
$Output1 = "$GdbDir\acceptance_metadata.gdb"

if (-not (Test-Path $Script1)) {
    Write-Host "  [FAIL] 找不到脚本: $Script1" -ForegroundColor Red
    exit 1
}

# 删除旧数据确保干净重建
if (Test-Path $Output1) {
    Remove-Item -Recurse -Force $Output1 -ErrorAction SilentlyContinue
}

& $PythonPath -X utf8 $Script1
if ($LASTEXITCODE -ne 0) {
    Write-Host "  [FAIL] acceptance_metadata.gdb 生成失败" -ForegroundColor Red
    exit 1
}
Write-Host "  [OK] acceptance_metadata.gdb 生成完成" -ForegroundColor Green
Write-Host ""

# ── 步骤 3: 生成 testcurve.gdb ──
Write-Host "【步骤 3】生成 testcurve.gdb" -ForegroundColor Cyan
$Script2 = "$ScriptDir\..\python\generate_all_data.py"
$Output2 = "$GdbDir\testcurve.gdb"

if (-not (Test-Path $Script2)) {
    Write-Host "  [FAIL] 找不到脚本: $Script2" -ForegroundColor Red
    exit 1
}

# 删除旧数据确保干净重建
if (Test-Path $Output2) {
    Remove-Item -Recurse -Force $Output2 -ErrorAction SilentlyContinue
}

$env:FAST_GDB_ARCPY_OUTPUT = $Output2
& $PythonPath -X utf8 $Script2
if ($LASTEXITCODE -ne 0) {
    Write-Host "  [FAIL] testcurve.gdb 生成失败" -ForegroundColor Red
    exit 1
}
Write-Host "  [OK] testcurve.gdb 生成完成" -ForegroundColor Green
Write-Host ""

# ── 步骤 4: 验证输出 ──
if (-not $SkipVerify) {
    Write-Host "【步骤 4】验证输出" -ForegroundColor Cyan
    $allOk = $true

    # 验证 acceptance_metadata.gdb
    if (Test-Path $Output1) {
        $size1 = (Get-ChildItem -Recurse $Output1 | Measure-Object -Property Length -Sum).Sum
        Write-Host "  [OK] acceptance_metadata.gdb ($([math]::Round($size1/1KB, 1)) KB)" -ForegroundColor Green
    } else {
        Write-Host "  [FAIL] acceptance_metadata.gdb 不存在" -ForegroundColor Red
        $allOk = $false
    }

    # 验证 acceptance_metadata 参考文件
    $RefDir = "$GdbDir\acceptance_metadata"
    $refFiles = @("manifest.json", "layer-inventory.csv", "field-inventory.csv",
                  "domain-expected.csv", "relationship-expected.csv",
                  "dataset-hierarchy.csv", "fid-objectid-mapping.csv",
                  "field-values-expected.csv", "metadata-expected.json",
                  "source-notes.md")
    $refOk = $true
    foreach ($f in $refFiles) {
        $refPath = "$RefDir\$f"
        if (Test-Path $refPath) {
            Write-Host "  [OK] 参考文件: $f" -ForegroundColor Green
        } else {
            Write-Host "  [WARN] 参考文件缺失: $f" -ForegroundColor Yellow
            $refOk = $false
        }
    }

    # 验证 testcurve.gdb
    if (Test-Path $Output2) {
        $size2 = (Get-ChildItem -Recurse $Output2 | Measure-Object -Property Length -Sum).Sum
        Write-Host "  [OK] testcurve.gdb ($([math]::Round($size2/1MB, 1)) MB)" -ForegroundColor Green
    } else {
        Write-Host "  [FAIL] testcurve.gdb 不存在" -ForegroundColor Red
        $allOk = $false
    }

    if (-not $allOk) {
        Write-Host "  [WARN] 部分验证未通过，请检查输出" -ForegroundColor Yellow
    } else {
        Write-Host "  [OK] 全部验证通过" -ForegroundColor Green
    }
    Write-Host ""
}

# ── 步骤 5: 清理 fix_*.py ──
if (-not $SkipCleanup) {
    Write-Host "【步骤 5】清理开发期 fix_*.py 补丁脚本" -ForegroundColor Cyan
    $fixFiles = Get-ChildItem "$ScriptDir\fix_*.py" -ErrorAction SilentlyContinue
    if ($fixFiles.Count -eq 0) {
        Write-Host "  无需清理，已无 fix_*.py 文件" -ForegroundColor Green
    } else {
        foreach ($f in $fixFiles) {
            Remove-Item -Path $f.FullName -Force
            Write-Host "  [DEL] $($f.Name)" -ForegroundColor DarkGray
        }
        Write-Host "  [OK] 已清理 $($fixFiles.Count) 个 fix 脚本" -ForegroundColor Green
    }
    Write-Host ""
}

# ── 汇总 ──
Write-Host ("=" * 60)
Write-Host "  生成完毕" -ForegroundColor Green
Write-Host "  acceptance_metadata.gdb -> $Output1"
Write-Host "  testcurve.gdb          -> $Output2"
Write-Host ""
Write-Host "  注意: 以下数据集需手动处理:" -ForegroundColor Yellow
Write-Host "  - test_data/gdb/参数化数据_liqs.gdb  (外部提供，无生成脚本)"
Write-Host "  - test_data/gdb/test_spatial_gdb.gdb (外部提供，无生成脚本)"
Write-Host "  - test_data/benchmark/wide_50_gdal.gdb (被测试引用但不存在)"
Write-Host ("=" * 60)
