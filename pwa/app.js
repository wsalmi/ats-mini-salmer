"use strict";

// ATS-Mini Web Serial controller — full Web UI parity over USB.
//
// Transport: Web Serial @ 115200 8N1. Uses the firmware's extended '?' line
// protocol (see ats-mini/Remote.cpp) plus the legacy single-char commands.
//   ?STATUS -> =STATUS {json}   ?MEM -> =MEM {json}   ?SCAN -> =SCAN {json}
//   ?LISTS  -> =LISTS {json}    ?SET k v -> =OK SET   ?SCANRUN lo hi cont/?SCANSTOP
//   ?MEM action slot [band hz mode] -> =OK MEM        ?LOG 0|1 -> =OK LOG

const $ = (i) => document.getElementById(i);
const txt = (i, v) => { const e = $(i); if (e) e.textContent = v; };
const supported = "serial" in navigator;

let port = null, writer = null, reader = null, keepReading = false;
let connected = false;
let lineBuf = "";
let pollT = null, memT = null;

// ---- Serial line RPC (serialized writes; '=' lines resolve pending cmds) ----
let inFlight = null;
const sendQ = [];

function enc(s) { return new TextEncoder().encode(s); }

// A serial sweep blocks the device for ~1-3 s while it runs, so allow generous
// time for a '?SCAN' response before giving up.
const RPC_TIMEOUT = 6000;

function pump() {
  if (inFlight || !sendQ.length || !writer) return;
  const e = sendQ.shift();
  writer.write(enc(e.cmd)).catch((err) => log("! write: " + err.message));
  if (!e.expect) { if (e.res) e.res(); pump(); return; }
  inFlight = e;
  e.timer = setTimeout(() => { inFlight = null; e.rej(new Error("timeout")); pump(); }, RPC_TIMEOUT);
}

// Fire-and-forget raw bytes (single-char cmds, F<hz>, console input).
function sendRaw(s) {
  return new Promise((res) => { sendQ.push({ cmd: s, expect: null, res }); pump(); });
}

// Query/response: resolves with the matching '=<tag> ...' line text. `tag` is
// the expected response prefix without the leading '=' ("STATUS", "SCAN",
// "MEM", "LISTS", or "OK" for acks). Only a matching '=' line (or "=ERR")
// resolves the request; any other line is treated as telemetry/noise.
function rpc(cmd, tag) {
  return new Promise((res, rej) => { sendQ.push({ cmd: cmd + "\r", expect: true, tag: tag, res, rej }); pump(); });
}

function lineMatches(line, tag) {
  if (line.indexOf("=ERR") === 0) return true;        // surface errors to caller
  if (!tag) return line[0] === "=";
  return line.indexOf("=" + tag) === 0;
}

function onLine(line) {
  if (line.length === 0) return;
  // Command responses are '=' prefixed and matched to the in-flight request by
  // tag. Everything else (echoes, the legacy CSV telemetry stream, screen-dump
  // hex, error noise) is routed only to the console and never resolves an RPC,
  // so an asynchronous stream can never corrupt '?SCAN'/'?STATUS' parsing.
  if (line[0] === "=" && inFlight && lineMatches(line, inFlight.tag)) {
    clearTimeout(inFlight.timer); const e = inFlight; inFlight = null; e.res(line); pump();
    return;
  }
  log("< " + line);
}

function parseTagged(line, tag) {
  // "=TAG {json}" -> parsed object, or null
  const pfx = "=" + tag + " ";
  if (line.indexOf(pfx) !== 0) return null;
  try { return JSON.parse(line.slice(pfx.length)); } catch (e) { return null; }
}

// ---- logging --------------------------------------------------------------
function log(msg) {
  const el = $("log"); if (!el) return;
  el.textContent += msg + "\n";
  if (el.textContent.length > 8000) el.textContent = el.textContent.slice(-6000);
  el.scrollTop = el.scrollHeight;
}

// ---- connection -----------------------------------------------------------
function setConnUI(on) {
  connected = on;
  $("dot").className = "dot " + (on ? "on" : "off");
  txt("connlbl", on ? "Disconnect" : "Connect");
  const nc = $("needconn"); if (nc) nc.style.display = on ? "none" : "block";
}

async function toggleConnect() { connected ? disconnect() : connect(); }

async function connect() {
  if (!supported) return;
  try {
    port = await navigator.serial.requestPort();
    await port.open({ baudRate: 115200, dataBits: 8, stopBits: 1, parity: "none" });
    writer = port.writable.getWriter();
    keepReading = true;
    setConnUI(true);
    readLoop();
    log("# connected");
    await rpc("?LOG 0", "OK").catch(() => {});      // stop the CSV status stream
    await loadLists();                         // theme / UTC dropdowns (one-shot)
    poll(); mload();
    pollT = setInterval(poll, 1000);
    memT = setInterval(mload, 3000);
    if (specActive) scanOnce();
  } catch (e) {
    log("! " + e.message);
    $("dot").className = "dot err";
  }
}

