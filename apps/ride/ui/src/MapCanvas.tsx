// MapCanvas.tsx - draws the city and 3,000 cars.
// Two stacked canvases: streets (drawn once) underneath, cars (redrawn when they move) on top.
// Car positions are read straight out of app.wasm memory: nothing is copied.
// Cars step 20 times a second; between steps they glide (drawn at 60 fps) so motion looks smooth.

import { createEffect, onCleanup, onMount } from 'solid-js';
import { cars, city, ride, tunnel } from './connection';
import { NO_RIDE } from './protocol';

type Point = { x: number; y: number } | null;

const SIZE = 640; // CSS pixels
// Phones have 3x screens: 3x3 = 9 pixels per dot. 2x looks the same for small dots
// and is less than half the work, so cheap phones stay smooth.
const DPR = Math.min(window.devicePixelRatio || 1, 2);

export default function MapCanvas(props: {
  pickup: Point;
  dropoff: Point;
  onPick: (x: number, y: number) => void;
}) {
  let streets!: HTMLCanvasElement;
  let canvas!: HTMLCanvasElement;
  let frame = 0;
  let drawnVersion = -1;
  let dirty = true;
  // Gliding redraws all cars 60 times a second. A slow phone can't keep up with that, so the map
  // watches its own frame time and falls back to drawing only when cars actually step (20/s).
  let frameMs = 16.7, lastFrame = 0, smooth = true;

  // Redraw when a pin or the ride changes, even if the cars did not move.
  createEffect(() => {
    props.pickup; props.dropoff; ride();
    dirty = true;
  });

  onMount(() => {
    for (const c of [streets, canvas]) { c.width = SIZE * DPR; c.height = SIZE * DPR; }

    const sctx = streets.getContext('2d')!;
    sctx.scale(DPR, DPR);
    const drawStreets = () => {
      const { city: size, road } = city();
      const s = SIZE / size;
      sctx.fillStyle = '#f3f4f6';
      sctx.fillRect(0, 0, SIZE, SIZE);
      sctx.strokeStyle = '#ffffff';
      sctx.lineWidth = 3;
      for (let v = 0; v <= size; v += road) {
        sctx.beginPath(); sctx.moveTo(v * s, 0); sctx.lineTo(v * s, SIZE); sctx.stroke();
        sctx.beginPath(); sctx.moveTo(0, v * s); sctx.lineTo(SIZE, v * s); sctx.stroke();
      }
    };
    drawStreets();

    const ctx = canvas.getContext('2d')!;
    ctx.scale(DPR, DPR);

    const draw = (now: number) => {
      frame = requestAnimationFrame(draw);
      if (lastFrame) frameMs = frameMs * 0.95 + Math.min(100, now - lastFrame) * 0.05;
      lastFrame = now;
      if (smooth && frameMs > 22) smooth = false;          // struggling: stop gliding
      else if (!smooth && frameMs < 17.5) smooth = true;   // plenty of time again: glide
      const a = smooth ? tunnel.alpha() : 1;
      if (tunnel.version === drawnVersion && !dirty && (a >= 1 || !smooth)) return; // nothing new: draw nothing
      drawnVersion = tunnel.version;
      dirty = false;
      const back = 1 - a; // draw each car between where it was and where it is now

      const s = SIZE / city().city;
      ctx.clearRect(0, 0, SIZE, SIZE);

      // All cars, in two passes so the colour only changes twice per frame.
      const { x, y, dx, dy, ride: busy, count } = cars;
      const px = (i: number) => (x[i] - dx[i] * back) * s, py = (i: number) => (y[i] - dy[i] * back) * s;
      ctx.fillStyle = '#64748b';
      for (let i = 0; i < count; i++) if (busy[i] === NO_RIDE) ctx.fillRect(px(i) - 1.5, py(i) - 1.5, 3, 3);
      ctx.fillStyle = '#f59e0b';
      for (let i = 0; i < count; i++) if (busy[i] !== NO_RIDE) ctx.fillRect(px(i) - 2, py(i) - 2, 4, 4);

      const r = ride();
      const pick = r && r.state !== 'done' ? { x: r.px, y: r.py } : props.pickup;
      const drop = r && r.state !== 'done' ? { x: r.dx, y: r.dy } : props.dropoff;

      // Route: drive along x first, then y (same rule the C cars follow).
      if (pick && drop) {
        ctx.strokeStyle = '#111827';
        ctx.lineWidth = 3;
        ctx.setLineDash([6, 5]);
        ctx.beginPath();
        ctx.moveTo(pick.x * s, pick.y * s);
        ctx.lineTo(drop.x * s, pick.y * s);
        ctx.lineTo(drop.x * s, drop.y * s);
        ctx.stroke();
        ctx.setLineDash([]);
      }

      // Rings around the 3 nearest free cars while you look at a quote.
      if (r?.state === 'quoted') {
        ctx.strokeStyle = '#2563eb';
        ctx.lineWidth = 2;
        for (const id of r.near) {
          if (id < 0 || id >= count) continue;
          ctx.beginPath();
          ctx.arc(px(id), py(id), 7, 0, Math.PI * 2);
          ctx.stroke();
        }
      }

      // Your car, big and green.
      if (r && r.car >= 0 && r.car < count && (r.state === 'arriving' || r.state === 'on_trip')) {
        ctx.fillStyle = '#16a34a';
        ctx.beginPath();
        ctx.arc(px(r.car), py(r.car), 7, 0, Math.PI * 2);
        ctx.fill();
      }

      if (pick) pin(ctx, pick.x * s, pick.y * s, '#16a34a');
      if (drop) pin(ctx, drop.x * s, drop.y * s, '#111827');
    };
    frame = requestAnimationFrame(draw);
  });

  onCleanup(() => cancelAnimationFrame(frame));

  const click = (e: MouseEvent) => {
    const rect = canvas.getBoundingClientRect();
    const size = city().city;
    const x = Math.round(((e.clientX - rect.left) / rect.width) * size);
    const y = Math.round(((e.clientY - rect.top) / rect.height) * size);
    props.onPick(Math.max(0, Math.min(size, x)), Math.max(0, Math.min(size, y)));
  };

  const layer = { position: 'absolute', inset: '0', width: '100%', height: '100%', display: 'block' } as const;

  return (
    <div
      style={{
        position: 'relative', width: '100%', 'max-width': `${SIZE}px`, 'aspect-ratio': '1',
        'border-radius': '12px', overflow: 'hidden',
      }}
    >
      <canvas ref={streets} style={layer} />
      <canvas ref={canvas} onClick={click} style={{ ...layer, cursor: 'crosshair' }} />
    </div>
  );
}

function pin(ctx: CanvasRenderingContext2D, x: number, y: number, color: string) {
  ctx.fillStyle = color;
  ctx.strokeStyle = '#ffffff';
  ctx.lineWidth = 3;
  ctx.beginPath();
  ctx.arc(x, y, 9, 0, Math.PI * 2);
  ctx.fill();
  ctx.stroke();
}
