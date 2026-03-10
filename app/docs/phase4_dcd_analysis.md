# Phase 4 — STM32N6 DCD + HS PHY + DMA + Cache — Analyse

> Erstellt: 2026-03-10 | Status: Analyse abgeschlossen, Implementierung offen

---

## 1. ST DCD Treiber-Analyse (ux_dcd_stm32)

### 1.1 Vorhandene Funktionen

Der ST USBX DCD Treiber liegt unter `modules/st_usbx/common/usbx_stm32_device_controllers/` und besteht aus:

| Datei | Funktion | Beschreibung |
|-------|----------|-------------|
| `ux_dcd_stm32_initialize.c` | `_ux_dcd_stm32_initialize()` | Alloziert `UX_DCD_STM32`, speichert `PCD_HandleTypeDef *`, registriert `_ux_dcd_stm32_function` als Dispatcher |
| `ux_dcd_stm32_uninitialize.c` | `_ux_dcd_stm32_uninitialize()` | Gegenstueck, Speicher freigeben |
| `ux_dcd_stm32_function.c` | `_ux_dcd_stm32_function()` | Dispatcher: routet USBX-Funktionen (EP create/destroy, transfer, stall, frame_number, address) |
| `ux_dcd_stm32_endpoint_create.c` | `_ux_dcd_stm32_endpoint_create()` | Oeffnet EP via `HAL_PCD_EP_Open()` |
| `ux_dcd_stm32_endpoint_destroy.c` | `_ux_dcd_stm32_endpoint_destroy()` | Schliesst EP via `HAL_PCD_EP_Close()` |
| `ux_dcd_stm32_endpoint_reset.c` | `_ux_dcd_stm32_endpoint_reset()` | Clears Stall via `HAL_PCD_EP_ClrStall()` |
| `ux_dcd_stm32_endpoint_stall.c` | `_ux_dcd_stm32_endpoint_stall()` | Setzt Stall via `HAL_PCD_EP_SetStall()` |
| `ux_dcd_stm32_endpoint_status.c` | `_ux_dcd_stm32_endpoint_status()` | Liest EP-Status |
| `ux_dcd_stm32_frame_number_get.c` | `_ux_dcd_stm32_frame_number_get()` | Liest `pcd_handle->FrameNumber` |
| `ux_dcd_stm32_transfer_request.c` | `_ux_dcd_stm32_transfer_request()` | RTOS-Modus: `HAL_PCD_EP_Transmit/Receive` + Semaphore-Wait |
| `ux_dcd_stm32_transfer_run.c` | `_ux_dcd_stm32_transfer_run()` | Standalone-Modus: State-Machine ohne Blocking |
| `ux_dcd_stm32_transfer_abort.c` | `_ux_dcd_stm32_transfer_abort()` | Transfer abbrechen |
| `ux_dcd_stm32_interrupt_handler.c` | `_ux_dcd_stm32_interrupt_handler()` | Delegiert an `HAL_PCD_IRQHandler()` |
| `ux_dcd_stm32_callback.c` | HAL Callbacks | Implementiert alle HAL_PCD Callbacks (Setup, DataIn, DataOut, Reset, Connect, Disconnect, Suspend, Resume, SOF, ISOINIncomplete, ISOOUTIncomplete) |

### 1.2 Architektur-Kern-Erkenntnis

**Der DCD ist familienagnostisch.** Er nutzt ausschliesslich die STM32 HAL PCD API (`HAL_PCD_*` Funktionen) und greift nie direkt auf OTG-Register zu. Das bedeutet:

- Der DCD selbst braucht **keine Anpassung** fuer STM32N6
- Die Anpassung erfolgt **unterhalb** des DCD, in:
  1. `PCD_HandleTypeDef` Konfiguration (wir fuellen `.Init` Felder)
  2. `HAL_PCD_MspInit()` (Clock, GPIO, NVIC — wir implementieren)
  3. FIFO-Konfiguration via `HAL_PCDEx_SetRxFiFo/SetTxFiFo`
  4. DMA/Cache-Konzept in unserem Wrapper

