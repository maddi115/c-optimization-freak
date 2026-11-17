// GENERATED from a .def file by engine/tools/gen_protocol.c. Do not edit; run make.

export const W = 64;
export const H = 64;
export const COLORS = 8;

export type HelloMsg = {
  t: 'hello';
  w: number;
  h: number;
  colors: number;
};

export type BoardMsg = {
  t: 'board';
  painted: number;
  online: number;
};

export type ServerMsg = HelloMsg | BoardMsg;

// Builders for every browser -> server command.
export const cmd = {
  paint: (cell: number, color: number) => `p,${Math.round(cell)},${Math.round(color)}`,
};
