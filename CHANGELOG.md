# Changelog

The user manual is available at <https://esp32-si4732.github.io/ats-mini/manual.html>. The firmware flashing instructions are available at <https://esp32-si4732.github.io/ats-mini/flash.html>

<!-- towncrier release notes start -->

## 3.01 (2026-06-14)


### Added

- Added an offline-capable, installable Web Serial PWA (under `pwa/`, published to GitHub Pages at `/pwa/`) that mirrors the full device web UI (Spectrum / Control / Memory / Config) over USB, plus a "Desktop App (USB)" link at the top of the device-served web page that opens it. The firmware serial protocol was extended (backward compatibly) with line-based `?` query/response commands (`?STATUS`, `?MEM`, `?SCAN`, `?LISTS`, `?SET`, `?SCANRUN`/`?SCANSTOP`, `?LOG`) that reuse the existing HTTP API JSON builders, pending-settings apply path, and waterfall device lock, so a serial client reaches parity with the web UI without WiFi.
- The web UI is now a single tabbed page (Spectrum / Control / Memory / Config) with client-side tab switching. The new Spectrum tab is an SDR-style view: a line spectrum with current-level (blue, filled), peak-hold (orange, with "reset peak"), per-point SNR (green) and optional average traces, vertical MHz grid lines, a dashed "Tuned" marker, a Center/Span/Resolution/points readout, Run/Single sweep control, Fast/Medium/Wide and a Span (kHz) input, trace toggles and a Viridis/Inferno/Grayscale waterfall heatmap aligned to the same frequency axis. Detected channels (adaptive noise-floor peak detection) are marked, and clicking the spectrum or waterfall tunes the radio (snapping to the nearest detected peak). While the Spectrum tab is in continuous Run mode the device is locked and audio held muted (reusing the existing web-waterfall mechanism), with the Control tab greyed out and a banner shown.


### Changed

