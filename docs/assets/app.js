// Black Crow Web Flasher — UI controller / state machine.
import { Flasher } from "./flasher.js";
import { SerialMonitor } from "./monitor.js";

const DISCORD_URL = "https://discord.gg/7hfsUcDPN";

/* ------------------------------------------------------------------ *
 * Tiny terminal renderer: honors \r (carriage return) for progress   *
 * lines and \n for new lines, into a single <pre> for performance.   *
 * ------------------------------------------------------------------ */
class Terminal {
  constructor(el) {
    this.el = el;
    this.lines = [];
    this.cur = "";
    this.MAX = 1500;
  }
  _render() {
    const text = this.cur ? this.lines.concat(this.cur).join("\n") : this.lines.join("\n");
    this.el.textContent = text;
    this.el.scrollTop = this.el.scrollHeight;
  }
  write(data) {
    const s = String(data);
    for (let i = 0; i < s.length; i++) {
      const ch = s[i];
      if (ch === "\r") this.cur = "";
      else if (ch === "\n") {
        this.lines.push(this.cur);
        this.cur = "";
      } else this.cur += ch;
    }
    if (this.lines.length > this.MAX) this.lines.splice(0, this.lines.length - this.MAX);
    this._render();
  }
  writeLine(data) {
    this.write(String(data) + "\n");
  }
  clean() {
    this.lines = [];
    this.cur = "";
    this._render();
  }
  get text() {
    return this.cur ? this.lines.concat(this.cur).join("\n") : this.lines.join("\n");
  }
}

/* ---------------------------- state ------------------------------- */
const $ = (id) => document.getElementById(id);
const state = {
  manifest: null,
  board: null, // selected board config
  port: null, // granted SerialPort
  busy: false,
  mode: "idle", // idle | connected | monitoring
};

let logTerm, monTerm, flasher, monitor;

/* ---------------------------- toasts ------------------------------ */
function toast(kind, title, msg, ms = 4200) {
  const stack = $("toastStack");
  const el = document.createElement("div");
  el.className = `toast ${kind}`;
  el.style.setProperty("--dur", `${ms}ms`);
  el.innerHTML = `<div class="t-title"></div><div class="t-msg"></div>`;
  el.querySelector(".t-title").textContent = title;
  el.querySelector(".t-msg").textContent = msg || "";
  stack.appendChild(el);
  setTimeout(() => {
    el.style.transition = "opacity .3s, transform .3s";
    el.style.opacity = "0";
    el.style.transform = "translateY(8px)";
    setTimeout(() => el.remove(), 320);
  }, ms);
}

/* -------------------------- step gating --------------------------- */
function setActiveStep(id) {
  ["step-board", "step-connect", "step-flash"].forEach((s) => {
    $(s).classList.toggle("active", s === id);
  });
}
function refreshUI() {
  const b = state.busy;
  const hasBoard = !!state.board;
  const connected = flasher.connected;
  const monitoring = monitor.active;

  $("step-connect").setAttribute("aria-disabled", String(!hasBoard));
  $("step-flash").setAttribute("aria-disabled", String(!connected && !monitoring));

  $("btnConnect").disabled = b || !hasBoard || monitoring;
  $("baudSelect").disabled = b || connected || monitoring;
  $("btnErase").disabled = b || !connected || monitoring;
  $("btnFlash").disabled = b || !connected || monitoring;

  // Monitor can run whenever we have a port (after a connect/flash).
  $("btnMonitor").disabled = b || (!connected && !monitoring && !state.port);
  $("btnMonitorCta").disabled = $("btnMonitor").disabled;
  $("monBaud").disabled = monitoring;

  $("btnMonitor").textContent = monitoring ? "■ Stop Monitor" : "▶ Open Serial Monitor";
  $("btnMonitorCta").textContent = monitoring ? "■ Stop Monitor" : "▶ Open Serial Monitor";
}
function setBusy(v, btn, labelBusy) {
  state.busy = v;
  if (btn) {
    if (v) {
      btn.dataset.label = btn.innerHTML;
      btn.innerHTML = `<span class="spin"></span>${labelBusy || "Working…"}`;
    } else if (btn.dataset.label) {
      btn.innerHTML = btn.dataset.label;
      delete btn.dataset.label;
    }
  }
  refreshUI();
}

