# USBX Standalone — SETUP-Paket Fluss & State Machine

Analysiert am 2026-03-12.
Quellen: `x-cube-n6-camera-capture-main/STM32Cube_FW_N6/Middlewares/ST/usbx/`

---

## 1. UX_STANDALONE → UX_DEVICE_STANDALONE (automatisch)

`common/core/inc/ux_api.h`, Zeilen 180–182:

```c
#if defined(UX_STANDALONE)
#if !defined(UX_DEVICE_STANDALONE)
#define UX_DEVICE_STANDALONE   // ← wird automatisch gesetzt!
#endif
```

Unser `UX_STANDALONE`-Define (in CMakeLists.txt) ist ausreichend.
`UX_DEVICE_STANDALONE` muss **nicht** explizit gesetzt werden.

---

## 2. Vollständiger SETUP-Paket Pfad

### Von IRQ bis Deskriptor-Antwort

```
IRQ feuert
  └─► HAL_PCD_IRQHandler(pcd_handle)
        └─► HAL_PCD_SetupStageCallback(hpcd)              [ISR-Kontext]
              ├─► Kopiert SETUP-Daten in transfer_request->setup[]
              ├─► Löscht STALLED/TRANSFER/DONE Flags auf ed[0]
              └─► [UX_DEVICE_STANDALONE Pfad — KEIN direkter Call!]
                  ed->status |= UX_DCD_STM32_ED_STATUS_SETUP_IN   ← nur Flag setzen
                  return;  [ISR kehrt sofort zurück, kein Blocking]

────── max. 1 ms ──────

ux_system_tasks_run()                                      [usbx_task_thread]
  └─► _ux_device_stack_tasks_run()
        ├─► dcd->ux_slave_dcd_function(dcd, UX_DCD_TASKS_RUN=18, NULL)
        │     └─► case UX_DCD_ISR_PENDING (= 18):   ← gleicher Wert! ux_api.h L1594
        │           _ux_dcd_stm32_setup_isr_pending(dcd_stm32)
        │             ├─► Re-Entry-Guard: prüft TASK_PENDING Flag
        │             ├─► prüft SETUP_IN Flag (vom Callback gesetzt)
        │             ├─► setzt TASK_PENDING, löscht SETUP_IN
        │             └─► _ux_dcd_stm32_setup_in(ed, transfer_request)
        │                   └─► _ux_device_stack_control_request_process()
        │                         └─► dcd->ux_slave_dcd_function(UX_DCD_TRANSFER_REQUEST)
        │                               └─► _ux_dcd_stm32_transfer_run()
        │                                     ├─► CHECK device_state == RESET → Exit!
        │                                     └─► HAL_PCD_EP_Transmit()  [Deskriptor TX]
        └─► class->task_function()             [erst nach Enumeration aktiv]
```

### Wichtig: UX_DCD_TASKS_RUN == UX_DCD_ISR_PENDING

```c
// ux_api.h, Zeile 1593–1594:
#define UX_DCD_ISR_PENDING   18
#define UX_DCD_TASKS_RUN     18   // identischer Wert!
```

`_ux_device_stack_tasks_run()` ruft `dcd_function(UX_DCD_TASKS_RUN)` auf.
`ux_dcd_stm32_function.c` behandelt dies im `case UX_DCD_ISR_PENDING` →
`_ux_dcd_stm32_setup_isr_pending()`.

### Nicht-Standalone vs. Standalone Unterschied

| Modus | HAL_PCD_SetupStageCallback | Verarbeitung |
|---|---|---|
| **Nicht-Standalone** | Ruft `_ux_device_stack_control_request_process()` direkt auf (synchron im ISR) | Sofort |
| **Standalone** | Setzt nur Status-Flag (`SETUP_IN`/`SETUP_OUT`/`SETUP_STATUS`) | Deferred via `ux_system_tasks_run()` |

---

## 3. Device State — kritische Voraussetzung

### Der Guard in `_ux_dcd_stm32_transfer_run()`

