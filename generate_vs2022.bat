@echo off
rem Makes D110Emu.sln and build\vs2022\*.vcxproj from premake5.lua (run it before the first build, and after updating the sources).
cd /d "%~dp0"
tools\premake5.exe vs2022
pause
