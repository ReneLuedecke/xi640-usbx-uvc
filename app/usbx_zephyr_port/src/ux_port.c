/**************************************************************************/
/*                                                                        */
/*  Xi640 — USBX Zephyr Port Layer Implementation                         */
/*  Target: STM32N6570 / Cortex-M55                                       */
/*                                                                        */
/*  Implementiert alle TX_* Primitiven auf Zephyr k_* Aufrufen.          */
/*  6 kritische Fixes integriert (siehe docs/usb_dma_cache_rules.md)     */
/*                                                                        */
/**************************************************************************/

#include "ux_port.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ux_port, LOG_LEVEL_DBG);

/* -----------------------------------------------------------------------
 * TX_THREAD — Thread Create / Delete / Suspend / Resume
 *
 * FIX 1: stack wird NICHT per k_malloc allokiert — muss vom Aufrufer
 *        als statischer Puffer übergeben werden.
 * FIX 2: k_sleep(K_FOREVER) suspendiert den CALLER — falsch!
 *        Stattdessen: k_thread_suspend(tid)
 * FIX 3: k_wakeup() bricht nur k_sleep ab — falsch für suspend!
 *        Stattdessen: k_thread_resume(tid)
 * -------------------------------------------------------------------- */

UINT tx_thread_create(TX_THREAD *thread_ptr,
                      CHAR      *name_ptr,
                      void     (*entry_function)(ULONG),
                      ULONG      entry_input,
                      VOID      *stack_start,
                      ULONG      stack_size,
                      UINT       priority,
                      UINT       preempt_threshold,
                      ULONG      time_slice,
                      UINT       auto_start)
{
    (void)preempt_threshold;
    (void)time_slice;

    if (!thread_ptr || !entry_function || !stack_start || stack_size == 0U) {
        return TX_PTR_ERROR;
    }

    /* Zephyr-Priorität: niedrigere Zahl = höhere Priorität
     * USBX verwendet 0–31, Zephyr Cooperative: 0–(CONFIG_NUM_COOP_PRIORITIES-1)
     * Mapping: USBX prio 2 → Zephyr prio 2 (cooperative) */
    int zephyr_prio = (int)priority;

    thread_ptr->stack      = (k_thread_stack_t *)stack_start;
    thread_ptr->stack_size = (size_t)stack_size;

    if (name_ptr) {
        strncpy(thread_ptr->name, name_ptr, sizeof(thread_ptr->name) - 1U);
        thread_ptr->name[sizeof(thread_ptr->name) - 1U] = '\0';
    }

    k_thread_create(&thread_ptr->thread,
                    thread_ptr->stack,
                    thread_ptr->stack_size,
                    (k_thread_entry_t)entry_function,
                    (void *)(uintptr_t)entry_input, NULL, NULL,
                    zephyr_prio,
                    0,
                    (auto_start == TX_AUTO_START) ? K_NO_WAIT : K_FOREVER);

    k_thread_name_set(&thread_ptr->thread, thread_ptr->name);

    LOG_DBG("tx_thread_create: %s prio=%d stack=%u",
            thread_ptr->name, zephyr_prio, (unsigned)stack_size);

    return TX_SUCCESS;
}

UINT tx_thread_delete(TX_THREAD *thread_ptr)
{
    if (!thread_ptr) {
        return TX_PTR_ERROR;
    }
    k_thread_abort(&thread_ptr->thread);
    return TX_SUCCESS;
}

/* FIX 2+3: suspend/resume korrekt */
UINT tx_thread_suspend(TX_THREAD *thread_ptr)
{
    if (!thread_ptr) {
        return TX_PTR_ERROR;
    }
    k_thread_suspend(&thread_ptr->thread);
    return TX_SUCCESS;
}

UINT tx_thread_resume(TX_THREAD *thread_ptr)
{
    if (!thread_ptr) {
        return TX_PTR_ERROR;
    }
    k_thread_resume(&thread_ptr->thread);
    return TX_SUCCESS;
}