```c
// ux_dcd_stm32_transfer_run.c, Zeile 107:
if (_ux_system_slave->ux_system_slave_device.ux_slave_device_state == UX_DEVICE_RESET)
{
    transfer_request->ux_slave_transfer_request_completion_code = UX_TRANSFER_BUS_RESET;
    return(UX_STATE_EXIT);   // ← alle Transfers abgelehnt!
}
```

Wenn `device_state == UX_DEVICE_RESET` → wird **kein einziger Deskriptor** gesendet.

### Wer setzt den State auf ATTACHED?

`HAL_PCD_ResetCallback()` (in `ux_dcd_stm32_callback.c`, Zeile 742–781):

```c
void HAL_PCD_ResetCallback(PCD_HandleTypeDef *hpcd)
{
    // 1. Falls schon verbunden → disconnect
    if (state != UX_DEVICE_RESET)
        _ux_device_stack_disconnect();

    // 2. USB-Speed aus hpcd->Init.speed setzen
    switch(hpcd->Init.speed) {
    case PCD_SPEED_HIGH:
        _ux_system_slave->ux_system_slave_speed = UX_HIGH_SPEED_DEVICE;
        break;
    // ...
    }

    // 3. Endpoints initialisieren (EP0 für SETUP-Empfang armen)
    _ux_dcd_stm32_initialize_complete();

    // 4. State-Transition: RESET → ATTACHED
    _ux_system_slave->ux_system_slave_device.ux_slave_device_state = UX_DEVICE_ATTACHED;
}
```

### Timing-Garantie (USB-Spec)

```
[USB Bus Reset Signal]
    │
    ▼
HAL_PCD_ResetCallback() → state = UX_DEVICE_ATTACHED
    │
    ▼  (USB Spec: Reset muss vor erstem SETUP kommen)
[GET_DESCRIPTOR SETUP-Paket]
    │
    ▼
HAL_PCD_SetupStageCallback() → SETUP_IN Flag setzen
    │
    ▼
ux_system_tasks_run() → transfer_run: state=ATTACHED ✓ → HAL_PCD_EP_Transmit()
```

---

## 4. Speed-Detection in HAL_PCD_ResetCallback

**Wichtig:** `HAL_PCD_ResetCallback` liest `hpcd->Init.speed` — das ist der
**konfigurierte** Wert (`PCD_SPEED_HIGH`), nicht die tatsächlich ausgehandelte Geschwindigkeit.

```c
// Wir setzen: hpcd->Init.speed = PCD_SPEED_HIGH
// → USBX setzt immer UX_HIGH_SPEED_DEVICE, unabhängig vom PHY-Lock

switch(hpcd->Init.speed) {
case PCD_SPEED_HIGH:
    _ux_system_slave->ux_system_slave_speed = UX_HIGH_SPEED_DEVICE;  // immer HS!
```

**Konsequenz:** Falls der PHY nicht in HS-Modus einrastet (Speed=1 im DSTS-Register),
arbeitet die Hardware mit FS (12 Mbit/s), aber USBX wählt das **HS-Descriptor-Framework**.
Der Host erwartet bei HS-Geräten einen **Device Qualifier Descriptor** (GET_DESCRIPTOR 0x06).
Falls dieser fehlt oder falsch ist → Enumeration-Fehler.

---

## 5. Mögliche Unterbrechungspunkte

| # | Stelle | Bedingung | Folge |
|---|---|---|---|
| **1** | `HAL_PCD_ResetCallback` wird nicht aufgerufen | IRQ feuert nicht, PHY-Problem | `state` bleibt `RESET` → alle Transfers mit `UX_STATE_EXIT` abgebrochen |
| **2** | `UX_DCD_STM32_ED_STATUS_TASK_PENDING` bleibt hängen | Vorheriger Task nicht abgeschlossen | Nachfolgende SETUPs komplett ignoriert |
| **3** | `UX_DISABLE` / `UX_RESTORE` fehlerhaft | BASEPRI statt PRIMASK → Race Condition ISR vs. tasks_run | Korrupte Flags, sporadische Fehler |
| **4** | `ux_slave_dcd_controller_hardware == NULL` | `_ux_dcd_stm32_initialize()` nicht gelaufen | Hard Fault in `SetupStageCallback` |
| **5** | HS-Framework gewählt, aber PHY in FS | Device Qualifier Descriptor fehlt oder falsch | Host meldet Fehler nach GET_DESCRIPTOR |
| **6** | `ux_system_tasks_run()` zu selten | `k_sleep(K_MSEC(1))` → max. 1 ms Latenz | Kein Problem (Host-Timeout ~500 ms) |