### 1.3 Kritische Einschraenkungen

| Problem | Detail | Auswirkung |
|---------|--------|------------|
| `UX_DCD_STM32_MAX_ED` = 4 | STM32N6 hat 9 bidirektionale EPs | **MUSS auf 9 erhoehen** via `-DUX_DCD_STM32_MAX_ED=9` |
| Kein Cache-Awareness | DCD ruft `HAL_PCD_EP_Transmit(buf)` direkt — kein Cache-Flush vor DMA | **Wrapper noetig** |
| Kein SOF Even/Odd im DCD | ISO Frame-Sync ist in `USB_EPStartXfer()` der LL implementiert | HAL erledigt das intern, kein DCD-Patch noetig |
| `ux_dcd_stm32_callback.c` ist monolithisch | ~900 Zeilen, alle HAL Callbacks in einer Datei | Nur lesen, nicht aendern — weak Symbols nutzen |

---

## 2. HAL-Abhaengigkeiten

### 2.1 STM32N6 HAL PCD Dateien (vorhanden und vollstaendig)

| Datei | Pfad (relativ zu modules/hal/stm32/stm32cube/stm32n6xx/) |
|-------|------|
| `stm32n6xx_hal_pcd.h` | `drivers/include/` |
| `stm32n6xx_hal_pcd.c` | `drivers/src/` |
| `stm32n6xx_hal_pcd_ex.h` | `drivers/include/` |
| `stm32n6xx_hal_pcd_ex.c` | `drivers/src/` |
| `stm32n6xx_ll_usb.h` | `drivers/include/` |
| `stm32n6xx_ll_usb.c` | `drivers/src/` |
| `stm32n657xx.h` | `soc/` (CMSIS Register-Definitionen) |

### 2.2 HAL-Funktionen die der DCD aufruft

| HAL-Funktion | DCD-Stelle | Beschreibung |
|-------------|-----------|--------------|
| `HAL_PCD_Init()` | **Nicht im DCD! Wir rufen sie auf** | Core + Device Init |
| `HAL_PCD_Start()` | **Nicht im DCD! Wir rufen sie auf** | Enables USB, Connect |
| `HAL_PCD_Stop()` | `_ux_dcd_stm32_function` (FORCE_DISCONNECT) | Stoppt USB |
| `HAL_PCD_IRQHandler()` | `_ux_dcd_stm32_interrupt_handler` | ISR Dispatch |
| `HAL_PCD_EP_Open()` | `_ux_dcd_stm32_endpoint_create` | Endpoint oeffnen |
| `HAL_PCD_EP_Close()` | `_ux_dcd_stm32_endpoint_destroy` | Endpoint schliessen |
| `HAL_PCD_EP_Transmit()` | `_ux_dcd_stm32_transfer_request/run` | IN Transfer starten |
| `HAL_PCD_EP_Receive()` | `_ux_dcd_stm32_transfer_request/run` | OUT Transfer starten |
| `HAL_PCD_EP_SetStall()` | `_ux_dcd_stm32_endpoint_stall` | EP Stall |
| `HAL_PCD_EP_ClrStall()` | `_ux_dcd_stm32_endpoint_reset` | EP Stall loesen |
| `HAL_PCD_SetAddress()` | `_ux_dcd_stm32_function` (SET_ADDRESS) | USB-Adresse setzen |
| `HAL_PCDEx_SetRxFiFo()` | **Nicht im DCD — wir muessen rufen** | RX FIFO Groesse |
| `HAL_PCDEx_SetTxFiFo()` | **Nicht im DCD — wir muessen rufen** | TX FIFO Groesse pro EP |

### 2.3 STM32N6-spezifische Besonderheiten vs. H7

