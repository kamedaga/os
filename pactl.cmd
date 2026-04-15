@echo off
setlocal

set "ROOT=%~dp0"
set "CARGO_TARGET_DIR=%ROOT%.artifacts\pactl-target-v2"
set "MANIFEST_PATH=%ROOT%tools\pactl\Cargo.toml"
set "PACTL_EXE=%CARGO_TARGET_DIR%\debug\pactl.exe"
set "BUILD_NEEDED=0"

for /f %%I in ('
    powershell -NoProfile -Command ^
      "$exe = '%PACTL_EXE%';" ^
      "if (-not (Test-Path $exe)) { '1'; exit };" ^
      "$exe_time = (Get-Item $exe).LastWriteTimeUtc;" ^
      "$manifest_time = (Get-Item '%MANIFEST_PATH%').LastWriteTimeUtc;" ^
      "$needs = $manifest_time -gt $exe_time;" ^
      "if (-not $needs) {" ^
      "  $src_root = '%ROOT%tools\\pactl\\src';" ^
      "  foreach ($file in Get-ChildItem -File -Recurse $src_root) {" ^
      "    if ($file.LastWriteTimeUtc -gt $exe_time) { $needs = $true; break }" ^
      "  }" ^
      "}" ^
      "if ($needs) { '1' } else { '0' }"
') do set "BUILD_NEEDED=%%I"

if "%BUILD_NEEDED%"=="1" (
    cargo build --quiet --manifest-path "%MANIFEST_PATH%"
    if errorlevel 1 exit /b %ERRORLEVEL%
)

"%PACTL_EXE%" %*
exit /b %ERRORLEVEL%