async function disconnect() {
  keepReading = false;
  if (pollT) clearInterval(pollT), pollT = null;
  if (memT) clearInterval(memT), memT = null;
  if (specRun) setRun(false);
  try { if (reader) { await reader.cancel(); reader.releaseLock(); reader = null; } } catch (e) {}
  try { if (writer) { writer.releaseLock(); writer = null; } } catch (e) {}
  try { if (port) { await port.close(); port = null; } } catch (e) {}
  setConnUI(false);
  log("# disconnected");
}

async function readLoop() {
  const dec = new TextDecoder();
  while (port && port.readable && keepReading) {
    reader = port.readable.getReader();
    try {
      while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        if (!value) continue;
        lineBuf += dec.decode(value, { stream: true });
        let idx;
        while ((idx = lineBuf.indexOf("\n")) >= 0) {
          onLine(lineBuf.slice(0, idx).replace(/\r$/, ""));
          lineBuf = lineBuf.slice(idx + 1);
        }
      }
    } catch (e) { log("! read: " + e.message); }
    finally { reader.releaseLock(); }
  }
}

// ---- command helpers ------------------------------------------------------
var lastUnit = "MHz", gFreqHz = 0, gFm = true;
function cmd(c) { if (connected) sendRaw(c); }
function setVal(k, v) { if (connected) rpc("?SET " + k + " " + v, "OK").catch(() => {}); }
function setStep(khz) { setVal("step", khz); }
function tuneHz(hz) { if (connected) sendRaw("F" + Math.round(hz) + "\r"); }
function fmtFreq(s) { var p = ("" + s).split("."); p[0] = p[0].replace(/\B(?=(\d{3})+(?!\d))/g, "\u2009"); return p.join("."); }
function setFreq() {
  var s = $("freqval").value.replace(/[\s\u2009,]/g, ""); var v = parseFloat(s); if (isNaN(v)) return;
  var hz = lastUnit == "MHz" ? Math.round(v * 1e6) : Math.round(v * 1000); tuneHz(hz);
}

// ---- control helpers (mirrors device web UI) ------------------------------
function fmtStep(k) { return k >= 1000 ? ((k % 1000 ? (k / 1000).toFixed(1) : (k / 1000)) + "M") : (k + "k"); }
var lastSteps = "";
function renderSteps(a) {
  if (!a) return; var key = a.join(","); if (key === lastSteps) return; lastSteps = key;
  var c = $("stepbtns"); if (!c) return; c.innerHTML = "";
  for (var i = 0; i < a.length; i++) {
    var b = document.createElement("button"); b.textContent = fmtStep(a[i]);
    b.onclick = (function (k) { return function () { setStep(k); }; })(a[i]); c.appendChild(b);
  }
}
var curAgcAuto = false, curMuted = false;
function toggleAgc() { setVal("agc", curAgcAuto ? 0 : 1); }
function toggleMute() { setVal("mute", curMuted ? 0 : 1); }
var lastModes = "";
function renderModes(d) {
  if (!d.modes) return; var c = $("modebtns"); if (!c) return;
  var key = d.modes.map(function (m) { return m.i; }).join(",");
  if (key !== lastModes) {
    lastModes = key; c.innerHTML = "";
    d.modes.forEach(function (m) {
      var b = document.createElement("button"); b.textContent = m.n; b.setAttribute("data-i", m.i);
      b.onclick = (function (i) { return function () { setVal("mode", i); }; })(m.i); c.appendChild(b);
    });
  }
  var bs = c.children; for (var i = 0; i < bs.length; i++) bs[i].classList.toggle("acc", +bs[i].getAttribute("data-i") === d.modeIdx);
}
var lastBands = "";
function fmt1(x) { return x % 1 ? x.toFixed(1) : x.toFixed(0); }
function bandRange(b) {
  if (b.fm) return fmt1(b.lo / 100) + " ~ " + fmt1(b.hi / 100) + " MHz";
  if (b.hi >= 1000000) return fmt1(b.lo / 1000) + " ~ " + fmt1(b.hi / 1000) + " MHz";
  return fmtFreq(b.lo) + " ~ " + fmtFreq(b.hi) + " kHz";
}
function renderBands(d) {
  if (!d.bands) return; var sel = $("bandsel"); if (!sel) return;
  var key = d.bands.map(function (b) { return b.i; }).join(",");
  if (key !== lastBands && sel !== document.activeElement) {
    lastBands = key; sel.innerHTML = "";
    d.bands.forEach(function (b) {
      var o = document.createElement("option"); o.value = b.i; o.textContent = b.n + " (" + bandRange(b) + ")"; sel.appendChild(o);
    });
    // Memory editor band list (names only), populated once.
    var eb = $("eband");
    if (eb && !eb.children.length) d.bands.forEach(function (b) {
      var o = document.createElement("option"); o.value = b.n; o.textContent = b.n; eb.appendChild(o);
    });
  }
  if (sel !== document.activeElement) sel.value = d.bandIdx;
}
function grp(p) { return p >= 66 ? "fill g" : p >= 33 ? "fill a" : "fill r"; }
function meter(fi, vi, p, t) { var f = $(fi); if (!f) return; p = Math.max(0, Math.min(100, p)); f.style.width = p + "%"; f.className = grp(p); txt(vi, t); }
function bdg(id, on, warn) { var e = $(id); if (e) e.className = "badge" + (on ? (warn ? " warn" : " on") : ""); }

