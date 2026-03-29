# ST Referenzprojekt Analyse — x-cube-n6-camera-capture

Analysiert am 2026-03-12. Ziel: Enumeration-Fehler beheben (Speed=1, VID=0000/PID=0002).
Referenz: `D:\Zephyr_Workbench\project_22\x-cube-n6-camera-capture-main\`

---

## Zusammenfassung: Kritische Unterschiede

| # | Aspekt | ST Referenz | Unser Code | Schweregrad |
|---|--------|------------|------------|------------|
| 1 | **PCD Handle Memory** | `.uncached_bss` + `aligned(32)` | Normaler cached SRAM | 🔴 KRITISCH |
| 2 | **DMA Enable** | `ENABLE` | `0` (TEST-Modus) | 🔴 Für Produktion |
| 3 | **FIFO RX** | 128 Words (512 B) | 1024 Words (4096 B) | 🟡 Tuning |
| 4 | **FIFO EP0 TX** | 16 Words (64 B) | 512 Words (2048 B) | 🟡 Tuning |
| 5 | **FIFO EP1 TX** | 768 Words (3072 B) | 2048 Words (8192 B) | 🟡 ISO-optimiert |
| 6 | **NVIC Enable in MspInit** | Ja (falsch!) | Nein → in start() | 🟢 Wir sind besser |
| 7 | **VDD USB Timeout-Guard** | Kein Timeout | Timeout + Log | 🟢 Wir sind besser |
| 8 | **interrupt_disable/restore** | PRIMASK-Intrinsics | PRIMASK-Intrinsics ✓ | 🟢 Identisch |

---

## 1. PCD Handle — Uncached Memory (KRITISCH)

### ST Lösung

**`Lib/uvcl/Src/uvcl_internal.h`:**
```c
#define UVCL_ALIGN_32  __attribute__((aligned(32)))
#define UVCL_UNCACHED  __attribute__((section(".uncached_bss")))
```

**`Lib/uvcl/Src/uvcl.c`, Zeile 44:**
```c
PCD_HandleTypeDef uvcl_pcd_handle UVCL_UNCACHED UVCL_ALIGN_32;
```

**Linker (`stm32n657xx_axisram.icf`):**
```
define region AXI_SRAM_UNCACHED_region = mem:[from 0x341f0000 to 0x341fffff];
place in AXI_SRAM_UNCACHED_region { readwrite section .uncached_bss };
define exported symbol __uncached_bss_start__ = start(AXI_SRAM_UNCACHED_region);
define exported symbol __uncached_bss_end__   = end(AXI_SRAM_UNCACHED_region) + 1;
```

### Unser Code

```c
static PCD_HandleTypeDef hpcd;  // ← normaler cached SRAM — FEHLER!
```

### Warum das Enumeration bricht

Der `PCD_HandleTypeDef` enthält interne Status-Flags und Puffer-Zeiger die der USB-DMA-Controller
direkt im RAM aktualisiert. Mit D-Cache aktiv (Cortex-M55 Standard):

```
USB DMA schreibt → RAM
CPU liest        → D-Cache (veraltet!) → falsche Werte
```

Folge: Der Device Descriptor wird zwar gesendet, aber byteweise verschoben/korrupt
→ Windows liest `VID=0000/PID=0002` statt `VID=FFFF/PID=0001`.

### Fix

In Zephyr gibt es keine `.uncached_bss` Section — wir verwenden eine feste AXISRAM-Adresse
in einem Bereich den wir per MPU als uncacheable konfigurieren.

Siehe `app/docs/usb_dma_cache_rules.md` für die Implementierung.

---

## 2. hpcd.Init Felder — Vergleich

| Feld | ST Referenz | Unser Code | Status |
|------|------------|------------|--------|
| `dev_endpoints` | 9 | 9 | ✅ Identisch |
| `dma_enable` | `ENABLE` | `0U` (Test!) | ⚠️ Produktion: `1U` |
| `speed` | `PCD_SPEED_HIGH` | `USB_OTG_SPEED_HIGH` | ✅ Identisch |
| `phy_itface` | `USB_OTG_HS_EMBEDDED_PHY` | `USB_OTG_HS_EMBEDDED_PHY` | ✅ Identisch |
| `Sof_enable` | `ENABLE` | `1U` | ✅ Identisch |
| `ep0_mps` | (nicht gesetzt) | `USB_OTG_MAX_EP0_SIZE` | 🟡 Prüfen |
| `low_power_enable` | `DISABLE` | `0U` | ✅ Identisch |
| `lpm_enable` | `DISABLE` | `0U` | ✅ Identisch |
| `battery_charging_enable` | (nicht gesetzt) | `0U` | 🟡 OK |
| `vbus_sensing_enable` | `DISABLE` | `0U` | ✅ Identisch |
| `use_dedicated_ep1` | `DISABLE` | `0U` | ✅ Identisch |
| `use_external_vbus` | `DISABLE` | `0U` | ✅ Identisch |

---

## 3. FIFO-Konfiguration

| FIFO | ST (Words) | ST (Bytes) | Unser (Words) | Unser (Bytes) | Faktor |
|------|-----------|-----------|--------------|--------------|--------|
| RX   | 128 (0x80)  | 512  | 1024 | 4096 | 8× |
| EP0 TX | 16 (0x10) | 64   | 512  | 2048 | 32× |
| EP1 TX | 768 (0x300) | 3072 | 2048 | 8192 | 2.7× |
| **Summe** | **912** | **3648** | **3584** | **14336** | **4×** |

**Bewertung:** ST-Werte sind minimal; unsere Werte sind für ISO-Streaming optimiert und liegen
im gültigen Bereich (Gesamt < 4096 Words). Kein Änderungsbedarf für Enumeration.

> ACHTUNG: Falls Enumeration mit unseren FIFO-Werten fehlschlägt und mit ST-Werten klappt,
> deutet das auf ein FIFO-Overflow-Problem hin. Dann FIFO-Werte an ST angleichen.

---

## 4. HAL_PCD_MspInit — Vergleich

ST-Code (`Src/main.c`, Zeilen 396–425):
```c
void HAL_PCD_MspInit(PCD_HandleTypeDef *hpcd)
{
    assert(hpcd->Instance == USB1_OTG_HS);      // ← assert fehlt bei uns
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWREx_EnableVddUSBVMEN();
    while (__HAL_PWR_GET_FLAG(PWR_FLAG_USB33RDY) == 0U);  // ← kein Timeout!
    HAL_PWREx_EnableVddUSB();
    __HAL_RCC_USB1_OTG_HS_CLK_ENABLE();
    USB1_HS_PHYC->USBPHYC_CR &= ~(0x7U << 0x4U);   // FSEL-Bits löschen
    USB1_HS_PHYC->USBPHYC_CR |= (0x2U << 0x4U);    // FSEL = 0x2 (24 MHz)
    __HAL_RCC_USB1_OTG_HS_PHY_CLK_ENABLE();
    HAL_NVIC_SetPriority(USB1_OTG_HS_IRQn, 6U, 0U); // ← FALSCH: zu früh!
    HAL_NVIC_EnableIRQ(USB1_OTG_HS_IRQn);            // ← FALSCH: vor USBX Init!
}
```

**Fazit:** Unsere Implementierung ist korrekt. ST enablet IRQ zu früh (vor USBX Init).
Unser `irq_enable()` in `xi640_dcd_start()` ist die richtige Reihenfolge.

---

## 5. Interrupt Guards — _ux_utility_interrupt_disable/restore

ST (`Lib/.../usbx.c`, Zeilen 520–534):
```c
ALIGN_TYPE _ux_utility_interrupt_disable(VOID)
{
    ALIGN_TYPE ret = __get_PRIMASK();
    __disable_irq();
    return ret;
}

