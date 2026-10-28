// Black Crow Web Flasher — esptool-js integration layer.
// Wraps ESPLoader/Transport so the UI stays simple. Verified against
// esptool-js@0.6.0: writeFlash expects fileArray[].data as Uint8Array,
// calculateMD5Hash receives a Uint8Array, and main() runs the bundled
// ESP32-C5 stub (fast + reliable erase).
import { ESPLoader, Transport } from "../vendor/esptool-bundle.js";

/** Hex MD5 of a Uint8Array using spark-md5 (loaded globally). */
function md5Hex(u8) {
  const SparkMD5 = window.SparkMD5;
  if (!SparkMD5) return "";
  // Slice to the exact view bytes (data may be a padded subarray).
  const buf = u8.buffer.slice(u8.byteOffset, u8.byteOffset + u8.byteLength);
  return SparkMD5.ArrayBuffer.hash(buf);
}

export class Flasher {
  /** @param {object} terminal IEspLoaderTerminal: { clean, writeLine, write } */
  constructor(terminal) {
    this.terminal = terminal;
    this.transport = null;
    this.esploader = null;
    this.chipName = null;
  }

  get connected() {
    return !!this.esploader;
  }

  /**
   * Open the port, detect the chip, upload the stub and switch baud.
   * Returns the chip description string. Cleans up the port on failure.
   */
  async connect(port, baudrate) {
    this.transport = new Transport(port, false);
    this.esploader = new ESPLoader({
      transport: this.transport,
      baudrate: baudrate,
      romBaudrate: 115200,
      terminal: this.terminal,
      debugLogging: false,
    });
    try {
      this.chipName = await this.esploader.main(); // detect + stub + changeBaud
      return this.chipName;
    } catch (err) {
      await this.disconnect();
      throw err;
    }
  }

  /** Best-effort read of the device MAC (returns null on failure). */
  async readMac() {
    try {
      return await this.esploader.chip.readMac(this.esploader);
    } catch (_) {
      return null;
    }
  }

  /** Full chip erase (requires the stub, which main() uploads). */
  async eraseFlash() {
    if (!this.esploader) throw new Error("Not connected");
    await this.esploader.eraseFlash();
  }

  /**
   * Flash the given parts, then hard-reset into the app.
   * @param {Array<{data: Uint8Array, offset: number}>} parts
   * @param {{flashSize:string, flashMode:string, flashFreq:string}} opts
   * @param {(fileIndex:number, written:number, total:number)=>void} onProgress
   */
  async writeFirmware(parts, opts, onProgress) {
    if (!this.esploader) throw new Error("Not connected");
    const options = {
      fileArray: parts.map((p) => ({ data: p.data, address: p.offset })),
      flashSize: opts.flashSize,
      flashMode: opts.flashMode,
      flashFreq: opts.flashFreq,
      eraseAll: false, // regions are overwritten; use Erase Flash for a clean wipe
      compress: true,
      reportProgress: onProgress,
    };
    if (window.SparkMD5) options.calculateMD5Hash = md5Hex; // verify after write
    await this.esploader.writeFlash(options);
    await this.esploader.after("hard_reset");
  }

  /** Close the transport and fully release the serial port. */
  async disconnect() {
    try {
      if (this.transport) await this.transport.disconnect();
    } catch (_) {
      /* already closed / lost */
    }
    this.transport = null;
    this.esploader = null;
    this.chipName = null;
  }
}
