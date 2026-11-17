// App.tsx - page layout. Keeps the two clicks (pickup, dropoff) and asks the C server for a quote.

import { createSignal, onMount } from 'solid-js';
import MapCanvas from './MapCanvas';
import RidePanel from './RidePanel';
import { city, connect, connected, ride, send } from './connection';
import { cmd } from './protocol';

type Point = { x: number; y: number } | null;

export default function App() {
  const [pickup, setPickup] = createSignal<Point>(null);
  const [dropoff, setDropoff] = createSignal<Point>(null);

  onMount(connect);

  const snap = (v: number) => Math.round(v / city().road) * city().road; // show the pin on a street right away

  const onPick = (x: number, y: number) => {
    const state = ride()?.state;
    if (state === 'arriving' || state === 'on_trip') return; // no changes mid-ride
    if (!pickup() || dropoff()) {
      if (state) send(cmd.cancel());
      setPickup({ x: snap(x), y: snap(y) });
      setDropoff(null);
    } else {
      const p = pickup()!;
      setDropoff({ x: snap(x), y: snap(y) });
      send(cmd.quote(p.x, p.y, x, y));
    }
  };

  const reset = () => {
    send(cmd.cancel());
    setPickup(null);
    setDropoff(null);
  };

  return (
    <main
      style={{
        'font-family': 'system-ui, sans-serif',
        color: '#111827',
        background: '#f9fafb',
        'min-height': '100vh',
        padding: '24px',
        'box-sizing': 'border-box',
      }}
    >
      <div style={{ 'max-width': '1000px', margin: '0 auto' }}>
        <div style={{ display: 'flex', 'align-items': 'center', gap: '12px', 'margin-bottom': '16px' }}>
          <h1 style={{ margin: 0, 'font-size': '24px' }}>Ride Mock</h1>
          <span
            style={{
              padding: '4px 10px',
              'border-radius': '999px',
              'font-size': '13px',
              background: connected() ? '#dcfce7' : '#fee2e2',
              color: connected() ? '#166534' : '#991b1b',
            }}
          >
            {connected() ? 'Connected to C server' : 'Connecting…'}
          </span>
        </div>
        <div style={{ display: 'flex', gap: '20px', 'flex-wrap': 'wrap', 'align-items': 'flex-start' }}>
          <div style={{ flex: '1 1 480px', 'max-width': '640px' }}>
            <MapCanvas pickup={pickup()} dropoff={dropoff()} onPick={onPick} />
          </div>
          <div style={{ flex: '1 1 260px', 'max-width': '320px' }}>
            <RidePanel hasPickup={!!pickup()} onReset={reset} />
          </div>
        </div>
        <p style={{ 'font-size': '13px', color: '#6b7280' }}>
          Grey dots are free cars, orange are busy. Downtown has more cars, so the edges cost more. The city runs 20x faster
          than real time.
        </p>
      </div>
    </main>
  );
}
