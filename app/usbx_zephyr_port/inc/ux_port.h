/**************************************************************************/
/*                                                                        */
/*  Xi640 — USBX Zephyr Port Layer                                        */
/*  Target: STM32N6570 / Cortex-M55                                       */
/*  Based on: stm32-mw-usbx cortex_m7 port (v6.4.0)                      */
/*                                                                        */
/*  Maps USBX/ThreadX primitives to Zephyr RTOS equivalents.             */
/*  ThreadX kernel is NOT used — Zephyr owns all scheduling.             */
/*                                                                        */
/**************************************************************************/

#ifndef UX_PORT_H
#define UX_PORT_H

/* -----------------------------------------------------------------------
 * 1. Standard includes
 * -------------------------------------------------------------------- */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

/* -----------------------------------------------------------------------
 * 2. UX_STANDALONE mode — kein ThreadX Kernel
 *    USBX benutzt nur Typen aus tx_api.h, nicht den TX-Scheduler.
 * -------------------------------------------------------------------- */
#define UX_STANDALONE

/* Basis-Typen (ersetzt tx_api.h Typen) */
#ifndef VOID
#define VOID                void
typedef char                CHAR;
typedef unsigned char       UCHAR;
typedef int                 INT;
typedef unsigned int        UINT;
typedef long                LONG;
typedef unsigned long       ULONG;
typedef short               SHORT;
typedef unsigned short      USHORT;
#endif

#ifndef ULONG64_DEFINED
typedef uint64_t            ULONG64;
#define ULONG64_DEFINED
#endif

#ifndef ALIGN_TYPE_DEFINED
#define ALIGN_TYPE          ULONG
#define ALIGN_TYPE_DEFINED
#endif

#ifndef SLONG_DEFINED
typedef LONG                SLONG;
#define SLONG_DEFINED
#endif

/* -----------------------------------------------------------------------
 * 3. ThreadX Objekt-Typen → Zephyr Primitiven
 *    USBX referenziert TX_THREAD, TX_MUTEX, TX_SEMAPHORE, TX_EVENT_FLAGS_GROUP
 *    Wir mappen diese auf Zephyr-Strukturen.
 * -------------------------------------------------------------------- */

/* TX_THREAD → k_thread (eingebettet, kein k_malloc) */
typedef struct UX_TX_THREAD_STRUCT {
    struct k_thread     thread;
    k_thread_stack_t   *stack;
    size_t              stack_size;
    char                name[32];
} TX_THREAD;

/* TX_MUTEX → k_mutex */
typedef struct k_mutex      TX_MUTEX;

/* TX_SEMAPHORE → k_sem */
typedef struct k_sem        TX_SEMAPHORE;

/* TX_EVENT_FLAGS_GROUP → k_event */
typedef struct k_event      TX_EVENT_FLAGS_GROUP;

/* TX_TIMER → k_timer */
typedef struct k_timer      TX_TIMER;

/* TX_BYTE_POOL — vereinfacht: kein echter Pool, nur Marker */
typedef struct UX_TX_BYTE_POOL_STRUCT {
    uint8_t            *start;
    size_t              size;
    size_t              used;
} TX_BYTE_POOL;

/* TX_BLOCK_POOL — nicht verwendet in USBX Device-only */
typedef struct UX_TX_BLOCK_POOL_STRUCT {
    uint8_t            *start;
    size_t              block_size;
    uint32_t            total_blocks;
} TX_BLOCK_POOL;

/* -----------------------------------------------------------------------
 * 4. ThreadX Konstanten
 * -------------------------------------------------------------------- */
#define TX_SUCCESS                  0x00U
#define TX_DELETED                  0x01U
#define TX_NO_INSTANCE              0x0DU
#define TX_WAIT_ABORTED             0x1AU
#define TX_WAIT_ERROR               0x04U
#define TX_PTR_ERROR                0x03U
#define TX_SIZE_ERROR               0x05U
#define TX_PRIORITY_ERROR           0x0CU
#define TX_START_ERROR              0x10U
#define TX_NO_MEMORY                0x10U
#define TX_CALLER_ERROR             0x13U
#define TX_MUTEX_ERROR              0x1CU
#define TX_NOT_AVAILABLE            0x1DU
#define TX_SEMAPHORE_ERROR          0x0AU

#define TX_NO_WAIT                  0x00000000UL
#define TX_WAIT_FOREVER             0xFFFFFFFFUL

#define TX_INT_DISABLE              1U
#define TX_INT_ENABLE               0U

#define TX_AUTO_START               1U
#define TX_DONT_START               0U

#define TX_AUTO_ACTIVATE            1U
#define TX_NO_ACTIVATE              0U

#define TX_AND                      0x02U
#define TX_AND_CLEAR                0x03U
#define TX_OR                       0x00U
#define TX_OR_CLEAR                 0x01U

