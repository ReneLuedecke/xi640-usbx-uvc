# Option 1: ISO High-Bandwidth in Zephyr UVC — Implementierungsplan

## Übersicht

**Basis:** Projekt 21 (Zephyr `device_next` UVC, Bulk, funktioniert)
**Ziel:** ISO High-Bandwidth (3×1024 = 3072 Bytes/Microframe, ~24 MB/s)
**Referenz:** ST FreeRTOS Projekt (`x-cube-n6-camera-capture`) für ISO-Logik

## Architektur

```
┌──────────────────────────────────────────────────────┐
│                    Applikation                        │
│  main.c (Projekt 21 Basis + Colorbar/Kamera-Feed)    │
├──────────────────────────────────────────────────────┤
│              Zephyr UVC Class (modifiziert)           │
│  usbd_uvc.c — Alt 0/1 + ISO EP + Probe/Commit       │
├──────────────────────────────────────────────────────┤
│              Zephyr USB device_next Stack             │
│  usbd_core.c — Enumeration, Control, SET_INTERFACE   │
├──────────────────────────────────────────────────────┤
│              Zephyr UDC DWC2 Treiber                 │
│  udc_dwc2.c — ISO Support bereits vorhanden (UAC2)   │
├──────────────────────────────────────────────────────┤
│              STM32N6 OTG HS Hardware                  │
└──────────────────────────────────────────────────────┘
```

Kein USBX, kein HAL PCD, kein Port-Layer. Alles über Zephyr's bewährten Stack.

---

## Phase 1: Zephyr UVC Source-Code forken (Tag 1)

### 1.1 Dateien kopieren
Aus `zephyr/subsys/usb/device_next/class/`:
- `usbd_uvc.c` → `app/src/usbd_uvc_iso.c`

Aus `zephyr/include/zephyr/usb/class/`:
- `usbd_uvc.h` → `app/inc/usbd_uvc_iso.h`

### 1.2 Aus Zephyr Build entfernen
In `prj.conf`:
```
# CONFIG_USBD_VIDEO_CLASS=n  ← Zephyr's eingebaute UVC deaktivieren
```
Stattdessen unsere modifizierte Version in `CMakeLists.txt` einbinden.

### 1.3 Verifizieren
Build + Flash → muss identisch zu Projekt 21 funktionieren (Bulk UVC, 1.5 FPS).
Erst wenn das funktioniert: weiter zu Phase 2.

---

## Phase 2: Deskriptoren für ISO umbauen (Tag 2-3)

### 2.1 Alternate Settings hinzufügen

**Aktuell (Bulk):**
```
Interface 1, Alt 0: 1 Bulk EP (bNumEndpoints=1)
```

**Neu (ISO, wie USB Video Class 1.1 Spec):**
```
Interface 1, Alt 0: 0 Endpoints (Idle — Pflicht für ISO!)
Interface 1, Alt 1: 1 ISO EP (Streaming)
```

ST-Referenz (`uvcl_desc.c` Zeile 886):
```c
build_uvc_std_vs_desc(..., alt_nb=0, ep_nb=0);  // Alt 0: kein EP
build_uvc_std_vs_desc(..., alt_nb=1, ep_nb=1);  // Alt 1: ISO EP
build_uvc_vs_ep_desc(..., wMaxPacketSize);       // ISO EP
```

### 2.2 Endpoint Descriptor ändern

**Aktuell:**
```c
.bmAttributes = USB_EP_TYPE_BULK,          // 0x02
.wMaxPacketSize = sys_cpu_to_le16(512),    // HS Bulk max
```

**Neu:**
```c
// Full-Speed:
.bmAttributes = 0x05,                      // ISO, Asynchronous
.wMaxPacketSize = sys_cpu_to_le16(1023),   // FS ISO max
.bInterval = 1,

// High-Speed:
.bmAttributes = 0x05,                      // ISO, Asynchronous
.wMaxPacketSize = sys_cpu_to_le16(0x1400), // 3×1024 High-Bandwidth
.bInterval = 1,
```

`0x1400` = Bits[10:0]=1024, Bits[12:11]=0b10 (2 additional transactions = 3 total).

