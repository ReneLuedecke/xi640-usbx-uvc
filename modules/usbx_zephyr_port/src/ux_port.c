/*
 * ux_port.c — USBX Zephyr OS Abstraction Layer
 * Xi640 Project 22
 *
 * Korrekturen ggü. naivem Port:
 *   [1] Kein k_malloc — ctx in UX_THREAD struct eingebettet
 *   [2] k_thread_suspend(tid) statt k_sleep(K_FOREVER)
 *   [3] k_thread_resume(tid) statt k_wakeup(tid)
 *   [4] UX_WAIT_FOREVER → K_FOREVER (nicht K_MSEC(0xFFFFFFFF))
 *   [5] Cache: 32-Byte Alignment erzwungen
 */
#include "ux_port.h"
#include <zephyr/sys/util.h>

/* ---------------------------------------------------------- */
/* Thread                                                     */
/* ---------------------------------------------------------- */
static void zephyr_thread_entry(void *p1, void *p2, void *p3)
{
    UX_THREAD *t = (UX_THREAD *)p1;
    t->ctx.entry(t->ctx.parameter);
    (void)p2; (void)p3;
}

UINT ux_zephyr_thread_create(
    UX_THREAD *t, CHAR *name,
    VOID (*entry)(ULONG), ULONG param,
    VOID *stack, ULONG stack_size, UINT prio)
{
    /* [1] ctx eingebettet — kein k_malloc */
    t->ctx.entry     = entry;
    t->ctx.parameter = param;
    t->stack         = stack;
    t->stack_size    = stack_size;
    t->priority      = prio;

    t->tid = k_thread_create(
        &t->thread, stack, stack_size,
        zephyr_thread_entry, t, NULL, NULL,
        (int)prio, 0, K_NO_WAIT);

    if (name) {
        k_thread_name_set(t->tid, name);
    }

    /* Thread direkt suspendieren — USBX resumed wenn noetig */
    k_thread_suspend(t->tid);

    return 0;
}

/* [3] k_thread_resume — nicht k_wakeup */
UINT ux_zephyr_thread_resume(UX_THREAD *t)
{
    k_thread_resume(t->tid);
    return 0;
}

/* [2] k_thread_suspend — nicht k_sleep */
UINT ux_zephyr_thread_suspend(UX_THREAD *t)
{
    k_thread_suspend(t->tid);
    return 0;
}

/* ---------------------------------------------------------- */
/* Semaphore                                                  */
/* ---------------------------------------------------------- */
UINT ux_zephyr_semaphore_create(UX_SEMAPHORE *s, UINT initial)
{
    k_sem_init(&s->sem, initial, UINT32_MAX);
    return 0;
}

UINT ux_zephyr_semaphore_put(UX_SEMAPHORE *s)
{
    k_sem_give(&s->sem);  /* ISR-safe */
    return 0;
}

/* [4] UX_WAIT_FOREVER → K_FOREVER */
UINT ux_zephyr_semaphore_get(UX_SEMAPHORE *s, ULONG wait)
{
    if (wait == UX_NO_WAIT)
        return (UINT)k_sem_take(&s->sem, K_NO_WAIT);
    if (wait == UX_WAIT_FOREVER)
        return (UINT)k_sem_take(&s->sem, K_FOREVER);
    return (UINT)k_sem_take(&s->sem, K_MSEC(wait));
}

/* ---------------------------------------------------------- */
/* Mutex (NIEMALS aus ISR aufrufen!)                          */
/* ---------------------------------------------------------- */
UINT ux_zephyr_mutex_create(UX_MUTEX *m)
{
    k_mutex_init(&m->mutex);
    return 0;
}

UINT ux_zephyr_mutex_get(UX_MUTEX *m)
{
    k_mutex_lock(&m->mutex, K_FOREVER);
    return 0;
}

UINT ux_zephyr_mutex_put(UX_MUTEX *m)
{
    k_mutex_unlock(&m->mutex);
    return 0;
}

/* ---------------------------------------------------------- */
/* Zeit                                                       */
/* ---------------------------------------------------------- */
ULONG ux_zephyr_time_get(void)
{
    return (ULONG)k_uptime_get_32();  /* 1ms Aufloesung */
}

/* ---------------------------------------------------------- */
/* Byte Pool (statischer Heap)                                */
/* ---------------------------------------------------------- */
UINT ux_zephyr_byte_pool_create(UX_BYTE_POOL *p, VOID *mem, ULONG size)
{
    k_heap_init(&p->heap, mem, (size_t)size);
    return 0;
}

VOID *ux_zephyr_byte_allocate(UX_BYTE_POOL *p, ULONG size)
{
    return k_heap_alloc(&p->heap, (size_t)size, K_NO_WAIT);
}

UINT ux_zephyr_byte_release(UX_BYTE_POOL *p, VOID *mem)
{
    k_heap_free(&p->heap, mem);
    return 0;
}

/* ---------------------------------------------------------- */
/* Cache Maintenance — [5] 32-Byte Alignment erzwungen        */
/* ---------------------------------------------------------- */
#define CACHE_LINE 32U

static inline void cache_aligned(void *addr, size_t size,
                                  void *out_addr, size_t *out_size)
{
    uintptr_t a = (uintptr_t)addr & ~(CACHE_LINE - 1U);
    *out_size    = ROUND_UP(size + ((uintptr_t)addr - a), CACHE_LINE);
    *(uintptr_t *)out_addr = a;
}

/* TX: vor DMA senden — cache clean */
void ux_cache_clean(void *addr, size_t size)
{
    uintptr_t a; size_t s;
    cache_aligned(addr, size, (void *)&a, &s);
    sys_cache_data_flush_range((void *)a, s);
}

/* RX: nach DMA empfangen — cache invalidate */
void ux_cache_invalidate(void *addr, size_t size)
{
    uintptr_t a; size_t s;
    cache_aligned(addr, size, (void *)&a, &s);
    sys_cache_data_invd_range((void *)a, s);
}
