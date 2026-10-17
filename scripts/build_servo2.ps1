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

# 注意：这里**不再**批量改名 display 下的 obj。
# 2026-10-16 踩过：那份清理逻辑会让 main 组件触发大范围重编（2000+ 目标），
# 表现为日志长时间停在 [3/10] 不动，看起来像卡死，实际只是很慢。
# obj 锁的问题只在**并发两个编译任务**时出现，单任务跑不需要预先清理。

Set-Location $BUILD
Write-Output "=== BUILD START $(Get-Date -Format 'HH:mm:ss') ==="
& "$TOOLS\ninja\1.12.1\ninja.exe" -j 8
$rc = $LASTEXITCODE
Write-Output "=== BUILD_EXIT=$rc ==="
if (Test-Path -LiteralPath "$BUILD\xiaozhi.bin") {
  Get-Item "$BUILD\xiaozhi.bin" | ForEach-Object {
    Write-Output ("BIN=" + $_.FullName + " SIZE=" + $_.Length + " MTIME=" + $_.LastWriteTime)
  }
}
exit $rc