// ---- status poll ----------------------------------------------------------
async function poll() {
  if (!connected) return;
  let line; try { line = await rpc("?STATUS", "STATUS"); } catch (e) { return; }
  const d = parseTagged(line, "STATUS"); if (!d) return;
  lastUnit = d.unit; gFm = (d.unit == "MHz"); gFreqHz = d.freqHz;
  var fe = $("freq"); if (fe) fe.innerHTML = fmtFreq(d.freq) + "<span class='u'>" + d.unit + "</span>";
  var me = $("meta"); if (me) me.innerHTML = "<b>" + d.band + "</b> &middot; " + d.mode;
  txt("setunit", d.unit); txt("sethint", "Enter the frequency in " + d.unit + ".");
  var fq = $("freqval"); if (fq) { fq.placeholder = "Set frequency (" + d.unit + ")"; if (fq !== document.activeElement) fq.value = fmtFreq(d.freq); }
  meter("rssiFill", "rssiVal", d.rssi / 90 * 100, d.rssi + " dB\u00b5V");
  meter("snrFill", "snrVal", d.snr / 30 * 100, d.snr + " dB");
  meter("battFill", "battVal", (d.batt - 3.0) / 1.2 * 100, d.batt + " V");
  bdg("bdgWifi", d.wifi > 0); bdg("bdgBle", d.ble > 0); bdg("bdgRds", !!(d.station && d.station.length));
  bdg("bdgMorse", d.morse > 0); bdg("bdgOvr", d.override, true); bdg("bdgMute", d.muted, true);
  txt("step", d.step); txt("bw", d.bw); txt("vol", d.muted ? "Muted" : d.vol); txt("agc", d.agcAuto ? "AGC" : ("Att " + d.agc));
  var sb = $("stepbox"); if (sb) sb.textContent = d.step; renderSteps(d.steps);
  curAgcAuto = !!d.agcAuto; curMuted = !!d.muted;
  var ab = $("agcbtn"); if (ab) ab.classList.toggle("acc", curAgcAuto);
  var mb = $("mutebtn"); if (mb) mb.classList.toggle("acc", curMuted);
  renderModes(d); renderBands(d);
  if (d.bands) { var cb = null; for (var bi = 0; bi < d.bands.length; bi++) if (d.bands[bi].i === d.bandIdx) cb = d.bands[bi];
    if (cb) { gBandLoHz = cb.fm ? cb.lo * 10000 : cb.lo * 1000; gBandHiHz = cb.fm ? cb.hi * 10000 : cb.hi * 1000; updateBandHint(); } }
  txt("station", d.station || "-"); txt("rt", d.rt || "-"); txt("morsetext", d.morseText || "-");
  var st = (d.station || "").trim(), rtx = (d.rt || "").trim();
  var ts = $("topStation"); if (ts) { ts.textContent = st; ts.style.display = st ? "block" : "none"; }
  var trt = $("topRt"); if (trt) { trt.textContent = rtx; trt.style.display = rtx ? "block" : "none"; }
  var ms = $("morse"); if (ms && ms !== document.activeElement) ms.value = d.morse;
  var ov = $("override"); if (ov && ov !== document.activeElement) ov.checked = d.override;
  cfgApply(d);
}

// ---- config ---------------------------------------------------------------
function sset(k, v) { setVal(k, v); }
function cfgWifi(v) { if (confirm("Changing Wi-Fi mode is applied on the device. Continue?")) sset("wifi", v); }
function csel(id, v) { var e = $(id); if (e && e !== document.activeElement) e.value = v; }
function cfgApply(d) {
  csel("brt", d.brt); csel("sleep", d.sleep); csel("theme", d.theme); csel("ui", d.ui);
  csel("sleepmode", d.sleepMode); csel("rds", d.rdsMode); csel("region", d.region); csel("utc", d.utc);
  csel("usb", d.usb); csel("ble", d.ble); csel("wifi", d.wifi);
  var z = $("zoom"); if (z && z !== document.activeElement) z.checked = !!d.zoom;
  var s = $("scroll"); if (s && s !== document.activeElement) s.checked = !!d.scroll;
  var bv = $("brtv"); if (bv) bv.textContent = d.brt; var sv = $("sleepv"); if (sv) sv.textContent = d.sleep ? d.sleep + "s" : "Off";
}
async function loadLists() {
  let line; try { line = await rpc("?LISTS", "LISTS"); } catch (e) { return; }
  const d = parseTagged(line, "LISTS"); if (!d) return;
  var th = $("theme"); if (th && d.themes) { th.innerHTML = ""; d.themes.forEach(function (n, i) { var o = document.createElement("option"); o.value = i; o.textContent = n; th.appendChild(o); }); }
  var ut = $("utc"); if (ut && d.utc) { ut.innerHTML = ""; d.utc.forEach(function (n, i) { var o = document.createElement("option"); o.value = i; o.textContent = n; ut.appendChild(o); }); }
  // Memory editor mode list (firmware mode tokens).
  var em = $("emode"); if (em && !em.children.length) ["FM", "LSB", "USB", "AM"].forEach(function (m) { var o = document.createElement("option"); o.value = m; o.textContent = m; em.appendChild(o); });
}

