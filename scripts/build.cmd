@echo off
rem Build BlockStorage (bypass PowerShell execution policy)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