| Merkmal | STM32H7 OTG HS | STM32N6 OTG HS | Konsequenz |
|---------|----------------|----------------|------------|
| PHY | Externes ULPI (`PCD_PHY_ULPI=1`) | Eingebetteter HS PHY (`USB_OTG_HS_EMBEDDED_PHY=3`) | `Init.phy_itface = USB_OTG_HS_EMBEDDED_PHY` |
| PHY Controller | Kein separater Block | `usbphyc1` @ 0x5803FC00 (separater Clock) | Clock fuer PHY enablen |
| USB Instanzen | 1x OTG HS | 2x OTG HS (USB1 @ 0x58040000, USB2 @ 0x58080000) | Board nutzt USB1 (CN18 USB-C) |
| OTG Core ID | `0x4F54300A` | `0x4F54310A` (vermutlich) | Neuer Core, ggf. neue Register |
| GAHBCFG HBSTLEN | 4 Bits (Pos 1..4) | 4 Bits (Pos 1..4) — identisch | INCR4 Default in HAL, INCR16 moeglich |
| FIFO Groesse | 4096 Words | 4096 Words (lt. DTS ram-size) | Identisch |
| Endpoints | Bis 9 bidir | 9 bidir (lt. DTS num-bidir-endpoints) | `UX_DCD_STM32_MAX_ED=9` |
| Security | Ohne TrustZone | TrustZone (NS/S Adressen) | Im FSBL-Modus irrelevant |

### 2.4 Zephyr Devicetree

Board-DTSI (`stm32n6570_dk_common.dtsi`):
```dts
zephyr_udc0: &usbotg_hs1 {
    status = "okay";
};
```

SOC-DTSI (`stm32n6.dtsi`):
```dts
usbotg_hs1: otghs@58040000 {
    compatible = "st,stm32n6-otghs", "st,stm32-otghs";
    reg = <0x58040000 0x2000>;
    interrupts = <177 0>;
    num-bidir-endpoints = <9>;
    ram-size = <4096>;
    maximum-speed = "high-speed";
    clocks = <&rcc STM32_CLOCK(AHB5, 26)>,
             <&rcc STM32_SRC_HSE OTGPHY1CKREF_SEL(1)>;
    phys = <&usbphyc1>;
};

usbphyc1: usbphyc@5803fc00 {
    compatible = "st,stm32-usbphyc";
    reg = <0x5803fc00 0x400>;
    clocks = <&rcc STM32_CLOCK(AHB5, 28)>;
};
```

**CubeMX nicht noetig** — wir konfigurieren HAL_PCD direkt.

---

## 3. Schicht-Architektur

```
+----------------------------------------+
|           USBX Device Stack            |  ruft _ux_dcd_stm32_function()
+----------------------------------------+
|        ST DCD (ux_dcd_stm32)           |  ruft HAL_PCD_EP_*()
+----------------------------------------+
|    xi640_dcd.c (UNSER Code)            |  Konfiguration + Glue
|    - PCD_HandleTypeDef Setup           |
|    - HAL_PCD_MspInit() Impl.           |
|    - FIFO Konfiguration                |
|    - DMA/Cache Wrapper                 |
|    - ISR -> Zephyr IRQ_CONNECT         |
+----------------------------------------+
|   STM32N6 HAL PCD (stm32n6xx_hal_pcd) |  Hardware-Abstraktion
+----------------------------------------+
|   STM32N6 LL USB (stm32n6xx_ll_usb)   |  Register-Zugriffe
+----------------------------------------+
|          USB OTG HS Hardware           |  0x58040000
+----------------------------------------+
```

### Initialisierungsreihenfolge

```
1. xi640_dcd_init()
   +-- 1a. Clock enablen (RCC: USB1 OTG HS + USB PHY Controller)
   +-- 1b. PCD_HandleTypeDef fuellen (Instance, Init-Felder)
   +-- 1c. HAL_PCD_Init()  ->  USB_CoreInit() + USB_DevInit()
   |        +-- ruft HAL_PCD_MspInit() (von uns implementiert)
   +-- 1d. FIFO konfigurieren (HAL_PCDEx_SetRxFiFo/SetTxFiFo)
   +-- 1e. DMA Burst-Laenge anpassen (GAHBCFG HBSTLEN) — optional
   +-- 1f. _ux_dcd_stm32_initialize(0, &hpcd) registrieren
   +-- 1g. IRQ_CONNECT + irq_enable
   +-- 1h. HAL_PCD_Start()  ->  USB_DevConnect()
```

