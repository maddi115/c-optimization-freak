// GENERATED from a .def file by engine/tools/gen_protocol.c. Do not edit; run make.

export const NO_RIDE = -1;
export const EV_TARGET = 1;
export const EV_RIDE = 2;

export type HelloMsg = {
  t: 'hello';
  cars: number;
  city: number;
  road: number;
};

export type RideMsg = {
  t: 'ride';
  state: 'none' | 'quoted' | 'arriving' | 'on_trip' | 'done';
  px: number;
  py: number;
  dx: number;
  dy: number;
  near: [number, number, number];
  nearEta: number;
  surge: number;
  price: number;
  km: number;
  tripMin: number;
  car: number;
  eta: number;
};

export type CityMsg = {
  t: 'city';
  free: number;
  cars: number;
};

export type ServerMsg = HelloMsg | RideMsg | CityMsg;

// Builders for every browser -> server command.
export const cmd = {
  quote: (px: number, py: number, dx: number, dy: number) => `q,${Math.round(px)},${Math.round(py)},${Math.round(dx)},${Math.round(dy)}`,
  book: () => `book`,
  cancel: () => `cancel`,
};
