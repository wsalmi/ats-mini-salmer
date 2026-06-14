"use strict";

// ATS-Mini Web Serial controller.
// Protocol: single-character ad hoc commands at 115200 8N1.
// Status is read from the device monitor stream (toggled with 't'), a CSV line
// emitted ~every 500ms:
//   VER,freq,bfo,bandCal,band,mode,step,bw,agc,vol,rssi,snr,tuneCap,voltage,seq

const $ = (id) => document.getElementById(id);

let port = null;
let writer = null;
let reader = null;
let keepReading = false;
let lineBuf = "";
let lastVolume = 30; // for mute/unmute restore

const supported = "serial" in navigator;

function log(msg) {
  const el = $("log");
  el.textContent += msg + "\n";
  el.scrollTop = el.scrollHeight;
}

function setConnected(state) {
  $("connectBtn").hidden = state;
  $("disconnectBtn").hidden = !state;
  $("dot").className = "dot " + (state ? "on" : "off");
  if (state) {
    $("statusText").textContent = "Connected (115200 8N1). Streaming status…";
  } else {
    $("statusText").innerHTML = "Not connected. Click <b>Connect</b> and pick the ATS-Mini serial port.";
  }
}

async function connect() {
  if (!supported) return;
  try {
    port = await navigator.serial.requestPort();
    await port.open({ baudRate: 115200, dataBits: 8, stopBits: 1, parity: "none" });
    writer = port.writable.getWriter();
    keepReading = true;
    setConnected(true);
    readLoop();
    // Ensure the status monitor stream is on.
    await send("t");
    log("# connected");
  } catch (e) {
    log("! " + e.message);
    $("dot").className = "dot err";
  }
}

async function disconnect() {
  keepReading = false;
  try {
    if (reader) { await reader.cancel(); reader.releaseLock(); reader = null; }
  } catch (e) { /* ignore */ }
  try {
    if (writer) { writer.releaseLock(); writer = null; }
  } catch (e) { /* ignore */ }
  try {
    if (port) { await port.close(); port = null; }
  } catch (e) { /* ignore */ }
  setConnected(false);
  log("# disconnected");
}

async function send(str) {
  if (!writer) { log("! not connected"); return; }
  await writer.write(new TextEncoder().encode(str));
}

async function readLoop() {
  const decoder = new TextDecoder();
  while (port && port.readable && keepReading) {
    reader = port.readable.getReader();
    try {
      while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        if (value) handleChunk(decoder.decode(value, { stream: true }));
      }
    } catch (e) {
      log("! read: " + e.message);
    } finally {
      reader.releaseLock();
    }
  }
}

function handleChunk(text) {
  lineBuf += text;
  let idx;
  while ((idx = lineBuf.indexOf("\n")) >= 0) {
    const line = lineBuf.slice(0, idx).replace(/\r$/, "");
    lineBuf = lineBuf.slice(idx + 1);
    if (line.length) handleLine(line);
  }
}

function handleLine(line) {
  const parts = line.split(",");
  // Status line: 15 numeric/text fields, first is the firmware version number.
  if (parts.length === 15 && /^\d+$/.test(parts[0])) {
    updateStatus(parts);
  } else {
    log("< " + line);
  }
}

function fmtFreq(freq, bfo, mode) {
  // FM: currentFrequency unit = 10 kHz. AM/SSB: unit = 1 kHz.
  // SSB display (Hz) = currentFrequency*1000 + bfo.
  if (mode === "FM") {
    return { value: (freq / 100).toFixed(2), unit: "MHz" };
  }
  if (mode === "LSB" || mode === "USB") {
    const hz = freq * 1000 + Number(bfo);
    return { value: (hz / 1000).toFixed(3), unit: "kHz" };
  }
  return { value: String(freq), unit: "kHz" };
}

function updateStatus(p) {
  const [, freq, bfo, , band, mode, step, bw, agc, vol, rssi, snr, , volt] = p;
  const f = fmtFreq(Number(freq), bfo, mode);
  $("freq").textContent = f.value;
  $("frequnit").textContent = f.unit;
  $("band").textContent = band;
  $("mode").textContent = mode;
  $("step").textContent = step;
  $("bw").textContent = bw;
  $("agc").textContent = agc;
  $("vol").textContent = vol + (Number(vol) === 0 ? " (mute)" : "");
  $("rssi").textContent = rssi;
  $("snr").textContent = snr;
  $("volt").textContent = (Number(volt) * 1.702 / 1000).toFixed(2);
  if (Number(vol) > 0) lastVolume = Number(vol);
}

// Send Set Frequency: input is in kHz, protocol expects Hz + carriage return.
async function setFrequency() {
  const khz = parseFloat($("freqInput").value);
  if (!isFinite(khz) || khz <= 0) { log("! invalid frequency"); return; }
  const hz = Math.round(khz * 1000);
  await send("F" + hz + "\r");
  log("> F" + hz);
}

// No dedicated mute command exists; ramp volume to 0 (or restore last value).
async function toggleMute() {
  const cur = Number($("vol").textContent) || 0;
  if (cur > 0) {
    lastVolume = cur;
    await send("v".repeat(cur));
    log("> mute");
  } else {
    await send("V".repeat(lastVolume));
    log("> unmute -> " + lastVolume);
  }
}

function init() {
  if (!supported) {
    $("unsupported").hidden = false;
    $("connectBtn").disabled = true;
  }

  $("connectBtn").addEventListener("click", connect);
  $("disconnectBtn").addEventListener("click", disconnect);
  $("setFreqBtn").addEventListener("click", setFrequency);
  $("freqInput").addEventListener("keydown", (e) => { if (e.key === "Enter") setFrequency(); });
  $("muteBtn").addEventListener("click", toggleMute);

  $("rawSendBtn").addEventListener("click", () => {
    const v = $("rawInput").value;
    if (v) { send(v); log("> " + v); $("rawInput").value = ""; }
  });
  $("rawInput").addEventListener("keydown", (e) => { if (e.key === "Enter") $("rawSendBtn").click(); });
  $("clearLogBtn").addEventListener("click", () => { $("log").textContent = ""; });

  document.querySelectorAll("button[data-cmd]").forEach((btn) => {
    btn.addEventListener("click", () => {
      const cmd = btn.getAttribute("data-cmd");
      send(cmd);
      log("> " + cmd);
    });
  });

  if (port) setConnected(false);

  // Register the service worker for offline app-shell loading.
  if ("serviceWorker" in navigator) {
    window.addEventListener("load", () => {
      navigator.serviceWorker.register("service-worker.js", { scope: "./" })
        .catch((e) => console.warn("SW registration failed:", e));
    });
  }
}

init();