UINT tx_thread_sleep(ULONG timer_ticks)
{
    /* FIX 4: UX_WAIT_FOREVER korrekt mappen */
    if (timer_ticks == TX_WAIT_FOREVER) {
        k_thread_suspend(k_current_get());
    } else {
        k_sleep(K_TICKS((k_ticks_t)timer_ticks));
    }
    return TX_SUCCESS;
}

/* -----------------------------------------------------------------------
 * TX_MUTEX
 *
 * FIX 6: Mutexe sind NICHT ISR-safe — niemals aus ISR aufrufen!
 *        Für ISR-Kontext: tx_semaphore verwenden.
 * -------------------------------------------------------------------- */

UINT tx_mutex_create(TX_MUTEX *mutex_ptr, CHAR *name_ptr, UINT inherit)
{
    (void)name_ptr;
    (void)inherit;

    if (!mutex_ptr) {
        return TX_PTR_ERROR;
    }
    k_mutex_init(mutex_ptr);
    return TX_SUCCESS;
}

UINT tx_mutex_delete(TX_MUTEX *mutex_ptr)
{
    (void)mutex_ptr;
    return TX_SUCCESS;
}

UINT tx_mutex_get(TX_MUTEX *mutex_ptr, ULONG wait_option)
{
    if (!mutex_ptr) {
        return TX_PTR_ERROR;
    }

    k_timeout_t timeout;
    if (wait_option == TX_WAIT_FOREVER) {
        timeout = K_FOREVER;
    } else if (wait_option == TX_NO_WAIT) {
        timeout = K_NO_WAIT;
    } else {
        timeout = K_TICKS((k_ticks_t)wait_option);
    }

    int ret = k_mutex_lock(mutex_ptr, timeout);
    return (ret == 0) ? TX_SUCCESS : TX_NOT_AVAILABLE;
}

UINT tx_mutex_put(TX_MUTEX *mutex_ptr)
{
    if (!mutex_ptr) {
        return TX_PTR_ERROR;
    }
    k_mutex_unlock(mutex_ptr);
    return TX_SUCCESS;
}

/* -----------------------------------------------------------------------
 * TX_SEMAPHORE — ISR-safe
 * -------------------------------------------------------------------- */

UINT tx_semaphore_create(TX_SEMAPHORE *semaphore_ptr,
                         CHAR         *name_ptr,
                         ULONG         initial_count)
{
    (void)name_ptr;

    if (!semaphore_ptr) {
        return TX_PTR_ERROR;
    }
    k_sem_init(semaphore_ptr, (unsigned int)initial_count, K_SEM_MAX_LIMIT);
    return TX_SUCCESS;
}

UINT tx_semaphore_delete(TX_SEMAPHORE *semaphore_ptr)
{
    (void)semaphore_ptr;
    return TX_SUCCESS;
}

UINT tx_semaphore_get(TX_SEMAPHORE *semaphore_ptr, ULONG wait_option)
{
    if (!semaphore_ptr) {
        return TX_PTR_ERROR;
    }

    k_timeout_t timeout;
    if (wait_option == TX_WAIT_FOREVER) {
        timeout = K_FOREVER;
    } else if (wait_option == TX_NO_WAIT) {
        timeout = K_NO_WAIT;
    } else {
        timeout = K_TICKS((k_ticks_t)wait_option);
    }

    int ret = k_sem_take(semaphore_ptr, timeout);
    return (ret == 0) ? TX_SUCCESS : TX_NO_INSTANCE;
}

UINT tx_semaphore_put(TX_SEMAPHORE *semaphore_ptr)
{
    if (!semaphore_ptr) {
        return TX_PTR_ERROR;
    }
    k_sem_give(semaphore_ptr);
    return TX_SUCCESS;
}

/* ISR-safe put (identisch, k_sem_give ist ISR-safe) */
UINT tx_semaphore_put_notify(TX_SEMAPHORE *semaphore_ptr,
                             VOID (*semaphore_put_notify)(TX_SEMAPHORE *))
{
    (void)semaphore_put_notify;
    return tx_semaphore_put(semaphore_ptr);
}

/* -----------------------------------------------------------------------
 * TX_EVENT_FLAGS_GROUP → k_event
 * -------------------------------------------------------------------- */

