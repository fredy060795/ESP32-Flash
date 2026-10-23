# Helm Camera Flasher - lokaler Server fuer Windows (wird von start.bat gestartet)
#
# - liefert die Flasher-Seite aus dem Ordner "web" unter http://localhost:<port>/ aus
# - listet die COM-Ports auf (GET /api/ports)
# - flasht mit esptool.exe und setzt danach den Kameranamen ueber die serielle Schnittstelle
#   (POST /api/flash, POST /api/rename, Fortschritt ueber GET /api/status)
#
# Fehlen Firmware oder esptool.exe, werden sie beim ersten Start heruntergeladen.
# Laeuft mit Windows PowerShell 5.1 (in Windows enthalten) und PowerShell 7.

param([int]$Port = 8765)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # sonst sind Downloads in PowerShell 5.1 sehr langsam
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Web = Join-Path $Root 'web'
$Firmware = Join-Path $Web 'firmware\esp32cam_ble.bin'
$EsptoolDir = Join-Path $Root 'tools\esptool'
$Esptool = Join-Path $EsptoolDir 'esptool.exe'
$LogFile = Join-Path ([IO.Path]::GetTempPath()) 'helm-camera-flasher.log'

$FirmwareUrl = 'https://fredy060795.github.io/ESP32-Flash/firmware/esp32cam_ble.bin'
$EsptoolUrl = 'https://github.com/espressif/esptool/releases/download/v4.8.1/esptool-v4.8.1-win64.zip'

$script:Job = $null

# ----------------------------------------------------------------------
# Vorbereitung
# ----------------------------------------------------------------------

function Get-Downloads {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    # Firmware bei jedem Start aktualisieren; ohne Internet bleibt die vorhandene Datei
    Write-Host 'Neueste Firmware wird geladen ...'
    New-Item -ItemType Directory -Force -Path (Split-Path $Firmware) | Out-Null
    try {
        Invoke-WebRequest -UseBasicParsing -Uri $FirmwareUrl -OutFile "$Firmware.download"
        Move-Item -Force "$Firmware.download" $Firmware
    } catch {
        Remove-Item -Force "$Firmware.download" -ErrorAction SilentlyContinue
        if (-not (Test-Path $Firmware)) { throw }
        Write-Host 'Kein Download moeglich, vorhandene Firmware wird verwendet.'
    }
    if ($IsLinux -or $IsMacOS) { return }  # nur zum Testen ausserhalb von Windows
    if (-not (Test-Path $Esptool)) {
        Write-Host 'esptool wird heruntergeladen ...'
        $zip = Join-Path ([IO.Path]::GetTempPath()) 'esptool-win64.zip'
        $tmp = Join-Path ([IO.Path]::GetTempPath()) 'esptool-win64-extract'
        Invoke-WebRequest -UseBasicParsing -Uri $EsptoolUrl -OutFile $zip
        if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
        Expand-Archive -Path $zip -DestinationPath $tmp
        New-Item -ItemType Directory -Force -Path $EsptoolDir | Out-Null
        Copy-Item -Path (Join-Path $tmp 'esptool-win64\*') -Destination $EsptoolDir -Recurse -Force
        Remove-Item -Recurse -Force $tmp, $zip
    }
}

# ----------------------------------------------------------------------
# COM-Ports
# ----------------------------------------------------------------------

function Get-ComPorts {
    $list = @()
    try {
        $devices = Get-CimInstance -ClassName Win32_PnPEntity -Filter "Name LIKE '%(COM%'" -ErrorAction Stop
        foreach ($d in $devices) {
            if ($d.Name -match '\((COM\d+)\)') {
                $list += [pscustomobject]@{ port = $Matches[1]; name = [string]$d.Name }
            }
        }
    } catch { }
    if ($list.Count -eq 0) {
        foreach ($p in [System.IO.Ports.SerialPort]::GetPortNames()) {
            $list += [pscustomobject]@{ port = $p; name = $p }
        }
    }
    # Nach Nummer sortieren, CH340/CP210x (typisch fuer ESP32-CAM-MB) zuerst
    $list | Sort-Object @{ Expression = { if ($_.name -match 'CH34|CP210|USB.*SERIAL') { 0 } else { 1 } } },
                        @{ Expression = { [int]($_.port -replace '\D', '') } }
}

