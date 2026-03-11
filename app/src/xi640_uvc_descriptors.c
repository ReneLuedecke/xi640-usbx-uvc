/**
 * @file    xi640_uvc_descriptors.c
 * @brief   USB/UVC Deskriptor-Bloecke fuer Xi640 UVC Kamera
 *
 * Alle Deskriptoren als statische const uint8_t Arrays.
 * Werden 1:1 an ux_device_stack_initialize() uebergeben.
 *
 * Struktur des HS-Frameworks (191 Bytes):
 *   [18]  Device Descriptor
 *   [9]   Configuration Descriptor  (wTotalLength = 173)
 *   [8]   Interface Association Descriptor (IAD)
 *   [9]   VC Interface Descriptor
 *   [13]  VC Class-Specific Header
 *   [18]  VC Input Terminal (Camera)
 *   [9]   VC Output Terminal (USB Streaming)
 *   [9]   VS Interface Alt0 (zero bandwidth)
 *   [14]  VS Input Header
 *   [27]  VS Format Uncompressed (YUYV)
 *   [30]  VS Frame Uncompressed (640x480 @ 30 FPS)
 *   [6]   VS Color Matching
 *   [9]   VS Interface Alt1 (High-Bandwidth ISO)
 *   [7]   EP1 IN ISO Endpoint
 *   [5]   CS Endpoint
 *
 * Copyright (c) 2026 Optris GmbH. Alle Rechte vorbehalten.
 */

#include "xi640_uvc_descriptors.h"

/* ──────────────────────────────────────────────────────────────────────── */
/*  HS Device Framework (191 Bytes)                                       */
/* ──────────────────────────────────────────────────────────────────────── */

