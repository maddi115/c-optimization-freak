// wasm.c - the device's copy of the todo list: the SAME todo.c, compiled to WebAssembly, plus
// the engine's client code. When the server says "2, item 5", this runs todo_apply right here.
#include "todo.c"

static Todos todos;
#define FK_STATE (&todos)
#define FK_STATE_BYTES ((int)sizeof(Todos))
static void fk_app_apply(void *s, int kind, int a, int b, int c) { todo_apply((Todos *)s, kind, a, b, c); }
static void fk_app_step(void *s) { todo_step((Todos *)s); }
#include "../../engine/wasm_glue.h"

// ---- todo-specific (the page reads the flags straight out of this memory)
EXPORT("todo_max")    int todo_max(void)    { return TODO_MAX; }
EXPORT("off_used")    int off_used(void)    { return (int)__builtin_offsetof(Todos, used); }
EXPORT("off_done")    int off_done(void)    { return (int)__builtin_offsetof(Todos, done); }
EXPORT("off_seq")     int off_seq(void)     { return (int)__builtin_offsetof(Todos, seq); }
EXPORT("todo_changes") int todo_changes(void) { return (int)todos.changes; }