#define TX_NO_TIME_SLICE            0U
#define TX_INHERIT                  1U
#define TX_NO_INHERIT               0U

/* -----------------------------------------------------------------------
 * 5. Interrupt-Kontrolle → Zephyr irq_lock/irq_unlock
 *    KRITISCH: Mutexe sind NICHT ISR-safe — nur Semaphore verwenden!
 * -------------------------------------------------------------------- */
#define UX_INT_SAVE_AREA            unsigned int _ux_irq_key;
#define UX_DISABLE_INTS             _ux_irq_key = irq_lock();
#define UX_RESTORE_INTS             irq_unlock(_ux_irq_key);

/* tx_interrupt_control Stub — wird von ux_port.h cortex_m7 referenziert */
static inline UINT tx_interrupt_control(UINT new_posture)
{
    if (new_posture == TX_INT_DISABLE) {
        return (UINT)irq_lock();
    } else {
        irq_unlock((unsigned int)new_posture);
        return TX_INT_ENABLE;
    }
}

/* -----------------------------------------------------------------------
 * 6. Cache-Operationen (Cortex-M55, 32-Byte Cache-Lines)
 * -------------------------------------------------------------------- */
#define UX_CACHE_LINE_SIZE          32U
#define UX_CACHE_ALIGN              __attribute__((aligned(UX_CACHE_LINE_SIZE)))

#ifdef CONFIG_DCACHE
#define UX_DMA_FLUSH(addr, size)    SCB_CleanDCache_by_Addr(     \
                                        (uint32_t *)(addr), (int32_t)(size))
#define UX_DMA_INVALIDATE(addr, size) SCB_InvalidateDCache_by_Addr( \
                                        (uint32_t *)(addr), (int32_t)(size))
#else
#define UX_DMA_FLUSH(addr, size)    do {} while(0)
#define UX_DMA_INVALIDATE(addr, size) do {} while(0)
#endif

/* -----------------------------------------------------------------------
 * 7. USBX Konfiguration
 * -------------------------------------------------------------------- */
#ifndef UX_PERIODIC_RATE
#define UX_PERIODIC_RATE                        1000U   /* 1ms Ticks */
#endif

#ifndef UX_MAX_CLASS_DRIVER
#define UX_MAX_CLASS_DRIVER                     4U
#endif

#ifndef UX_MAX_SLAVE_CLASS_DRIVER
#define UX_MAX_SLAVE_CLASS_DRIVER               2U
#endif

#ifndef UX_MAX_HCD
#define UX_MAX_HCD                              1U
#endif

#ifndef UX_MAX_DEVICES
#define UX_MAX_DEVICES                          1U
#endif

#ifndef UX_MAX_ED
#define UX_MAX_ED                               16U
#endif

#ifndef UX_MAX_TD
#define UX_MAX_TD                               8U
#endif

#ifndef UX_MAX_ISO_TD
#define UX_MAX_ISO_TD                           8U
#endif

#ifndef UX_THREAD_STACK_SIZE
#define UX_THREAD_STACK_SIZE                    (2U * 1024U)
#endif

#ifndef UX_THREAD_PRIORITY_DCD
#define UX_THREAD_PRIORITY_DCD                  2U
#endif

#ifndef UX_THREAD_PRIORITY_HCD
#define UX_THREAD_PRIORITY_HCD                  2U
#endif

#ifndef UX_NO_TIME_SLICE
#define UX_NO_TIME_SLICE                        0U
#endif

#ifndef UX_SLAVE_REQUEST_CONTROL_MAX_LENGTH
#define UX_SLAVE_REQUEST_CONTROL_MAX_LENGTH     256U
#endif

#ifndef UX_SLAVE_REQUEST_DATA_MAX_LENGTH
/* ST Referenz: 1024 * USBL_PACKET_PER_MICRO_FRAME = 1024 * 3 = 3072 */
#define UX_SLAVE_REQUEST_DATA_MAX_LENGTH        3072U
#endif

/* Memory-mapped IO (kein IO-Instructions Modus) */
#define inpb(a)     *((UCHAR  *)(a))
#define inpw(a)     *((USHORT *)(a))
#define inpl(a)     *((ULONG  *)(a))
#define outpb(a,b)  *((UCHAR  *)(a)) = ((UCHAR )(b))
#define outpw(a,b)  *((USHORT *)(a)) = ((USHORT)(b))
#define outpl(a,b)  *((ULONG  *)(a)) = ((ULONG )(b))

/* -----------------------------------------------------------------------
 * 8. Version
 * -------------------------------------------------------------------- */
#ifdef UX_SYSTEM_INIT
CHAR _ux_version_id[] =
    "Xi640 USBX Zephyr Port — Cortex-M55 — v1.0.0";
#else
extern CHAR _ux_version_id[];
#endif

#endif /* UX_PORT_H */
