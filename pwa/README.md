# ATS-Mini Desktop (USB) — Web Serial PWA

A standalone, offline-capable, installable Progressive Web App that mirrors the
**full device web UI** (Spectrum / Control / Memory / Config tabs) over **USB**
using the [Web Serial API](https://developer.mozilla.org/en-US/docs/Web/API/Web_Serial_API)
and the firmware's ad hoc serial protocol — no WiFi required.

**Live app:** <https://wsalmi.github.io/ats-mini-salmer/pwa/>

## Tabs / features

- **Spectrum** — line traces (current / peak / SNR / avg), Viridis/Inferno/Grayscale
  waterfall colormaps, Init/End window with kHz/MHz unit selectors + validation +
  band hint, adaptive axis, gridlines, hover crosshair, click-to-tune (snaps to
  detected peaks), Run/Single, and the device lock while sweeping. Sweeps run over
  USB serial, so the refresh rate is lower than the device's on-screen view.
- **Control** — frequency readout, RSSI/SNR/battery meters, RDS station + radiotext,
  status badges, tuning/band/mode/step/bandwidth/volume/mute/AGC, direct frequency
  entry, frequency-override toggle, Morse decoder readout, and a raw serial console.
- **Memory** — live editable memory list (tune / save / clear / manual set).
- **Config** — every on-device setting (brightness, theme, UI layout, scroll, zoom,
  sleep + sleep mode, RDS mode, FM region, time zone, USB / Bluetooth / Wi-Fi modes).

## Key points

- **Chromium only.** Web Serial is available in Chrome, Edge and Opera on desktop.
  Firefox and Safari are unsupported and the app shows a clear message there.
- **USB, not WiFi.** The PWA never talks to `http://atsmini.local`; it speaks to
  the device over the USB serial port (115200 8N1), so HTTPS mixed-content
  blocking does not apply.
- **Offline & installable.** A service worker caches the app shell so it loads
  without a network. Use the browser's *Install app* option to add it to your
  desktop.

## Usage

1. On the receiver enable `Settings → USB Port → Ad hoc`.
2. Plug the receiver into USB and open the app.
3. Click **Connect** and select the ATS-Mini serial port.
4. Use the on-screen controls. Live status (frequency, band, mode, volume, RSSI,
   SNR, battery) is read from the device status stream.

## Serial protocol

All at 115200 8N1 (see `ats-mini/Remote.cpp`).

### Legacy single-char commands (used for momentary actions)

`R`/`r` tune ±, `V`/`v` volume ±, `B`/`b` band ±, `M`/`m` mode ±, `S`/`s` step ±,
`W`/`w` bandwidth ±, `A`/`a` AGC/att ±, `L`/`l` backlight ±, `O`/`o` sleep on/off,
`e` encoder click, `E` short press, `P` freq-override toggle, `F<Hz>\r` set
frequency, `t` toggle CSV status stream, `#`/`$` memory set/list.

### Extended `?` line protocol (added for this PWA)

Each command starts with `?`, ends with CR (`\r`); responses are single CR/LF
lines prefixed so the client can parse them out of the stream:

| Command | Response | Purpose |
|---------|----------|---------|
| `?STATUS` | `=STATUS {json}` | full status (same JSON as the HTTP `/api/status`) |
| `?MEM` | `=MEM {json}` | memory slots (same as `/api/memory`) |
| `?SCAN` | `=SCAN {json}` | latest scan data: `rssi[]`, `snr[]`, `startHz`, `stepHz`, `count`, `busy`, `done` |
| `?LISTS` | `=LISTS {json}` | theme + UTC-offset name lists (one-shot) |
| `?SET key val` | `=OK SET` | change a setting (`brt theme ui zoom scroll sleep sleepmode rds region utc usb ble wifi step agc mute mode band override morse freq`) |
| `?SCANRUN lo hi cont` | `=OK SCANRUN` | run a scan over `[lo,hi]` Hz (`0 0` = centered); `cont=1` keeps the device locked for continuous sweeps |
| `?SCANSTOP` | `=OK SCANSTOP` | stop continuous scan / release the lock |
| `?MEM action slot [band hz mode]` | `=OK MEM` | `tune` / `save` / `clear` / `set` a slot |
| `?LOG 0\|1` | `=OK LOG` | turn the legacy CSV status stream off/on |

The PWA sends `?LOG 0` on connect (so the CSV stream doesn't add noise) and then
polls `?STATUS` ~1/s, `?MEM` ~1/3 s, and drives sweeps with `?SCANRUN` + `?SCAN`.

### Device lock during serial scans

`?SCANRUN` with `cont=1` reuses the **same** `webWaterfallActive()` mutual-exclusion
lock the HTTP waterfall uses: the device pauses normal tuning/listening, holds the
audio mute for the session, and shows the lock screen. The lock auto-clears after
~3 s if the client stops requesting scans (or on `?SCANSTOP`).

Frequency conventions: FM `frequency` is in 10 kHz units, AM/SSB in 1 kHz; SSB
display (Hz) = `frequency*1000 + bfo`. Per-point SNR is included alongside RSSI in
scan data, so the spectrum view shows both.