// ---- memory ---------------------------------------------------------------
function mfmt(hz, m) { return m == "FM" ? (hz / 1e6).toFixed(2) + " MHz" : (hz / 1000).toFixed(0) + " kHz"; }
async function mload() {
  if (!connected) return;
  let line; try { line = await rpc("?MEM", "MEM"); } catch (e) { return; }
  const d = parseTagged(line, "MEM"); if (!d) return;
  var t = $("mlist"); if (!t) return; t.innerHTML = "";
  txt("mcount", "(" + d.used.length + "/" + d.total + ")");
  if (!d.used.length) { t.innerHTML = "<tr><td colspan='4' class='CENTER'>No stations stored</td></tr>"; return; }
  d.used.forEach(function (m) {
    var tr = document.createElement("tr");
    tr.innerHTML = "<td class='LABEL'>" + (m.s < 10 ? "0" : "") + m.s + "</td><td>" + mfmt(m.f, m.m) + "</td>" +
      "<td>" + m.b + " " + m.m + "</td><td><button onclick='act(\"tune\"," + m.s + ")'>Tune</button> " +
      "<button onclick='act(\"clear\"," + m.s + ")'>Clear</button></td>";
    t.appendChild(tr);
  });
}
function mrefresh() { setTimeout(mload, 500); }
function act(a, s) { if (connected) rpc("?MEM " + a + " " + s, "OK").then(mrefresh).catch(() => {}); }
function saveCur() { var s = $("saveslot").value; if (connected) rpc("?MEM save " + s, "OK").then(mrefresh).catch(() => {}); }
function setSlot() {
  var s = $("eslot").value, b = $("eband").value, m = $("emode").value, v = parseFloat($("efreq").value);
  if (isNaN(v)) { alert("Enter a frequency"); return; }
  var hz = $("eunit").value == "M" ? Math.round(v * 1e6) : Math.round(v * 1000);
  if (connected) rpc("?MEM set " + s + " " + b + " " + hz + " " + m, "OK").then(mrefresh).catch(() => {});
}

// ---- tabs -----------------------------------------------------------------
var specActive = false;
function showTab(t) {
  var ps = document.querySelectorAll(".pane"); for (var i = 0; i < ps.length; i++) ps[i].classList.toggle("on", ps[i].id === t);
  var as = document.querySelectorAll(".nav a[data-tab]"); for (var i = 0; i < as.length; i++) as[i].classList.toggle("on", as[i].getAttribute("data-tab") === t);
  specActive = (t === "spectrum");
  if (specActive) { fitCanvas(); redraw(); if (connected && specRun && !scanning) scanOnce(); }
  else if (specRun) setRun(false);
}

// ---- colormaps ------------------------------------------------------------
var CM = {
  viridis: [[68, 1, 84], [59, 82, 139], [33, 145, 140], [94, 201, 98], [253, 231, 37]],
  inferno: [[0, 0, 4], [87, 16, 110], [188, 55, 84], [249, 142, 9], [252, 255, 164]],
  gray: [[0, 0, 0], [64, 64, 64], [128, 128, 128], [192, 192, 192], [255, 255, 255]]
};
function cmap(v) {
  v = Math.max(0, Math.min(0.999, v)); var st = CM[$("cmap").value] || CM.viridis;
  var f = v * (st.length - 1), i = Math.floor(f), t = f - i, a = st[i], b = st[i + 1];
  return [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t];
}

// ---- spectrum / scan over serial ------------------------------------------
function lockRadio(on) {
  var nn = document.querySelectorAll(".lockable"); for (var i = 0; i < nn.length; i++) nn[i].classList.toggle("locked", on);
  var ban = $("wfban"); if (ban) ban.style.display = on ? "block" : "none";
}
var specRun = false, scanning = false, scanPollT = null, lastScan = null;
var peakHold = null, avgAcc = null, avgN = 0, wfRows = [], detPeaks = [];
var curLoHz = 0, curHiHz = 0;   // explicit scan window (0 = centered default)
function setRunBtn() { var b = $("runbtn"); if (b) { b.textContent = specRun ? "Stop" : "Run"; b.classList.toggle("acc", specRun); } txt("specstat", specRun ? "running" : "idle"); }
function setRun(on) {
  specRun = on; setRunBtn(); lockRadio(on);
  if (!connected) { specRun = false; setRunBtn(); lockRadio(false); return; }
  if (on) { if (!scanning) scanOnce(); }
  else { if (scanPollT) { clearTimeout(scanPollT); scanPollT = null; } rpc("?SCANSTOP", "OK").catch(() => {}); }
}
function single() { if (specRun) setRun(false); if (!scanning) scanOnce(); }

