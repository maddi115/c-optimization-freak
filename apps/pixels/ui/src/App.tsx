// App.tsx - the pixels page. Uses ROWS: the engine sends only squares that changed,
// and only inside the part of the board you chose to view.

import { createSignal, For, onMount } from 'solid-js';
import { PORT } from './engine/engine_proto';
import { Tunnel } from './engine/tunnel';
import { cmd, type BoardMsg, type HelloMsg } from './protocol';

const PALETTE = ['#ffffff', '#1f2937', '#ef4444', '#f59e0b', '#facc15', '#22c55e', '#3b82f6', '#a855f7'];
const SIZE = 512;

export default function App() {
  const [connected, setConnected] = createSignal(false);
  const [hello, setHello] = createSignal<HelloMsg>({ t: 'hello', w: 64, h: 64, colors: 8 });
  const [board, setBoard] = createSignal<BoardMsg | null>(null);
  const [color, setColor] = createSignal(2);
  const [view, setView] = createSignal<'all' | 'top'>('all');
  const [bytes, setBytes] = createSignal(0);
  let canvas!: HTMLCanvasElement;
  let painting = false;

  const tunnel = new Tunnel({
    port: PORT,
    events: {
      onConnected: setConnected,
      onRows: () => draw(),
      onJson: (m) => {
        if (m.t === 'hello') setHello(m as HelloMsg);
        else if (m.t === 'board') setBoard(m as BoardMsg);
      },
    },
  });

  const viewRows = () => (view() === 'all' ? hello().w * hello().h : (hello().w * hello().h) / 2);

  function draw() {
    const ctx = canvas.getContext('2d');
    if (!ctx) return;
    const { w, h } = hello(), cw = SIZE / w, ch = SIZE / h, seen = viewRows();
    ctx.fillStyle = '#f3f4f6';            // grid lines show through the 0.5 px gaps
    ctx.fillRect(0, 0, SIZE, SIZE);
    for (let i = 0; i < w * h; i++) {
      ctx.fillStyle = i < seen ? PALETTE[tunnel.rows[i] ?? 0] ?? '#fff' : '#e5e7eb';
      ctx.fillRect((i % w) * cw, Math.floor(i / w) * ch, cw - 0.5, ch - 0.5);
    }
    if (seen < w * h) {
      ctx.fillStyle = '#6b7280';
      ctx.font = '14px system-ui';
      ctx.fillText('Not viewing this half: the server sends nothing for it', 16, SIZE * 0.75);
    }
  }

  function paintAt(e: PointerEvent) {
    const r = canvas.getBoundingClientRect(), { w, h } = hello();
    const x = Math.floor(((e.clientX - r.left) / r.width) * w), y = Math.floor(((e.clientY - r.top) / r.height) * h);
    const cell = y * w + x;
    if (x < 0 || y < 0 || x >= w || y >= h || cell >= viewRows() || tunnel.rows[cell] === color()) return;
    tunnel.rows[cell] = color();          // show it right away; the server's copy follows within a tick
    tunnel.send(cmd.paint(cell, color()));
    draw();
  }

  function choose(v: 'all' | 'top') {
    setView(v);
    tunnel.subscribe(0, v === 'all' ? hello().w * hello().h : (hello().w * hello().h) / 2);
    draw();
  }

  onMount(() => {
    canvas.width = SIZE;
    canvas.height = SIZE;
    tunnel.start();
    setInterval(() => setBytes(tunnel.bytesPerSec), 1000);
  });

  const btn = (on: boolean) => ({
    padding: '6px 12px', 'border-radius': '8px', border: '1px solid #d1d5db', cursor: 'pointer',
    background: on ? '#111827' : '#ffffff', color: on ? '#ffffff' : '#111827',
  });

  return (
    <main style={{ 'font-family': 'system-ui, sans-serif', 'max-width': '560px', margin: '32px auto', padding: '0 16px', color: '#111827' }}>
      <h1 style={{ 'font-size': '24px', margin: '0 0 4px' }}>Pixels</h1>
      <p style={{ margin: '0 0 12px', color: '#4b5563' }}>
        A shared board on the same C engine as the ride app. Open two tabs and paint.
      </p>
      <div style={{ display: 'flex', gap: '12px', 'align-items': 'center', 'margin-bottom': '12px', 'font-size': '14px' }}>
        <span style={{ padding: '4px 10px', 'border-radius': '999px', background: connected() ? '#dcfce7' : '#fee2e2', color: connected() ? '#166534' : '#991b1b' }}>
          {connected() ? 'Connected to C engine' : 'Connecting…'}
        </span>
        <span style={{ color: '#4b5563' }}>{board()?.online ?? 0} online · {board()?.painted ?? 0} strokes · {bytes()} bytes/s</span>
      </div>
      <div style={{ display: 'flex', gap: '8px', 'margin-bottom': '12px' }}>
        <For each={PALETTE}>
          {(c, i) => (
            <button aria-label={`Color ${i() + 1}`} onClick={() => setColor(i())}
              style={{ width: '32px', height: '32px', background: c, 'border-radius': '8px', cursor: 'pointer',
                border: color() === i() ? '3px solid #111827' : '1px solid #d1d5db' }} />
          )}
        </For>
      </div>
      <div style={{ display: 'flex', gap: '8px', 'margin-bottom': '12px', 'align-items': 'center', 'font-size': '14px' }}>
        <span>View:</span>
        <button style={btn(view() === 'all')} onClick={() => choose('all')}>Whole board</button>
        <button style={btn(view() === 'top')} onClick={() => choose('top')}>Top half only</button>
      </div>
      <canvas ref={canvas}
        style={{ width: '100%', 'aspect-ratio': '1', 'border-radius': '8px', cursor: 'crosshair', 'touch-action': 'none', display: 'block' }}
        onPointerDown={(e) => { painting = true; paintAt(e); }}
        onPointerMove={(e) => painting && paintAt(e)}
        onPointerUp={() => (painting = false)}
        onPointerLeave={() => (painting = false)} />
    </main>
  );
}
