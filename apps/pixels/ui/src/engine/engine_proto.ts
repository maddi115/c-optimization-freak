// GENERATED from a .def file by engine/tools/gen_protocol.c. Do not edit; run make.

export const PORT = 8080;
export const TICK_MS = 50;
export const CHUNK_BYTES = 1024;
export const MSG_SNAPSHOT = 2;
export const MSG_TICK = 3;
export const MSG_ROWS = 4;
export const MSG_CHUNKS = 5;
export const MSG_PATCH = 6;
export const SNAPSHOT_HEADER = 12;
export const TICK_HEADER = 8;
export const ROWS_HEADER = 8;
export const PATCH_HEADER = 12;
export const EVENT_BYTES = 8;
export const FLAG_CHECKSUM = 1;
export const FLAG_ZIP = 2;
export const FLAG_FULL = 4;

export type EngineMsg = {
  t: 'engine';
  lockstepBytes: number;
  rows: number;
  rowSize: number;
  tickMs: number;
};

export type StatsMsg = {
  t: 'stats';
  simUs: number;
  bytesPerTick: number;
  snapshotBytes: number;
  online: number;
  resyncs: number;
  heals: number;
};

export type ServerMsg = EngineMsg | StatsMsg;

// Builders for every browser -> server command.
export const cmd = {
  join: (zip: number) => `join,${Math.round(zip)}`,
  sync: () => `sync`,
  sub: (first: number, count: number) => `sub,${Math.round(first)},${Math.round(count)}`,
  fix: (tick: number) => `fix,${Math.round(tick)}`,
  patch: (first: number, mask: number) => `patch,${Math.round(first)},${Math.round(mask)}`,
};