ST-Referenz (`uvcl_desc.c` Zeile 946):
```c
wMaxPacketSize = 1024 | ((USBL_PACKET_PER_MICRO_FRAME - 1) << 11);
// Mit USBL_PACKET_PER_MICRO_FRAME=3: 1024 | (2 << 11) = 0x1400
```

### 2.3 Configuration Descriptor Update
- `wTotalLength` neu berechnen (2 Interface Descriptors statt 1)
- `bNumInterfaces` bleibt 2 (VC + VS)

---

## Phase 3: SET_INTERFACE Handler (Tag 3-4)

### 3.1 Alternate Setting Callback

Zephyr's `usbd_uvc.c` hat bereits `uvc_update()` — aber es tut nichts.

**Neu implementieren:**
```c
static void uvc_update(struct usbd_class_data *const c_data,
                       const uint8_t iface, const uint8_t alternate)
{
    if (iface != VS_INTERFACE_NUMBER) return;
    
    if (alternate == 0) {
        // Host sagt: Streaming STOP
        // ISO EP deaktivieren
        uvc_stop_streaming(c_data);
    } else if (alternate == 1) {
        // Host sagt: Streaming START
        // ISO EP aktivieren, Buffer vorbereiten
        uvc_start_streaming(c_data);
    }
}
```

ST-Referenz: `uvcl_usbx.c` macht das über USBX `ux_device_class_video_change()` 
Callback — aber die Logik ist gleich: Alt 0 = Stop, Alt 1 = Start.

### 3.2 UDC ISO EP Enable/Disable

Zephyr's UDC API:
```c
udc_ep_enable(dev, ep_addr, USB_EP_TYPE_ISO, max_packet_size, interval);
udc_ep_disable(dev, ep_addr);
```

Der `udc_dwc2.c` Treiber unterstützt das bereits (verifiziert durch UAC2 Audio Class).

---

## Phase 4: ISO Streaming Transfer (Tag 4-6)

### 4.1 Payload-Format (UVC Header + Daten)

Jeder ISO-Transfer beginnt mit einem 2-Byte UVC Payload Header:
```
Byte 0: bHeaderLength = 2
Byte 1: bmHeaderInfo = BIT(0) FID toggle | BIT(1) EOF
```

Gefolgt von Bilddaten (max 3072 - 2 = 3070 Bytes pro Microframe).

ST-Referenz (`uvcl.c` Zeile 130-180): Baut UVC Header + kopieret Bilddaten in den
Payload-Buffer, ruft `ux_device_class_video_write_payload_commit()` auf.

### 4.2 Transfer-Mechanismus

Zephyr UDC API für ISO:
```c
struct net_buf *buf = udc_ep_buf_alloc(dev, ep, max_packet_size);
// Fülle buf mit UVC Header + Bilddaten
net_buf_add_mem(buf, uvc_header, 2);
net_buf_add_mem(buf, frame_data + offset, chunk_size);
udc_ep_enqueue(dev, buf);
```

