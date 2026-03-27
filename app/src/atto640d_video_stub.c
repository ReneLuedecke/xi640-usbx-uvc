/**
 * @file atto640d_video_stub.c
 * @brief Lynred ATTO640D-04 Zephyr Video-Driver Stub
 *
 * Der DCMIPP-Treiber (video_stm32_dcmipp.c) benötigt ein Zephyr-Device am
 * Source-Endpoint (DT remote-endpoint-label). Dieses Stub-Device erfüllt
 * die compile-time und runtime Anforderungen:
 *
 * - Compile-time: DEVICE_DT_INST_DEFINE registriert das Device, damit
 *   SOURCE_DEV(inst) in video_stm32_dcmipp.c einen gültigen Ordinal hat.
 * - Runtime: Alle Video-API-Calls geben 0 zurück (No-Op).
 *   Die echte Hardware-Ansteuerung läuft über lynred_atto640d.c.
 *
 * Zephyr 4.3.x video_driver_api (include/zephyr/drivers/video.h):
 *   Mandatory: set_format, get_format, set_stream (bool enable), get_caps
 *   Optional:  enqueue, dequeue, flush, set_ctrl, ...
 *
 * Initialisierungspriorität 55 < 61 (DCMIPP) — Stub ist bereit bevor
 * der DCMIPP-Treiber device_is_ready(source_dev) prüft.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/video.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(atto640d_stub, LOG_LEVEL_INF);

#define DT_DRV_COMPAT lynred_atto640d_04

/* Unterstützte Ausgabeformate des ATTO640D-04 (via CPLD/AD9649)
 *
 * VIDEO_PIX_FMT_RGB565 = FourCC "RGBP" = VIDEO_FOURCC('R','G','B','P').
 * Warum RGB565 statt SBGGR8?
 *   - video_bits_per_pixel(RGB565) = 16 → Treiber setzt pitch = 640×2 = 1280
 *     automatisch korrekt (stm32_dcmipp_set_fmt Zeile 585:
 *     stm32_dcmipp_compute_fmt_pitch() überschreibt fmt->pitch = width×bpp/8).
 *     Mit SBGGR8 (bpp=8) wurde pitch=640 gesetzt — fmt->size=307200 falsch.
 *   - DCMIPP_FORMAT_RGB565 (0x22) wird via stream_enable() in PRCR FORMAT gesetzt;
 *     danach patchen wir auf MONO14B (0x4D) — Schrittweite näher als RAW8 (0x2A).
 *   - EDM: DCMIPP_INTERFACE_14BITS ist in stm32_dcmipp_conf_parallel() hardcodiert
 *     (Zeile 348) — unabhängig vom pixelformat immer korrekt.
 *
 * Muss mit CONFIG_VIDEO_STM32_DCMIPP_SENSOR_PIXEL_FORMAT="RGBP" übereinstimmen.
 * Dump-Pipe-Kompatibilität: format_support[] hat DUMP_PIPE_FMT(RGB565, RGB565) ✓
 * input_fmt_desc[] hat INPUT_FMT(RGB565, RGB565, ...) ✓
 */
static const struct video_format_cap atto640d_fmts[] = {
    {
        .pixelformat = VIDEO_PIX_FMT_RGB565,
        .width_min   = 640, .width_max   = 640, .width_step  = 0,
        .height_min  = 480, .height_max  = 480, .height_step = 0,
    },
    { 0 }  /* Sentinel */
};

/* Aktuell gesetztes Format (wird von DCMIPP set_format abgefragt) */
static struct video_format atto640d_current_fmt = {
    .type        = VIDEO_BUF_TYPE_OUTPUT,
    .pixelformat = VIDEO_PIX_FMT_RGB565,
    .width       = 640,
    .height      = 480,
    .pitch       = 640 * 2,  /* 2 Bytes/Pixel (uint16_t), 14-Bit ADC in uint16_t LE */
};

static int atto640d_set_format(const struct device *dev, struct video_format *fmt)
{
    ARG_UNUSED(dev);
    atto640d_current_fmt = *fmt;
    LOG_DBG("set_format: pixfmt=0x%08x %dx%d", fmt->pixelformat, fmt->width, fmt->height);
    return 0;
}

static int atto640d_get_format(const struct device *dev, struct video_format *fmt)
{
    ARG_UNUSED(dev);
    *fmt = atto640d_current_fmt;
    return 0;
}

static int atto640d_get_caps(const struct device *dev, struct video_caps *caps)
{
    ARG_UNUSED(dev);
    caps->format_caps = atto640d_fmts;
    return 0;
}

/* set_stream: enable=true → stream start, enable=false → stream stop */
static int atto640d_set_stream(const struct device *dev, bool enable,
                               enum video_buf_type type)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(type);
    /* Hardware-Streaming läuft über atto640d_init() / atto640d_shutdown() */
    LOG_DBG("set_stream: %s (hardware managed by atto640d_init)",
            enable ? "start" : "stop");
    return 0;
}

static const struct video_driver_api atto640d_stub_api = {
    /* Mandatory callbacks */
    .set_format = atto640d_set_format,
    .get_format = atto640d_get_format,
    .set_stream = atto640d_set_stream,
    .get_caps   = atto640d_get_caps,
    /* Optional callbacks: enqueue/dequeue not needed for source-only stub */
};

static int atto640d_stub_init(const struct device *dev)
{
    ARG_UNUSED(dev);
    LOG_INF("ATTO640D Video-Stub bereit (Zephyr Device-Kette für DCMIPP)");
    return 0;
}

/* Priorität 55 < 61 (DCMIPP) → Stub ist bereit bevor DCMIPP device_is_ready() prüft */
#define ATTO640D_STUB_DEFINE(inst)                      \
    DEVICE_DT_INST_DEFINE(inst,                         \
                          atto640d_stub_init, NULL,     \
                          NULL, NULL,                   \
                          POST_KERNEL, 55,              \
                          &atto640d_stub_api);

DT_INST_FOREACH_STATUS_OKAY(ATTO640D_STUB_DEFINE)
