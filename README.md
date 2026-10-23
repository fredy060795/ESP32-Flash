# ESP32-Flash – Helm Camera

Firmware und Web-Flasher für die **ESP32-CAM** (AI-Thinker / ARCELI mit OV2640 und USB-C-Board ESP32-CAM-MB)
als Bluetooth-Helmkamera.

- **Nur Bluetooth (BLE):** kein WLAN, kein Passwort, kein Pairing. BLE ist ab dem Einschalten aktiv.
- **Handy bleibt im Internet:** Die Handy-App holt die Bilder per Bluetooth und kann sie direkt weiterschicken (TOC/HQ, ATAK).
- **Name beim Flashen wählbar:** Standard `Helm Camera`, im Flasher frei änderbar (z. B. `Helm 1`, `Helm Alpha`).
- **Flashen ohne Software:** Webseite in Chrome/Edge öffnen, Board anstecken, „Flashen“ klicken.

## Flashen unter Windows mit start.bat (empfohlen)

1. Repository herunterladen (**Code → Download ZIP**) und entpacken – oder das fertige Paket
   **Helm-Camera-Flasher-Windows** unter **Actions** → letzter Lauf → Artifacts laden (funktioniert auch offline).
2. Kamera auf das USB-C-Board stecken und mit dem PC verbinden.
3. **start.bat** doppelklicken. Der Flasher öffnet sich im Browser (jeder Browser geht).
4. **COM-Port** der Kamera wählen (meist „USB-SERIAL CH340 (COMx)“), Namen eintragen → **Flashen**.
5. Warten bis „Fertig!“ erscheint, Board abziehen. Das schwarze Fenster danach schließen.

`start.bat` startet einen kleinen lokalen Server (Windows PowerShell, keine Installation nötig) und lädt beim
Start die neueste Firmware und – nur beim ersten Mal – esptool herunter.

Welche Firmware geflasht wird, steht auf der Seite unter **Firmware**:
1. eine selbst ausgewählte `.bin`-Datei (z. B. aus dem Actions-Download), sonst
2. die neueste Version von GitHub Pages, sonst
3. die mitgelieferte Datei `firmware/bin/esp32cam_ble.bin` aus dem Ordner.

Es muss die komplette Datei `esp32cam_ble.bin` sein (Image ab Adresse 0x0), nicht der App-Teil allein.

## Web-Flasher ohne Download

**https://fredy060795.github.io/ESP32-Flash/**

1. Seite in **Chrome** oder **Edge** öffnen, Kamera anstecken.
2. Bei **COM-Port** „COM-Port auswählen …“ wählen und im Browser-Fenster den Port der Kamera bestätigen.
3. Namen eintragen → **Flashen**.

Mit **Nur Namen ändern** kann man eine schon geflashte Kamera umbenennen, ohne neu zu flashen.

Probleme beim Verbinden:
- Windows braucht den **CH340-Treiber**.
- Ein Datenkabel verwenden, kein reines Ladekabel.
- **IO0** am USB-Board gedrückt halten, kurz **RST** drücken, IO0 loslassen, nochmal „Flashen“.

## Benutzen

Die Handy-App verbindet sich per Bluetooth mit der Kamera (Name wie beim Flashen vergeben) und
empfängt JPEG-Bilder, einzeln oder fortlaufend. Das Protokoll steht in [PROTOCOL.md](PROTOCOL.md).

Zum Ausprobieren gibt es auf der Flasher-Seite **Kamera testen** (`app.html`): verbinden, Live-Bild, Foto,
Auflösung, Licht, Umbenennen. Bluetooth im Browser: Android mit Chrome, PC/Mac mit Chrome oder Edge,
iPhone mit der App **Bluefy** (Safari kann kein Bluetooth).

Bluetooth schafft keine flüssige Videoübertragung: Bei 320x240 sind je nach Handy etwa 2–5 Bilder/s möglich.

Die rote LED auf der Rückseite blitzt alle 2 Sekunden kurz, solange kein Handy verbunden ist.

Hinweis: Ohne Pairing kann sich jeder in Bluetooth-Reichweite (ca. 10–30 m) mit der Kamera verbinden,
solange sie nicht schon mit einem Handy verbunden ist (eine Verbindung gleichzeitig).

## Selbst bauen

Die GitHub Action (`.github/workflows/build.yml`) baut bei jedem Push die Firmware (Download unter
Actions → Run → `esp32cam_ble-firmware`). Auf `main` veröffentlicht sie zusätzlich den Web-Flasher auf
GitHub Pages. Dafür einmalig unter **Settings → Pages → Source** „GitHub Actions“ auswählen.

Lokal mit Arduino IDE oder arduino-cli:

- Board-Paket **esp32 2.0.17** (Espressif), Board **AI Thinker ESP32-CAM**, Partition Scheme **Huge APP**
- Bibliotheken **NimBLE-Arduino 1.4.3** und **ArduinoJson 7.2.1**
- Sketch: `firmware/esp32cam_ble/esp32cam_ble.ino`

```sh
arduino-cli compile --fqbn esp32:esp32:esp32cam:PartitionScheme=huge_app firmware/esp32cam_ble
```
