// connection.ts - the ride app's link to the engine. The hard work is in C (app.wasm) and
// engine/tunnel.ts; this file only turns messages into SolidJS signals and points at the car
// arrays inside app.wasm memory.

import { createSignal } from 'solid-js';
import { PORT, type StatsMsg } from './engine/engine_proto';
import { Tunnel } from './engine/tunnel';
import type { CityMsg, HelloMsg, RideMsg } from './protocol';

export const [connected, setConnected] = createSignal(false);
export const [city, setCity] = createSignal<HelloMsg>({ t: 'hello', cars: 0, city: 1000, road: 50 });
export const [ride, setRide] = createSignal<RideMsg | null>(null);
export const [stats, setStats] = createSignal<StatsMsg | null>(null);
export const [cityStats, setCityStats] = createSignal<CityMsg | null>(null);
export const [problem, setProblem] = createSignal('');
export const [link, setLink] = createSignal({ bytesPerSec: 0, matches: 0, chunkHeals: 0, fullCopies: 0 });

export const tunnel = new Tunnel({
  wasmUrl: '/app.wasm',
  port: PORT,
  events: {
    onConnected: (on) => { setConnected(on); if (!on) setRide(null); },
    onProblem: setProblem,
    onJson: (m) => {
      if (m.t === 'hello') setCity(m as HelloMsg);
      else if (m.t === 'ride') setRide((m as RideMsg).state === 'none' ? null : (m as RideMsg));
      else if (m.t === 'stats') setStats(m as StatsMsg);
      else if (m.t === 'city') setCityStats(m as CityMsg);
    },
  },
});

// Views straight into app.wasm memory: nothing is copied, and they are NOT signals on purpose
// (3,000 cars change 20 times a second; the canvas reads them directly).
export const cars = {
  x: new Int16Array(0), y: new Int16Array(0), dx: new Int16Array(0), dy: new Int16Array(0),
  ride: new Int16Array(0), count: 0,
};

export async function connect() {
  await tunnel.start();
  const w = tunnel.wasm;
  if (!w) return;
  const at = (name: string) => w.fk_state_ptr() + (w[name] as () => number)();
  const n = (w.num_cars as () => number)(), buf = w.memory.buffer;
  cars.x = new Int16Array(buf, at('off_x'), n);
  cars.y = new Int16Array(buf, at('off_y'), n);
  cars.dx = new Int16Array(buf, at('off_dx'), n);
  cars.dy = new Int16Array(buf, at('off_dy'), n);
  cars.ride = new Int16Array(buf, at('off_ride'), n);
  cars.count = n;
  setInterval(() => setLink({
    bytesPerSec: tunnel.bytesPerSec, matches: tunnel.matches(), chunkHeals: tunnel.chunkHeals(), fullCopies: tunnel.fullCopies(),
  }), 1000);
}

export const send = (text: string) => tunnel.send(text);
