# ESP32-Flash

Firmware und Web-Flasher für die **ESP32-CAM** (AI-Thinker / ARCELI mit OV2640 und USB-C-Board ESP32-CAM-MB).

- **Flashen ohne Software:** Webseite in Chrome/Edge öffnen, Board anstecken, „Installieren“ klicken.
- **Eigener WLAN-Hotspot:** Die Kamera macht ihr eigenes WLAN auf. Jeder kann sich mit dem Handy verbinden, ohne Router und ohne Internet.
- **Bluetooth:** Mit dem Handy per Bluetooth verbinden, Live-Bild (niedrige Bildrate) und Fotos über Bluetooth, Hotspot-Passwort anzeigen, Heim-WLAN, Licht und Auflösung einstellen.

## Web-Flasher

**https://fredy060795.github.io/ESP32-Flash/**

1. Kamera auf das USB-C-Board stecken, mit dem PC verbinden.
2. Seite in **Chrome** oder **Edge** öffnen → **Installieren** → Port wählen (meist „USB-SERIAL CH340“).
3. Nach dem Flashen **RST** an der Kamera drücken.

Probleme beim Verbinden:
- Windows braucht den **CH340-Treiber**.
- Ein Datenkabel verwenden, kein reines Ladekabel.
- **IO0** am USB-Board gedrückt halten, kurz **RST** drücken, IO0 loslassen, nochmal „Installieren“.

## Kamera benutzen

| Weg | Was geht | Wie |
|---|---|---|
| Bluetooth | Einstellungen, Hotspot-Passwort anzeigen, Fotos, Live-Bild mit ca. 1–3 Bildern/s bei 320x240 | Kamera-App `app.html` auf der Flasher-Seite → „Verbinden“ |
| WLAN-Hotspot | Flüssiges Live-Video bis 1600x1200, Fotos | WLAN `ESP32-CAM-XXXX`, Passwort `kamera123`, dann http://192.168.4.1 |
| Heim-WLAN (optional) | Wie Hotspot, aber im eigenen Netz | In der App unter „Mit Heim-WLAN verbinden“ eintragen |

Bluetooth im Browser: Android mit Chrome, PC/Mac mit Chrome oder Edge. Auf dem iPhone die App **Bluefy** verwenden (Safari kann kein Bluetooth).

Video läuft über WLAN, weil Bluetooth dafür zu langsam ist. Bluetooth ist für Einrichtung, Einzelbilder und eine langsame Vorschau gedacht.

Hinweis: Jeder in Bluetooth-Reichweite (ca. 10–30 m) kann sich mit der Kamera verbinden und das Hotspot-Passwort sehen. Das ist so gewollt, damit jeder, dem du die Kamera gibst, sie ohne Einrichtung nutzen kann.

Die rote LED auf der Rückseite blitzt alle 2 Sekunden kurz, solange kein Handy per Bluetooth verbunden ist.

## Webseiten der Kamera (im Hotspot oder Heim-WLAN)

| Adresse | Inhalt |
|---|---|
| `http://<ip>/` | Live-Bild mit Bedienung |
| `http://<ip>:81/stream` | MJPEG-Stream (z. B. für VLC) |
| `http://<ip>/capture` | Einzelbild als JPEG |
| `http://<ip>/status` | Status als JSON |
| `http://<ip>/control?var=framesize\|quality\|flash&val=<zahl>` | Einstellungen |

## Bluetooth-Schnittstelle

Service `7b3c0001-6b1a-4d6e-9c55-3a2f1e0c8a01`

| Characteristic | UUID | Funktion |
|---|---|---|
| Status | `7b3c0002-…` | Lesen/Notify, JSON mit Name, Hotspot, WLAN, Kamera-Einstellungen |
| Befehl | `7b3c0003-…` | Schreiben, JSON-Befehl |
| Bild | `7b3c0004-…` | Notify, JPEG in Paketen |

Befehle:

```json
{"cmd":"snap"}
{"cmd":"wifi","ssid":"MeinWLAN","pass":"geheim"}
{"cmd":"wifi","ssid":"","pass":""}
{"cmd":"ap","ssid":"Kamera-Garten","pass":"mindestens8"}
{"cmd":"cam","framesize":5,"quality":12}
{"cmd":"flash","value":128}
{"cmd":"name","name":"Kamera-1"}
{"cmd":"reboot"}
{"cmd":"reset"}
```

Bildpakete (erstes Byte = Typ, Zahlen little endian):
`0x01` Start (u32 Länge, u16 Breite, u16 Höhe) · `0x02` Daten (u32 Offset + JPEG-Bytes) · `0x03` Ende · `0x04` Fehler (Text).

## Selbst bauen

Die GitHub Action (`.github/workflows/build.yml`) baut bei jedem Push die Firmware. Auf `main` veröffentlicht sie zusätzlich den Web-Flasher auf GitHub Pages. Dafür einmalig unter **Settings → Pages → Source** „GitHub Actions“ auswählen.

Lokal mit Arduino IDE oder arduino-cli:

- Board-Paket **esp32 2.0.17** (Espressif), Board **AI Thinker ESP32-CAM**, Partition Scheme **Huge APP**
- Bibliotheken **NimBLE-Arduino 1.4.3** und **ArduinoJson 7.2.1**
- Sketch: `firmware/esp32cam_ble/esp32cam_ble.ino`

```sh
arduino-cli compile --fqbn esp32:esp32:esp32cam:PartitionScheme=huge_app firmware/esp32cam_ble
```
