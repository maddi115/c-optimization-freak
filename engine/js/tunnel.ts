// tunnel.ts - the small browser-only part of the engine (copied into each app's ui/src/engine/
// by make; do not edit the copy). All message handling lives in C (engine/client.c), compiled
// into the app's app.wasm. This file only does what needs a browser:
//   - the WebSocket (connect, join, reconnect)
//   - timing: a small jitter buffer, so lag spikes don't make the screen stutter
//   - JSON messages
// It only asks the browser for animation frames while there is something to do, so an idle
// page on a weak machine uses almost no CPU.

import { MSG_TICK, TICK_MS, type EngineMsg, type StatsMsg } from './engine_proto';

// What every app.wasm exports (engine/wasm_glue.h). Apps can export more (see apps/ride/wasm.c).
export type ClientWasm = {
  memory: WebAssembly.Memory;
  fk_setup(): void; fk_reset(): void;
  fk_in_ptr(): number; fk_in_cap(): number; fk_receive(n: number): number;
  fk_out_ptr(): number; fk_out_len(): number; fk_out_clear(): void;
  fk_state_ptr(): number; fk_state_bytes(): number; fk_synced(): number; fk_tick(): number;
  fk_matches(): number; fk_heals(): number; fk_fulls(): number;
  fk_rows_alloc(nrows: number, rowSize: number): number;
  [name: string]: unknown;
};

export type TunnelEvents = {
  onJson?: (m: { t: string; [k: string]: unknown }) => void;  // every JSON message (engine and app)
  onConnected?: (on: boolean) => void;
  onRows?: () => void;                                        // the rows copy changed
  onState?: () => void;                                       // the lockstep copy stepped or was replaced
  onMemory?: () => void;                                      // wasm memory grew: re-create views
  onProblem?: (text: string) => void;
};

const STATE = 1, ROWS = 2, SEND = 4;   // fk_receive() result bits (engine/client.c)
const BUFFER = 2;                      // run about 2 ticks (100 ms) behind, so late messages don't stutter
const MAX_BUFFER = 8;                  // more than this waiting: catch up by stepping faster

export class Tunnel {
  wasm?: ClientWasm;
  engine?: EngineMsg;
  stats?: StatsMsg;
  connected = false;
  version = 0;                  // goes up whenever the lockstep state steps or is replaced
  rows = new Uint8Array(0);     // ROWS apps: the copy of the rows (lives in wasm memory)
  bytesPerSec = 0;

  private ws?: WebSocket;
  private queue: (ArrayBuffer | string)[] = [];
  private ticksWaiting = 0;
  private running = false;
  private nextStepAt = 0;
  private lastStepAt = 0;
  private received = 0;
  private frame = 0;
  private decoder = new TextDecoder();

  constructor(private opts: { wasmUrl?: string; port: number; events: TunnelEvents }) {}

  async start() {
    try {
      const bytes = await (await fetch(this.opts.wasmUrl ?? '/app.wasm')).arrayBuffer();
      this.wasm = (await WebAssembly.instantiate(bytes, {})).instance.exports as unknown as ClientWasm;
      this.wasm.fk_setup();
    } catch {
      this.problem('Could not load app.wasm. Run `make page`.');
      return;
    }
    setInterval(() => { this.bytesPerSec = this.received; this.received = 0; }, 1000);
    this.connect();
  }

  send(text: string) { if (this.ws?.readyState === WebSocket.OPEN) this.ws.send(text); }
  subscribe(first: number, count: number) { this.send(`sub,${Math.round(first)},${Math.round(count)}`); }

  // Lockstep apps: how far (0..1) we are between the last step and the next, for smooth drawing.
  alpha() { return Math.min(1, Math.max(0, (performance.now() - this.lastStepAt) / TICK_MS)); }
  matches() { return this.wasm?.fk_matches() ?? 0; }
  chunkHeals() { return this.wasm?.fk_heals() ?? 0; }
  fullCopies() { return this.wasm?.fk_fulls() ?? 0; }