---

## 6. Diagnose-Checkliste

### Log direkt nach dem Flash

```
HAL_PCD_MspInit: VDD USB + FSEL=24MHz + Clocks OK (USBPHYC_CR=0x00000020)
HAL_PCD_Init OK — USBPHYC_CR=0x00000020
```

**USBPHYC_CR muss `0x00000020` sein** (Bits[6:4] = 0x2 = FSEL 24 MHz).
Wenn `0x00000000`: PHY Reference Clock nicht gesetzt → HS-PHY-Lock unmöglich.

### Log nach 5 Sekunden (Diagnostik-Loop)

```
USB: Speed=? Frame=? ISOinc=? IRQ=?
```

| Wert | Bedeutung | Nächste Aktion |
|---|---|---|
| `IRQ=0` | USB-ISR feuert nicht | VDD USB / PHY / IRQ_CONNECT prüfen |
| `IRQ>0, Speed=1` | FS statt HS — PHY kein HS-Lock | PHY FSEL / VDD USB Timing prüfen; Device Qualifier Deskriptor prüfen |
| `IRQ>0, Speed=0` | HS aktiv ✓ | DMA aktivieren; Uncached Memory für PCD Handle |
| `Frame>0` | SOF-Pakete kommen an | Enumeration läuft (auch bei FS) |

---

## 7. Status unserer Implementierung

| Aspekt | Status | Referenz |
|---|---|---|
| `UX_STANDALONE` → `UX_DEVICE_STANDALONE` automatisch | ✅ | `ux_api.h` L180–182 |
| Init-Reihenfolge: IRQ enable erst nach `_ux_dcd_stm32_initialize()` | ✅ | `xi640_dcd_start()` |
| `_ux_utility_interrupt_disable/restore` = CMSIS PRIMASK | ✅ | `ux_port.c` |
| `_ux_dcd_stm32_initialize()` zweiter Parameter = `&hpcd` | ✅ | `xi640_dcd_register_usbx()` |
| Erster Parameter `dcd_io` = `USB1_OTG_HS` | ✅ (wird ignoriert) | `UX_PARAMETER_NOT_USED` |
| `HAL_PCD_Start()` separat in `xi640_dcd_start()` | ✅ | Nicht von USBX aufgerufen |
| HS-Descriptor Framework vorhanden | ✅ | `xi640_uvc_descriptors.c` |
| Device Qualifier Descriptor im FS-Framework | ⚠️ prüfen | Bei FS-Betrieb erforderlich |

---

## 8. Referenzen

| Datei | Inhalt |
|---|---|
| `common/core/src/ux_system_tasks_run.c` | Entry Point, ruft `_ux_device_stack_tasks_run()` |
| `common/core/src/ux_device_stack_tasks_run.c` | DCD-Tasks + Class-Tasks |
| `common/usbx_stm32_device_controllers/ux_dcd_stm32_callback.c` | `HAL_PCD_SetupStageCallback`, `HAL_PCD_ResetCallback`, `_ux_dcd_stm32_setup_isr_pending` |
| `common/usbx_stm32_device_controllers/ux_dcd_stm32_function.c` | `UX_DCD_ISR_PENDING` Case → `_ux_dcd_stm32_setup_isr_pending` |
| `common/usbx_stm32_device_controllers/ux_dcd_stm32_transfer_run.c` | Device-State-Guard, `HAL_PCD_EP_Transmit` |
| `common/usbx_stm32_device_controllers/ux_dcd_stm32_initialize.c` | `pcd_handle = (PCD_HandleTypeDef *)parameter` |
| `common/usbx_stm32_device_controllers/ux_dcd_stm32_interrupt_handler.c` | PCD-Handle Pfad via `_ux_system_slave` |
| `common/core/inc/ux_api.h` | `UX_DCD_TASKS_RUN == UX_DCD_ISR_PENDING == 18` |