---

## 4. FIFO Konfiguration — Detailberechnung

```
Gesamt: 4096 Words (= 16384 Bytes) lt. DTS ram-size

RX FIFO:         1024 Words  (4096 Bytes)    -- Offset 0
EP0 TX FIFO:      512 Words  (2048 Bytes)    -- Offset 1024
EP1 ISO TX FIFO: 2048 Words  (8192 Bytes)    -- Offset 1536
DMA Reserve:       18 Words  (implizit in GDFIFOCFG)
                 -----
Summe:           3584 Words  (87.5% belegt)
Frei:             512 Words  (Puffer fuer spaetere EPs)
```

API-Aufrufe:
```c
HAL_PCDEx_SetRxFiFo(&hpcd, 1024);       // RX: 1024 Words
HAL_PCDEx_SetTxFiFo(&hpcd, 0, 512);     // EP0 TX: 512 Words
HAL_PCDEx_SetTxFiFo(&hpcd, 1, 2048);    // EP1 TX: 2048 Words (ISO)
```

---

## 5. Host-testbar vs. Hardware-pflichtig

| Komponente | Host-testbar | Begruendung |
|-----------|-------------|-------------|
| PCD_HandleTypeDef Init-Werte | Ja | Pure Datenstruktur, Werte pruefen |
| FIFO Berechnung (Offsets, Groessen) | Ja | Reine Arithmetik |
| DMA Burst Config (HBSTLEN Wert) | Ja | Konstanten-Validierung |
| Cache-Alignment Utility | Ja | `ROUND_UP(size, 32)` |
| `HAL_PCD_MspInit()` Clock-Sequenz | Nein | RCC Register, Hardware |
| USB Enumeration (HS vs. FS) | Nein | Braucht Host + PHY |
| ISO Transfer Timing | Nein | Braucht USB Analyser |
| DMA + Cache Interaktion | Nein | Braucht echtes RAM + DMA |

---

## 6. Risiken und offene Fragen

### 6.1 HBSTLEN INCR4 vs. INCR16

Die STM32N6 HAL setzt in `USB_CoreInit()` explizit `HBSTLEN = INCR4` (Wert 0x3) in `stm32n6xx_ll_usb.c:107-109`:

```c
USBx->GAHBCFG &= ~(USB_OTG_GAHBCFG_HBSTLEN);
USBx->GAHBCFG |= USB_OTG_GAHBCFG_HBSTLEN_INCR4;
USBx->GAHBCFG |= USB_OTG_GAHBCFG_DMAEN;
```

**Empfehlung:**
1. Zunaechst mit HAL-Default INCR4 starten (erst mal enumerieren)
2. INCR8 testen (Kompromiss)
3. INCR16 nur wenn INCR4/INCR8 die 18 MB/s nicht schaffen
4. INCR16 mit PSRAM ist definitiv riskant — aber ISO-Buffer liegen in AXISRAM

### 6.2 USB PHY Controller (usbphyc1)

STM32N6 hat separaten USB PHY Controller Block (`usbphyc1` @ 0x5803FC00):
- Braucht der PHY eine eigene Initialisierung ueber dedizierte Register?
- Ist `HAL_PCD_Init()` mit `phy_itface = USB_OTG_HS_EMBEDDED_PHY` ausreichend?
- Kein `stm32n6xx_hal_usbphyc.h` gefunden — vermutlich in LL USB integriert

**TODO:** In CubeN6 Beispielprojekten nach USB-Initialisierung suchen.

### 6.3 OTG Core Version

STM32N6 koennte OTG Core `0x4F54310A` nutzen (vs. `0x4F54300A` bei H7). Die LL-Header definieren:
```c
#define USB_OTG_CORE_ID_300A    0x4F54300AU
#define USB_OTG_CORE_ID_310A    0x4F54310AU
```

