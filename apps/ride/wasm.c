// wasm.c - the browser's copy of the ride city: the SAME sim.c, compiled to WebAssembly,
// plus the engine's client code (wasm_glue.h), so app.wasm does all the work in C.
#include "sim.c"

static City city;
#define FK_STATE (&city)
#define FK_STATE_BYTES SIM_STATE_BYTES
static void fk_app_apply(void *s, int kind, int a, int b, int c) { sim_apply((City *)s, kind, a, b, c); }
static void fk_app_step(void *s) { sim_step((City *)s); }
#include "../../engine/wasm_glue.h"

// ---- ride-specific (apps/ride/ui reads the car arrays straight out of this memory)
EXPORT("num_cars") int num_cars(void) { return NUM_CARS; }
EXPORT("off_x")    int off_x(void)    { return (int)__builtin_offsetof(City, x); }
EXPORT("off_y")    int off_y(void)    { return (int)__builtin_offsetof(City, y); }
EXPORT("off_dx")   int off_dx(void)   { return (int)__builtin_offsetof(City, dx); }
EXPORT("off_dy")   int off_dy(void)   { return (int)__builtin_offsetof(City, dy); }
EXPORT("off_ride") int off_ride(void) { return (int)__builtin_offsetof(City, ride); }