async function scanOnce() {
  if (scanning || !connected) return;
  scanning = true; txt("specstat", specRun ? "running" : "sweep");
  try { await rpc("?SCANRUN " + curLoHz + " " + curHiHz + " " + (specRun ? 1 : 0), "OK"); } catch (e) {}
  pollScan(0);
}
async function pollScan(tries) {
  scanPollT = null;
  let line; try { line = await rpc("?SCAN", "SCAN"); } catch (e) {
    if (tries < 120 && connected) scanPollT = setTimeout(function () { pollScan(tries + 1); }, 120); else scanDone(); return;
  }
  const d = parseTagged(line, "SCAN");
  if (!d) { scanDone(); return; }
  if (d.busy) { if (tries < 240 && connected) scanPollT = setTimeout(function () { pollScan(tries + 1); }, 100); else scanDone(); return; }
  if (d.count > 0) procScan(d);
  scanDone();
  if (specRun && specActive && connected) scanPollT = setTimeout(scanOnce, 40);
}
function scanDone() { scanning = false; if (!specRun) txt("specstat", "idle"); }
function procScan(d) {
  lastScan = d; var n = d.count, a = d.rssi;
  if (!peakHold || peakHold.length !== n) { peakHold = a.slice(); avgAcc = a.slice(); avgN = 1; }
  else { for (var i = 0; i < n; i++) { if (a[i] > peakHold[i]) peakHold[i] = a[i]; avgAcc[i] += a[i]; } avgN++; }
  detectPeaks(d); pushWf(d); redraw(); updateReadout(d); refreshRangeFields(d);
}
function detectPeaks(d) {
  var a = d.rssi, n = d.count; detPeaks = []; if (n < 3) return;
  var srt = a.slice().sort(function (x, y) { return x - y; }); var floor = srt[Math.floor(n * 0.4)];
  var mx = srt[n - 1], th = floor + Math.max(4, (mx - floor) * 0.35);
  for (var i = 1; i < n - 1; i++) if (a[i] >= th && a[i] >= a[i - 1] && a[i] > a[i + 1]) detPeaks.push(i);
}
function pushWf(d) {
  if (!$("tWf").checked) return; var c = $("wfs"), W = c.width; if (!W) return;
  var a = d.rssi, n = d.count, mn = Math.min.apply(null, a), mx = Math.max.apply(null, a), rg = (mx - mn) || 1;
  var row = new Float32Array(W);
  for (var i = 0; i < W; i++) { var fi = i / (W - 1) * (n - 1), lo = Math.floor(fi), t = fi - lo; var v0 = a[lo], v1 = a[Math.min(n - 1, lo + 1)]; row[i] = ((v0 + (v1 - v0) * t) - mn) / rg; }
  wfRows.unshift(row); if (wfRows.length > c.height) wfRows.pop();
}

