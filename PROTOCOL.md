# Helm Camera – Bluetooth-Protokoll

Für die Handy-App (z. B. [Wintak_video_streams_ESP32](https://github.com/fredy060795/Wintak_video_streams_ESP32)).
Die Kamera spricht nur Bluetooth Low Energy (BLE). Es gibt kein WLAN, kein Pairing und kein Passwort.
Das Handy bleibt im normalen Internet und kann die Bilder direkt weiterleiten (z. B. ins TOC/HQ).

## Finden

- Werbung (Advertising) mit der Service-UUID `7b3c0001-6b1a-4d6e-9c55-3a2f1e0c8a01`
- Gerätename = beim Flashen eingetragener Name (Standard `Helm Camera`, max. 29 Byte UTF-8)
- Web Bluetooth: `navigator.bluetooth.requestDevice({ filters: [{ services: ['7b3c0001-6b1a-4d6e-9c55-3a2f1e0c8a01'] }] })`
- Eine Verbindung gleichzeitig. Nach dem Trennen wirbt die Kamera sofort wieder.

## GATT

| Characteristic | UUID | Eigenschaften |
|---|---|---|
| Status | `7b3c0002-6b1a-4d6e-9c55-3a2f1e0c8a01` | Read, Notify |
| Befehl | `7b3c0003-6b1a-4d6e-9c55-3a2f1e0c8a01` | Write, Write Without Response |
| Bild | `7b3c0004-6b1a-4d6e-9c55-3a2f1e0c8a01` | Notify |

### Status

JSON (UTF-8). Kommt per Notify bei jeder Änderung und alle 5 Sekunden. Bei kleiner MTU ist die
Notification abgeschnitten, deshalb nach jeder Notification den Wert neu **lesen**.

```json
{"name":"Helm Camera","fw":"2.0.0","camera":true,"framesize":5,"quality":12,
 "interval":0,"streaming":false,"flash":0,"uptime":42}
```

### Befehle

JSON (UTF-8, max. 255 Byte) auf die Befehl-Characteristic schreiben:

| Befehl | Wirkung |
|---|---|
| `{"cmd":"snap"}` | Ein Bild senden |
| `{"cmd":"stream","on":true}` | Fortlaufend Bilder senden, so schnell es geht |
| `{"cmd":"stream","on":true,"interval":1000}` | Fortlaufend, höchstens ein Bild pro 1000 ms (wird gespeichert) |
| `{"cmd":"stream","on":false}` | Stream anhalten (passiert auch beim Trennen automatisch) |
| `{"cmd":"cam","framesize":5,"quality":12}` | Auflösung / JPEG-Qualität (4–63, kleiner = besser), wird gespeichert |
| `{"cmd":"flash","value":0}` | Weiße LED 0–255 |
| `{"cmd":"name","name":"Helm 3"}` | Umbenennen, Kamera startet neu |
| `{"cmd":"reboot"}` | Neustart |
| `{"cmd":"reset"}` | Einstellungen zurücksetzen (Name wird wieder `Helm Camera`), Neustart |

`framesize`: 1 = 160x120, 5 = 320x240, 8 = 640x480, 9 = 800x600, 10 = 1024x768, 11 = 1280x720, 13 = 1600x1200.
Über Bluetooth sind 320x240 (ca. 5–15 KB pro Bild) ein guter Wert, je nach Handy etwa 2–5 Bilder/s.

### Bild

Jedes Bild ist ein JPEG, verteilt auf mehrere Notifications. Byte 0 ist der Pakettyp,
Zahlen sind little endian:

| Typ | Aufbau |
|---|---|
| `0x01` Start | `u32` Länge, `u16` Breite, `u16` Höhe, `u32` Bildnummer |
| `0x02` Daten | `u32` Offset, dann JPEG-Bytes bis zum Ende des Pakets |
| `0x03` Ende | `u32` Bildnummer |
| `0x04` Fehler | UTF-8-Text |

Ein Bild ist vollständig, wenn beim Ende-Paket die Summe der Daten-Bytes gleich der Länge ist.
Sonst das Bild verwerfen und auf das nächste warten. Die Paketgröße richtet sich nach der
ausgehandelten MTU (bis 512 Byte), deshalb sollte die App eine große MTU anfordern (Android: `requestMtu(517)`;
Chrome macht das automatisch).

Beispiel (JavaScript):

```js
let frame = null;
imageChr.addEventListener('characteristicvaluechanged', ev => {
  const dv = ev.target.value;
  switch (dv.getUint8(0)) {
    case 1: frame = { buf: new Uint8Array(dv.getUint32(1, true)), got: 0 }; break;
    case 2: {
      const part = new Uint8Array(dv.buffer, dv.byteOffset + 5, dv.byteLength - 5);
      frame?.buf.set(part, dv.getUint32(1, true));
      if (frame) frame.got += part.length;
      break;
    }
    case 3:
      if (frame && frame.got === frame.buf.length) onJpeg(new Blob([frame.buf], { type: 'image/jpeg' }));
      frame = null;
      break;
  }
});
```

## USB-Seriell (nur für den Flasher)

115200 Baud, Zeilen mit `\n`:

| Senden | Antwort |
|---|---|
| `NAME=<name>` | `OK NAME=<name>`, danach Neustart |
| `INFO` | `INFO {status-json}` |

Beim Start gibt die Kamera `READY NAME=<name>` aus.