/* --------------------------- helpers ------------------------------ */
async function fetchBytes(path) {
  // Defense-in-depth: the manifest is same-origin, but never let a path in it
  // reach off-origin. Only accept a plain relative path (no scheme, no
  // protocol-relative "//", no leading "/", no ".." traversal).
  if (typeof path !== "string" || !/^[A-Za-z0-9._\-/]+$/.test(path) || path.startsWith("/") || path.includes("//") || path.includes("..")) {
    throw new Error(`Refused unsafe firmware path: ${path}`);
  }
  const res = await fetch(path, { cache: "no-cache" });
  if (!res.ok) throw new Error(`${path} → HTTP ${res.status}`);
  return new Uint8Array(await res.arrayBuffer());
}
function portLabel(port) {
  try {
    const i = port.getInfo();
    if (i && i.usbVendorId != null) {
      const hex = (n) => n.toString(16).padStart(4, "0");
      return `USB ${hex(i.usbVendorId)}:${hex(i.usbProductId)}`;
    }
  } catch (_) {}
  return "Serial port";
}

/** Point the post-flash "Download the app" button at the manifest's APK.
 *  Mirrors fetchBytes' guard: only a plain same-origin relative path is
 *  accepted, so a bad manifest can't turn the button into an off-origin link. */
function wireAppDownload(app) {
  const btn = $("btnAppDownload");
  if (!btn || !app || typeof app.file !== "string") return;
  const file = app.file;
  if (!/^[A-Za-z0-9._\-/]+$/.test(file) || file.startsWith("/") || file.includes("//") || file.includes("..")) return;
  btn.setAttribute("href", file);
  const base = file.split("/").pop();
  if (base) btn.setAttribute("download", base);
  if (app.version) $("appVersion").textContent = app.version;
  if (typeof app.size === "number" && app.size > 0) {
    $("appSize").textContent = ` · ${(app.size / 1048576).toFixed(1)} MB`;
  }
}

/* --------------------------- actions ------------------------------ */
function selectBoard(id) {
  const board = state.manifest.boards[id];
  if (!board) return;
  state.board = board;
  document.querySelectorAll(".board-card").forEach((c) => {
    const on = c.dataset.board === id;
    c.classList.toggle("selected", on);
    c.setAttribute("aria-pressed", String(on));
  });
  $("flashTargetName").textContent = board.name;
  setActiveStep("step-connect");
  refreshUI();
  logTerm.writeLine(`» Board selected: ${board.name} (${board.flashSize}, chip ${board.chip}).`);
  $("step-connect").scrollIntoView({ behavior: "smooth", block: "center" });
}

async function connect() {
  if (state.busy || !state.board) return;
  const btn = $("btnConnect");
  try {
    if (monitor.active) await monitor.stop();
    if (flasher.connected) await flasher.disconnect(); // release before re-opening
    if (!state.port) {
      state.port = await navigator.serial.requestPort(); // native COM chooser
    }
  } catch (err) {
    if (err && err.name === "NotFoundError") return; // user cancelled the picker
    toast("err", "Port error", err.message);
    return;
  }
  setBusy(true, btn, "Connecting…");
  logTerm.writeLine(`» Connecting on ${portLabel(state.port)} at ${$("baudSelect").value} baud…`);
  try {
    const baud = parseInt($("baudSelect").value, 10);
    const chip = await flasher.connect(state.port, baud);
    let mac = "";
    try {
      mac = await flasher.readMac();
    } catch (_) {}
    $("ciChip").textContent = chip || state.board.chip;
    $("ciMac").textContent = mac || "—";
    $("ciPort").textContent = portLabel(state.port);
    $("ciTarget").textContent = `${state.board.flashSize} · ${state.board.flashMode} · ${state.board.flashFreq}`;
    $("chipInfo").classList.add("show");
    state.mode = "connected";
    setActiveStep("step-flash");
    toast("ok", "Connected", `${chip} is ready to flash.`);
    $("step-flash").scrollIntoView({ behavior: "smooth", block: "center" });
  } catch (err) {
    toast("err", "Could not connect", err.message);
    logTerm.writeLine(`‼ Connect failed: ${err.message}`);
    logTerm.writeLine(`  Tip: hold BOOT, tap RESET, release BOOT, then try again.`);
  } finally {
    setBusy(false, btn);
  }
}