  private connect() {
    const url = import.meta.env.DEV
      ? `ws://${location.hostname}:${this.opts.port}`
      : `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}`;
    const ws = new WebSocket(url);
    this.ws = ws;
    ws.binaryType = 'arraybuffer';
    ws.onopen = () => {
      this.connected = true;
      this.wasm!.fk_reset();
      this.opts.events.onConnected?.(true);
      ws.send('join,1');                 // nothing comes until we join; 1 = send packed copies
    };
    ws.onclose = () => {
      this.connected = false;
      this.running = false;
      this.queue = [];
      this.ticksWaiting = 0;
      this.opts.events.onConnected?.(false);
      setTimeout(() => this.connect(), 1000);
    };
    ws.onmessage = (e) => {
      this.received += typeof e.data === 'string' ? e.data.length : e.data.byteLength;
      this.queue.push(e.data);
      if (this.isTick(e.data)) this.ticksWaiting++;
      this.pump();
    };
  }

  private problem(text: string) { this.opts.events.onProblem?.(text); }
  private isTick(m: ArrayBuffer | string) { return typeof m !== 'string' && new Uint8Array(m, 0, 1)[0] === MSG_TICK; }

  // Ask for another animation frame only while ticks are waiting.
  private schedule() {
    if (!this.frame && this.ticksWaiting) this.frame = requestAnimationFrame(() => { this.frame = 0; this.pump(); });
  }

  // Handle messages strictly in arrival order. Tick messages wait for their turn (jitter buffer);
  // everything else is handled as soon as it reaches the front.
  private pump() {
    const w = this.wasm!;
    while (this.queue.length) {
      const m = this.queue[0];
      if (this.isTick(m) && w.fk_synced()) {
        const now = performance.now();
        if (!this.running) {
          if (this.ticksWaiting < BUFFER) break;                  // fill the buffer first
          this.running = true;
          this.nextStepAt = now;
        }
        if (now < this.nextStepAt && this.ticksWaiting <= MAX_BUFFER) break; // not time yet
        this.nextStepAt = Math.max(this.nextStepAt + TICK_MS, now - 250);
        this.lastStepAt = now;
        if (this.ticksWaiting <= 1) this.running = false;         // ran dry: refill before going on
      }
      this.queue.shift();
      if (this.isTick(m)) this.ticksWaiting--;
      if (typeof m === 'string') this.onText(m);
      else this.feed(m);
    }
    this.schedule();
  }

  // Hand one binary message to the C code in app.wasm.
  private feed(buf: ArrayBuffer) {
    const w = this.wasm!;
    if (buf.byteLength > w.fk_in_cap()) { this.problem('A message from the server was too big.'); return; }
    new Uint8Array(w.memory.buffer, w.fk_in_ptr(), buf.byteLength).set(new Uint8Array(buf));
    const r = w.fk_receive(buf.byteLength);
    if (r & SEND) {                       // the C code wants to tell the server something (fix, patch, sync)
      const text = this.decoder.decode(new Uint8Array(w.memory.buffer, w.fk_out_ptr(), w.fk_out_len()));
      w.fk_out_clear();
      for (const line of text.split('\n')) if (line) this.send(line);
    }
    if (r & STATE) { this.version++; this.opts.events.onState?.(); }
    if (r & ROWS) this.opts.events.onRows?.();
  }

  private onText(text: string) {
    const m = JSON.parse(text);
    if (m.t === 'engine') {
      this.engine = m;
      if (m.rows > 0 && this.rows.length !== m.rows * m.rowSize) {   // room for the rows, once
        const ptr = this.wasm!.fk_rows_alloc(m.rows, m.rowSize);
        if (!ptr) { this.problem('Not enough memory for the rows.'); return; }
        this.rows = new Uint8Array(this.wasm!.memory.buffer, ptr, m.rows * m.rowSize);
        this.opts.events.onMemory?.();
      }
    } else if (m.t === 'stats') this.stats = m;
    this.opts.events.onJson?.(m);
  }
}
