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
#include <zephyr/logging/log.h>
#include <zephyr/usb/class/usbd_uvc.h>

#include "lynred_atto640d.h"

#include <soc.h>              /* STM32N6 HAL: RIF, RCC, NVIC, SCB_NS */
#include "dcmipp_capture.h"

LOG_MODULE_REGISTER(uvc_sample, LOG_LEVEL_INF);


const static struct device *const uvc_dev = DEVICE_DT_GET(DT_NODELABEL(uvc));

/* sw-generator: nur für uvc_set_video_dev() + Format-Advertisement nötig.
 * Wird NICHT für Streaming verwendet. */
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
	LOG_INF("Benchmark gestartet, Frame-Groesse: %u Bytes (DCMIPP-Quelle)",
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
	struct video_buffer *done_buf;
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

	/* RIFSC: DCMIPP DMA-Master + Peripheral-Slave → CID1 Secure+Priv */
	__HAL_RCC_RIFSC_CLK_ENABLE();
	{
		RIMC_MasterConfig_t rimc = { 0 };
		rimc.MasterCID = RIF_CID_1;
		rimc.SecPriv   = RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV;
		HAL_RIF_RIMC_ConfigMasterAttributes(RIF_MASTER_INDEX_DCMIPP, &rimc);
		HAL_RIF_RISC_SetSlaveSecureAttributes(RIF_RISC_PERIPH_INDEX_DCMIPP,
						      RIF_ATTRIBUTE_SEC | RIF_ATTRIBUTE_PRIV);
	}
	LOG_INF("DCMIPP RIFSC: CID1 Secure+Priv gesetzt");

	/* DCMIPP IRQ48 Fix: ITNS[1] bit16 auf Secure zwingen + SCB_NS->VTOR korrigieren.
	 * Bug: ITNS[1] bit16=1 (NonSecure) → Vektor liest aus SCB_NS->VTOR → Flash
	 * (0x18005abc) → Hard Fault. Fix A: bit löschen. Fix B: NS-VTOR korrigieren. */
	{
		uint32_t vtor_s = SCB->VTOR;

		if (NVIC->ITNS[1] & (1U << 16)) {
			LOG_WRN("ITNS bit16 gesetzt — korrigiere DCMIPP auf Secure");
			irq_disable(48);
			NVIC->ITNS[1] &= ~(1U << 16);
			irq_enable(48);
		}
		if (SCB_NS->VTOR != vtor_s) {
			LOG_WRN("SCB_NS->VTOR falsch (0x%08x), korrigiere",
				SCB_NS->VTOR);
			SCB_NS->VTOR = vtor_s;
		}
		LOG_INF("DCMIPP IRQ: ITNS[1]=0x%08x VTOR_NS=0x%08x",
			NVIC->ITNS[1], SCB_NS->VTOR);
	}

	/* RISAF1: DCMIPP DMA → PSRAM (0x90000000) write access für CID1 */
	{
		RISAF_BaseRegionConfig_t risaf_cfg = { 0 };

		risaf_cfg.Filtering      = RISAF_FILTER_ENABLE;
		risaf_cfg.Secure         = RIF_ATTRIBUTE_SEC;
		risaf_cfg.PrivWhitelist  = RIF_CID_1;
		risaf_cfg.ReadWhitelist  = RIF_CID_1;
		risaf_cfg.WriteWhitelist = RIF_CID_1;
		risaf_cfg.StartAddress   = 0x00000000U;
		risaf_cfg.EndAddress     = 0x01FFFFFFU;
		HAL_RIF_RISAF_ConfigBaseRegion(RISAF1, RISAF_REGION_1, &risaf_cfg);
		LOG_INF("RISAF1: CID1 PSRAM konfiguriert");
	}

	/* DCMIPP Parallel-Interface Init (HAL Bypass Modus) */
	LOG_INF("=== DCMIPP Capture Init ===");
	ret = dcmipp_capture_init();
	if (ret != 0) {
		LOG_ERR("dcmipp_capture_init fehlgeschlagen: %d", ret);
		return ret;
	}
	LOG_INF("DCMIPP: HAL-Bypass Streaming aktiv");

	/* ---- Setup: video_dev (sw-generator) nur fuer Format-Advertisement ---- */

	if (!device_is_ready(video_dev)) {
		LOG_ERR("video_dev %s nicht bereit", video_dev->name);
		return -ENODEV;
	}

	ret = video_get_caps(video_dev, &video_caps);
	if (ret != 0) {
		LOG_ERR("video_get_caps fehlgeschlagen: %d", ret);
		return ret;
	}

	/* Verknuepft uvc_dev mit video_dev fuer Descriptor-Generierung.
	 * sw-generator liefert KEINE Frames — nur Metadaten fuer Setup. */
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
	LOG_WRN("USB Speed: %u (0=FS 1=HS), MPS: %u",
		usbd_bus_speed(sample_usbd),
		(usbd_bus_speed(sample_usbd) == USBD_SPEED_HS) ? 512U : 64U);

	/* ---- Buffer-Allokation ---- */

	const uint32_t frame_size = fmt.size;
	const int buf_count = CONFIG_VIDEO_BUFFER_POOL_NUM_MAX;

	LOG_INF("Alloziere %d Buffers a %u Bytes", buf_count, frame_size);

	struct video_buffer *bufs[CONFIG_VIDEO_BUFFER_POOL_NUM_MAX];

	for (int i = 0; i < buf_count; i++) {
		bufs[i] = video_buffer_aligned_alloc(frame_size, CONFIG_VIDEO_BUFFER_POOL_ALIGN,
						     K_NO_WAIT);
		if (bufs[i] == NULL) {
			LOG_ERR("video_buffer_alloc[%d] fehlgeschlagen", i);
			return -ENOMEM;
		}
		bufs[i]->bytesused = 0;
		bufs[i]->type = VIDEO_BUF_TYPE_INPUT;
	}

	/* ── Buffer-Queue initial befüllen ─────────────────────────────────── */
	bench_reset(frame_size);

	for (int i = 0; i < buf_count; i++) {
		/* Puffer initialisieren: YUYV Grau als Platzhalter */
		memset(bufs[i]->buffer, 0x80, frame_size);
		bufs[i]->bytesused = frame_size;
		bufs[i]->type = VIDEO_BUF_TYPE_INPUT;
		ret = video_enqueue(uvc_dev, bufs[i]);
		if (ret != 0) {
			LOG_ERR("video_enqueue[%d] initial fehlgeschlagen: %d",
				i, ret);
			return ret;
		}
	}

	/* ── DCMIPP Erster Frame — Sensor + Pipeline prüfen ────────────────── */
	{
		uint8_t *test_data;
		uint32_t test_size;

		LOG_INF("Warte auf ersten DCMIPP-Frame (max 2 s)...");
		ret = dcmipp_capture_get_frame(&test_data, &test_size, 2000);
		if (ret != 0) {
			LOG_ERR("Kein DCMIPP-Frame nach 2 s (ret=%d) — "
				"Sensor-Problem?", ret);
		} else {
			uint16_t *px = (uint16_t *)test_data;

			LOG_INF("DCMIPP OK: [0]=%u [1]=%u [2]=%u [3]=%u [4]=%u (14-Bit)",
				px[0] & 0x3FFF, px[1] & 0x3FFF, px[2] & 0x3FFF,
				px[3] & 0x3FFF, px[4] & 0x3FFF);
		}
	}

	/* ── DCMIPP → UVC Streaming-Loop ────────────────────────────────────── */
	while (true) {
		uint8_t *dcmipp_data;
		uint32_t dcmipp_size;

		/* 1. Warte auf DCMIPP-Frame (blockiert bis DMA fertig, max 100 ms) */
		ret = dcmipp_capture_get_frame(&dcmipp_data, &dcmipp_size, 100);
		if (ret == -ETIMEDOUT) {
			LOG_WRN("DCMIPP Timeout — Sensor antwortet nicht");
			k_sleep(K_MSEC(1));
			continue;
		}
		if (ret != 0) {
			LOG_ERR("DCMIPP Fehler: %d", ret);
			break;
		}

		/* 2. UVC-Buffer holen (K_NO_WAIT: Frame droppen wenn USB zu langsam) */
		ret = video_dequeue(uvc_dev, &done_buf, K_NO_WAIT);
		if (ret != 0) {
			LOG_DBG("Frame gedroppt — kein UVC-Buffer frei");
			continue;
		}

		bench_update(frame_size);

		/* 3. DCMIPP-Daten → UVC-Buffer kopieren (Option A: raw passthrough).
		 * ATTO640D liefert 14-Bit in uint16_t — erscheint als verfärbtes Bild.
		 * Für Graustufen-YUYV: Option B im Plan aktivieren. */
		memcpy(done_buf->buffer, dcmipp_data,
		       MIN(dcmipp_size, frame_size));
		done_buf->bytesused = frame_size;

		/* 4. Buffer zurück in USB-Queue */
		done_buf->type = VIDEO_BUF_TYPE_INPUT;
		ret = video_enqueue(uvc_dev, done_buf);
		if (ret != 0) {
			LOG_ERR("video_enqueue fehlgeschlagen: %d", ret);
			break;
		}
	}

	return 0;
}
