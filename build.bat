@echo off
setlocal
cd /d "%~dp0"

set MSBUILD="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"

REM Prefer building the solution to ensure correct dependencies
%MSBUILD% "NFSAddon.sln" /p:Configuration=Debug /m /t:Rebuild /v:minimal
if errorlevel 1 goto :eof

endlocal