const uint8_t xi640_uvc_hs_framework[] = {

    /* ── Device Descriptor (18 Bytes) ────────────────────────────────── */
    0x12,               /* bLength = 18 */
    0x01,               /* bDescriptorType = Device */
    0x00, 0x02,         /* bcdUSB = 2.00 */
    0xEF,               /* bDeviceClass = Miscellaneous (fuer IAD) */
    0x02,               /* bDeviceSubClass = Common Class */
    0x01,               /* bDeviceProtocol = Interface Association */
    0x40,               /* bMaxPacketSize0 = 64 (HS EP0) */
    0xFF, 0xFF,         /* idVendor = 0xFFFF (Testmodus) */
    0x01, 0x00,         /* idProduct = 0x0001 */
    0x00, 0x01,         /* bcdDevice = 1.00 */
    0x01,               /* iManufacturer = String 1 */
    0x02,               /* iProduct = String 2 */
    0x00,               /* iSerialNumber = 0 */
    0x01,               /* bNumConfigurations = 1 */

    /* ── Configuration Descriptor (9 Bytes, wTotalLength = 173) ──────── */
    0x09,               /* bLength = 9 */
    0x02,               /* bDescriptorType = Configuration */
    0xAD, 0x00,         /* wTotalLength = 173 (0xAD) */
    0x02,               /* bNumInterfaces = 2 (VC + VS) */
    0x01,               /* bConfigurationValue = 1 */
    0x00,               /* iConfiguration = 0 */
    0x80,               /* bmAttributes = Bus Powered */
    0x32,               /* bMaxPower = 100mA */

    /* ── Interface Association Descriptor (8 Bytes) ───────────────────── */
    0x08,               /* bLength = 8 */
    0x0B,               /* bDescriptorType = IAD */
    0x00,               /* bFirstInterface = 0 (VC) */
    0x02,               /* bInterfaceCount = 2 */
    0x0E,               /* bFunctionClass = Video */
    0x03,               /* bFunctionSubClass = Video Interface Collection */
    0x00,               /* bFunctionProtocol = PC_PROTOCOL_UNDEFINED */
    0x00,               /* iFunction = 0 */

    /* ── VideoControl Interface Descriptor (9 Bytes) ─────────────────── */
    0x09,               /* bLength = 9 */
    0x04,               /* bDescriptorType = Interface */
    0x00,               /* bInterfaceNumber = 0 */
    0x00,               /* bAlternateSetting = 0 */
    0x00,               /* bNumEndpoints = 0 */
    0x0E,               /* bInterfaceClass = Video */
    0x01,               /* bInterfaceSubClass = Video Control */
    0x00,               /* bInterfaceProtocol = PC_PROTOCOL_UNDEFINED */
    0x00,               /* iInterface = 0 */

    /* ── VC Class-Specific Header (13 Bytes) ─────────────────────────── */
    /* wTotalLength = VC_Header(13) + IT(18) + OT(9) = 40 = 0x28        */
    0x0D,               /* bLength = 13 */
    0x24,               /* bDescriptorType = CS_INTERFACE */
    0x01,               /* bDescriptorSubType = VC_HEADER */
    0x10, 0x01,         /* bcdUVC = 1.10 */
    0x28, 0x00,         /* wTotalLength = 40 */
    0x00, 0x6C, 0xDC, 0x02, /* dwClockFrequency = 48,000,000 Hz (LE) */
    0x01,               /* bInCollection = 1 */
    0x01,               /* baInterfaceNr[0] = 1 (VS Interface) */

    /* ── VC Input Terminal — Camera (18 Bytes) ────────────────────────── */
    0x12,               /* bLength = 18 */
    0x24,               /* bDescriptorType = CS_INTERFACE */
    0x02,               /* bDescriptorSubType = VC_INPUT_TERMINAL */
    0x01,               /* bTerminalID = 1 */
    0x01, 0x02,         /* wTerminalType = ITT_CAMERA (0x0201) */
    0x00,               /* bAssocTerminal = 0 */
    0x00,               /* iTerminal = 0 */
    0x00, 0x00,         /* wObjectiveFocalLengthMin = 0 */
    0x00, 0x00,         /* wObjectiveFocalLengthMax = 0 */
    0x00, 0x00,         /* wOcularFocalLength = 0 */
    0x03,               /* bControlSize = 3 */
    0x00, 0x00, 0x00,   /* bmControls = 0 */

    /* ── VC Output Terminal — USB Streaming (9 Bytes) ────────────────── */
    0x09,               /* bLength = 9 */
    0x24,               /* bDescriptorType = CS_INTERFACE */
    0x03,               /* bDescriptorSubType = VC_OUTPUT_TERMINAL */
    0x02,               /* bTerminalID = 2 */
    0x01, 0x01,         /* wTerminalType = TT_STREAMING (0x0101) */
    0x00,               /* bAssocTerminal = 0 */
    0x01,               /* bSourceID = 1 (verbunden mit IT 1) */
    0x00,               /* iTerminal = 0 */

    /* ── VS Interface Alt0 — Zero Bandwidth (9 Bytes) ────────────────── */
    0x09,               /* bLength = 9 */
    0x04,               /* bDescriptorType = Interface */
    0x01,               /* bInterfaceNumber = 1 */
    0x00,               /* bAlternateSetting = 0 */
    0x00,               /* bNumEndpoints = 0 */
    0x0E,               /* bInterfaceClass = Video */
    0x02,               /* bInterfaceSubClass = Video Streaming */
    0x00,               /* bInterfaceProtocol = PC_PROTOCOL_UNDEFINED */
    0x00,               /* iInterface = 0 */

    /* ── VS Class-Specific Input Header (14 Bytes) ───────────────────── */
    /* wTotalLength = Hdr(14)+Format(27)+Frame(30)+Color(6) = 77 = 0x4D */
    0x0E,               /* bLength = 14 */
    0x24,               /* bDescriptorType = CS_INTERFACE */
    0x01,               /* bDescriptorSubType = VS_INPUT_HEADER */
    0x01,               /* bNumFormats = 1 */
    0x4D, 0x00,         /* wTotalLength = 77 */
    0x81,               /* bEndpointAddress = 0x81 (EP1 IN) */
    0x00,               /* bmInfo = 0 */
    0x02,               /* bTerminalLink = 2 (OT Terminal ID) */
    0x00,               /* bStillCaptureMethod = 0 */
    0x00,               /* bTriggerSupport = 0 */
    0x00,               /* bTriggerUsage = 0 */
    0x01,               /* bControlSize = 1 */
    0x00,               /* bmaControls[0] = 0 */

    /* ── VS Format Uncompressed — YUYV (27 Bytes) ────────────────────── */
    /* YUYV GUID: {32595559-0000-0010-8000-00AA00389B71}                 */
    0x1B,               /* bLength = 27 */
    0x24,               /* bDescriptorType = CS_INTERFACE */
    0x04,               /* bDescriptorSubType = VS_FORMAT_UNCOMPRESSED */
    0x01,               /* bFormatIndex = 1 */
    0x01,               /* bNumFrameDescriptors = 1 */
    /* guidFormat (YUYV, 16 Bytes): */
    0x59, 0x55, 0x59, 0x32, 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71,
    0x10,               /* bBitsPerPixel = 16 */
    0x01,               /* bDefaultFrameIndex = 1 */
    0x00,               /* bAspectRatioX = 0 */
    0x00,               /* bAspectRatioY = 0 */
    0x00,               /* bmInterlaceFlags = 0 */
    0x00,               /* bCopyProtect = 0 */

    /* ── VS Frame Uncompressed — 640x480 @ 30 FPS (30 Bytes) ─────────── */
    /* dwMinBitRate = dwMaxBitRate = 640*480*30*16 = 147,456,000 bps     */
    /* dwMaxVideoFrameBufferSize   = 640*480*2 = 614,400 = 0x00096000    */
    /* dwDefaultFrameInterval      = 333,333 (100ns-Einheiten = 30 FPS)  */
    0x1E,               /* bLength = 30 */
    0x24,               /* bDescriptorType = CS_INTERFACE */
    0x05,               /* bDescriptorSubType = VS_FRAME_UNCOMPRESSED */
    0x01,               /* bFrameIndex = 1 */
    0x00,               /* bmCapabilities = 0 */
    0x80, 0x02,         /* wWidth  = 640 (LE: 0x0280) */
    0xE0, 0x01,         /* wHeight = 480 (LE: 0x01E0) */
    0x00, 0x00, 0xCA, 0x08, /* dwMinBitRate = 147,456,000 (LE) */
    0x00, 0x00, 0xCA, 0x08, /* dwMaxBitRate = 147,456,000 (LE) */
    0x00, 0x60, 0x09, 0x00, /* dwMaxVideoFrameBufferSize = 614,400 (LE) */
    0x15, 0x16, 0x05, 0x00, /* dwDefaultFrameInterval = 333,333 (LE) */
    0x01,               /* bFrameIntervalType = 1 (diskret) */
    0x15, 0x16, 0x05, 0x00, /* dwFrameInterval[0] = 333,333 (LE) */

    /* ── VS Color Matching Descriptor (6 Bytes) ──────────────────────── */
    0x06,               /* bLength = 6 */
    0x24,               /* bDescriptorType = CS_INTERFACE */
    0x0D,               /* bDescriptorSubType = VS_COLORFORMAT */
    0x01,               /* bColorPrimaries = 1 (BT.709) */
    0x01,               /* bTransferCharacteristics = 1 (BT.709) */
    0x04,               /* bMatrixCoefficients = 4 (SMPTE 170M) */

    /* ── VS Interface Alt1 — High-Bandwidth ISO (9 Bytes) ────────────── */
    0x09,               /* bLength = 9 */
    0x04,               /* bDescriptorType = Interface */
    0x01,               /* bInterfaceNumber = 1 */
    0x01,               /* bAlternateSetting = 1 */
    0x01,               /* bNumEndpoints = 1 */
    0x0E,               /* bInterfaceClass = Video */
    0x02,               /* bInterfaceSubClass = Video Streaming */
    0x00,               /* bInterfaceProtocol = PC_PROTOCOL_UNDEFINED */
    0x00,               /* iInterface = 0 */

    /* ── EP1 IN ISO Endpoint (7 Bytes) ───────────────────────────────── */
    /* wMaxPacketSize = 0x1400: Bits 12:11 = 10b (3 Transaktionen)      */
    /*   + Bits 10:0 = 1024 Bytes -> 3 x 1024 = 3072 Bytes/Microframe   */
    0x07,               /* bLength = 7 */
    0x05,               /* bDescriptorType = Endpoint */
    0x81,               /* bEndpointAddress = 0x81 (EP1 IN) */
    0x05,               /* bmAttributes = ISO + Asynchron */
    0x00, 0x14,         /* wMaxPacketSize = 0x1400 (LE) */
    0x01,               /* bInterval = 1 (jedes Microframe = 125 us) */

    /* ── CS Endpoint General (5 Bytes) ───────────────────────────────── */
    0x05,               /* bLength = 5 */
    0x25,               /* bDescriptorType = CS_ENDPOINT */
    0x01,               /* bDescriptorSubType = EP_GENERAL */
    0x00, 0x60,         /* wMaxTransferSize = 24576 = 0x6000 (LE) */
};