async function eraseFlash() {
  if (state.busy || !flasher.connected) return;
  if (!confirm("Erase the ENTIRE flash on the device?\n\nThis wipes the firmware and all stored data. You will need to flash again afterwards.")) return;
  const btn = $("btnErase");
  setBusy(true, btn, "Erasing…");
  logTerm.writeLine("» Erasing entire flash…");
  try {
    await flasher.eraseFlash();
    toast("ok", "Flash erased", "The chip is now blank. Click Start Flash to install firmware.");
    logTerm.writeLine("✓ Flash erased.");
  } catch (err) {
    toast("err", "Erase failed", err.message);
    logTerm.writeLine(`‼ Erase failed: ${err.message}`);
  } finally {
    setBusy(false, btn);
  }
}

async function startFlash() {
  if (state.busy || !flasher.connected) return;
  const btn = $("btnFlash");
  setBusy(true, btn, "Flashing…");
  $("progressWrap").classList.add("show");
  setProgress(0, "Preparing…");
  setFlashStage("flashing", "Streaming firmware to the chip…");
  try {
    logTerm.writeLine(`» Downloading firmware for ${state.board.name}…`);
    const parts = [];
    for (const p of state.board.parts) {
      const data = await fetchBytes(p.path);
      parts.push({ name: p.name, offset: p.offset, data });
      logTerm.writeLine(`  loaded ${p.name} (${data.length.toLocaleString()} bytes) → 0x${p.offset.toString(16)}`);
    }
    const names = state.board.parts.map((p) => p.name);
    await flasher.writeFirmware(
      parts,
      { flashSize: state.board.flashSize, flashMode: state.board.flashMode, flashFreq: state.board.flashFreq },
      (fileIndex, written, total) => {
        const pct = total > 0 ? Math.round((written / total) * 100) : 0;
        const name = names[fileIndex] || `File ${fileIndex + 1}`;
        setProgress(pct, `Writing ${name} (${fileIndex + 1}/${names.length})`);
      }
    );
    setProgress(100, "Done");
    setFlashStage("done", "Done — firmware installed, device rebooting");
    // The board hard-resets and (native USB) re-enumerates, which invalidates
    // the current port handle. Drop it so the next Connect / Monitor click
    // re-opens the picker on the fresh device instead of a dead handle.
    state.port = null;
    toast("ok", "Flash complete", "Firmware installed. The device is rebooting.");
    logTerm.writeLine("✓ Flash complete — device reset into the new firmware.");
    const appReady = $("btnAppDownload").getAttribute("href") !== "#";
    if (appReady) $("appCta").classList.add("show");
    $("discordCta").classList.add("show");
    (appReady ? $("appCta") : $("discordCta")).scrollIntoView({ behavior: "smooth", block: "start" });
  } catch (err) {
    setFlashStage("idle", "Flash failed — see the log below");
    toast("err", "Flash failed", err.message);
    logTerm.writeLine(`‼ Flash failed: ${err.message}`);
    logTerm.writeLine(`  Tip: hold BOOT, tap RESET, release BOOT, reconnect, then retry.`);
  } finally {
    setBusy(false, btn);
  }
}

function setProgress(pct, label) {
  $("progressBar").style.width = `${pct}%`;
  $("progressPct").textContent = `${pct}%`;
  if (label) $("progressName").textContent = label;
}

/** Drive the chip visual: state is "idle" | "flashing" | "done". */
function setFlashStage(stateName, text) {
  const el = $("flashStage");
  el.classList.toggle("flashing", stateName === "flashing");
  el.classList.toggle("done", stateName === "done");
  if (text) $("flashStageText").textContent = text;
}

async function toggleMonitor() {
  if (state.busy) return;
  if (monitor.active) {
    await monitor.stop();
    return;
  }
  // Starting: free the port from esptool first.
  try {
    if (flasher.connected) await flasher.disconnect();
    if (!state.port) {
      state.port = await navigator.serial.requestPort();
    }
  } catch (err) {
    if (err && err.name === "NotFoundError") return;
    toast("err", "Monitor error", err.message);
    return;
  }
  $("monitorCard").classList.remove("hidden");
  $("chipInfo").classList.remove("show");
  const baud = parseInt($("monBaud").value, 10);
  monTerm.writeLine(`── monitor @ ${baud} baud · resetting device to stream logs ──`);
  try {
    await monitor.start(state.port, baud);
    state.mode = "monitoring";
    toast("info", "Monitor open", `Reading the device console at ${baud} baud.`);
  } catch (err) {
    // The port handle may be stale (e.g. the device re-enumerated after a
    // flash/reset). Drop it so clicking again re-opens the picker.
    state.port = null;
    toast("err", "Monitor error", `${err.message} — click again to re-select the port.`);
    monTerm.writeLine(`[could not open monitor: ${err.message}]`);
  }
  refreshUI();
  $("monitorCard").scrollIntoView({ behavior: "smooth", block: "center" });
}