// ---- range window controls ------------------------------------------------
var rangeInit = false, prevLou = "kHz", prevHiu = "kHz", dirtyLo = false, dirtyHi = false;
var gBandLoHz = 0, gBandHiHz = 0;
function uHz(v, u) { return u == "MHz" ? Math.round(v * 1e6) : Math.round(v * 1000); }
function fmtIn(hz, u) { return u == "MHz" ? (hz / 1e6).toFixed(3) : (hz / 1000).toFixed(0); }
function clrFx() { var l = $("loinp"), h = $("hiinp"); if (l) l.className = "sp"; if (h) h.className = "sp"; }
function markDirty(w) { if (w == "lo") dirtyLo = true; else dirtyHi = true; clrFx(); var m = $("rngmsg"); if (m) { m.className = "rngmsg sp"; m.textContent = ""; } }
function rngKey(e) { if (e.key == "Enter") { e.preventDefault(); applyRange(); } }
function rngMsg(kind, t) { var m = $("rngmsg"); if (m) { m.className = "rngmsg sp" + (kind ? " " + kind : ""); m.textContent = t || ""; } }
function fldFx(l, h) { var li = $("loinp"), hi = $("hiinp"); if (li) li.className = "sp" + (l ? " " + l : ""); if (hi) hi.className = "sp" + (h ? " " + h : ""); }
function applyRange() {
  var lo = parseFloat($("loinp").value), hi = parseFloat($("hiinp").value);
  var bl = isNaN(lo), bh = isNaN(hi); if (bl || bh) { fldFx(bl ? "fbad" : "", bh ? "fbad" : ""); rngMsg("bad", "Enter Init and End"); return; }
  var lh = uHz(lo, $("lou").value), hh = uHz(hi, $("hiu").value);
  if (lh >= hh) { fldFx("fbad", "fbad"); rngMsg("bad", "Init must be < End"); return; }
  var clh = lh, chh = hh, reason = "", fl = "", fh = "";
  if (gBandLoHz && gBandHiHz) {
    if (clh < gBandLoHz) { clh = gBandLoHz; reason = "Init raised to band min"; fl = "fadj"; }
    if (chh > gBandHiHz) { chh = gBandHiHz; reason = (reason ? "Init/End clamped to band" : "End lowered to band max"); fh = "fadj"; }
  }
  if (clh >= chh) { fldFx("fbad", "fbad"); rngMsg("bad", "Range out of band"); return; }
  curLoHz = clh; curHiHz = chh; wfRows = []; if (!scanning) scanOnce();
  dirtyLo = dirtyHi = false;
  if (reason) { $("loinp").value = fmtIn(clh, $("lou").value); $("hiinp").value = fmtIn(chh, $("hiu").value); fldFx(fl, fh); rngMsg("adj", reason + " \u2192 using " + axFmt(clh) + " \u2013 " + axFmt(chh)); }
  else { fldFx("fok", "fok"); rngMsg("ok", "applied"); setTimeout(function () { if (!dirtyLo && !dirtyHi) { clrFx(); rngMsg("", ""); } }, 2500); }
}
function convUnit(w) {
  var sel = $(w + "u"), inp = $(w + "inp"), prev = (w == "lo" ? prevLou : prevHiu), v = parseFloat(inp.value);
  if (!isNaN(v)) inp.value = fmtIn(uHz(v, prev), sel.value);
  if (w == "lo") prevLou = sel.value; else prevHiu = sel.value; updateBandHint();
}
function winPreset(wkhz) {
  var c = gFreqHz || (lastScan ? (lastScan.startHz + lastScan.stepHz * (lastScan.count - 1) / 2) : 0); if (!c) return;
  var half = wkhz * 500, lo = Math.max(0, c - half), hi = c + half;
  var u = gFm ? "MHz" : "kHz"; $("lou").value = u; $("hiu").value = u; prevLou = u; prevHiu = u; updateBandHint();
  $("loinp").value = fmtIn(lo, u); $("hiinp").value = fmtIn(hi, u); dirtyLo = dirtyHi = false;
  var bs = document.querySelectorAll(".sdrbar button[data-sp]"); for (var i = 0; i < bs.length; i++) bs[i].classList.toggle("acc", +bs[i].getAttribute("data-sp") === wkhz);
  applyRange();
}
function updateBandHint() {
  var h = $("bandhint"); if (!h) return; if (!gBandLoHz || !gBandHiHz) { h.textContent = ""; return; }
  var u = $("lou") ? $("lou").value : (gFm ? "MHz" : "kHz");
  h.textContent = "Band " + fmtIn(gBandLoHz, u) + " \u2013 " + fmtIn(gBandHiHz, u) + " " + u;
}
function refreshRangeFields(d) {
  var lou = $("lou"), hiu = $("hiu"); if (!lou) return;
  if (!rangeInit) { var u = gFm ? "MHz" : "kHz"; lou.value = u; hiu.value = u; prevLou = u; prevHiu = u; rangeInit = true; }
  var endHz = d.startHz + d.stepHz * (d.count - 1), li = $("loinp"), hi = $("hiinp");
  if (!dirtyLo && li !== document.activeElement) li.value = fmtIn(d.startHz, lou.value);
  if (!dirtyHi && hi !== document.activeElement) hi.value = fmtIn(endHz, hiu.value);
}
function resetPeak() { peakHold = null; avgAcc = null; avgN = 0; redraw(); }
function toggleWf() { fitCanvas(); redraw(); }

// ---- frequency<->x mapping + axis -----------------------------------------
function xToHz(x, W) { if (!lastScan) return 0; var n = lastScan.count; var fi = x / W * (n - 1); return lastScan.startHz + lastScan.stepHz * fi; }
function hzToX(hz, W) { if (!lastScan) return -1; var n = lastScan.count; var fi = (hz - lastScan.startHz) / lastScan.stepHz; return fi / (n - 1) * W; }
function gridLines() {
  if (!lastScan) return []; var n = lastScan.count; var lo = lastScan.startHz, hi = lastScan.startHz + lastScan.stepHz * (n - 1);
  var span = hi - lo; if (span <= 0) return []; var steps = [1e5, 2e5, 5e5, 1e6, 2e6, 5e6, 1e7]; var st = steps[0];
  for (var i = 0; i < steps.length; i++) { if (span / steps[i] <= 8) { st = steps[i]; break; } st = steps[i]; }
  var g = [], f = Math.ceil(lo / st) * st; for (; f <= hi; f += st) g.push(f); return g;
}
function axKHz() { return !gFm || (lastScan && lastScan.stepHz * (lastScan.count - 1) < 1e6); }
function axDec() { var sp = lastScan ? lastScan.stepHz * (lastScan.count - 1) : 0; return sp >= 2e7 ? 1 : (sp >= 2e6 ? 2 : 3); }
function axFmt(hz) { return axKHz() ? fmtFreq(Math.round(hz / 1000)) + " kHz" : (hz / 1e6).toFixed(axDec()) + " MHz"; }
function axWidth(hz) { return axKHz() ? fmtFreq(Math.round(hz / 1000)) + " kHz" : (hz / 1e6).toFixed(3) + " MHz"; }