const uint32_t xi640_uvc_hs_framework_len = sizeof(xi640_uvc_hs_framework);

/* ──────────────────────────────────────────────────────────────────────── */
/*  FS Framework — identisch zu HS (Phase 5: HS-PHY immer HS)            */
/*  EP1 wMaxPacketSize = 0x00FF (255 Bytes, FS ISO)                       */
/* ──────────────────────────────────────────────────────────────────────── */

const uint8_t xi640_uvc_fs_framework[] = {

    /* Device Descriptor (18 Bytes) — identisch */
    0x12, 0x01, 0x00, 0x02, 0xEF, 0x02, 0x01, 0x40,
    0xFF, 0xFF, 0x01, 0x00, 0x00, 0x01, 0x01, 0x02,
    0x00, 0x01,

    /* Configuration Descriptor (9 Bytes) — identisch, wTotalLength=173 */
    0x09, 0x02, 0xAD, 0x00, 0x02, 0x01, 0x00, 0x80, 0x32,

    /* IAD (8 Bytes) */
    0x08, 0x0B, 0x00, 0x02, 0x0E, 0x03, 0x00, 0x00,

    /* VC Interface (9 Bytes) */
    0x09, 0x04, 0x00, 0x00, 0x00, 0x0E, 0x01, 0x00, 0x00,

    /* VC Header (13 Bytes) */
    0x0D, 0x24, 0x01, 0x10, 0x01, 0x28, 0x00,
    0x00, 0x6C, 0xDC, 0x02, 0x01, 0x01,

    /* VC Input Terminal (18 Bytes) */
    0x12, 0x24, 0x02, 0x01, 0x01, 0x02, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,

    /* VC Output Terminal (9 Bytes) */
    0x09, 0x24, 0x03, 0x02, 0x01, 0x01, 0x00, 0x01, 0x00,

    /* VS Interface Alt0 (9 Bytes) */
    0x09, 0x04, 0x01, 0x00, 0x00, 0x0E, 0x02, 0x00, 0x00,

    /* VS Input Header (14 Bytes) */
    0x0E, 0x24, 0x01, 0x01, 0x4D, 0x00, 0x81, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x01, 0x00,

    /* VS Format (27 Bytes) */
    0x1B, 0x24, 0x04, 0x01, 0x01,
    0x59, 0x55, 0x59, 0x32, 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71,
    0x10, 0x01, 0x00, 0x00, 0x00, 0x00,

    /* VS Frame (30 Bytes) */
    0x1E, 0x24, 0x05, 0x01, 0x00, 0x80, 0x02, 0xE0, 0x01,
    0x00, 0x00, 0xCA, 0x08, 0x00, 0x00, 0xCA, 0x08,
    0x00, 0x60, 0x09, 0x00, 0x15, 0x16, 0x05, 0x00,
    0x01, 0x15, 0x16, 0x05, 0x00,

    /* VS Color Matching (6 Bytes) */
    0x06, 0x24, 0x0D, 0x01, 0x01, 0x04,

    /* VS Interface Alt1 (9 Bytes) */
    0x09, 0x04, 0x01, 0x01, 0x01, 0x0E, 0x02, 0x00, 0x00,

    /* EP1 ISO FS — wMaxPacketSize = 0x00FF (255 Bytes, FS ISO) */
    0x07, 0x05, 0x81, 0x01, 0xFF, 0x00, 0x01,

    /* CS Endpoint (5 Bytes) */
    0x05, 0x25, 0x01, 0x00, 0x60,
};