function onMonitorClosed() {
  state.mode = "idle"; // flasher was disconnected; a reconnect is needed to flash again
  monTerm.writeLine("── monitor stopped ──");
  refreshUI();
}

/* -------------------------- bootstrap ----------------------------- */
function copyText(text, okMsg) {
  navigator.clipboard?.writeText(text).then(
    () => toast("ok", "Copied", okMsg),
    () => toast("err", "Copy failed", "Clipboard was blocked by the browser.")
  );
}

async function init() {
  // Browser capability gate.
  const supported = "serial" in navigator && window.isSecureContext;
  if (!supported) {
    $("app").classList.add("hidden");
    $("unsupported").classList.add("show");
    return;
  }

  logTerm = new Terminal($("logConsole"));
  monTerm = new Terminal($("monitorConsole"));
  flasher = new Flasher({
    clean: () => {}, // keep the user's log; esptool rarely calls this
    writeLine: (d) => logTerm.writeLine(d),
    write: (d) => logTerm.write(d),
  });
  monitor = new SerialMonitor((t) => monTerm.write(t), onMonitorClosed);

  try {
    const res = await fetch("firmware/firmware.json", { cache: "no-cache" });
    state.manifest = await res.json();
    $("fwVersion").textContent = state.manifest.firmwareVersion || "";
    wireAppDownload(state.manifest.app);
    logTerm.writeLine(`Black Crow Web Flasher ready. Firmware: ${state.manifest.firmwareVersion}.`);
  } catch (err) {
    toast("err", "Manifest error", "Could not load firmware/firmware.json.");
    logTerm.writeLine(`‼ Failed to load firmware manifest: ${err.message}`);
    return;
  }

  // Wire events.
  document.querySelectorAll(".board-card").forEach((card) => {
    card.addEventListener("click", () => selectBoard(card.dataset.board));
  });
  $("btnConnect").addEventListener("click", connect);
  $("btnErase").addEventListener("click", eraseFlash);
  $("btnFlash").addEventListener("click", startFlash);
  $("btnMonitor").addEventListener("click", toggleMonitor);
  $("btnMonitorCta").addEventListener("click", toggleMonitor);
  $("btnLogClear").addEventListener("click", () => logTerm.clean());
  $("btnLogCopy").addEventListener("click", () => copyText(logTerm.text, "Log copied to clipboard."));
  $("btnMonitorClear").addEventListener("click", () => monTerm.clean());
  document.querySelectorAll("[data-discord]").forEach((a) => (a.href = DISCORD_URL));

  initMotion();
  setActiveStep("step-board");
  refreshUI();
}

/* Scroll-reveal + subtle pointer-driven tilt/sheen on the board cards.
   All of it is disabled under prefers-reduced-motion or coarse pointers. */
function initMotion() {
  const reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  const revealEls = document.querySelectorAll(".reveal");

  if (reduceMotion || !("IntersectionObserver" in window)) {
    revealEls.forEach((el) => el.classList.add("in"));
  } else {
    const io = new IntersectionObserver(
      (entries, obs) =>
        entries.forEach((e) => {
          if (e.isIntersecting) {
            e.target.classList.add("in");
            obs.unobserve(e.target);
          }
        }),
      { threshold: 0.12 }
    );
    revealEls.forEach((el) => io.observe(el));
  }

  if (reduceMotion || !window.matchMedia("(pointer: fine)").matches) return;
  document.querySelectorAll(".board-card").forEach((card) => {
    card.addEventListener("pointermove", (ev) => {
      const r = card.getBoundingClientRect();
      const px = (ev.clientX - r.left) / r.width; // 0..1
      const py = (ev.clientY - r.top) / r.height; // 0..1
      card.style.setProperty("--ry", `${(px - 0.5) * 10}deg`);
      card.style.setProperty("--rx", `${(0.5 - py) * 8}deg`);
      card.style.setProperty("--px", `${px * 100}%`);
      card.style.setProperty("--py", `${py * 100}%`);
    });
    card.addEventListener("pointerleave", () => {
      card.style.setProperty("--ry", "0deg");
      card.style.setProperty("--rx", "0deg");
    });
  });
}

document.addEventListener("DOMContentLoaded", init);
