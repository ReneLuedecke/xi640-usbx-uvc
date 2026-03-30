# Zephyr Tree Patches — Xi640 Composite Fix

Diese Patches modifizieren den Zephyr-Tree (`project_22/zephyr/`) und werden
bei `west update` überschrieben. Nach jedem Update neu anwenden:

```bash
cd D:/Zephyr_Workbench/project_22/zephyr
git apply ../app/zephyr-patches/0001-usbd_init-two-pass-class-init-before-ep-assignment.patch
git apply ../app/zephyr-patches/0003-sample_usbd_init-hid-after-uvc-registration.patch
# Patch 0002 optional (Debug-Hexdump, kann weggelassen werden)
```

## Patches

### 0001 — `usbd_init.c`: Two-Pass Class Init (KRITISCH)

**Datei:** `subsys/usb/device_next/usbd_init.c`

**Problem:** `uvc_add_format()` wird vor `usbd_init()` aufgerufen und
überschreibt den `if1_ep_hs`-Platzhalter im UVC HS-Deskriptor-Array.
`init_configuration_inst(UVC)` sieht dann keinen EP-Deskriptor →
`config_ep_bm` EP1-IN-Bit fehlt → HID bekommt EP1 (0x81) →
Kollision mit UVC Bulk → Host lehnt Deskriptor ab.

**Fix:** `init_configuration()` in zwei Pässe aufgeteilt:
- Pass 1: Alle `usbd_class_init()` (UVC fügt EP zurück ins Array)
- Pass 2: Alle `init_configuration_inst()` (EP-Bitmap korrekt)

---

### 0002 — `usbd_ch9.c`: Config Descriptor Hexdump (optional/Debug)

**Datei:** `subsys/usb/device_next/usbd_ch9.c`

Gibt den vollständigen Configuration Descriptor (345 Bytes) als Hexdump
aus wenn `buf->len > 256`. Nützlich zur Verifikation nach Patches.
Kann nach erfolgreicher Verifikation weggelassen werden.

---

### 0003 — `sample_usbd_init.c`: HID nach UVC registrieren (KRITISCH)

**Datei:** `samples/subsys/usb/common/sample_usbd_init.c`

**Problem:** Zephyr `STRUCT_SECTION_ITERABLE` sortiert Klassen alphabetisch:
`hid_0 < uvc_c_data_0` → HID bekommt Interface 0 statt Interface 2.

**Fix:**
- HID in Blocklist von `usbd_register_all_classes()`
- Manuell mit `usbd_register_class("hid_0", ...)` nach UVC registrieren

---

## Upstream melden

Diese Patches sollten als Zephyr Issue/PR gemeldet werden:

- **0001:** "usbd: init: run class init before EP assignment for composite devices"
  → Betrifft alle Composite-Devices mit UVC + anderer Klasse
- **0003:** "usbd: sample: allow manual class registration order"
  → Alternativ: Zephyr-seitige Unterstützung für Registrierungs-Reihenfolge