- Fixed the web Spectrum tab so continuous "Run" sweeps actually lock the device (the default tab was not marked active, so the sweep loop ran only once and the 3s waterfall timeout released the lock immediately); the device now shows the "Waterfall (Web UI) — radio paused" screen and ignores the encoder while Run is active, and unlocks on Stop / Single-complete / leaving the tab / 3s timeout. Stopped the Resolution readout flicker by only rewriting the readout/"Res" value when it changes (the status poll was blanking it every second). Added a hover crosshair with a floating frequency tooltip over the spectrum and waterfall, and click-to-tune now works while running (the tune is queued and applied between sweeps), with the result reflected on the Control tab via the live status.
- Refreshed the README to document the tabbed web UI (Spectrum / Control / Memory / Config), the SDR spectrum + waterfall with click-to-tune, the RDS display, the offline Web Serial PWA, and the extended line-based serial protocol, with new screenshots of each tab and the PWA shell.
- The Morse "CW (audio)" source is now always compiled in and selectable on screen. Instead of being gated behind the `-DMORSE_AUDIO_INPUT` compile flag, the menu always shows an advisory that it needs the optional audio->IO11 hardware mod; using it without the mod just reads an unconnected pin (at your own risk).
- The web Spectrum tab now defines the scan window with explicit "Init" and "End" frequency inputs (each with its own kHz/MHz unit selector, defaulting to MHz for FM and kHz for AM/SSB) instead of a single center-based "Span" box. The window [Init, End] is sent to /api/scan as `lo`/`hi` (Hz), clamped to the current band edges, and the backend derives the per-point step to span it over WATERFALL_POINTS samples (scanRun/scanInit gained an optional explicit start-frequency argument; default centered behavior is unchanged). The returned startHz/stepHz/count describe the actual window, so the grid, ruler, hover-crosshair frequency mapping, click-to-tune and the Center/Span/Resolution/pts readout all stay correct. The Narrow/Medium/Wide buttons now set a window of that width (in kHz) centered on the tuned frequency. Field values are pre-filled from the active window but never overwritten while focused. The spectrum frequency axis now picks its display unit adaptively (kHz for AM/SSB or windows narrower than 1 MHz, otherwise MHz with decimals scaled to the span), applied consistently to the top grid labels, waterfall ruler, hover tooltip, "Tuned" marker and the Center/Span readout. The Init/End inputs no longer silently discard typed values: a field stays "dirty" once edited (the refresh loop won't overwrite it even after blur) until you apply it with Enter or the "Set range" button. Applying shows clear feedback — a green "applied" state when accepted, or an amber state with a reason ("Init must be < End", "End lowered to band max", etc.) and the adjusted values actually used when clamped to the band; invalid entries turn red and keep your value for editing. A "Band lo – hi" hint next to the inputs shows the current band's edges in the selected unit. Switching to the Control tab now stops a running sweep and releases the device lock immediately (no 3s wait), and the redundant inline "Res" value was removed (resolution is already in the readout line above the canvas).


### Fixed

- Fixed the offline Web Serial PWA spectrum/waterfall not updating and a runaway serial flood. While a bridge-driven RSSI scan was running, the device's serial abort check consumed the connecting client's `?SCAN` poll bytes, which aborted the scan and desynced the `?`-command framing so leftover bytes ran as legacy single-char commands (notably `C`, the screen-capture dump that floods the port). The firmware now leaves serial bytes untouched while a web/serial scan is in progress, and the PWA's serial RPC matches `=`-prefixed responses to requests by tag and routes all other lines (echoes, CSV telemetry) to the console only, so an asynchronous stream can never corrupt scan/status parsing.

## 3.00 (2026-06-12)


### Added

- Added dynamic quick-select tuning step buttons and a live step readout to the Web UI Tuning card. The buttons now reflect the actual steps supported by the current band/mode (sourced from the new "steps" field in /api/status), so each button applies a distinct, real step that matches its label.
- Added more live controls to the Web UI Status page: an AGC On/Off toggle (pressed when AGC is automatic, un-pressed in manual/attenuator mode) placed before the existing AGC+/- buttons, a Mute toggle placed before the volume buttons, a mode selector that lists the modes valid for the current band, and a band selector dropdown listing all selectable bands (picking one switches the radio to that band's mode), each option showing its frequency range with thousands-grouped kHz values (e.g. "VHF (64 ~ 108 MHz)", "31M (9\u2009000 ~ 11\u2009000 kHz)"). The RDS station name and radio text are now also shown directly under the big frequency readout at the top of the page, and collapse out of view when empty. The mode and band lists are mode-aware: selecting a band switches the mode to that band's mode, and the band list updates live when the mode changes (FM bands only appear in FM mode, AM/SW bands in non-FM modes). New `agcAuto`, `modeIdx`, `modes`, `bandIdx` and `bands` fields were added to `/api/status`, applied from the main loop via `/api/set?agc|mute|mode|band=...`.
- Expanded the Web UI: the "Set" frequency box now uses the active unit (kHz/MHz) instead of raw Hz, an RSSI waterfall lets you scan the current band from the browser, the memory page is now a live, editable station list (tune, save current, clear, or manually write any slot), and the config page exposes all on-device settings (brightness, theme, UI layout, sleep, RDS mode, FM region, time zone, USB/Bluetooth/Wi-Fi modes) with immediate, persisted changes.
- Frequency limit override that allows tuning beyond the firmware band edges (still within the SI4732 physical range). It is locked by default for normal behavior and can be toggled via `Settings -> Freq Limit`, the Web UI, or the `P` serial command.
- Morse (CW) decoder with a pluggable signal source. It defaults to the RSSI/SNR envelope (works on stock hardware) and can use an audio input on GPIO11 (ADC2) when the optional hardware mod is present (built with `-DMORSE_AUDIO_INPUT`). The decoded text is shown on screen and in the Web UI. Selectable via `Settings -> Morse`.
- On-device RSSI waterfall reachable via Menu > Waterfall: a continuous scrolling heatmap of band activity. Rotating the encoder moves the scan center frequency and a button press exits back to normal operation, restoring the previous frequency and mute state. While the waterfall is active (on the device, or as the web "Auto" mode) it owns the radio, so normal tuning and listening are paused; the web UI greys out the radio controls and shows a "Waterfall ativo — rádio pausado" banner. The web waterfall now polls the scan status until it completes (using a new `busy` field) instead of relying on a fixed delay, fixing the case where the waterfall never loaded.
- The Web UI RSSI waterfall now overlays vertical frequency grid lines on the canvas with a matching tick ruler underneath (5 evenly spaced labels in the active unit, MHz for FM and thousands-grouped kHz for AM/SSB), so columns can be read off as frequencies. A "Span" selector (1x/2x/4x) was added to widen the scanned window by increasing the per-point scan step instead of the point count, so the covered span grows without slowing the row refresh rate (span = WATERFALL_POINTS x step, clamped to the current band edges). The on-device waterfall (Menu > Waterfall) gained matching subtle vertical frequency grid lines at the quarter divisions plus lo/center/hi frequency labels that track the tuned frequency live.
- Web UI that replicates the radio controls (tuning, band, mode, step, bandwidth, volume, AGC, direct frequency entry) with live status updates. It also shows the RDS station name, RDS radio text, and the decoded Morse text, and exposes the Morse source and frequency-limit override settings. Radio commands are queued and executed in the main loop to avoid racing with the receiver.


### Changed

- Make possible using either RSSI or SNR for Squelch. The setting is saved separately for each mode (FM, LSB, USB, AM).
- Redesigned the Web UI with a modern, responsive dark "receiver console" theme (card-based layout, pill tab navigation, accent colors) replacing the old plain tables. The Status page now has a high-contrast LED-style frequency readout with a clearly separated unit, mode-aware formatting (MHz for FM, thousands-separated kHz for AM/SSB), grouped control panels (Tuning, Band & Mode, Step & Bandwidth, Volume & AGC) with segmented buttons, and a radio-style signal panel with animated S-meter/SNR/battery bars, status badges (Wi-Fi, BLE, RDS, CW, Override, Mute) and the RSSI waterfall integrated below the meters. The Memory and Config pages share the same theme. Visual-only change: all endpoints, JSON fields and behavior are unchanged.
- Simplified the Web UI Signal card scan controls: removed the redundant "Scan now" button and renamed the auto-scan toggle to "Waterfall: Off"/"Waterfall: On".
- Sped up the RSSI waterfall refresh on both the web and the device. The per-scan point count (`WATERFALL_POINTS`) was reduced from 100 to 40 so each scan row completes faster (the on-device and web renderers already interpolate the points across the full width); the dominant per-row cost is the scan itself (points x per-point SI473x retune+settle), so fewer points directly speeds the row cadence. The web client also now polls the running scan every 120ms (was 500ms) and re-arms the next scan after 60ms (was 300ms), tightening the loop between rows. The per-point tuning delay (`WATERFALL_TUNE_DELAY`) is left at 10ms, already at the safe minimum for valid RSSI reads. Fewer points narrows the default scanned span (FM ~4MHz, AM/SSB ~400kHz at 1x) and lowers resolution slightly, both recovered on the web via the Span selector.
- The Web UI "Set frequency" box now stays live-updated with the current tuned frequency (unless you are editing it) and shows thousands-grouped digits matching the big readout, making fine adjustments easier; grouping/spaces are stripped on submit so tuning still works.


### Fixed

- Fixed the audio opening/closing repeatedly while the RSSI waterfall ran via the Web UI. The web waterfall now signals an explicit on/off mode (`/api/scan?auto=1`/`auto=0`): the device enters a locked state showing a "Waterfall (Web UI) — radio paused" screen and ignores the encoder/buttons, mutual-exclusion just like the on-device waterfall. Audio is muted once for the whole session instead of flapping on/off between scans, and restored on exit. If the web client stops polling (e.g. the tab is closed) the device auto-unlocks and resumes after a 3 second timeout.

## 2.35 (2026-05-03)


### Fixed

- Try to connect to other WiFi access points besides the strongest one (including hidden SSIDs if the corresponding option is enabled). [#326](https://github.com/esp32-si4732/ats-mini/issues/326)
- Do not abort the EiBi download process immediately <https://github.com/esp32-si4732/ats-mini/discussions/325>.

## 2.34 (2026-05-01)


### Added

- "PSRAM not detected" fatal error screen (shown if a wrong f/w variant was flashed).
- Add `E` serial command that emulates encoder short press.
- Add `F` serial command to tune directly to a frequency in Hz within the current band. In SSB modes, sub-kHz digits set the BFO as well.
- Experimental Bluetooth LE control is available under Settings->Bluetooth: use Ad hoc for the BLE remote-control protocol, or HID to connect supported Bluetooth remotes/keyboards for tuning and menu actions. It may be unstable.
- Experimental support for LILYGO T-Embed SI4732, see <https://esp32-si4732.github.io/ats-mini/hardware.html#lilygo-t-embed-si4732> for more details.


### Changed

- Abort the long running operations (Seek, Scan, EiBi) via any remote command
- Change the set theme serial command from ! to ^.
- Drop workaround for https://github.com/espressif/arduino-esp32/issues/11742. Use faster method (WiFiMulti) to connect to 2-nd or 3-rd configured access point.
- Remote control over USB serial port is disabled by default. To enable it, go to Settings->USB Port and set it to Ad hoc mode.


### Fixed

- Add an exhaustive (hopefully) list of UTC offsets. WARNING: please adjust your UTC offset again (the stored index is no longer valid). [#287](https://github.com/esp32-si4732/ats-mini/issues/287)
- Fix out of range menu controls due to fast encoder rotation.

## 2.33 (2025-09-22)


### Changed

- Adjust gamma for display ID 0x04858552 so the themes look closer to how they were designed (at least Orange now doesn't look like a lemon).

## 2.32 (2025-09-16)


### Removed

- Remove the dynamic CPU frequecy feature introduced in v2.31 (it caused sound artifacts when rotating the encoder). [#244](https://github.com/esp32-si4732/ats-mini/issues/244)
- Do not show the "Add" hint on an empty memory slot to prevent confusion with click vs short press.


### Changed

- Avoid drawing background color when drawing text. This dramatically helps UI customization modding efforts (like setting a background image instead of a plain color, [for example](https://github.com/esp32-si4732/ats-mini/discussions/240)). [#239](https://github.com/esp32-si4732/ats-mini/issues/239)
- Move the Web UI credentials form fields below the Wi-Fi settings. [#241](https://github.com/esp32-si4732/ats-mini/issues/241)


### Fixed

- Fix Wi-Fi connection issue to 2nd or 3rd access point configured on the settings web page. [#244](https://github.com/esp32-si4732/ats-mini/issues/244)

## 2.31 (2025-09-13)


### Removed

- Remove faster tuning in Seek mode on SSB and in Scan mode via press & rotate in favor of the new accelerated encoder control.
- Remove the ENABLE_HOLDOFF compile-time option.


### Added

- Encoder acceleration.
- Encoder click now cancels the EiBi schedule download process.


### Changed

- Reduce the upper CB band limit to 28MHz. [#205](https://github.com/esp32-si4732/ats-mini/issues/205)
- Independent USB/LSB calibration values. WARNING: this change will reset the bands settings. [#220](https://github.com/esp32-si4732/ats-mini/issues/220)
- Render partial frequency numbers on the tuning scale around screen edges. [#235](https://github.com/esp32-si4732/ats-mini/issues/235)
- Disable the Memory menu timeout (it is a surfing mode like Seek or Scan). Short press (0.5 sec) saves/clears a slot, click closes the menu.
- EXPERIMENTAL: overclock the I2C bus to 800kHz (affects Si4732).
- Set CPU freq to 240 MHz on encoder rotation, drop back to 80 MHz after 10 seconds of no activity. This results in snappier UI.


### Fixed

- Fix AVC wrapping to avoid selecting odd AVC values. [#207](https://github.com/esp32-si4732/ats-mini/issues/207)
- Add 100ms delay after Si4732 POWER_ON to fix the "Si4732 not detected" issue [#213](https://github.com/esp32-si4732/ats-mini/issues/213)
- Fix misbehaving squelch when changing bands.

## 2.30 (2025-08-07)


### Added

- Add Scan mode. Press the encoder for 0.5 seconds to rescan, press & rotate to tune using a larger step. The scan process can be aborted by clicking or rotating the encoder.


### Changed

- Switch from EEPROM to Preferences library to store the receiver settings. This change removes some old limitations and enables more flexible settings management. WARNING: upgrading to this firmware version from an older one will reset the settings. Also a forced reset might be required (hold the encoder and power on the receiver). [#94](https://github.com/esp32-si4732/ats-mini/issues/94)
- Mute audio amp during seek action to prevent audible artifacts. [#190](https://github.com/esp32-si4732/ats-mini/issues/190)
- Display "Loading SSB" message in the zoomed menu area.
- Extend the 16m broadcast band a bit to include CRI on 17490.
- Increase the number of memory slots to 99.


### Fixed

- Do not lose SSB sub kHz digits when storing Memory slots. [#109](https://github.com/esp32-si4732/ats-mini/issues/109)
- Restore saved bandwidth.
- Use default step when switching modes or memories.

## 2.28 (2025-07-01)


### Added

- Add UTC+5:30 offset for India (Asia/Kolkata). Users with offsets greater than 5:30 might need to readjust their timezone settings (menu indexes have been shifted).
- Enable PSRAM using different build artifacts for OSPI and QSPI ESP32-S3 modules. For more info see <https://esp32-si4732.github.io/ats-mini/flash.html#firmware-files>.


### Changed

- Much better seek sensitivity (SI4735 library patch by @zhang-chong). [#129](https://github.com/esp32-si4732/ats-mini/issues/129)
- 200kHz FM step now uses odd frequencies (99.1, 99.3, etc). [#161](https://github.com/esp32-si4732/ats-mini/issues/161)

### Fixed

- Fix loud clicks when changing bands/modes on the PCB version without the mute circuit. [#103](https://github.com/esp32-si4732/ats-mini/issues/103)
- Do not shadow station names by zoomed menu in the seek mode. [#157](https://github.com/esp32-si4732/ats-mini/issues/157)

## 2.27 (2025-06-07)


### Added

- Allow connecting to the receiver's web UI using the atsmini.local mDNS name in addition to an IP address. [#145](https://github.com/esp32-si4732/ats-mini/issues/145)


### Changed

- Disable Seek mode (menu) timeout.


### Fixed

- Clear RSSI, SNR, and station name when doing normal Seek. [#146](https://github.com/esp32-si4732/ats-mini/issues/146)
- Fix backwards EiBi seek from 30000kHz.

## 2.26 (2025-06-02)


### Added

- Show DHCP-assigned IP address on the About system screen.


### Fixed

- Fix crash when trying download the EiBi schedule in offline mode. [#132](https://github.com/esp32-si4732/ats-mini/issues/132)
- Fix timeout when connecting to Wi-Fi access points.

## 2.25 (2025-05-31)


### Removed

- Disable EEPROM backup/restore option on the settings web page. If you used this feature to restore the EEPROM and now see strange bugs when switching bands, please reset the receiver settings.


### Fixed

- Fix blinking RDS and static frequency name.

## 2.24 (2025-05-30)


### Added

- EiBi schedule support, see https://esp32-si4732.github.io/ats-mini/manual.html#schedule


### Fixed

- Reapply Squelch after waking up from CPU Sleep mode. [#127](https://github.com/esp32-si4732/ats-mini/issues/127)

## 2.23 (2025-05-26)


### Added

- Ability to select FM de-emphasis setting based on region. [#85](https://github.com/esp32-si4732/ats-mini/issues/85)
- Show MAC-address on the receiver status web page. [#114](https://github.com/esp32-si4732/ats-mini/issues/114)
- Add ALL-CT RDS options for those who prefer precise time over WiFi.
- EEPROM backup/restore via the receiver web interface. Restore only works on compatible firmware versions.
- New optional UI layout with large S-meter and S/N-meter.


### Fixed

- Escape quotes in web form fields (like SSIDs & passwords). [#113](https://github.com/esp32-si4732/ats-mini/issues/113)
- Fix the AVC bug, huge thanks to Dave (G8PTN)! [#117](https://github.com/esp32-si4732/ats-mini/issues/117)

## 2.22 (2025-05-23)


### Removed

- Removed the Mute menu option. Use short press instead while in the volume adjustment mode.


### Added

- Experimental Squelch option based on RSSI threshold. Unlikely to work in SSB mode. To turn it off quickly, short press (>0.5 sec) the encoder button in the Squelch menu mode. [#32](https://github.com/esp32-si4732/ats-mini/issues/32)
- Help screen and system info screen (see `Settings->About`). The help screen is also displayed on first start.


### Changed

- Use short press to delete a memory slot.


### Fixed

- Fix restoring memory slots that belong to the bands with the same names. [#100](https://github.com/esp32-si4732/ats-mini/issues/100)

## 2.21 (2025-05-21)


### Changed

- Make the Wi-Fi icon a bit more lightweight.


### Fixed

- Disable the automatic tuning capacitor. [#97](https://github.com/esp32-si4732/ats-mini/issues/97)
- NTP time synchronization no longer ignores seconds.

## 2.20 (2025-05-18)


### Added

- Direct frequency input mode. Press and rotate the encoder to select the step (digit or "half-digit"), rotate the encoder to adjust the frequency, use short press to align frequency to the current step. To exit the mode, click the encoder or wait for a couple of seconds. [#26](https://github.com/esp32-si4732/ats-mini/issues/26)
- `Settings->UTC Offset` now affects the displayed time (whether it was received from RDS or NTP). [#44](https://github.com/esp32-si4732/ats-mini/issues/44)
- `Settings->Scroll Dir.` option to reverse the menu scroll direction. [#79](https://github.com/esp32-si4732/ats-mini/issues/79)
- Add SNR to the serial console log.
- Stop the automatic seek process by clicking the encoder button (exit by rotating it still works).
- Use press+rotate for manual fine tuning in Seek mode.
- Wi-Fi mode to sync time over NTP, view the receiver status and Memory slots.


### Changed

- Experimental: re-enable automatic antenna capacitor on FM, MW and 160M bands (as it was in 1.06 and earlier), plus reset it when switching between AM bands. [#13](https://github.com/esp32-si4732/ats-mini/issues/13)
- The short press (volume) shortcut is no longer global (now works only in VFO mode) and can be used by menus and other modal modes. [#26](https://github.com/esp32-si4732/ats-mini/issues/26)
- SSB tuning is now aligned to the current step. [#76](https://github.com/esp32-si4732/ats-mini/issues/76)
- Completely silence speaker output on mute. [#78](https://github.com/esp32-si4732/ats-mini/issues/78)
- Decrease RSSI & SNR thresholds in FM/AM seek modes.
- Theme editor can be enabled or disabled via the `T` terminal command, without recompiling the firmware.


### Fixed

- Set seek step according to the current step. [#5](https://github.com/esp32-si4732/ats-mini/issues/5)
- SSB memory slots now store frequencies with 100Hz precision. Serial commands that work with memory slots now expect frequencies in Hz. [#79](https://github.com/esp32-si4732/ats-mini/issues/79)
- Fixed wrong menu titles. [#80](https://github.com/esp32-si4732/ats-mini/issues/80)
- Fix audible clicks when scrolling over empty memory slots.


### Improved Documentation

- Add web-based Memory slot edit/backup/restore tool: <https://esp32-si4732.github.io/ats-mini/memory.html>

## 2.14 (2025-05-06)


### Added

- Zoomed menu mode for accessibility purposes (enabled via Settings->Zoom menu). [#71](https://github.com/esp32-si4732/ats-mini/issues/71)
- Serial commands `$` and `#` to backup and restore memory slots.


### Changed

- Extended 75M and 90M band boundaries.

## 2.13 (2025-05-01)


### Added

- Add Seek menu - automatic scan on AM/FM, faster tuning step on SSB. [#26](https://github.com/esp32-si4732/ats-mini/issues/26)
- CPU sleep mode (light sleep) to reduce the power consumption even more.


### Changed

- Increase seek timeout from 8 seconds to 10 minutes.


### Fixed

- Get rid of the short display blink that shows some visual garbage at power on.

## 2.12 (2025-04-29)


### Removed

- Remove the main menu counter.


### Added

- Added RDS radio text, program type, and PI code. Can be enabled via Settings->RDS. [#9](https://github.com/esp32-si4732/ats-mini/issues/9)
- Memory menu to store favorite stations. Press the encoder on an empty slot to store the current frequency and mode, rotate the encoder to select a slot. WARNING: this firmware version resets the receiver settings. [#56](https://github.com/esp32-si4732/ats-mini/issues/56)
- Add `I` and `i` hotkeys to tweak the calibration value. Add separate BFO and calibration fields to the remote log. [#60](https://github.com/esp32-si4732/ats-mini/issues/60)
- Add 10kHz step for WFM mode
- Add `Settings->Sleep Mode`. Controls whether the encoder is locked or not during sleep. Unlocked mode allows tuning (and setting the volume) with less self-induced noise.


### Changed

- Remote encoder keys changed. Use `R` and `r` to rotate it and `e` to click.


### Fixed

- Get rid of 1M and other AM steps in LSB/USB steps menu [#15](https://github.com/esp32-si4732/ats-mini/issues/15)
- Extend the 25, 31, 41, 49, and 60 meter bands. [#50](https://github.com/esp32-si4732/ats-mini/issues/50)

## 2.11 (2025-04-22)


### Added

- Added named frequencies database with FT8 and SSTV frequencies as samples.
- New Magenta color theme


### Changed

- Make RDS clock optional, move it to the info panel. To enable it, set `Settings->RDS` to `PS+CT`. Also clock is synchronized only once until the receiver or RDS CT is turned on and off (this helps to avoid getting wrong time from incorrectly configured FM stations).  WARNING: this firmware resets the receiver settings! [#39](https://github.com/esp32-si4732/ats-mini/issues/39)
- Added DID/DST registers display to About screen.
- Added RGB color bar to About screen.
- Set the default sleep timeout to 0 (disabled)


### Fixed

- Use BGR color order on the GC9307 display type [#41](https://github.com/esp32-si4732/ats-mini/issues/41)
- Fixed display of CB channels. [#51](https://github.com/esp32-si4732/ats-mini/issues/51)
- SSB calibration wasn't applied when changing bands or modes. Now it is. [#53](https://github.com/esp32-si4732/ats-mini/issues/53)

## 2.10 (2025-04-18)


### Changed

- Move Calibration to the Settings menu
- New bands list: more bands, different names and limits, sorted by modulation type for faster switching. WARNING: this change resets the receiver settings!
- Pixel-wise tuning scale scrolling
- Simplify remote serial output, change encoder keybindings. Use `E` and `e` to simulate the encoder rotation, `p` to push the button.


### Fixed

- Fix the screenshot palette. You can use the following oneliner to make a screenshot: `echo -n C | socat stdio /dev/cu.usbmodem14401,echo=0,raw | xxd -r -p > /tmp/screenshot.bmp` [#40](https://github.com/esp32-si4732/ats-mini/issues/40)
- Autodetect the display type and invert/mirror the picture if needed [#41](https://github.com/esp32-si4732/ats-mini/issues/41)
- Fix SSB band limits checks [#46](https://github.com/esp32-si4732/ats-mini/issues/46)
- Hopefully fix the SSB noise tone issue [#46](https://github.com/esp32-si4732/ats-mini/issues/46)
- The "EEPROM Resetting" screen is not hidden

## 2.00 (2025-04-15)


### Added

- New Space theme.


### Changed

- Major code refactoring: split code into modules, get rid of global variables, etc. Huge thanks to Marat Fayzullin for doing the heavy lifting. [#35](https://github.com/esp32-si4732/ats-mini/issues/35)
- Bump ESP32 Arduino core to 3.2.0
- Disable delayed screen update by default
- Remove the Z timezone marker from RDS clock.

## 1.09 (2025-04-03)


### Added

- + Now aligning frequency to the step when tuning in AM and FM modes. [#30](https://github.com/esp32-si4732/ats-mini/issues/30)
- **RDS Time Synchronization**: Added support for displaying the current time synchronized via RDS (Radio Data System) when tuned to FM stations broadcasting time information. The time is displayed below the battery icon and includes a "Z" suffix to indicate UTC time. The time display is only visible after successful synchronization with an RDS signal. [#34](https://github.com/esp32-si4732/ats-mini/issues/34)


### Changed

- Make the Night color theme less bright, fix theme length

## 1.08 (2025-03-25)


### Added

- Support press+rotate for faster SSB tuning [#4](https://github.com/esp32-si4732/ats-mini/issues/4)
- Add channel information for CB. Support European and Russian channel lists from A to H. [#21](https://github.com/esp32-si4732/ats-mini/issues/21)


### Changed

- The default CB channel has been changed to the most popular frequency in Russia 27135 kHz (C15E). [#21](https://github.com/esp32-si4732/ats-mini/issues/21)
- Remove some delays and extra screen repaints (might or might not make the tuning process a bit faster)


### Fixed

- Fix getLastStep for LW/MW [#18](https://github.com/esp32-si4732/ats-mini/issues/18)
- Fix the volume shortcut bug with active menu [#20](https://github.com/esp32-si4732/ats-mini/issues/20)

## 1.07 (2025-03-21)


### Fixed

- Reenable USB Serial [#12](https://github.com/esp32-si4732/ats-mini/issues/12)
- Fix ANTCAP value readings [#13](https://github.com/esp32-si4732/ats-mini/issues/13)
- Disable automatic antenna capacitor [#13](https://github.com/esp32-si4732/ats-mini/issues/13)

## 1.06 (2025-03-20)


### Changed

- Unmute the audio on volume middle press shortcut [#1](https://github.com/esp32-si4732/ats-mini/issues/1)
- Lock encoder rotation during sleep, [#8](https://github.com/esp32-si4732/ats-mini/issues/8)
- Redraw the battery indicator in iOS style [#11](https://github.com/esp32-si4732/ats-mini/issues/11)
- Print tuning capacitor value to Serial port

## 1.05 (2025-03-16)


### Added

- Add EEPROM write icon
- New color themes: Bluesky, eInk, Pager, Orange, Night, Phosphor
- Support for color themes
- Theme editor. To enable it, recompile the firmware with THEME_EDITOR=1 and connect via USB serial port. Press @ to print the current theme, change it using any text editor (see themes.h for details), then press ! and paste the updated theme (effective until the receiver is powered off). Once you are happy, add the resulting colors to themes.h. Check out a useful color picker as well https://chrishewett.com/blog/true-rgb565-colour-picker/


### Changed

- Add repo URL to the About screen, hide battery


### Fixed

- Fix tuner scale height on MW1 & MW2 bands

## 1.04 (2025-03-12)


### Changed

- Refactor the code to make it more DRY
- Return the Volos Project UI with some changes. The new s-meter also serves as a stereo indicator on FM.

## 1.03 (2025-03-11)


### Added

- Add screenshot feature, new command to toggle serial log. Send "C" over the serial port to make a screenshot, then feed the resulting HEX dump to the "xxd -r -p" command to get a BMP image. Use the "t" command to toggle the serial log.


### Changed

- Center and underline the menu headers. Thanks to R8ADR for the patch


### Fixed

- Fix "P" (button press) serial command

## 1.02 (2025-03-11)


### Added

- * Display timeout has been added (Settings > Sleep), a value of zero disables the timeout
  * Turning the display off/on by long pressing the encoder (2 seconds). This reduces QRM from the display
  * Added "O" and "o" commands (display off and on) to the serial port protocol
- AM/FM station search by scrolling with the encoder pressed (the search can also be stopped by rotating the encoder)
- Pressing the encoder for more than 0.5 seconds triggers volume adjustment (this compensates the Volume menu option no longer being highlighted by default)


### Changed

- * The menu has been reorganized, and the Spare items have been removed
  * A Settings menu has been added, and the display brightness adjustment option has been moved there
  * The position in the main menu and settings menu is remembered (until power is turned off)
  * The startup splash screen has been moved to Settings > About (but the reset of settings by turning on the receiver with the encoder pressed still works)
  * The Seek Up/Down menu items have been removed
- Changed the charging indicator (lightning icon instead of the EXT text)


### Fixed

- Fixed the duplication of the MW2 band name

## 1.01 (2025-03-11)

Identical to 1.01 by Dave (G8PTN), just recompiled for `esp32:esp32@3.1.3` Arduino core.

### Added

- Added "MODE" configuration per band (FM, AM, LSB, USB)


### Changed

- Improved tuning speed by delaying the display updates

## 1.00 (2025-03-11)

Identical to 1.00 by Dave (G8PTN), just recompiled for `esp32:esp32@3.1.3` Arduino core.

### Added

- Added "Brightness" menu option

  * This controls the PWM from 32 to 255 (full on) in steps of steps of 32
  * When the brightness is set lower than 255, PSU or RFI noise may be present
- Added "Calibration" menu option

  This allows the SI4732 reference clock offset to be compensated per band
- Added Automatic Volume Control (AVC) menu option. This allows the maximum audio gain to be adjusted.
- Added GPIO1 (Output) control (0=FM, 1 = AM/SSB)
- Added a REMOTE serial interface for debug control and monitoring
- User interface modified:

  * Removed the frequency scale
  * Set "Volume" as the default adjustment parameter
  * Modifed the S-Meter size and added labels
  * All actions now use a single press of the rotary encoder button, with a 10s timeout
  * Added status bar with indicators for Display and EEPROM write activity
  * Added unit labels for "Step" and "BW"
  * Added SSB tuning step options 10Hz, 25Hz, 50Hz, 0.1k and 0.5k
  * Added background refresh of main screen
- VFO/BFO tuning mechanism added based on Goshante ATS_EX firmware

  * This provides "chuff" free tuning over a 28kHz span (+/- 14kHz)
  * Compile option "BFO_MENU_EN" for debug purposes, manual BFO is not required


### Changed

- Modified FM steps options (50k, 100k, 200k, 1M)
- Modified the audio mute behaviour

  * Previously the rx.setAudioMute() appeared to unmute when changing band
  * The "Mute" option now toggles the volume level between 0 and previous value
- Modified the battery monitoring function

  * Uses set voltages for 25%, 50% and 75% with a configurable hysteresis voltage
  * Added voltage reading to status bar
- Settings for AGC/ATTN, SoftMute and AVC stored in EEPROM per mode

  AGC/ATTN (FM, AM, SSB), SoftMute (AM, SSB), AVC (AM, SSB)


### Fixed

- Fix compilation errors related to ledc* calls

  https://docs.espressif.com/projects/arduino-esp32/en/latest/migration_guides/2.x_to_3.0.html#ledc
