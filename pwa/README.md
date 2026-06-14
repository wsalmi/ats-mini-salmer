# ATS-Mini Desktop (USB) — Web Serial PWA

A standalone, offline-capable, installable Progressive Web App that controls the
ATS-Mini receiver over **USB** using the [Web Serial API](https://developer.mozilla.org/en-US/docs/Web/API/Web_Serial_API)
and the firmware's existing ad hoc serial protocol.

**Live app:** <https://wsalmi.github.io/ats-mini-salmer/pwa/>

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

Single-character commands at 115200 8N1 (see `ats-mini/Remote.cpp`):

| Cmd | Action | Cmd | Action |
|-----|--------|-----|--------|
| `R`/`r` | Tune up/down (encoder) | `V`/`v` | Volume up/down |
| `B`/`b` | Band +/- | `M`/`m` | Mode +/- |
| `S`/`s` | Step +/- | `W`/`w` | Bandwidth +/- |
| `A`/`a` | AGC/Att +/- | `L`/`l` | Backlight +/- |
| `O`/`o` | Sleep on/off | `P` | Toggle frequency override |
| `e` | Encoder click | `E` | Encoder short press |
| `F<Hz>\r` | Set frequency (Hz) | `t` | Toggle status stream |

There is no dedicated mute command, so "Mute" ramps the volume to 0 and restores
the previous value on unmute.

**Status stream** (`t` toggles it; one CSV line ~every 500 ms):

```
VER,frequency,bfo,bandCal,band,mode,step,bandwidth,agc,volume,rssi,snr,tuningCap,voltage,seq
```

FM frequency is in 10 kHz units; AM/SSB in 1 kHz. SSB display (Hz) =
`frequency*1000 + bfo`. Battery volts = `voltage * 1.702 / 1000`. RDS and
per-point spectrum data are **not** exposed over serial, so they are not shown.
