/*
 * ux_port.h — USBX Zephyr OS Abstraction Layer
 * Xi640 Project 22
 *
 * Maps USBX/ThreadX OS primitives to Zephyr kernel APIs.
 * USBX seit Eclipse ThreadX 6.4.1 RTOS-unabhängig nutzbar (MIT).
 *
 * KRITISCHE REGELN:
 *   - Kein k_malloc im Hot Path
 *   - ctx in UX_THREAD eingebettet (kein separates Alloc)
 *   - k_thread_suspend/resume (NICHT k_sleep/k_wakeup)
 *   - UX_WAIT_FOREVER explizit auf K_FOREVER mappen
 *   - Mutexe NIE aus ISR-Kontext
 *   - DMA Buffer 32-Byte aligned
 */
#ifndef UX_PORT_H
#define UX_PORT_H

#include <zephyr/kernel.h>
#include <zephyr/sys/cache.h>

/* ---------------------------------------------------------- */
/* USBX Basis-Typen                                           */
/* ---------------------------------------------------------- */
typedef unsigned char   UCHAR;
typedef unsigned short  USHORT;
typedef unsigned int    UINT;
typedef unsigned long   ULONG;
typedef void            VOID;
typedef char            CHAR;

#define UX_NULL         NULL
#define UX_NO_WAIT      0UL
#define UX_WAIT_FOREVER 0xFFFFFFFFUL

/* ---------------------------------------------------------- */
/* Thread                                                     */
/* ---------------------------------------------------------- */
struct _ux_thread_start_ctx {
    VOID (*entry)(ULONG);
    ULONG parameter;
};

typedef struct _UX_THREAD_STRUCT {
    struct k_thread             thread;
    k_tid_t                     tid;
    void                        *stack;
    ULONG                       stack_size;
    UINT                        priority;
    struct _ux_thread_start_ctx ctx;   /* eingebettet — kein malloc! */
} UX_THREAD;

/* ---------------------------------------------------------- */
/* Semaphore                                                  */
/* ---------------------------------------------------------- */
typedef struct _UX_SEMAPHORE_STRUCT {
    struct k_sem sem;
} UX_SEMAPHORE;

/* ---------------------------------------------------------- */
/* Mutex                                                      */
/* ---------------------------------------------------------- */
typedef struct _UX_MUTEX_STRUCT {
    struct k_mutex mutex;
} UX_MUTEX;

/* ---------------------------------------------------------- */
/* Byte Pool (statischer Heap)                                */
/* ---------------------------------------------------------- */
typedef struct _UX_BYTE_POOL_STRUCT {
    struct k_heap heap;
} UX_BYTE_POOL;

/* ---------------------------------------------------------- */
/* API Deklarationen                                          */
/* ---------------------------------------------------------- */
UINT  ux_zephyr_thread_create(UX_THREAD *t, CHAR *name,
          VOID (*entry)(ULONG), ULONG param,
          VOID *stack, ULONG stack_size, UINT prio);
UINT  ux_zephyr_thread_resume(UX_THREAD *t);
UINT  ux_zephyr_thread_suspend(UX_THREAD *t);

UINT  ux_zephyr_semaphore_create(UX_SEMAPHORE *s, UINT initial);
UINT  ux_zephyr_semaphore_put(UX_SEMAPHORE *s);
UINT  ux_zephyr_semaphore_get(UX_SEMAPHORE *s, ULONG wait);

UINT  ux_zephyr_mutex_create(UX_MUTEX *m);
UINT  ux_zephyr_mutex_get(UX_MUTEX *m);
UINT  ux_zephyr_mutex_put(UX_MUTEX *m);

ULONG ux_zephyr_time_get(void);

UINT  ux_zephyr_byte_pool_create(UX_BYTE_POOL *p, VOID *mem, ULONG size);
VOID *ux_zephyr_byte_allocate(UX_BYTE_POOL *p, ULONG size);
UINT  ux_zephyr_byte_release(UX_BYTE_POOL *p, VOID *mem);

/* Cache Maintenance (32-Byte aligned, Cortex-M55) */
void  ux_cache_clean(void *addr, size_t size);
void  ux_cache_invalidate(void *addr, size_t size);

#endif /* UX_PORT_H */