**Mitigation:** GSNPSID Register auslesen nach Init und im Log ausgeben.

### 6.4 DMA + Cache Interaktion

Die HAL setzt `ep->dma_addr` und schreibt in `DIEPDMA/DOEPDMA`:
- **Vor jedem TX:** Buffer muss Cache-Clean sein (`sys_cache_data_flush_range`)
- **Nach jedem RX:** Buffer muss Cache-Invalidate sein (`sys_cache_data_invd_range`)

**Loesung:**
- TX (ISO OUT zu Host): Cache-Flush **vor** `HAL_PCD_EP_Transmit()`, im Thread-Kontext
- RX (Control EP0 IN): Cache-Invalidate **nach** DMA-Complete Callback

### 6.5 UX_DCD_STM32_MAX_ED

Default `UX_DCD_STM32_MAX_ED = 4` in `ux_dcd_stm32.h:63` — STM32N6 hat 9 EPs.
UVC braucht nur EP0 + EP1, aber auf 9 setzen fuer Zukunftssicherheit.

### 6.6 Was NICHT ohne Hardware validiert werden kann

| Test | Begruendung |
|------|-------------|
| USB Enumeration HS vs. FS | PHY Verhalten, Kabel, Host |
| DMA + Cache Korrektheit | Race Conditions nur unter Last sichtbar |
| ISO Transfer Integritaet | Timing-kritisch, Host-abhaengig |
| FIFO Under/Overrun | Nur unter echtem USB Traffic |
| PHY Clock Lock | HSE -> PHY PLL muss locken |
| Bus Contention mit DCMIPP/Ethernet | AXI Interconnect Scheduling |
| Reconnect-Stabilitaet | USB Hot-Plug Sequenz |

---

## 7. Kritische Code-Stellen

| Datei | Zeile | Was |
|-------|-------|-----|
| `stm32n6xx_ll_usb.c` | 107-109 | HAL setzt DMA Burst auf INCR4 (nicht INCR16) |
| `ux_dcd_stm32.h` | 63 | Default `UX_DCD_STM32_MAX_ED=4` — zu niedrig |
| `stm32n6xx_ll_usb.h` | 297 | `USB_OTG_HS_EMBEDDED_PHY = 3U` — STM32N6 spezifisch |
| `stm32n6.dtsi` | usbotg_hs1 | IRQ 177, ram-size 4096, 9 EPs |

---

## 8. Zusammenfassung: Fertig vs. zu tun

### Bereits vorhanden (1:1 nutzbar)
- USBX DCD Treiber (`ux_dcd_stm32_*.c`) — familienagnostisch
- STM32N6 HAL PCD (`stm32n6xx_hal_pcd.c/.h`) — vollstaendig
- STM32N6 LL USB (`stm32n6xx_ll_usb.c/.h`) — mit DMA + ISO Support
- CMSIS Header (`stm32n657xx.h`) — alle USB OTG Register definiert
- Zephyr DTS (`usbotg_hs1`, `usbphyc1`)
- FIFO API (`HAL_PCDEx_SetRxFiFo/SetTxFiFo`)
- ISO Even/Odd Frame Sync (in `USB_EPStartXfer()`)

### Von uns zu implementieren (xi640_dcd.c)

| Arbeitspaket | Komplexitaet | Risiko |
|-------------|-------------|--------|
| `PCD_HandleTypeDef` Init-Werte fuellen | Niedrig | Niedrig |
| `HAL_PCD_MspInit()` Override (Clock, PHY) | Mittel | Mittel |
| FIFO Konfiguration (3 API-Calls) | Niedrig | Niedrig |
| DMA Burst-Laenge tunen (optional) | Niedrig | Mittel |
| Cache-Wrapper fuer TX/RX | Mittel | **Hoch** |
| Zephyr ISR Anbindung (`IRQ_CONNECT`) | Niedrig | Niedrig |
| `_ux_dcd_stm32_initialize` Aufruf | Niedrig | Niedrig |
| `UX_DCD_STM32_MAX_ED=9` Compile-Define | Niedrig | Niedrig |
