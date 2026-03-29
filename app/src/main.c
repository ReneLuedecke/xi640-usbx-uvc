/*
 * Copyright (c) 2025 tinyVision.ai Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Phase ISO-1: DCMIPP HAL-Bypass Streaming — echte Kamera-Pipeline.
 * DCMIPP liefert Frames via DMA, Security-Init (RIFSC/RISAF) freigegeben.
 * Timer-Simulation entfernt.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sample_usbd.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/drivers/video.h>
#include <zephyr/video/video.h>          /* video_import_buffer */
#include <zephyr/logging/log.h>
#include <zephyr/usb/class/usbd_uvc.h>
#include <zephyr/cache.h>

#include "lynred_atto640d.h"

#include <soc.h>              /* STM32N6 HAL: RIF, RCC, NVIC, SCB_NS */
#include "dcmipp_capture.h"

LOG_MODULE_REGISTER(uvc_sample, LOG_LEVEL_INF);

/* UVC TX-Buffer in AXISRAM3/4 — DWC2 DMA erreicht AXISRAM (AXI-Bus).
 * PSRAM (0x90000000) war der Crash-Auslöser: DWC2 DMA kann XSPI-mapped PSRAM
 * nicht lesen (RISAF für XSPI-Region fehlte für USB-DMA-Master-CID).
 * AXISRAM3 (0x34200000, 512 KB) + AXISRAM4 (0x34280000, 512 KB) aktiviert im Overlay.
 * buf0: 0x34200000..0x34296000 (614400 B, AXISRAM3+4)
 * buf1: 0x34296000..0x3432C000 (614400 B, AXISRAM4+5) */
#define UVC_AXISRAM_BUF0  0x34200000UL
#define UVC_AXISRAM_BUF1  0x34296000UL
/* Wrapper-Structs: nur .index und .type werden von video_enqueue() genutzt.
 * video_import_buffer() trägt .buffer/.size in video_buf[idx] ein. */
static struct video_buffer uvc_static_bufs[2];

const static struct device *const uvc_dev = DEVICE_DT_GET(DT_NODELABEL(uvc));

/* sw-generator: Format-Advertisement + optionaler Colorbar-Diagnose-Pfad.
 * CONFIG_XI640_UVC_SOURCE_DCMIPP=n → sw-generator liefert echte Colorbar-Daten an UVC. */
const static struct device *const video_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_camera));

static struct video_caps video_caps = {.type = VIDEO_BUF_TYPE_OUTPUT};

/* ---- Benchmark-Statistik ---- */
static struct {
	uint32_t frames;
	uint32_t bytes;
	int64_t  start_ms;
} bench;

static void bench_reset(uint32_t frame_size)
{
	bench.frames   = 0;
	bench.bytes    = 0;
	bench.start_ms = k_uptime_get();
	LOG_INF("Benchmark gestartet, Frame-Groesse: %u Bytes",
		frame_size);
}

static void bench_update(uint32_t frame_size)
{
	bench.frames++;
	bench.bytes += frame_size;

	if (bench.frames % 30 == 0) {
		int64_t elapsed = k_uptime_get() - bench.start_ms;

		if (elapsed > 0) {
			uint32_t fps  = (bench.frames * 1000U) / (uint32_t)elapsed;
			uint32_t kbps = (uint32_t)((uint64_t)bench.bytes * 8U /
						   (uint32_t)elapsed);

			LOG_INF("Bench: %u Frames | %u FPS | %u kbit/s",
				bench.frames, fps, kbps);
		}
	}
}
/* ---- Ende Benchmark ---- */

/* ---- Format-Setup (nutzt video_caps vom sw-generator) ---- */

static bool app_is_supported_format(uint32_t pixfmt)
{
	return pixfmt == VIDEO_PIX_FMT_JPEG ||
	       pixfmt == VIDEO_PIX_FMT_YUYV ||
	       pixfmt == VIDEO_PIX_FMT_NV12 ||
	       pixfmt == VIDEO_PIX_FMT_H264;
}

static bool app_has_supported_format(void)
{
	const struct video_format_cap *const fmts = video_caps.format_caps;

	for (int i = 0; fmts[i].pixelformat != 0; i++) {
		if (app_is_supported_format(fmts[i].pixelformat)) {
			return true;
		}
	}
	return false;
}

