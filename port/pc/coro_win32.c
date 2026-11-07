#include "../core/coro.h"
#include <windows.h>
#include <stdlib.h>

struct Coro { void *fiber; void (*fn)(void *); void *arg; };

static Coro g_main_coro;

static VOID CALLBACK fiber_entry(LPVOID p) {
    Coro *c = (Coro *)p;
    c->fn(c->arg);
    /* a coroutine must never return: there is nowhere to go */
    abort();
}

Coro *coro_init_main(void) {
    if (!g_main_coro.fiber) g_main_coro.fiber = ConvertThreadToFiber(NULL);
    return &g_main_coro;
}

Coro *coro_create(void (*fn)(void *), void *arg, size_t stack_size) {
    Coro *c = calloc(1, sizeof *c);
    c->fn = fn;
    c->arg = arg;
    c->fiber = CreateFiber(stack_size, fiber_entry, c);
    return c->fiber ? c : NULL;
}

void coro_switch(Coro *to) { SwitchToFiber(to->fiber); }