VOID _ux_utility_interrupt_restore(ALIGN_TYPE flags)
{
    if (!flags)
        __enable_irq();
}
```

**Status:** ✅ Unsere `ux_port.c` Implementierung ist seit letztem Fix identisch.

---

## 6. Device Descriptor — ST Referenz

ST (`Lib/uvcl/Src/uvcl_desc.c`):
```
bLength          = 18
bDescriptorType  = 0x01
bcdUSB           = 0x0200  (USB 2.0)
bDeviceClass     = 0xEF   (Misc — für IAD/Multi-Interface!)
bDeviceSubClass  = 0x02
bDeviceProtocol  = 0x01   (IAD)
bMaxPacketSize   = 64
idVendor         = 0x0483  (ST Microelectronics)
idProduct        = 0x5780
bcdDevice        = 0x0100
bNumConfigurations = 1
```

**Kritisch:** `bDeviceClass = 0xEF` + `bDeviceSubClass = 0x02` + `bDeviceProtocol = 0x01`
ist Pflicht wenn IAD (Interface Association Descriptor) verwendet wird.
Falsche Werte → Host erkennt das Gerät nicht als UVC.

**TODO:** Unsere Deskriptoren in `xi640_uvc_descriptors.c` auf diese Werte prüfen!

---

## 7. USBX Pool — Memory Placement bei ST

ST platziert den USBX Memory Pool ebenfalls in `.uncached_bss`:

```c
// aus usbx.c
static uint8_t usbx_memory[USBX_MEMORY_SIZE]
    __attribute__((section(".uncached_bss")))
    __attribute__((aligned(32)));