static int app_add_format(uint32_t pixfmt, uint32_t width, uint32_t height, bool has_sup_fmts)
{
	struct video_format fmt = {
		.pixelformat = pixfmt,
		.width = width,
		.height = height,
		.type = VIDEO_BUF_TYPE_OUTPUT,
	};
	int ret;

	if (has_sup_fmts && !app_is_supported_format(pixfmt)) {
		return 0;
	}

	/* Größe berechnen lassen — video_dev (sw-gen) als Referenz */
	ret = video_set_compose_format(video_dev, &fmt);
	if (ret != 0) {
		LOG_ERR("Format %s %ux%u nicht setzbar",
			VIDEO_FOURCC_TO_STR(fmt.pixelformat), fmt.width, fmt.height);
		return ret;
	}

	if (fmt.size > CONFIG_VIDEO_BUFFER_POOL_SZ_MAX) {
		LOG_WRN("Format %ux%u uebersprungen (%u > %u)",
			fmt.width, fmt.height, fmt.size, CONFIG_VIDEO_BUFFER_POOL_SZ_MAX);
		return 0;
	}

	ret = uvc_add_format(uvc_dev, &fmt);
	if (ret == -ENOMEM) {
		return 0;
	}

	LOG_INF("Format hinzugefuegt: %s %ux%u (%u Bytes)",
		VIDEO_FOURCC_TO_STR(fmt.pixelformat), fmt.width, fmt.height, fmt.size);
	return ret;
}

struct video_resolution {
	uint16_t width;
	uint16_t height;
};

static struct video_resolution video_common_fmts[] = {
	{ .width = 160,  .height = 120  },
	{ .width = 320,  .height = 240  },
	{ .width = 640,  .height = 480  },
	{ .width = 854,  .height = 480  },
	{ .width = 800,  .height = 600  },
	{ .width = 1280, .height = 720  },
	{ .width = 1280, .height = 1024 },
	{ .width = 1920, .height = 1080 },
	{ .width = 3840, .height = 2160 },
};

static int app_add_filtered_formats(void)
{
	const bool has_sup_fmts = app_has_supported_format();
	int ret;

	for (int i = 0; video_caps.format_caps[i].pixelformat != 0; i++) {
		const struct video_format_cap *vcap = &video_caps.format_caps[i];
		uint32_t pixelformat = vcap->pixelformat;
		int count = 1;

		ret = app_add_format(pixelformat, vcap->width_min, vcap->height_min, has_sup_fmts);
		if (ret != 0) {
			return ret;
		}

		if (vcap->width_min != vcap->width_max || vcap->height_min != vcap->height_max) {
			ret = app_add_format(pixelformat, vcap->width_max, vcap->height_max,
					     has_sup_fmts);
			if (ret != 0) {
				return ret;
			}
			count++;
		}

		if (vcap->width_step == 0 && vcap->height_step == 0) {
			continue;
		}

		for (int j = 0; j < ARRAY_SIZE(video_common_fmts); j++) {
			if (count >= CONFIG_APP_VIDEO_MAX_RESOLUTIONS) {
				break;
			}
			if (!IN_RANGE(video_common_fmts[j].width,
				      vcap->width_min, vcap->width_max) ||
			    !IN_RANGE(video_common_fmts[j].height,
				      vcap->height_min, vcap->height_max)) {
				continue;
			}
			if ((video_common_fmts[j].width - vcap->width_min) % vcap->width_step ||
			    (video_common_fmts[j].height - vcap->height_min) % vcap->height_step) {
				continue;
			}
			ret = app_add_format(pixelformat, video_common_fmts[j].width,
					     video_common_fmts[j].height, has_sup_fmts);
			if (ret != 0) {
				return ret;
			}
			count++;
		}
	}

	return 0;
}

