// RidePanel.tsx - the card on the right: steps, price, ride status.
// It only shows numbers the C server already worked out.

import { Match, Show, Switch } from 'solid-js';
import { cityStats, link, problem, ride, send, stats } from './connection';
import { cmd } from './protocol';

const minutes = (citySeconds: number) => `${Math.max(0.1, citySeconds / 60).toFixed(1)} min`;

const card = {
  background: '#ffffff',
  border: '1px solid #e5e7eb',
  'border-radius': '12px',
  padding: '16px',
};

const primary = {
  width: '100%',
  padding: '12px',
  'border-radius': '10px',
  border: 'none',
  background: '#111827',
  color: '#ffffff',
  'font-size': '15px',
  'font-weight': '600',
  cursor: 'pointer',
};

const secondary = { ...primary, background: '#f3f4f6', color: '#111827', 'margin-top': '8px' };

export default function RidePanel(props: { hasPickup: boolean; onReset: () => void }) {
  return (
    <div style={{ display: 'flex', 'flex-direction': 'column', gap: '12px' }}>
      <div style={card}>
        <Switch>
          <Match when={!ride() && !props.hasPickup}>
            <h2 style={{ margin: '0 0 4px', 'font-size': '18px' }}>Where to?</h2>
            <p style={{ margin: 0, color: '#4b5563' }}>Click the map to set your pickup.</p>
          </Match>

          <Match when={!ride() && props.hasPickup}>
            <h2 style={{ margin: '0 0 4px', 'font-size': '18px' }}>Pickup set</h2>
            <p style={{ margin: 0, color: '#4b5563' }}>Now click where you want to go.</p>
          </Match>

          <Match when={ride()?.state === 'quoted' && ride()}>
            {(r) => (
              <>
                <div style={{ display: 'flex', 'justify-content': 'space-between', 'align-items': 'baseline' }}>
                  <h2 style={{ margin: 0, 'font-size': '18px' }}>Ride</h2>
                  <span style={{ 'font-size': '28px', 'font-weight': '700' }}>${r().price.toFixed(2)}</span>
                </div>
                <Show when={r().surge > 1}>
                  <span
                    style={{
                      display: 'inline-block',
                      'margin-top': '6px',
                      padding: '2px 8px',
                      'border-radius': '999px',
                      background: '#fef3c7',
                      color: '#92400e',
                      'font-size': '13px',
                      'font-weight': '600',
                    }}
                  >
                    {r().surge.toFixed(1)}x surge: few free cars nearby
                  </span>
                </Show>
                <table style={{ width: '100%', margin: '12px 0', 'font-size': '14px', color: '#374151' }}>
                  <tbody>
                    <tr><td>Pickup in</td><td style={{ 'text-align': 'right' }}>{minutes(r().nearEta)}</td></tr>
                    <tr><td>Trip</td><td style={{ 'text-align': 'right' }}>{r().km.toFixed(1)} km · {r().tripMin.toFixed(1)} min</td></tr>
                    <tr><td>Nearest drivers</td><td style={{ 'text-align': 'right' }}>#{r().near.filter((n) => n >= 0).join(', #')}</td></tr>
                  </tbody>
                </table>
                <button style={primary} onClick={() => send(cmd.book())}>Request ride</button>
                <button style={secondary} onClick={props.onReset}>Change route</button>
              </>
            )}
          </Match>

          <Match when={ride()?.state === 'arriving' && ride()}>
            {(r) => (
              <>
                <h2 style={{ margin: '0 0 4px', 'font-size': '18px' }}>Driver #{r().car} is on the way</h2>
                <p style={{ margin: '0 0 12px', color: '#4b5563' }}>Arriving in {minutes(r().eta)}. The green dot is your car.</p>
                <button style={secondary} onClick={props.onReset}>Cancel ride</button>
              </>
            )}
          </Match>

          <Match when={ride()?.state === 'on_trip' && ride()}>
            {(r) => (
              <>
                <h2 style={{ margin: '0 0 4px', 'font-size': '18px' }}>On your trip</h2>
                <p style={{ margin: 0, color: '#4b5563' }}>{minutes(r().eta)} to your dropoff.</p>
              </>
            )}
          </Match>

          <Match when={ride()?.state === 'done' && ride()}>
            {(r) => (
              <>
                <h2 style={{ margin: '0 0 4px', 'font-size': '18px' }}>You have arrived</h2>
                <p style={{ margin: '0 0 12px', color: '#4b5563' }}>Total ${r().price.toFixed(2)}. Thanks for riding.</p>
                <button style={primary} onClick={props.onReset}>New ride</button>
              </>
            )}
          </Match>
        </Switch>
      </div>

      <Show when={problem()}>
        <div style={{ ...card, background: '#fef2f2', 'border-color': '#fecaca', color: '#991b1b', 'font-size': '14px' }}>
          {problem()}
        </div>
      </Show>

      <Show when={stats()}>
        {(s) => (
          <div style={{ ...card, 'font-size': '13px', color: '#4b5563', 'line-height': '1.6' }}>
            <strong style={{ color: '#111827' }}>C engine</strong>
            <div>{(cityStats()?.cars ?? 0).toLocaleString()} cars · {((cityStats()?.cars ?? 0) - (cityStats()?.free ?? 0)).toLocaleString()} on rides</div>
            <div>{(s().simUs / 1000).toFixed(3)} ms of work per tick</div>
            <div>{s().online} {s().online === 1 ? 'person' : 'people'} connected</div>
            <strong style={{ color: '#111827', display: 'block', 'margin-top': '8px' }}>Lockstep tunnel</strong>
            <div>Your browser runs the same C code as the server (app.wasm)</div>
            <div>City: {(s().snapshotBytes / 1024).toFixed(1)} KB, sent once, packed by our own C packer</div>
            <div>Car updates: {s().bytesPerTick.toFixed(1)} bytes per tick</div>
            <div>Receiving now: {link().bytesPerSec.toLocaleString()} bytes/s</div>
            <div>Checks matched: {link().matches} · chunk heals: {link().chunkHeals} · full copies: {link().fullCopies}</div>
          </div>
        )}
      </Show>
    </div>
  );
}