Der `udc_dwc2.c` Treiber handhabt den Rest:
- SOF-Synchronisation (ISO Transfer pro Microframe)
- Double Buffering (bereits für UAC2 implementiert, PR #82135)
- Incomplete ISO IN Handling

### 4.3 Frame-Chunking

640×480 YUYV = 614400 Bytes.
Bei 3070 Bytes/Microframe = 200 Microframes = 25 ms pro Frame = 40 FPS max.

```c
while (offset < frame_size) {
    chunk = MIN(max_payload - 2, frame_size - offset);
    // Baue UVC Header
    header[0] = 2;
    header[1] = (offset + chunk >= frame_size) ? (fid | 0x02) : fid;  // EOF
    // Sende über UDC
    buf = build_iso_payload(header, frame_data + offset, chunk);
    udc_ep_enqueue(dev, buf);
    offset += chunk;
}
fid ^= 0x01;  // FID toggle für nächsten Frame
```

---

## Phase 5: UVC Probe/Commit (Tag 6-7)

### 5.1 Video Probe Control

Der Host verhandelt das Video-Format über Probe/Commit:
```
GET_CUR / SET_CUR / GET_MIN / GET_MAX / GET_DEF
auf VS_PROBE_CONTROL und VS_COMMIT_CONTROL
```

Zephyr's `usbd_uvc.c` hat bereits einen Probe/Commit Handler — aber nur für Bulk.
Änderung: `dwMaxPayloadTransferSize` auf ISO MPS setzen (3072 statt 512).

ST-Referenz (`usbx/usbx.c` Zeile 212):
```c
req.dwMaxPayloadTransferSize = is_hs() ? UVC_ISO_HS_MPS : UVC_ISO_FS_MPS;
// UVC_ISO_HS_MPS = 1024 * USBL_PACKET_PER_MICRO_FRAME = 3072
```

### 5.2 dwMaxVideoFrameSize
Muss korrekt auf die Frame-Größe gesetzt werden:
- 640×480 YUYV: 614400 Bytes
- Wird aus dem Video-Format berechnet

---

## Phase 6: Integration + Test (Tag 7-8)

### 6.1 Konfiguration
```conf
# prj.conf
CONFIG_USB_DEVICE_STACK_NEXT=y
CONFIG_UDC_BUF_POOL_SIZE=65536     # größer für ISO Buffers
CONFIG_VIDEO=y
CONFIG_VIDEO_SW_GENERATOR=y        # für Test ohne Kamera
CONFIG_VIDEO_BUFFER_POOL_SZ_MAX=614400
CONFIG_MAIN_STACK_SIZE=4096
```

### 6.2 DTS Overlay
```dts
&zephyr_udc0 {
    maximum-speed = "high-speed";
};
```

### 6.3 Test-Kriterien
- [ ] Windows Gerätemanager: "USB-Videogerät" oder "Xi640 UVC"
- [ ] USBTreeView: VID/PID korrekt, ISO EP 0x1400 wMaxPacketSize
- [ ] AMCap: Bild sichtbar (Colorbar)
- [ ] FPS > 20 bei 640×480 YUYV

---

## Risiken + Mitigation

| Risiko | Wahrscheinlichkeit | Mitigation |
|--------|-------------------|------------|
| `udc_dwc2.c` ISO auf N6 instabil | Mittel | UAC2 Audio funktioniert → ISO Layer OK |
| Zephyr UVC Refactoring komplex | Mittel | Minimal-Ansatz: nur EP-Typ + Alt Settings |
| DMA/RIFSC Problem kehrt zurück | Niedrig | Zephyr DWC2 Treiber hat eigene Workarounds |
| Probe/Commit inkompatibel | Niedrig | ST-Referenz als Vorlage |

---

## Zeitplan

| Tag | Aufgabe | Ergebnis |
|-----|---------|----------|
| 1 | Fork + Basis verifizieren | Bulk UVC funktioniert mit geforkem Code |
| 2-3 | Deskriptoren umbauen | ISO EP in Deskriptoren, Alt 0/1 |
| 3-4 | SET_INTERFACE Handler | Alt Setting Wechsel funktioniert |
| 4-6 | ISO Transfer implementieren | Erste ISO Pakete am Analyzer sichtbar |
| 6-7 | Probe/Commit anpassen | Windows erkennt Video-Format |
| 7-8 | Integration + FPS Test | 640×480 @ >20 FPS Colorbar |

**Gesamt: ~8 Arbeitstage** — realistisch bis ~25. März, gut vor dem 1. Mai Deadline.

---

## Referenz-Dateien

### Von ST (lesen, nicht kopieren):
- `uvcl_desc.c` — ISO Deskriptor-Aufbau, wMaxPacketSize-Berechnung
- `uvcl.c` Zeile 100-200 — UVC Payload Header + Chunking
- `usbx/usbx.c` Zeile 200-220 — Probe/Commit dwMaxPayloadTransferSize

### Von Zephyr (modifizieren):
- `usbd_uvc.c` — Hauptdatei, Deskriptoren + Transfer-Logik
- `usbd_uvc.h` — API Header

### Von Zephyr (lesen, als Referenz für ISO):
- `usbd_uac2.c` — ISO EP Handling im Zephyr device_next Stack
- `udc_dwc2.c` — DWC2 ISO Transfer-Implementierung