// ---- drawing --------------------------------------------------------------
function redraw() { drawSpec(); drawWf(); }
function drawSpec() {
  var c = $("spec"); if (!c) return; var x = c.getContext("2d"), W = c.width, H = c.height;
  x.clearRect(0, 0, W, H); x.fillStyle = "#070b10"; x.fillRect(0, 0, W, H);
  if (!lastScan) return; var d = lastScan, n = d.count;
  var all = d.rssi.concat(peakHold || []); var mn = Math.min.apply(null, all), mx = Math.max.apply(null, all); var rg = (mx - mn) || 1;
  var pad = 18; function Y(v) { return H - pad - ((v - mn) / rg) * (H - pad * 2); }
  x.strokeStyle = "rgba(255,178,74,0.25)"; x.fillStyle = "#8a99a8"; x.font = "10px monospace"; x.lineWidth = 1;
  var gl = gridLines(), lab = $("tLabels").checked;
  for (var i = 0; i < gl.length; i++) { var gx = Math.round(hzToX(gl[i], W)) + 0.5; x.beginPath(); x.moveTo(gx, 0); x.lineTo(gx, H); x.stroke(); if (lab) x.fillText(axFmt(gl[i]), gx + 2, 11); }
  if ($("tSnr").checked && d.snr) { var s = d.snr, smn = Math.min.apply(null, s), smx = Math.max.apply(null, s), srg = (smx - smn) || 1; x.strokeStyle = "#37d67a"; x.lineWidth = 1.5; x.beginPath(); for (var i = 0; i < n; i++) { var px = i / (n - 1) * W, py = H - pad - ((s[i] - smn) / srg) * (H - pad * 2) * 0.85; i ? x.lineTo(px, py) : x.moveTo(px, py); } x.stroke(); }
  if ($("tAvg").checked && avgAcc && avgN) { x.strokeStyle = "#16c79a"; x.lineWidth = 1; x.beginPath(); for (var i = 0; i < n; i++) { var px = i / (n - 1) * W, py = Y(avgAcc[i] / avgN); i ? x.lineTo(px, py) : x.moveTo(px, py); } x.stroke(); }
  if ($("tPeak").checked && peakHold) { x.strokeStyle = "#ffb24a"; x.lineWidth = 1.5; x.beginPath(); for (var i = 0; i < n; i++) { var px = i / (n - 1) * W, py = Y(peakHold[i]); i ? x.lineTo(px, py) : x.moveTo(px, py); } x.stroke(); }
  x.strokeStyle = "#5aa9ff"; x.fillStyle = "rgba(90,169,255,0.18)"; x.lineWidth = 1.5; x.beginPath(); x.moveTo(0, H);
  for (var i = 0; i < n; i++) { var px = i / (n - 1) * W, py = Y(d.rssi[i]); x.lineTo(px, py); } x.lineTo(W, H); x.closePath(); x.fill();
  x.beginPath(); for (var i = 0; i < n; i++) { var px = i / (n - 1) * W, py = Y(d.rssi[i]); i ? x.lineTo(px, py) : x.moveTo(px, py); } x.stroke();
  x.fillStyle = "#ffd27a"; for (var i = 0; i < detPeaks.length; i++) { var pi = detPeaks[i], px = pi / (n - 1) * W, py = Y(d.rssi[pi]); x.beginPath(); x.arc(px, py - 4, 2.5, 0, 7); x.fill(); }
  var tx = hzToX(gFreqHz, W); if (tx >= 0 && tx <= W) { x.strokeStyle = "#e7eef5"; x.setLineDash([5, 4]); x.lineWidth = 1; x.beginPath(); x.moveTo(tx, 0); x.lineTo(tx, H); x.stroke(); x.setLineDash([]); if (lab) { x.fillStyle = "#e7eef5"; x.fillText("Tuned " + axFmt(gFreqHz), Math.min(tx + 3, W - 90), H - 4); } }
}
function drawWf() {
  var c = $("wfs"); if (!c) return; c.style.display = $("tWf").checked ? "block" : "none"; if (!$("tWf").checked) return;
  var x = c.getContext("2d"), W = c.width, H = c.height;
  var img = x.createImageData(W, H);
  for (var y = 0; y < H; y++) { var row = wfRows[y]; for (var i = 0; i < W; i++) { var v = row ? row[i] : 0, col = cmap(v), o = (y * W + i) * 4; img.data[o] = col[0]; img.data[o + 1] = col[1]; img.data[o + 2] = col[2]; img.data[o + 3] = 255; } }
  x.putImageData(img, 0, 0);
  x.fillStyle = "#cfe2f5"; x.font = "10px monospace"; var gl = gridLines();
  for (var i = 0; i < gl.length; i++) { var gx = Math.round(hzToX(gl[i], W)); x.strokeStyle = "rgba(255,255,255,0.15)"; x.beginPath(); x.moveTo(gx + 0.5, 0); x.lineTo(gx + 0.5, H); x.stroke(); if ($("tLabels").checked) x.fillText(axFmt(gl[i]), gx + 2, H - 3); }
}
var lastReadout = "";
function updateReadout(d) {
  var n = d.count, lo = d.startHz, hi = d.startHz + d.stepHz * (n - 1); var ctr = (lo + hi) / 2;
  var html = "Center <b>" + axFmt(ctr) + "</b> &nbsp; Span <b>" + axWidth(hi - lo) + "</b> &nbsp; Resolution <b>" + (d.stepHz / 1000) + " kHz/pt</b> &nbsp; <b>" + n + "</b> pts";
  if (html !== lastReadout) { lastReadout = html; var r = $("readout"); if (r) r.innerHTML = html; }
}
function canvClick(c, ev) {
  if (!lastScan) return; var rc = c.getBoundingClientRect(); var x = (ev.clientX - rc.left) / rc.width * c.width;
  var hz = xToHz(x, c.width); var n = lastScan.count, best = -1, bd = 1e18;
  for (var i = 0; i < detPeaks.length; i++) { var phz = lastScan.startHz + lastScan.stepHz * detPeaks[i]; var dd = Math.abs(phz - hz); if (dd < bd) { bd = dd; best = phz; } }
  var span = lastScan.stepHz * (n - 1); if (best >= 0 && bd < span * 0.03) hz = best; tuneHz(hz);
}
function fitCanvas() { var s = $("spec"), w = $("wfs"); if (!s) return; var W = s.clientWidth || 800; s.width = W; s.height = 300; w.width = W; w.height = 240; }
function hover(c, ev) {
  if (!lastScan) return; var st = $("specstack"), sr = st.getBoundingClientRect(), cr = c.getBoundingClientRect();
  var frac = (ev.clientX - cr.left) / cr.width; frac = Math.max(0, Math.min(1, frac));
  var n = lastScan.count, hz = lastScan.startHz + lastScan.stepHz * frac * (n - 1), xpx = ev.clientX - sr.left;
  var xl = $("xline"); xl.style.left = xpx + "px"; xl.style.height = st.clientHeight + "px"; xl.style.display = "block";
  var xh = $("xhair"); xh.textContent = axFmt(hz); xh.style.left = xpx + "px"; xh.style.display = "block";
}
function hoverOut() { var xl = $("xline"); if (xl) xl.style.display = "none"; var xh = $("xhair"); if (xh) xh.style.display = "none"; }

