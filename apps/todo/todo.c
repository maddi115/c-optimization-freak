// todo.c - the todo rules. The SAME file runs on the server (app.c) and on every phone and PC
// (wasm.c, compiled to WebAssembly). So the server never sends "here is your new list":
// it sends a tiny number that says what happened, and each device runs it here itself.
//
//   1 = ADD     item a now exists (its text arrives once, as a row)
//   2 = TOGGLE  item a: done <-> not done
//   3 = DELETE  item a is gone
//   4 = CLEAR   every done item is gone
#pragma once
#include <stdint.h>

#define TODO_MAX 256        // items
#define TEXT_BYTES 64       // one row of text per item: up to 63 letters + a 0 at the end

enum { EV_ADD = 1, EV_TOGGLE = 2, EV_DELETE = 3, EV_CLEAR = 4 };

// The part every device keeps an exact copy of (about 1.5 KB). No pointers, fixed size.
typedef struct {
    uint8_t used[TODO_MAX];     // 1 = this slot holds an item
    uint8_t done[TODO_MAX];     // 1 = ticked off
    uint32_t seq[TODO_MAX];     // when it was added, so the list keeps its order
    uint32_t next_seq;
    uint32_t changes;           // goes up on every real change: the page redraws only then
} Todos;

static void todo_apply(Todos *t, int kind, int a, int b, int c) {
    (void)b; (void)c;
    if (kind == EV_CLEAR) {
        for (int i = 0; i < TODO_MAX; i++) if (t->used[i] && t->done[i]) t->used[i] = t->done[i] = 0;
        t->changes++;
        return;
    }
    if (a < 0 || a >= TODO_MAX) return;
    if (kind == EV_ADD) { t->used[a] = 1; t->done[a] = 0; t->seq[a] = t->next_seq++; t->changes++; }
    else if (kind == EV_TOGGLE && t->used[a]) { t->done[a] ^= 1; t->changes++; }
    else if (kind == EV_DELETE && t->used[a]) { t->used[a] = t->done[a] = 0; t->changes++; }
}

static void todo_step(Todos *t) { (void)t; }   // nothing changes by itself: only people change todos
