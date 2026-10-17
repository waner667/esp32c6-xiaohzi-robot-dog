$ErrorActionPreference = 'Continue'
$TOOLS = '<app>\ESP\Espressif\tools'
$IDF   = '<IDF_PATH>'
$PYENV = '<app>\ESP\Espressif\python_env\idf6.1_py3.13_env'
$PROJ  = '<repo>\me\xiaozhi-esp32-2.5.2'
$BUILD = "$PROJ\build"

$env:IDF_PATH = $IDF
$env:IDF_TOOLS_PATH = $TOOLS
$env:IDF_PYTHON_ENV_PATH = $PYENV

$extra = @()
foreach ($p in @(
  "$TOOLS\ninja\1.12.1",
  "$TOOLS\cmake\3.30.2",
  "$TOOLS\ccache\4.12.1\ccache-4.12.1-windows-x86_64",
  "$TOOLS\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin",
  "$IDF\tools"
)) { if (Test-Path -LiteralPath $p) { $extra += $p } }
$env:PATH = ($extra -join ';') + ';' + $PYENV + '\Scripts;' + $env:PATH

Set-Location $BUILD

# 阶段 1：先单文件编译 compact_c6_board.cc.obj，快速暴露语法/链接错误
Write-Output "=== STAGE1 single-obj $(Get-Date -Format 'HH:mm:ss') ==="
& "$TOOLS\ninja\1.12.1\ninja.exe" -j 1 "esp-idf/main/CMakeFiles/__idf_main.dir/boards/bread-compact-c6/compact_c6_board.cc.obj"
$rc1 = $LASTEXITCODE
Write-Output "=== STAGE1_EXIT=$rc1 ==="

if ($rc1 -ne 0) {
    Write-Output "STAGE1 FAILED, abort."
    exit $rc1
}

# 阶段 2：全量编译
Write-Output "=== STAGE2 full build $(Get-Date -Format 'HH:mm:ss') ==="
& "$TOOLS\ninja\1.12.1\ninja.exe" -j 8
$rc2 = $LASTEXITCODE
Write-Output "=== BUILD_EXIT=$rc2 ==="
if (Test-Path -LiteralPath "$BUILD\xiaozhi.bin") {
  Get-Item "$BUILD\xiaozhi.bin" | ForEach-Object {
    Write-Output ("BIN=" + $_.FullName + " SIZE=" + $_.Length + " MTIME=" + $_.LastWriteTime)
  }
}
exit $rc2
