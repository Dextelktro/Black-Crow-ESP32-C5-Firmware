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
    await port.open({ baudRate: baud });
    // Keep the device running (do not hold it in reset) where supported.
    try {
      await port.setSignals({ dataTerminalReady: false, requestToSend: false });
    } catch (_) {
      /* USB-Serial-JTAG may not support signals */
    }
    this.running = true;
    const decoder = new TextDecoder("utf-8", { fatal: false });
    this.reader = port.readable.getReader();
    // Read loop runs in the background.
    (async () => {
      try {
        while (this.running) {
          const { value, done } = await this.reader.read();
          if (done) break;
          if (value && value.length) this.onData(decoder.decode(value, { stream: true }));
        }
      } catch (err) {
        if (this.running) this.onData(`\n[monitor stopped: ${err.message}]\n`);
      } finally {
        await this._cleanup();
      }
    })();
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