# ----------------------------------------------------------------------
# Flashen / Umbenennen (laeuft als Hintergrund-Job, Ausgabe in $LogFile)
# ----------------------------------------------------------------------

$Worker = {
    param($Mode, $Esptool, $Firmware, $ComPort, $Name, $Erase, $LogFile)

    function Log($text) { Add-Content -Path $LogFile -Value $text -Encoding UTF8 }

    function Invoke-Esptool([string[]]$EspArgs) {
        Log ('> esptool ' + ($EspArgs -join ' '))
        & $Esptool @EspArgs 2>&1 | ForEach-Object { Log ([string]$_) }
        if ($LASTEXITCODE -ne 0) { throw "Verbindung zur Kamera fehlgeschlagen (esptool Fehlercode $LASTEXITCODE). Port pruefen oder IO0 gedrueckt halten und RST druecken." }
    }

    function Set-CameraName([bool]$ResetFirst) {
        Log "STEP Name wird gesetzt"
        $sp = New-Object System.IO.Ports.SerialPort $ComPort, 115200
        $sp.Encoding = New-Object System.Text.UTF8Encoding $false
        $sp.DtrEnable = $false
        $sp.RtsEnable = $false
        try { $sp.Open() } catch { throw "$ComPort ist belegt oder nicht vorhanden. Anderes Programm (z. B. Arduino) schliessen oder Board neu anstecken." }
        try {
            if ($ResetFirst) {
                $sp.RtsEnable = $true      # EN low = Reset
                Start-Sleep -Milliseconds 100
                $sp.RtsEnable = $false
            }
            $buf = ''
            for ($i = 0; $i -lt 20; $i++) {
                $sp.Write("NAME=$Name`n")
                Start-Sleep -Milliseconds 1000
                $buf += $sp.ReadExisting()
                if ($buf -match 'OK NAME=') { Log $buf; return }
                if ($buf -match 'ERR NAME') { Log $buf; throw 'Name ungueltig' }
            }
            Log $buf
            throw 'Kamera antwortet nicht. RST druecken und "Nur Namen aendern" versuchen.'
        } finally {
            $sp.Close()
        }
    }

    try {
        if ($Mode -eq 'flash') {
            $base = @('--chip', 'esp32', '--port', $ComPort, '--baud', '460800')
            if ($Erase) {
                Log 'STEP Kamera wird geloescht'
                Invoke-Esptool ($base + @('--after', 'no_reset', 'erase_flash'))
            }
            Log 'STEP Flashen'
            Invoke-Esptool ($base + @('write_flash', '0x0', $Firmware))
            Start-Sleep -Milliseconds 1500
        }
        Set-CameraName ($Mode -ne 'flash')
        Log 'RESULT OK'
    } catch {
        Log ('RESULT ERR ' + $_.Exception.Message)
    }
}

function Start-Work($Mode, $Body) {
    if ($script:Job -and $script:Job.State -eq 'Running') { throw 'Es laeuft schon ein Vorgang.' }
    $name = ([string]$Body.name).Trim()
    $bytes = [Text.Encoding]::UTF8.GetByteCount($name)
    if ($bytes -lt 1 -or $bytes -gt 29) { throw 'Name muss 1 bis 29 Zeichen haben.' }
    if (-not ([string]$Body.port -match '^COM\d+$|^/dev/')) { throw 'Bitte einen COM-Port waehlen.' }
    if ($Mode -eq 'flash' -and -not (Test-Path $Firmware)) { throw 'Firmware-Datei fehlt.' }
    if ($Mode -eq 'flash' -and -not (Test-Path $Esptool)) { throw 'esptool.exe fehlt.' }
    Set-Content -Path $LogFile -Value '' -Encoding UTF8
    if ($script:Job) { Remove-Job -Job $script:Job -Force -ErrorAction SilentlyContinue }
    $script:Job = Start-Job -ScriptBlock $Worker -ArgumentList $Mode, $Esptool, $Firmware, ([string]$Body.port), $name, ([bool]$Body.erase), $LogFile
}

function Get-Status {
    $log = ''
    if (Test-Path $LogFile) {
        $fs = [IO.File]::Open($LogFile, 'Open', 'Read', 'ReadWrite')
        try { $log = (New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8)).ReadToEnd() } finally { $fs.Close() }
    }
    $running = [bool]($script:Job -and $script:Job.State -eq 'Running')
    [pscustomobject]@{ running = $running; log = $log }
}