int main(void)
{
	struct usbd_context *sample_usbd;
	struct video_format fmt = {0};
	struct video_frmival frmival = {0};
	int ret;

	/* ---- Lynred ATTO640D-04 Bolometer Init (vor DCMIPP und USB!) ---- */
	/* MC muss laufen bevor DCMIPP Daten empfangen kann. */
	int atto_ret = atto640d_init();

	if (atto_ret == ATTO640D_OK) {
		LOG_INF("ATTO640D: Sequencer aktiv, Free-run @ 33 MHz");
	} else {
		LOG_ERR("ATTO640D init FAILED: %d", atto_ret);
		/* Weiterlaufen mit sw-generator (kein echter Sensor) */
	}

	/* ── Security-Init: DCMIPP RIF-Zugriff freigeben ──────────────────────
	 * MUSS vor dcmipp_capture_init() stehen!
	 * Ohne diese Konfiguration: DCMIPP IRQ zeigt auf Flash → Hard Fault.
	 * Identisch mit P16 main.c (bewährt, getestet). */

	/* APB5 Bus + DCMIPP Peripheral Clock */
	RCC->BUSENR  |= RCC_BUSENR_APB5EN;
	RCC->APB5ENR |= RCC_APB5ENR_DCMIPPEN;
	LOG_INF("DCMIPP: APB5EN + DCMIPPEN gesetzt (0x%08x)", RCC->APB5ENR);

	/* RIFSC: DMA-Master CID-Konfiguration für alle aktiven DMA-Master.
	 *
	 * Ohne RIMC-Konfiguration hat ein DMA-Master CID_NONE und kann nur
	 * auf RISAF-Regionen ohne CID-Filter zugreifen (z.B. AXISRAM1 Default).
	 * AXISRAM3-6 sind nach RAMCFG-Aktivierung CID-geschützt → DMA Master
	 * braucht CID1-Zuweisung um darauf zuzugreifen.
	 *
	 * DCMIPP (Index 9): Liest Pixel vom CPLD, schreibt nach AXISRAM1.
	 * OTG1   (Index 4): USB OTG HS DMA liest UVC-Frame aus AXISRAM3/4.
	 * OTG2   (Index 5): Zweiter OTG-Kanal (zur Sicherheit mitkonfiguriert). */
	__HAL_RCC_RIFSC_CLK_ENABLE();
	{
		RIMC_MasterConfig_t rimc = { 0 };
		rimc.MasterCID = RIF_CID_1;
		rimc.SecPriv   = RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV;

		HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_DCMIPP, &rimc);
		HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_DCMIPP,
						      RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);

		/* USB OTG DMA-Master: AXISRAM3/4 als UVC TX-Buffer erreichbar machen. */
		HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_OTG1, &rimc);
		HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_OTG2, &rimc);
	}
	LOG_INF("RIMC: DCMIPP + OTG1 + OTG2 → CID1 Secure+Priv gesetzt");

	/* Security-Fix: ALLE IRQs auf Secure zwingen + VTOR_NS korrekt setzen.
	 *
	 * ROOT CAUSE des Hard Faults:
	 *   USB OTG HS IRQ (ca. IRQ 77, ITNS[2] bit 13) war Non-Secure (ITNS-Bit gesetzt).
	 *   Beim ersten USB-Interrupt nach Stream-Start:
	 *     → CPU wechselt in Non-Secure Handler Mode
	 *     → liest Vektortabelle aus VTOR_NS = 0x34180400 (Secure-Alias!)
	 *     → Non-Secure CPU darf 0x34xxxxxx nicht lesen → Bus Fault → VECTTBL HardFault
	 *
	 * Fix A: ALLE ITNS-Register löschen → kein IRQ mehr Non-Secure.
	 *   Zephyr läuft vollständig im Secure-Modus — Non-Secure IRQs sind unerwünscht.
	 *
	 * Fix B: VTOR_NS auf NS-Alias setzen (0x24xxxxxx statt 0x34xxxxxx).
	 *   Bit 28 unterscheidet Secure- (0x3x) von Non-Secure-Alias (0x2x) auf STM32N6.
	 *   Auch wenn kein IRQ mehr NS ist: VTOR_NS korrekt halten (Sicherheitsnetz). */
	{
		uint32_t vtor_s = SCB->VTOR;
		unsigned int irq_key;

		/* Fix A: Alle 16 ITNS-Register löschen (deckt IRQ 0..511) */
		irq_key = irq_lock();
		for (int _i = 0; _i < (int)ARRAY_SIZE(NVIC->ITNS); _i++) {
			NVIC->ITNS[_i] = 0U;
		}
		irq_unlock(irq_key);

		/* Fix B: VTOR_NS = NS-Alias (0x34xxxxxx → 0x24xxxxxx, bit28 löschen) */
		SCB_NS->VTOR = vtor_s & ~(1UL << 28);

		LOG_INF("Security-Fix: alle ITNS=0, VTOR_S=0x%08x VTOR_NS=0x%08x",
			SCB->VTOR, SCB_NS->VTOR);
	}

	/* DCMIPP Parallel-Interface Init (HAL Bypass Modus) */
	LOG_INF("=== DCMIPP Capture Init ===");
	ret = dcmipp_capture_init();
	if (ret != 0) {
		LOG_ERR("dcmipp_capture_init fehlgeschlagen: %d", ret);
		return ret;
	}
	LOG_INF("DCMIPP: HAL-Bypass Streaming aktiv");

	/* ---- USB: Format-Advertisement (sw-generator als Metadaten-Quelle) ---- */

	if (!device_is_ready(video_dev)) {
		LOG_ERR("video_dev %s nicht bereit", video_dev->name);
		return -ENODEV;
	}
	ret = video_get_caps(video_dev, &video_caps);
	if (ret != 0) {
		LOG_ERR("video_get_caps fehlgeschlagen: %d", ret);
		return ret;
	}
	uvc_set_video_dev(uvc_dev, video_dev);
	ret = app_add_filtered_formats();
	if (ret != 0) {
		return ret;
	}

	/* ---- USB Stack starten ---- */
	sample_usbd = sample_usbd_init_device(NULL);
	if (sample_usbd == NULL) {
		return -ENODEV;
	}
	ret = usbd_enable(sample_usbd);
	if (ret != 0) {
		return ret;
	}

	/* ---- Warte auf Host Format-Auswahl ---- */
	LOG_INF("Warte auf Host Format-Auswahl...");
	while (true) {
		fmt.type = VIDEO_BUF_TYPE_INPUT;
		ret = video_get_format(uvc_dev, &fmt);
		if (ret == 0) {
			break;
		}
		if (ret != -EAGAIN) {
			LOG_ERR("video_get_format fehlgeschlagen: %d", ret);
			return ret;
		}
		k_sleep(K_MSEC(10));
	}
	ret = video_get_frmival(uvc_dev, &frmival);
	if (ret != 0) {
		LOG_WRN("video_get_frmival fehlgeschlagen: %d", ret);
	}
	LOG_INF("Host gewaehlt: %s %ux%u @ %u/%u",
		VIDEO_FOURCC_TO_STR(fmt.pixelformat), fmt.width, fmt.height,
		frmival.denominator, frmival.numerator);
	LOG_WRN("USB Speed: %u (0=FS 1=HS)",
		usbd_bus_speed(sample_usbd));

	/* ── Streaming-Loop: DCMIPP oder SW-Generator → UVC ──────────────────
	 * CONFIG_XI640_UVC_SOURCE_DCMIPP=y (default): DCMIPP liefert Frames.
	 * CONFIG_XI640_UVC_SOURCE_DCMIPP=n:           SW-Generator Colorbar.
	 *   Diagnose-Build: west build ... -- -DCONFIG_XI640_UVC_SOURCE_DCMIPP=n */

