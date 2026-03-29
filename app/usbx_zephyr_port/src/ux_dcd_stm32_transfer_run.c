/**************************************************************************/
/*                                                                        */
/*       Copyright (c) Microsoft Corporation. All rights reserved.        */
/*                                                                        */
/*       This software is licensed under the Microsoft Software License   */
/*       Terms for Microsoft Azure RTOS. Full text of the license can be  */
/*       found in the LICENSE file at https://aka.ms/AzureRTOS_EULA       */
/*       and in the root directory of this software.                      */
/*                                                                        */
/**************************************************************************/

/* Xi640 lokale Kopie — einzige Aenderung: __DSB()/__ISB() vor DMA-Start.
 *
 * Problem: ARM Memory Ordering auf Cortex-M55 — DMA liest Buffer bevor
 * CPU-Writes abgeschlossen sind (Store Buffer nicht geleert).
 * __DSB() = Data Synchronization Barrier: wartet bis alle ausstehenden
 * Speicher-Writes im RAM sichtbar sind bevor DMA gestartet wird.
 * __ISB() = Instruction Synchronization Barrier: Pipeline leeren.
 *
 * Referenz: ARM Cortex-M Programming Guide, "Memory Barriers" Kapitel.
 * Symptom ohne Fix: bLength=0x00 statt 0x12 (DMA liest ungeschriebene Bytes). */

#define UX_SOURCE_CODE
#define UX_DCD_STM32_SOURCE_CODE

#include "ux_api.h"
#include "ux_dcd_stm32.h"
#include "ux_utility.h"
#include "ux_device_stack.h"

#include <cmsis_compiler.h>  /* __DSB(), __ISB() */


#if defined(UX_DEVICE_STANDALONE)
UINT  _ux_dcd_stm32_transfer_run(UX_DCD_STM32 *dcd_stm32, UX_SLAVE_TRANSFER *transfer_request)
{
UX_INTERRUPT_SAVE_AREA

UX_SLAVE_ENDPOINT       *endpoint;
UX_DCD_STM32_ED         *ed;
ULONG                   ed_status;


    /* Get the pointer to the logical endpoint from the transfer request.  */
    endpoint =  transfer_request -> ux_slave_transfer_request_endpoint;

    /* Get the physical endpoint address in the endpoint container.  */
    ed =  (UX_DCD_STM32_ED *) endpoint -> ux_slave_endpoint_ed;

    UX_DISABLE

    /* Get current ED status.  */
    ed_status = ed -> ux_dcd_stm32_ed_status;

    /* Invalid state.  */
    if (_ux_system_slave -> ux_system_slave_device.ux_slave_device_state == UX_DEVICE_RESET)
    {
        transfer_request -> ux_slave_transfer_request_completion_code = UX_TRANSFER_BUS_RESET;
        UX_RESTORE
        return(UX_STATE_EXIT);
    }

    /* ED stalled.  */
    if (ed_status & UX_DCD_STM32_ED_STATUS_STALLED)
    {
        transfer_request -> ux_slave_transfer_request_completion_code = UX_TRANSFER_STALLED;
        UX_RESTORE
        return(UX_STATE_NEXT);
    }

    /* ED transfer in progress.  */
    if (ed_status & UX_DCD_STM32_ED_STATUS_TRANSFER)
    {
        if (ed_status & UX_DCD_STM32_ED_STATUS_DONE)
        {

            /* Keep used, stall and task pending bits.  */
            ed -> ux_dcd_stm32_ed_status &= (UX_DCD_STM32_ED_STATUS_USED |
                                        UX_DCD_STM32_ED_STATUS_STALLED |
                                        UX_DCD_STM32_ED_STATUS_TASK_PENDING);
            UX_RESTORE
            return(UX_STATE_NEXT);
        }
        UX_RESTORE
        return(UX_STATE_WAIT);
    }


    /* Start transfer.  */
    ed -> ux_dcd_stm32_ed_status |= UX_DCD_STM32_ED_STATUS_TRANSFER;

    /* Check for transfer direction.  Is this a IN endpoint ? */
    if (transfer_request -> ux_slave_transfer_request_phase == UX_TRANSFER_PHASE_DATA_OUT)
    {

        /* Barrier: alle CPU-Writes in den Buffer muessen im RAM sein bevor DMA liest. */
        __DSB();
        __ISB();

        /* Transmit data.  */
        HAL_PCD_EP_Transmit(dcd_stm32 -> pcd_handle,
                            endpoint->ux_slave_endpoint_descriptor.bEndpointAddress,
                            transfer_request->ux_slave_transfer_request_data_pointer,
                            transfer_request->ux_slave_transfer_request_requested_length);
    }
    else
    {

        /* Barrier: sicherstellen dass Receive-Buffer leer/gueltig im RAM. */
        __DSB();
        __ISB();

        /* We have a request for a SETUP or OUT Endpoint.  */
        /* Receive data.  */
        HAL_PCD_EP_Receive(dcd_stm32 -> pcd_handle,
                            endpoint->ux_slave_endpoint_descriptor.bEndpointAddress,
                            transfer_request->ux_slave_transfer_request_data_pointer,
                            transfer_request->ux_slave_transfer_request_requested_length);
    }

    /* Return to caller with WAIT.  */
    UX_RESTORE
    return(UX_STATE_WAIT);
}
#endif /* defined(UX_DEVICE_STANDALONE) */
