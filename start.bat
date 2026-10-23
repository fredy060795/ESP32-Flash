@echo off
rem Helm Camera Flasher: startet den lokalen Flasher und oeffnet ihn im Browser.
rem Fenster offen lassen, solange geflasht wird.
title Helm Camera Flasher
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\server.ps1"
if errorlevel 1 pause