#if CONFIG_XI640_UVC_SOURCE_DCMIPP
	/* ── DCMIPP → UVC (Produktionspfad) ────────────────────────────────
	 * RIMC für OTG1+OTG2 in Security-Init gesetzt → DWC2 DMA hat CID1.
	 * UVC TX Buffer in AXISRAM3/4 — DWC2 DMA + CPU nutzen denselben CID1. */
	LOG_INF("=== DCMIPP→UVC Streaming ===");

	/* video_import_buffer() registriert externe AXISRAM-Adressen in video_buf[].
	 * video_enqueue() benutzt &video_buf[buf->index], NICHT buf->buffer direkt.
	 * Ohne import wäre video_buf[0].buffer = NULL → UVC-Treiber greift auf NULL zu. */
	{
		uint16_t idx;

		ret = video_import_buffer((uint8_t *)UVC_AXISRAM_BUF0, fmt.size, &idx);
		if (ret != 0) {
			LOG_ERR("video_import_buffer[0] fehlgeschlagen: %d", ret);
			return ret;
		}
		uvc_static_bufs[0].index = idx;
		uvc_static_bufs[0].type  = VIDEO_BUF_TYPE_INPUT;
		LOG_INF("AXISRAM BUF0 @ 0x%08x → video_buf[%u]",
			UVC_AXISRAM_BUF0, idx);

		ret = video_import_buffer((uint8_t *)UVC_AXISRAM_BUF1, fmt.size, &idx);
		if (ret != 0) {
			LOG_ERR("video_import_buffer[1] fehlgeschlagen: %d", ret);
			return ret;
		}
		uvc_static_bufs[1].index = idx;
		uvc_static_bufs[1].type  = VIDEO_BUF_TYPE_INPUT;
		LOG_INF("AXISRAM BUF1 @ 0x%08x → video_buf[%u]",
			UVC_AXISRAM_BUF1, idx);
	}

	/* Initial-Enqueue: bytesused=0 (von video_import_buffer gesetzt).
	 * UVC sendet leere Frames (nur Header) bis DCMIPP Daten liefert — safe. */
	ret = video_enqueue(uvc_dev, &uvc_static_bufs[0]);
	if (ret != 0) {
		LOG_ERR("initial enqueue[0] fehlgeschlagen: %d", ret);
		return ret;
	}
	ret = video_enqueue(uvc_dev, &uvc_static_bufs[1]);
	if (ret != 0) {
		LOG_ERR("initial enqueue[1] fehlgeschlagen: %d", ret);
		return ret;
	}
	LOG_INF("DCMIPP→UVC: beide Buffer eingereiht — starte Loop");

	{
		uint8_t            *frame_data;
		uint32_t            frame_size;
		uint32_t            frame_count = 0U;
		struct video_buffer *done_buf;

		while (true) {
			ret = dcmipp_capture_get_frame(&frame_data, &frame_size,
						       100);
			if (ret == -ETIMEDOUT) {
				k_sleep(K_MSEC(1));
				continue;
			}
			if (ret != 0) {
				LOG_ERR("DCMIPP Fehler: %d", ret);
				break;
			}

			ret = video_dequeue(uvc_dev, &done_buf, K_NO_WAIT);
			if (ret != 0) {
				k_sleep(K_MSEC(1));
				continue;
			}

			memcpy(done_buf->buffer, frame_data,
			       MIN(frame_size, fmt.size));
			sys_cache_data_flush_range(done_buf->buffer,
						   MIN(frame_size, fmt.size));
			done_buf->bytesused = fmt.size;
			done_buf->type      = VIDEO_BUF_TYPE_INPUT;

			frame_count++;
			if (frame_count <= 5U || frame_count % 50U == 0U) {
				LOG_INF("DCMIPP→UVC frame %u buf=%p",
					frame_count, (void *)done_buf->buffer);
			}
			ret = video_enqueue(uvc_dev, done_buf);
			if (ret != 0) {
				LOG_ERR("video_enqueue fehlgeschlagen: %d", ret);
				break;
			}
			bench_update(fmt.size);
		}
	}