# ----------------------------------------------------------------------
# HTTP
# ----------------------------------------------------------------------

$Types = @{
    '.html' = 'text/html; charset=utf-8'; '.css' = 'text/css; charset=utf-8'
    '.js' = 'text/javascript; charset=utf-8'; '.json' = 'application/json'
    '.bin' = 'application/octet-stream'; '.png' = 'image/png'; '.ico' = 'image/x-icon'
}

function Send($ctx, [int]$code, [byte[]]$bytes, [string]$type) {
    $res = $ctx.Response
    $res.StatusCode = $code
    $res.ContentType = $type
    $res.Headers.Add('Cache-Control', 'no-store')
    $res.ContentLength64 = $bytes.Length
    $res.OutputStream.Write($bytes, 0, $bytes.Length)
    $res.OutputStream.Close()
}

function Send-Json($ctx, [int]$code, $obj) {
    $json = ConvertTo-Json -InputObject $obj -Depth 4 -Compress
    Send $ctx $code ([Text.Encoding]::UTF8.GetBytes($json)) 'application/json; charset=utf-8'
}

function Read-Body($ctx) {
    $reader = New-Object IO.StreamReader($ctx.Request.InputStream, [Text.Encoding]::UTF8)
    $text = $reader.ReadToEnd()
    if (-not $text) { return [pscustomobject]@{} }
    ConvertFrom-Json $text
}

function Handle($ctx) {
    $path = $ctx.Request.Url.AbsolutePath
    $method = $ctx.Request.HttpMethod
    try {
        if ($path -eq '/api/ports') { Send-Json $ctx 200 @(Get-ComPorts); return }
        if ($path -eq '/api/status') { Send-Json $ctx 200 (Get-Status); return }
        if ($path -eq '/api/flash' -and $method -eq 'POST') { Start-Work 'flash' (Read-Body $ctx); Send-Json $ctx 200 @{ ok = $true }; return }
        if ($path -eq '/api/rename' -and $method -eq 'POST') { Start-Work 'rename' (Read-Body $ctx); Send-Json $ctx 200 @{ ok = $true }; return }
    } catch {
        Send-Json $ctx 400 @{ ok = $false; error = $_.Exception.Message }
        return
    }

    if ($path -eq '/') { $path = '/index.html' }
    $file = [IO.Path]::GetFullPath((Join-Path $Web ([Uri]::UnescapeDataString($path).TrimStart('/'))))
    if ($file.StartsWith([IO.Path]::GetFullPath($Web)) -and (Test-Path $file -PathType Leaf)) {
        $type = $Types[[IO.Path]::GetExtension($file).ToLower()]
        if (-not $type) { $type = 'application/octet-stream' }
        Send $ctx 200 ([IO.File]::ReadAllBytes($file)) $type
    } else {
        Send $ctx 404 ([Text.Encoding]::UTF8.GetBytes('Nicht gefunden')) 'text/plain; charset=utf-8'
    }
}

# ----------------------------------------------------------------------

Write-Host ''
Write-Host '  Helm Camera Flasher'
Write-Host '  ==================='
Write-Host ''
try {
    Get-Downloads
} catch {
    Write-Host ('Download fehlgeschlagen: ' + $_.Exception.Message) -ForegroundColor Red
    Write-Host 'Internetverbindung pruefen. Der Flasher startet trotzdem.'
}

$listener = $null
for ($p = $Port; $p -lt $Port + 20; $p++) {
    try {
        $listener = New-Object System.Net.HttpListener
        $listener.Prefixes.Add("http://localhost:$p/")
        $listener.Start()
        $Port = $p
        break
    } catch {
        $listener = $null
    }
}
if (-not $listener) { throw 'Kein freier Port fuer den lokalen Server gefunden.' }

$url = "http://localhost:$Port/"
Write-Host "  Flasher laeuft unter $url"
Write-Host '  Dieses Fenster offen lassen. Zum Beenden schliessen.'
Write-Host ''
if (-not $env:HELM_NO_BROWSER) { Start-Process $url }

while ($listener.IsListening) {
    $ctx = $listener.GetContext()
    try { Handle $ctx } catch { Write-Host $_.Exception.Message -ForegroundColor Red }
}