UINT tx_event_flags_create(TX_EVENT_FLAGS_GROUP *group_ptr, CHAR *name_ptr)
{
    (void)name_ptr;

    if (!group_ptr) {
        return TX_PTR_ERROR;
    }
    k_event_init(group_ptr);
    return TX_SUCCESS;
}

UINT tx_event_flags_delete(TX_EVENT_FLAGS_GROUP *group_ptr)
{
    (void)group_ptr;
    return TX_SUCCESS;
}

UINT tx_event_flags_get(TX_EVENT_FLAGS_GROUP *group_ptr,
                        ULONG                 requested_flags,
                        UINT                  get_option,
                        ULONG                *actual_flags_ptr,
                        ULONG                 wait_option)
{
    if (!group_ptr || !actual_flags_ptr) {
        return TX_PTR_ERROR;
    }

    k_timeout_t timeout;
    if (wait_option == TX_WAIT_FOREVER) {
        timeout = K_FOREVER;
    } else if (wait_option == TX_NO_WAIT) {
        timeout = K_NO_WAIT;
    } else {
        timeout = K_TICKS((k_ticks_t)wait_option);
    }

    bool reset = (get_option == TX_AND_CLEAR) || (get_option == TX_OR_CLEAR);
    bool all   = (get_option == TX_AND) || (get_option == TX_AND_CLEAR);

    uint32_t events;
    if (all) {
        events = k_event_wait_all(group_ptr,
                                  (uint32_t)requested_flags,
                                  reset, timeout);
    } else {
        events = k_event_wait(group_ptr,
                              (uint32_t)requested_flags,
                              reset, timeout);
    }

    *actual_flags_ptr = (ULONG)events;
    return (events != 0U) ? TX_SUCCESS : TX_NO_INSTANCE;
}

UINT tx_event_flags_set(TX_EVENT_FLAGS_GROUP *group_ptr,
                        ULONG                 flags_to_set,
                        UINT                  set_option)
{
    if (!group_ptr) {
        return TX_PTR_ERROR;
    }

    if (set_option == TX_OR || set_option == TX_OR_CLEAR) {
        k_event_post(group_ptr, (uint32_t)flags_to_set);
    } else {
        k_event_set(group_ptr, (uint32_t)flags_to_set);
    }
    return TX_SUCCESS;
}

/* -----------------------------------------------------------------------
 * TX_TIMER → k_timer (vereinfacht)
 * -------------------------------------------------------------------- */

UINT tx_timer_create(TX_TIMER *timer_ptr,
                     CHAR     *name_ptr,
                     VOID    (*expiration_function)(ULONG),
                     ULONG     expiration_input,
                     ULONG     initial_ticks,
                     ULONG     reschedule_ticks,
                     UINT      auto_activate)
{
    (void)name_ptr;
    (void)expiration_function;
    (void)expiration_input;
    (void)initial_ticks;
    (void)reschedule_ticks;
    (void)auto_activate;

    if (!timer_ptr) {
        return TX_PTR_ERROR;
    }
    /* TODO Phase 4: Timer-Callbacks für USB SOF Timing implementieren */
    return TX_SUCCESS;
}

UINT tx_timer_delete(TX_TIMER *timer_ptr)
{
    if (!timer_ptr) {
        return TX_PTR_ERROR;
    }
    k_timer_stop(timer_ptr);
    return TX_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Hilfsfunktionen
 * -------------------------------------------------------------------- */

ULONG tx_time_get(void)
{
    return (ULONG)k_uptime_ticks();
}

VOID tx_thread_relinquish(void)
{
    k_yield();
}

/* -----------------------------------------------------------------------
 * _ux_utility_interrupt_disable / _ux_utility_interrupt_restore
 *
 * USBX ruft diese als externe C-Funktionen auf wenn TX_API_H nicht
 * definiert ist (d.h. kein ThreadX, nur Zephyr Port).
 * Mapping auf Zephyr irq_lock / irq_unlock (ISR-safe).
 * -------------------------------------------------------------------- */

ALIGN_TYPE _ux_utility_interrupt_disable(VOID)
{
    return (ALIGN_TYPE)irq_lock();
}

VOID _ux_utility_interrupt_restore(ALIGN_TYPE flags)
{
    irq_unlock((unsigned int)flags);
}
