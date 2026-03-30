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
#include "xi640_hid.h"

#include <soc.h>              /* STM32N6 HAL: RIF, RCC, NVIC, SCB_NS */
#include "dcmipp_capture.h"

LOG_MODULE_REGISTER(uvc_sample, LOG_LEVEL_INF);

/* Phase 4: DCMIPP frame_buf_a/b direkt als UVC-Pool-Einträge.
 * Kein separater AXISRAM3/4-Buffer mehr — DWC2 DMA liest aus AXISRAM1
 * (OTG1 + OTG2 haben CID1 via RIMC → Zugriff auf alle AXISRAM-Bänke OK).
 * Wrapper-Structs: nur .index und .type werden von video_enqueue() genutzt.
 * video_import_buffer() trägt .buffer/.size in video_buf[idx] ein.
 * video_dequeue() liefert &video_buf[idx] zurück (Pool-Eintrag direkt). */
static struct video_buffer uvc_static_bufs[2];

const static struct device *const uvc_dev = DEVICE_DT_GET(DT_NODELABEL(uvc));

/* sw-generator: Format-Advertisement + optionaler Colorbar-Diagnose-Pfad.
 * CONFIG_XI640_UVC_SOURCE_DCMIPP=n → sw-generator liefert echte Colorbar-Daten an UVC. */
const static struct device *const video_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_camera));

static struct video_caps video_caps = {.type = VIDEO_BUF_TYPE_OUTPUT};

/* ---- Benchmark-Statistik (Sliding Window, 30 Frames) ---- */
static struct {
	uint32_t window_frames;    /* Frames im aktuellen Fenster */
	uint32_t window_start_ms;  /* Fensterbeginn (k_uptime_get_32) */
	uint32_t total_frames;     /* Gesamt-Frames seit Reset */
} bench;

static void bench_reset(uint32_t frame_size)
{
	bench.window_frames   = 0U;
	bench.window_start_ms = k_uptime_get_32();
	bench.total_frames    = 0U;
	LOG_INF("Benchmark gestartet, Frame-Groesse: %u Bytes", frame_size);
}