const uint32_t xi640_uvc_fs_framework_len = sizeof(xi640_uvc_fs_framework);

/* ──────────────────────────────────────────────────────────────────────── */
/*  String Framework                                                       */
/*  Format pro Eintrag: [lang_lo][lang_hi][string_idx][len][ascii...]     */
/* ──────────────────────────────────────────────────────────────────────── */

const uint8_t xi640_uvc_string_framework[] = {
    /* String 1: Manufacturer "Optris GmbH" (11 Zeichen) */
    0x09, 0x04,         /* Language: English US (0x0409) */
    0x01,               /* String Index = 1 */
    0x0B,               /* Laenge = 11 */
    'O', 'p', 't', 'r', 'i', 's', ' ', 'G', 'm', 'b', 'H',

    /* String 2: Product "Xi640 UVC Camera" (16 Zeichen) */
    0x09, 0x04,         /* Language: English US */
    0x02,               /* String Index = 2 */
    0x10,               /* Laenge = 16 */
    'X', 'i', '6', '4', '0', ' ', 'U', 'V', 'C', ' ',
    'C', 'a', 'm', 'e', 'r', 'a',
};

const uint32_t xi640_uvc_string_framework_len = sizeof(xi640_uvc_string_framework);

/* ──────────────────────────────────────────────────────────────────────── */
/*  Language ID Framework                                                  */
/*  0x0409 = English US (Little-Endian)                                   */
/* ──────────────────────────────────────────────────────────────────────── */

const uint8_t xi640_uvc_language_id_framework[] = {
    0x09, 0x04,         /* Language ID: English US (0x0409) */
};

const uint32_t xi640_uvc_language_id_framework_len =
    sizeof(xi640_uvc_language_id_framework);