#else /* CONFIG_XI640_UVC_SOURCE_DCMIPP=n → SW-Generator Diagnose-Pfad */

	/* ── SW-Generator Colorbar → UVC (Diagnose-Modus) ──────────────────
	 * DCMIPP läuft im Hintergrund (Init lief durch), liefert aber nicht
	 * an USB. SW-Generator Colorbar verifiziert die UVC-Pipeline isoliert.
	 * Buffers kommen vom Zephyr Heap (AXISRAM1) — DWC2 DMA-Zugriff bekannt OK.
	 *
	 * Wenn dies crasht → P16-Änderungen haben UVC/Security kaputt gemacht.
	 * Wenn dies läuft  → Problem liegt im DCMIPP→UVC Übergabe-Code. */
	LOG_INF("=== SW-Generator Colorbar → UVC (Diagnose) ===");
	LOG_INF("SW-Gen: video_dev=%p ready=%d fmt.size=%u",
		(void *)video_dev, device_is_ready(video_dev), fmt.size);

	{
		struct video_buffer *swgen_bufs[2];
		struct video_buffer *vbuf;
		struct video_buffer *done_buf;
		uint32_t             frame_count = 0U;
		struct video_format  swgen_fmt   = fmt;

		swgen_fmt.type = VIDEO_BUF_TYPE_OUTPUT;
		LOG_INF("SW-Gen: video_set_format...");
		ret = video_set_format(video_dev, &swgen_fmt);
		if (ret != 0) {
			LOG_WRN("video_set_format(sw-gen) Fehler: %d", ret);
		}
		LOG_INF("SW-Gen: video_set_format ret=%d", ret);

		for (int i = 0; i < 2; i++) {
			LOG_INF("SW-Gen: alloc buf[%d] size=%u (K_NO_WAIT)...",
				i, fmt.size);
			/* K_NO_WAIT statt K_FOREVER — blockiert nicht bei Heap-Engpass */
			swgen_bufs[i] = video_buffer_alloc(fmt.size, K_NO_WAIT);
			if (!swgen_bufs[i]) {
				LOG_ERR("video_buffer_alloc fehlgeschlagen (%d) — "
					"Heap zu klein fuer %u Bytes?", i, fmt.size);
				return -ENOMEM;
			}
			LOG_INF("SW-Gen: buf[%d] @ %p", i,
				(void *)swgen_bufs[i]->buffer);
			swgen_bufs[i]->type = VIDEO_BUF_TYPE_OUTPUT;
			LOG_INF("SW-Gen: enqueue buf[%d]...", i);
			ret = video_enqueue(video_dev, swgen_bufs[i]);
			if (ret != 0) {
				LOG_ERR("video_enqueue(sw-gen %d) Fehler: %d",
					i, ret);
				return ret;
			}
			LOG_INF("SW-Gen buf[%d] @ %p enqueued OK", i,
				(void *)swgen_bufs[i]->buffer);
		}

		LOG_INF("SW-Gen: starte stream (video_stream_start)...");
		ret = video_stream_start(video_dev, VIDEO_BUF_TYPE_OUTPUT);
		if (ret != 0) {
			LOG_ERR("video_stream_start(sw-gen) Fehler: %d", ret);
			return ret;
		}
		LOG_INF("SW-Gen: stream gestartet, beginne Loop");

		while (true) {
			/* 1. Colorbar-Frame holen */
			LOG_DBG("SW-Gen: warte auf Frame von video_dev...");
			ret = video_dequeue(video_dev, &vbuf, K_MSEC(200));
			if (ret != 0) {
				LOG_WRN("SW-Gen dequeue timeout (frame %u) ret=%d",
					frame_count, ret);
				continue;
			}

			/* 2. Cache flush — CPU hat Colorbar erzeugt → DMA-lesbar */
			sys_cache_data_flush_range(vbuf->buffer,
						   vbuf->bytesused);
			vbuf->type = VIDEO_BUF_TYPE_INPUT;

			/* 3. An UVC übergeben */
			LOG_DBG("SW-Gen: enqueue Frame an uvc_dev...");
			ret = video_enqueue(uvc_dev, vbuf);
			if (ret != 0) {
				LOG_ERR("video_enqueue(uvc) Fehler: %d", ret);
				break;
			}
			frame_count++;
			if (frame_count <= 5U || frame_count % 30U == 0U) {
				LOG_INF("SW-Gen→UVC frame %u buf=%p",
					frame_count, (void *)vbuf->buffer);
			}

			/* 4. Fertig-Buffer von UVC zurückfordern */
			LOG_DBG("SW-Gen: warte auf fertig-Buffer von uvc_dev...");
			ret = video_dequeue(uvc_dev, &done_buf, K_MSEC(500));
			if (ret != 0) {
				LOG_WRN("UVC dequeue timeout (frame %u) ret=%d",
					frame_count, ret);
				continue;
			}

			/* 5. Buffer an sw-generator zurückgeben */
			done_buf->type = VIDEO_BUF_TYPE_OUTPUT;
			LOG_DBG("SW-Gen: gebe Buffer zurueck an video_dev...");
			video_enqueue(video_dev, done_buf);

			bench_update(fmt.size);
		}
	}

#endif /* CONFIG_XI640_UVC_SOURCE_DCMIPP */

	return 0;
}