// ---- init -----------------------------------------------------------------
function init() {
  if (!supported) { var u = $("unsupported"); if (u) u.hidden = false; var c = $("connlink"); if (c) c.style.opacity = ".5"; }

  $("rawSendBtn").addEventListener("click", function () { var v = $("rawInput").value; if (v && connected) { sendRaw(v.indexOf("?") === 0 ? v + "\r" : v); log("> " + v); $("rawInput").value = ""; } });
  $("rawInput").addEventListener("keydown", function (e) { if (e.key === "Enter") $("rawSendBtn").click(); });
  $("clearLogBtn").addEventListener("click", function () { $("log").textContent = ""; });
  $("freqval").addEventListener("keydown", function (e) { if (e.key === "Enter") setFreq(); });

  window.addEventListener("resize", function () { if (specActive) { fitCanvas(); redraw(); } });
  $("spec").addEventListener("click", function (e) { canvClick(this, e); });
  $("wfs").addEventListener("click", function (e) { canvClick(this, e); });
  $("spec").addEventListener("mousemove", function (e) { hover(this, e); });
  $("wfs").addEventListener("mousemove", function (e) { hover(this, e); });
  $("specstack").addEventListener("mouseleave", hoverOut);

  setRunBtn(); fitCanvas(); redraw();

  if ("serviceWorker" in navigator) {
    window.addEventListener("load", function () {
      navigator.serviceWorker.register("service-worker.js", { scope: "./" }).catch(function (e) { console.warn("SW reg failed:", e); });
    });
  }
}

// Expose handlers used by inline HTML attributes.
window.showTab = showTab; window.toggleConnect = toggleConnect;
window.cmd = cmd; window.setVal = setVal; window.setFreq = setFreq;
window.toggleMute = toggleMute; window.toggleAgc = toggleAgc;
window.sset = sset; window.cfgWifi = cfgWifi;
window.act = act; window.saveCur = saveCur; window.setSlot = setSlot;
window.setRun = setRun; window.single = single; window.winPreset = winPreset;
window.applyRange = applyRange; window.convUnit = convUnit; window.markDirty = markDirty;
window.rngKey = rngKey; window.resetPeak = resetPeak; window.toggleWf = toggleWf; window.redraw = redraw;

init();