static void bench_update(uint32_t frame_size)
{
	bench.window_frames++;
	bench.total_frames++;

	if (bench.window_frames >= 30U) {
		uint32_t now     = k_uptime_get_32();
		uint32_t elapsed = now - bench.window_start_ms;

		if (elapsed > 0U) {
			uint32_t fps  = (30U * 1000U) / elapsed;
			uint32_t kbps = (uint32_t)(
				((uint64_t)bench.window_frames * frame_size * 8U)
				/ elapsed);
			LOG_INF("UVC: %u FPS | %u kbit/s "
				"(Fenster %u ms) | gesamt: %u Frames",
				fps, kbps, elapsed, bench.total_frames);
		}

		bench.window_start_ms = now;
		bench.window_frames   = 0U;
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

	/* ---- HID Interface registrieren (vor USB-Stack-Start) ---- */
	ret = xi640_hid_init();
	if (ret != 0) {
		LOG_ERR("xi640_hid_init fehlgeschlagen: %d", ret);
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

	/* ── DEBUG: USB-Register-Zustand nach usbd_enable() ─────────────────
	 * Vergleich UVC-only vs UVC+HID: GINTMSK und DCFG müssen identisch sein.
	 * DCFG[1:0] DSPD: 0=HS, 1=FS(extern PHY), 3=FS(intern PHY).
	 * DCTL[1] RWUSIG, DCTL[2] SFTDISCON (0=connected, 1=soft-disconnect).
	 * GINTMSK: welche Interrupts aktiv sind (WKUINT Bit17, USBSUSP Bit11).
	 * TEMPORÄR — nach Root-Cause-Analyse entfernen. */
	{
		volatile uint32_t *gintmsk =
			(volatile uint32_t *)(DT_REG_ADDR(DT_NODELABEL(usbotg_hs1)) + 0x018U);
		volatile uint32_t *gintsts =
			(volatile uint32_t *)(DT_REG_ADDR(DT_NODELABEL(usbotg_hs1)) + 0x014U);
		volatile uint32_t *dcfg =
			(volatile uint32_t *)(DT_REG_ADDR(DT_NODELABEL(usbotg_hs1))
					      + USB_OTG_DEVICE_BASE + 0x000U);
		volatile uint32_t *dctl =
			(volatile uint32_t *)(DT_REG_ADDR(DT_NODELABEL(usbotg_hs1))
					      + USB_OTG_DEVICE_BASE + 0x004U);

		LOG_INF("USB-REG nach usbd_enable: GINTMSK=0x%08x GINTSTS=0x%08x"
			" DCFG=0x%08x DCTL=0x%08x",
			*gintmsk, *gintsts, *dcfg, *dctl);
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
	/* printk statt LOG_INF: GCC 12 (SDK 0.17.4) kann VA_STACK_ALIGN(long double)
	 * nicht als compile-time integer auswerten → BUILD_ASSERT schlaegt fehl.
	 * printk nutzt runtime-cbvprintf, kein static package → kein Assert. */
	char fourcc_str[5];
	memcpy(fourcc_str, VIDEO_FOURCC_TO_STR(fmt.pixelformat), sizeof(fourcc_str));
	printk("Host gewaehlt: %s %ux%u @ %u/%u\n",
		fourcc_str, fmt.width, fmt.height,
		frmival.denominator, frmival.numerator);
	LOG_WRN("USB Speed: %u (0=FS 1=HS)",
		usbd_bus_speed(sample_usbd));

	/* ── Phase 5: OTG-HS TX-FIFO Tuning ────────────────────────────────────
	 * udc_stm32.c berechnet EP1-TX FIFO = DIV_ROUND_UP(MPS=512, 4) = 128 Words
	 * = 512 Bytes = 1×MPS. 2776 B freies DRAM bleiben ungenutzt.
	 * Fix: EP1-TX auf Rest des DRAM ausweiten (822 Words = 3288 B = 6.4×MPS).
	 * → Weniger FIFO-Stalls, besserer Burst-Durchsatz für Bulk-IN.
	 *
	 * Timing: nach Enumeration (Host hat Format gewählt), vor erstem Transfer.
	 * Kein aktiver Transfer auf EP1 → FIFO-Flush ist sicher.
	 *
	 * Basis-Adresse: DT usbotg_hs1 @ 0x08040000 (AHB5 auf STM32N6). */
	{
		volatile uint32_t *grxfsiz   =
			(volatile uint32_t *)(DT_REG_ADDR(DT_NODELABEL(usbotg_hs1)) + 0x024U);
		volatile uint32_t *gnptxfsiz =
			(volatile uint32_t *)(DT_REG_ADDR(DT_NODELABEL(usbotg_hs1)) + 0x028U);
		volatile uint32_t *dieptxf1  =
			(volatile uint32_t *)(DT_REG_ADDR(DT_NODELABEL(usbotg_hs1)) + 0x104U);
		volatile uint32_t *dieptxf2  =
			(volatile uint32_t *)(DT_REG_ADDR(DT_NODELABEL(usbotg_hs1)) + 0x108U);
		volatile uint32_t *grstctl   =
			(volatile uint32_t *)(DT_REG_ADDR(DT_NODELABEL(usbotg_hs1)) + 0x010U);

		uint32_t rx_words  = *grxfsiz   & 0xFFFFU;
		uint32_t ep0_depth = (*gnptxfsiz >> 16) & 0xFFFFU;
		uint32_t ep1_depth = (*dieptxf1 >> 16) & 0xFFFFU;
		uint32_t ep1_start = *dieptxf1 & 0xFFFFU;
		uint32_t total     = rx_words + ep0_depth + ep1_depth;

		LOG_INF("FIFO: RX=%uW(%uB) EP0=%uW(%uB) EP1=%uW(%uB) frei=%uB",
			rx_words,  rx_words  * 4U,
			ep0_depth, ep0_depth * 4U,
			ep1_depth, ep1_depth * 4U,
			4096U - total * 4U);

		/* EP1 TX FIFO auf verfügbaren Rest ausweiten.
		 * 16W (64B = 1×HID-MPS) am Ende für EP2 HID TX FIFO reservieren.
		 * Ohne diese Reservierung: EP1 (202-1023) überlagert EP2-FIFO (~330)
		 * → Spurious resume / EP2-Korruption.
		 * Flush EP1 zuerst: GRSTCTL.TXFNUM=1 (EP1), TXFFLSH=1 (bit5). */
		*grstctl = (1UL << 6) | (1UL << 5);     /* TXFNUM=1, TXFFLSH=1 */
		while (*grstctl & (1UL << 5)) {}         /* Warte bis auto-clear */

		/* EP1: Wörter ep1_start bis (1024-16-1) = 806W */
		uint32_t ep2_fifo_words = 16U;           /* 64B = 1×HID-MPS */
		uint32_t ep1_new_depth  = (4096U / 4U) - ep1_start - ep2_fifo_words;
		*dieptxf1 = (ep1_new_depth << 16) | ep1_start;

		LOG_INF("FIFO Tuning: EP1-TX %uW(%uB) → %uW(%uB) @ Word %u",
			ep1_depth, ep1_depth * 4U,
			ep1_new_depth, ep1_new_depth * 4U, ep1_start);

		/* EP2 (HID): direkt nach EP1 platzieren.
		 * Flush EP2 TX FIFO vor Reconfig. */
		*grstctl = (2UL << 6) | (1UL << 5);     /* TXFNUM=2, TXFFLSH=1 */
		while (*grstctl & (1UL << 5)) {}

		uint32_t ep2_start = ep1_start + ep1_new_depth;
		*dieptxf2 = (ep2_fifo_words << 16) | ep2_start;

		LOG_INF("FIFO Tuning: EP2-TX %uW(%uB) @ Word %u (HID)",
			ep2_fifo_words, ep2_fifo_words * 4U, ep2_start);
	}

	/* ── Streaming-Loop: DCMIPP oder SW-Generator → UVC ──────────────────
	 * CONFIG_XI640_UVC_SOURCE_DCMIPP=y (default): DCMIPP liefert Frames.
	 * CONFIG_XI640_UVC_SOURCE_DCMIPP=n:           SW-Generator Colorbar.
	 *   Diagnose-Build: west build ... -- -DCONFIG_XI640_UVC_SOURCE_DCMIPP=n */

#if CONFIG_XI640_UVC_SOURCE_DCMIPP
	/* ── DCMIPP → UVC (Produktionspfad) ────────────────────────────────
	 * RIMC für OTG1+OTG2 in Security-Init gesetzt → DWC2 DMA hat CID1.
	 * UVC TX Buffer in AXISRAM3/4 — DWC2 DMA + CPU nutzen denselben CID1. */
	LOG_INF("=== DCMIPP→UVC Streaming ===");

	/* Phase 4: DCMIPP-Buffer direkt als UVC-Pool-Einträge registrieren.
	 * video_enqueue() liest &video_buf[buf->index].buffer → direkt AXISRAM1.
	 * Kein Umweg über AXISRAM3/4 — 1.2 MB gespart, ein Kopier-Hop weniger. */
	{
		uint8_t  *dcmipp_buf_a, *dcmipp_buf_b;
		uint16_t  idx;

		dcmipp_capture_get_buffers(&dcmipp_buf_a, &dcmipp_buf_b);

		ret = video_import_buffer(dcmipp_buf_a, fmt.size, &idx);
		if (ret != 0) {
			LOG_ERR("video_import_buffer[a] fehlgeschlagen: %d", ret);
			return ret;
		}
		uvc_static_bufs[0].index = idx;
		uvc_static_bufs[0].type  = VIDEO_BUF_TYPE_INPUT;
		LOG_INF("DCMIPP buf_a @ %p → video_buf[%u]",
			(void *)dcmipp_buf_a, idx);

		ret = video_import_buffer(dcmipp_buf_b, fmt.size, &idx);
		if (ret != 0) {
			LOG_ERR("video_import_buffer[b] fehlgeschlagen: %d", ret);
			return ret;
		}
		uvc_static_bufs[1].index = idx;
		uvc_static_bufs[1].type  = VIDEO_BUF_TYPE_INPUT;
		LOG_INF("DCMIPP buf_b @ %p → video_buf[%u]",
			(void *)dcmipp_buf_b, idx);
	}

	/* Initial-Enqueue: beide Buffer einreihen (bytesused=0 — UVC sendet
	 * leere Header-Frames bis DCMIPP Nutzdaten liefert, das ist safe). */
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
	LOG_INF("Phase 4 Pipeline: beide Buffer eingereiht — starte Loop");

	{
		uint8_t            *frame_data;
		uint32_t            frame_size;
		uint32_t            frame_count = 0U;
		struct video_buffer *done_buf;

		bench_reset(fmt.size);

		while (true) {
			/* 1. DCMIPP-Frame capturen — PARALLEL zu laufendem USB-Transfer.
			 *    DCMIPP braucht ~25ms (40 FPS), USB ~38ms (26 FPS).
			 *    → get_frame() kehrt zurück während USB noch ~13ms läuft.
			 *    D-Cache-Invalidierung bereits in get_frame() enthalten. */
			ret = dcmipp_capture_get_frame(&frame_data, &frame_size,
						       200);
			if (ret == -ETIMEDOUT) {
				k_sleep(K_MSEC(1));
				continue;
			}
			if (ret != 0) {
				LOG_ERR("DCMIPP Fehler: %d", ret);
				break;
			}

			/* 2. Blockierendes Dequeue — USB ~13ms nach get_frame() fertig.
			 *    K_MSEC(100) statt K_NO_WAIT: kein spurious get_frame() retry
			 *    mehr. done_buf == &video_buf[idx] (Pool-Eintrag direkt). */
			ret = video_dequeue(uvc_dev, &done_buf, K_MSEC(100));
			if (ret != 0) {
				LOG_WRN("UVC dequeue timeout (frame %u): %d",
					frame_count, ret);
				/* frame_data verloren — nächste Iteration holt neuen Frame */
				continue;
			}

			/* 3. Zero-Copy: Pool-Eintrag direkt auf DCMIPP-Frame zeigen.
			 *    CPU schreibt NICHT in diesen Buffer → kein cache flush nötig.
			 *    Invalidierung in dcmipp_capture_get_frame() ausreichend. */
			done_buf->buffer    = frame_data;
			done_buf->size      = frame_size;
			done_buf->bytesused = frame_size;
			done_buf->type      = VIDEO_BUF_TYPE_INPUT;

			frame_count++;
			LOG_DBG("DCMIPP→UVC frame %u buf=%p",
				frame_count, (void *)done_buf->buffer);

			/* 4. USB-Transfer starten — nächste Iteration beginnt sofort
			 *    mit get_frame() und überlappt so mit diesem Transfer. */
			ret = video_enqueue(uvc_dev, done_buf);
			if (ret != 0) {
				LOG_ERR("video_enqueue fehlgeschlagen: %d", ret);
				break;
			}
			bench_update(frame_size);
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
		bench_reset(fmt.size);

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