```

**TODO:** Auch unseren USBX-Pool in uncached Memory verschieben.

---

## 8. Permanenter Fix — Implementierungsplan

### Schritt 1: MPU-Region für uncached AXISRAM definieren

Wir verwenden eine dedizierte AXISRAM-Subregion als uncached. Keine neue Linker-Section
nötig — feste Adressierung wie bei unseren Frame-Buffern.

**Adresse:** `0x34280000` (AXISRAM4, Anfang — 64 KB für uncached Objekte)

Zephyr MPU-Konfiguration im Board-Overlay oder `main.c`:
```c
// MPU Region: AXISRAM4 als Device/Strongly-Ordered (uncacheable)
ARM_MPU_SetRegionEx(7U,
    0x34280000U,                              // BaseAddr
    ARM_MPU_RBAR_AP(1U, 0U) |                // RW, Privileged
    ARM_MPU_RBAR_SH(0U),                     // Non-shareable
    ARM_MPU_RLAR_AttrIndx(2U) | 1U);         // AttrIndx=2: Device-nGnRnE
```

### Schritt 2: PCD Handle in uncached Memory

```c
// xi640_dcd.c
#define XI640_PCD_HANDLE_ADDR  0x34280000UL
static PCD_HandleTypeDef * const hpcd_ptr =
    (PCD_HandleTypeDef *)XI640_PCD_HANDLE_ADDR;
```

### Schritt 3: USBX Pool in uncached Memory

```c
// xi640_uvc_stream.c oder main.c
#define XI640_USBX_POOL_ADDR   (0x34280000UL + sizeof(PCD_HandleTypeDef) + 32UL)
```

---

## 9. DMA-Off-Test — Auswertung

| Ergebnis | Schlussfolgerung | Nächste Aktion |
|----------|-----------------|----------------|
| VID/PID korrekt, Enumeration OK | Cache-Kohärenz war die Ursache | Uncached-Fix implementieren, DMA re-enablen |
| Immer noch falsche VID/PID | Deskriptor-Problem zusätzlich | Descriptors byteweise prüfen |
| Speed wechselt auf 0 (HS) | PHY-Init griff ohne DMA-Interferenz | PHY-Diagnose-Logs prüfen |
| Speed bleibt 1 (FS) | PHY/VDD-Problem unabhängig von DMA | USBPHYC_CR Readback-Log prüfen |
