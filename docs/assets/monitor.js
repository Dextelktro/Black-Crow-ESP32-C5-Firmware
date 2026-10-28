// Black Crow Web Flasher — raw serial monitor.
// Reads the device console directly over Web Serial (independent of
// esptool-js). The flasher must be disconnected first so the port is free.
export class SerialMonitor {
  /**
   * @param {(text:string)=>void} onData   called with decoded chunks
   * @param {(reason?:string)=>void} onClose called once when the monitor ends
   */
  constructor(onData, onClose) {
    this.onData = onData;
    this.onClose = onClose || (() => {});
    this.port = null;
    this.reader = null;
    this.running = false;
    this._closed = false;
  }

  get active() {
    return this.running;
  }

  /** Open the port at `baud` and stream decoded text until stop(). */
  async start(port, baud) {
    if (this.running) return;
    this.port = port;
    this._closed = false;

    // The port may still be releasing from a prior esptool session; tolerate
    // an "already open" state instead of failing the whole monitor.
    try {
      await port.open({ baudRate: baud });
    } catch (err) {
      if (!/already open/i.test(String(err && err.message))) throw err;
    }

    // Acquire the reader before marking active; on failure, release the port
    // so we don't leak an open handle.
    let reader;
    try {
      reader = port.readable.getReader();
    } catch (err) {
      try { await port.close(); } catch (_) {}
      this.port = null;
      throw err;
    }
    this.reader = reader;
    this.running = true;

    const decoder = new TextDecoder("utf-8", { fatal: false });
    // Read loop runs in the background.
    (async () => {
      try {
        while (this.running) {
          const { value, done } = await reader.read();
          if (done) break;
          if (value && value.length) this.onData(decoder.decode(value, { stream: true }));
        }
      } catch (err) {
        if (this.running) this.onData(`\n[monitor stopped: ${err.message}]\n`);
      } finally {
        await this._cleanup();
      }
    })();

    // Best-effort hard reset so the chip reboots into the app and streams fresh
    // logs. The ESP32-C5 console is on USB-Serial-JTAG, so right after an
    // esptool connect the chip sits in download mode and prints nothing until
    // it is reset. Asserting RTS pulses the internal reset; keeping DTR low
    // means a normal boot (not download mode). Done after the reader starts so
    // the boot banner is captured; ignored where signals aren't supported.
    try {
      await port.setSignals({ dataTerminalReady: false, requestToSend: true });
      await new Promise((r) => setTimeout(r, 120));
      await port.setSignals({ dataTerminalReady: false, requestToSend: false });
    } catch (_) {
      /* Some platforms reject control signals on USB-Serial-JTAG — ignore. */
    }
  }

  /** Stop streaming and release the port. */
  async stop() {
    if (!this.running && this._closed) return;
    this.running = false;
    try {
      if (this.reader) await this.reader.cancel(); // unblocks read() -> loop exits -> _cleanup
    } catch (_) {
      await this._cleanup();
    }
  }

  async _cleanup() {
    if (this._closed) return;
    this._closed = true;
    this.running = false;
    try {
      if (this.reader) this.reader.releaseLock();
    } catch (_) {}
    this.reader = null;
    try {
      if (this.port) await this.port.close();
    } catch (_) {}
    this.port = null;
    this.onClose();
  }
}
