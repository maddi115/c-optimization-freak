// App.tsx - the todo page. SolidJS only draws. The list itself lives in C (app.wasm):
// the server sends a tiny numbered action (1 add, 2 tick, 3 delete, 4 clear) and the
// device's own copy of todo.c runs it. Text arrives once per item, as a row.

import { createSignal, For, Show, onMount } from 'solid-js';
import { PORT } from './engine/engine_proto';
import { Tunnel } from './engine/tunnel';
import { cmd, type ListMsg } from './protocol';

type Item = { id: number; text: string; done: boolean; seq: number };
const TEXT_BYTES = 64;

export default function App() {
  const [connected, setConnected] = createSignal(false);
  const [items, setItems] = createSignal<Item[]>([]);
  const [list, setList] = createSignal<ListMsg | null>(null);
  const [draft, setDraft] = createSignal('');
  const [bytes, setBytes] = createSignal(0);
  const decoder = new TextDecoder();
  let seen = -1;

  const tunnel = new Tunnel({
    port: PORT,
    events: {
      onConnected: (on) => { setConnected(on); seen = -1; },
      onState: () => { if (changes() !== seen) read(); },   // ticks with no change cost nothing
      onRows: () => read(),
      onJson: (m) => { if (m.t === 'list') setList(m as ListMsg); },
    },
  });

  const changes = () => (tunnel.wasm ? (tunnel.wasm.todo_changes as () => number)() : 0);

  // Read the list straight out of app.wasm memory (views are made fresh: memory can grow).
  function read() {
    const w = tunnel.wasm;
    if (!w || !w.fk_synced()) return;
    seen = changes();
    const max = (w.todo_max as () => number)(), base = w.fk_state_ptr(), buf = w.memory.buffer;
    const used = new Uint8Array(buf, base + (w.off_used as () => number)(), max);
    const done = new Uint8Array(buf, base + (w.off_done as () => number)(), max);
    const seq = new Uint32Array(buf, base + (w.off_seq as () => number)(), max);
    const rows = tunnel.rows, out: Item[] = [];
    for (let id = 0; id < max; id++) {
      if (!used[id]) continue;
      const row = rows.subarray(id * TEXT_BYTES, id * TEXT_BYTES + TEXT_BYTES);
      const end = row.indexOf(0);
      out.push({ id, text: decoder.decode(row.subarray(0, end < 0 ? TEXT_BYTES : end)), done: !!done[id], seq: seq[id] });
    }
    out.sort((a, b) => a.seq - b.seq);
    setItems(out);
  }

  function add(e: Event) {
    e.preventDefault();
    const text = draft().trim();
    if (!text) return;
    tunnel.send('a,' + text);
    setDraft('');
  }

  onMount(() => {
    tunnel.start();
    setInterval(() => setBytes(tunnel.bytesPerSec), 1000);
  });

  const left = () => items().filter((i) => !i.done).length;

  return (
    <main style={{ 'font-family': 'system-ui, sans-serif', 'max-width': '520px', margin: '32px auto', padding: '0 16px', color: '#111827' }}>
      <h1 style={{ 'font-size': '24px', margin: '0 0 4px' }}>Todo</h1>
      <p style={{ margin: '0 0 12px', color: '#4b5563', 'font-size': '14px' }}>
        Shared list on the C engine. Open two tabs. Ticking or deleting sends one small number;
        your device runs the change itself.
      </p>
      <div style={{ display: 'flex', gap: '12px', 'align-items': 'center', 'margin-bottom': '16px', 'font-size': '14px' }}>
        <span style={{ padding: '4px 10px', 'border-radius': '999px', background: connected() ? '#dcfce7' : '#fee2e2', color: connected() ? '#166534' : '#991b1b' }}>
          {connected() ? 'Connected to C engine' : 'Connecting…'}
        </span>
        <span style={{ color: '#4b5563' }}>{list()?.online ?? 0} online · {bytes()} bytes/s</span>
      </div>

      <form onSubmit={add} style={{ display: 'flex', gap: '8px', 'margin-bottom': '16px' }}>
        <input value={draft()} onInput={(e) => setDraft(e.currentTarget.value)} maxLength={63}
          placeholder="What needs doing?" aria-label="New todo"
          style={{ flex: '1', padding: '10px 12px', 'font-size': '16px', border: '1px solid #d1d5db', 'border-radius': '8px' }} />
        <button type="submit"
          style={{ padding: '10px 16px', 'font-size': '16px', border: '0', 'border-radius': '8px', background: '#111827', color: '#fff', cursor: 'pointer' }}>
          Add
        </button>
      </form>

      <ul style={{ 'list-style': 'none', padding: '0', margin: '0', border: '1px solid #e5e7eb', 'border-radius': '8px' }}>
        <For each={items()} fallback={<li style={{ padding: '16px', color: '#6b7280' }}>Nothing to do. Add something above.</li>}>
          {(item) => (
            <li style={{ display: 'flex', 'align-items': 'center', gap: '12px', padding: '10px 12px', 'border-bottom': '1px solid #f3f4f6' }}>
              <input type="checkbox" checked={item.done} aria-label={`Done: ${item.text}`}
                onChange={() => tunnel.send(cmd.toggle(item.id))} style={{ width: '18px', height: '18px', cursor: 'pointer' }} />
              <span style={{ flex: '1', 'font-size': '16px', 'text-decoration': item.done ? 'line-through' : 'none', color: item.done ? '#9ca3af' : '#111827' }}>
                {item.text}
              </span>
              <button aria-label={`Delete: ${item.text}`} onClick={() => tunnel.send(cmd.remove(item.id))}
                style={{ border: '0', background: 'none', color: '#9ca3af', 'font-size': '18px', cursor: 'pointer' }}>×</button>
            </li>
          )}
        </For>
      </ul>

      <Show when={items().length}>
        <div style={{ display: 'flex', 'justify-content': 'space-between', 'margin-top': '12px', 'font-size': '14px', color: '#4b5563' }}>
          <span>{left()} left</span>
          <button onClick={() => tunnel.send(cmd.clear())}
            style={{ border: '1px solid #d1d5db', background: '#fff', 'border-radius': '8px', padding: '4px 10px', cursor: 'pointer' }}>
            Clear done
          </button>
        </div>
      </Show>
    </main>
  );
}
