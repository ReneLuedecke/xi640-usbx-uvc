# Xi640 — Memory Layout

## STM32N6570 Speicherregionen

| Region | Adresse | Größe | Verwendung |
|--------|---------|-------|------------|
| AXISRAM1 | 0x34000000 | 2 MB | Zephyr SRAM (Heap, Stacks) |
| AXISRAM2 | 0x34180400 | 511 KB | FSBL Code + BSS |
| AXISRAM3 | 0x34200000 | 512 KB | Video Frame Buffer A |
| AXISRAM4 | 0x34280000 | 512 KB | Video Frame Buffer B |
| AXISRAM5 | 0x34300000 | 512 KB | VENC Input Buffer |
| AXISRAM6 | 0x34380000 | 512 KB | VENC Output / Netzwerk |
| VENC RAM | 0x34400000 | 256 KB | H.264 Encoder intern |
| PSRAM | 0x90000000 | 32 MB | Große Buffer, Netzwerk |
| Flash | 0x70000000 | 128 MB | Code (XIP) |

## Hot/Cold Memory Regeln

**HOT (AXISRAM — kein Cache-Problem):**
- ISR-Handler
- DMA Descriptors
- USB ISO Transfer Buffer
- VENC EWL Speicher
- Zephyr Kernel-Objekte

**COLD (PSRAM — Cache-Konzept erforderlich):**
- Netzwerk-Buffer (TX/RX)
- JPEG/H.264 Output
- Logging Buffer
- Große Lookup-Tables

## Cache-Regeln (Cortex-M55)

```c
// VOR DMA-Transfer: Cache flush (CPU → Memory)
SCB_CleanDCache_by_Addr((uint32_t *)buf, size);

// NACH DMA-Transfer: Cache invalidate (Memory → CPU)
SCB_InvalidateDCache_by_Addr((uint32_t *)buf, size);

// Alignment: 32 Byte (Cache-Line Größe)
__attribute__((aligned(32))) uint8_t dma_buf[SIZE];
```

## PSRAM Konfiguration (APS256XXN)

```dts
&aps256xxn_obr {
    max-frequency = <DT_FREQ_M(100)>;  /* 200 MHz instabil */
    read-latency  = <4>;
    write-latency = <1>;
    fixed-latency;
    burst-length  = <3>;
    rbx;
    st,csbound    = <11>;
    st,refresh    = <320>;             /* Pflichtfeld ab Zephyr 4.4 */
};
```

**Performance bei 100 MHz:** ~109 MB/s Write, ~202 MB/s Read

## USB ISO Buffer

```
AXISRAM3: 0x34200000
  [0x000000] Frame Buffer Ping  (614.400 B = 0x96000)
  [0x096000] Frame Buffer Pong  (614.400 B = 0x96000)
  [0x12C000] USBX ISO TX Buffer (49.152 B  = 0xC000)
  [0x138000] frei
```
