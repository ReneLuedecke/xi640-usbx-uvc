/*
 * xi640_hid.c — Xi640 USB HID-Interface
 *
 * Vendor Usage Page 0xFFA0 — kompatibel mit Optris PIX Connect / IRImager SDK.
 * Composite Device: UVC (Interface 0+1) + HID (Interface 2).
 *
 * Datenfluss:
 *   Host OUT Report (40 B) → output_report() Callback → Semaphore → HID-Thread
 *   HID-Thread: xi640_hid_process_command() → hid_device_submit_report() (64 B)
 *
 * Cache-Regeln (Cortex-M55):
 *   - HID-Buffer: __aligned(32), nicht im Hot Path → kein sys_cache_data_*_range nötig
 *   - I2C-Transfer: Zephyr I2C-Treiber handhabt Cache intern
 */

#include "xi640_hid.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/class/usbd_hid.h>

LOG_MODULE_REGISTER(xi640_hid, LOG_LEVEL_INF);

/* ── ATTO640D I2C-Konstanten ─────────────────────────────────────────────── */
#define ATTO640D_I2C_ADDR       0x12U
#define ATTO640D_REG_DAC_GFID   0x004BU
#define ATTO640D_REG_DAC_GSK_A  0x004CU
#define ATTO640D_REG_DAC_GSK_B  0x004DU

/* ── Firmware-Version (Stub) ─────────────────────────────────────────────── */
#define XI640_FW_VERSION_LSB    0x01U
#define XI640_FW_VERSION_MSB    0x00U

/* ── Gerätekonstanten ────────────────────────────────────────────────────── */
#define XI640_HKD_FLAGS_HZ100   (1U << 5)   /* Byte 19 Bit5: 1 = 100Hz/52fps */

/* ── HID Report Deskriptor ───────────────────────────────────────────────── */
/* Vendor Usage Page 0xFFA0, Physical Collection.
 * Usage Page 0xFFA1 für Report-Items (Byte-Array, signed).
 * Input Report: 64 Bytes (Device → Host, Interrupt IN).
 * Output Report: 40 Bytes (Host → Device, Interrupt OUT). */
static const uint8_t xi640_hid_rdesc[] = {
    0x06, 0xA0, 0xFF,   /* Usage Page (Vendor 0xFFA0) */
    0x09, 0x01,         /* Usage (Vendor 1) */
    0xA1, 0x01,         /* Collection (Application) */
    0x09, 0x02,         /*   Usage (Vendor 2) */
    0xA1, 0x00,         /*   Collection (Physical) */
    0x06, 0xA1, 0xFF,   /*   Usage Page (Vendor 0xFFA1) */
    /* Input Report: 64 Bytes */
    0x09, 0x03,         /*   Usage (Vendor 3) */
    0x09, 0x04,         /*   Usage (Vendor 4) */
    0x15, 0x80,         /*   Logical Minimum (-128) */
    0x25, 0x7F,         /*   Logical Maximum (127) */
    0x35, 0x00,         /*   Physical Minimum (0) */
    0x45, 0xFF,         /*   Physical Maximum (255) */
    0x75, 0x08,         /*   Report Size (8 bits) */
    0x95, 0x40,         /*   Report Count (64) */
    0x81, 0x02,         /*   Input (Data, Variable, Absolute) */
    /* Output Report: 40 Bytes */
    0x09, 0x05,         /*   Usage (Vendor 5) */
    0x09, 0x06,         /*   Usage (Vendor 6) */
    0x15, 0x80,         /*   Logical Minimum (-128) */
    0x25, 0x7F,         /*   Logical Maximum (127) */
    0x35, 0x00,         /*   Physical Minimum (0) */
    0x45, 0xFF,         /*   Physical Maximum (255) */
    0x75, 0x08,         /*   Report Size (8 bits) */
    0x95, 0x28,         /*   Report Count (40) */
    0x91, 0x02,         /*   Output (Data, Variable, Absolute) */
    0xC0,               /*   End Collection (Physical) */
    0xC0,               /* End Collection (Application) */
};

/* ── Statische Puffer (Cache-Line aligned, Cortex-M55) ──────────────────── */
static __aligned(32) uint8_t xi640_hid_cmd[XI640_HID_OUT_SIZE];
static __aligned(32) uint8_t xi640_hid_rsp[XI640_HID_IN_SIZE];

/* ── Thread + Synchronisation ────────────────────────────────────────────── */
K_THREAD_STACK_DEFINE(xi640_hid_stack, 1536);
static struct k_thread xi640_hid_thread;
static K_SEM_DEFINE(xi640_hid_sem, 0, 1);

static const struct device *xi640_hid_dev;
static bool xi640_hid_iface_ready;

/* ── ATTO640D I2C-Schreibfunktion ────────────────────────────────────────── */
/* 16-Bit Register-Adressierung, MSB first (UG038). */
static int atto640d_write_reg(uint16_t reg, uint16_t val)
{
    const struct device *i2c = DEVICE_DT_GET(DT_NODELABEL(i2c1));
    uint8_t buf[4] = {
        (uint8_t)((reg >> 8) & 0xFFU),
        (uint8_t)(reg & 0xFFU),
        (uint8_t)((val >> 8) & 0xFFU),
        (uint8_t)(val & 0xFFU),
    };

    if (!device_is_ready(i2c)) {
        LOG_ERR("I2C1 nicht bereit");
        return -ENODEV;
    }
    int ret = i2c_write(i2c, buf, sizeof(buf), ATTO640D_I2C_ADDR);

    if (ret != 0) {
        LOG_ERR("ATTO640D I2C Write Reg=0x%04x Val=0x%04x: %d", reg, val, ret);
    }
    return ret;
}

/* ── Housekeeping-Response befüllen ─────────────────────────────────────── */
static void xi640_hid_fill_housekeeping(uint8_t *in_report)
{
    memset(in_report, 0, XI640_HID_IN_SIZE);

    /* Bytes 12-13: Gehäusetemperatur (Stub: 25.0 °C → 250 = 0x00FA) */
    in_report[XI640_HKD_TEMP_MSB] = 0x00U;
    in_report[XI640_HKD_TEMP_LSB] = 0xFAU;

    /* Bytes 15-16: Firmware-Version */
    in_report[XI640_HKD_FW_LSB] = XI640_FW_VERSION_LSB;
    in_report[XI640_HKD_FW_MSB] = XI640_FW_VERSION_MSB;

    /* Byte 19 Bit5: 1 = 100Hz/52fps Modus */
    in_report[XI640_HKD_FLAGS] = XI640_HKD_FLAGS_HZ100;
}

/* ── Kommando-Verarbeitung ───────────────────────────────────────────────── */
void xi640_hid_process_command(const uint8_t *out_report, uint8_t *in_report)
{
    memset(in_report, 0, XI640_HID_IN_SIZE);

    if (out_report[0] != XI640_CMD_CYP) {
        LOG_DBG("Unbekannte Klasse: 0x%02x", out_report[0]);
        return;
    }

    uint8_t cmd = out_report[1];

    switch (cmd) {

    case XI640_CMD_DAC_SET: {
        uint8_t  dac_reg = out_report[2];
        uint16_t dac_val;

        switch (dac_reg) {
        case XI640_DAC_GFID:
            /* FX2LP-kompatible XOR-Invertierung:
             * BlazeBuffer[4] = 0xFF ^ EP1OUTBUF[3]
             * BlazeBuffer[5] = 0xF0 ^ EP1OUTBUF[4]
             * Erhält Kompatibilität mit bestehenden Kalibrierungsdaten. */
            dac_val = ((uint16_t)(0xFFU ^ out_report[3]) << 8) |
                       (uint16_t)(0xF0U ^ out_report[4]);
            LOG_INF("DAC GFID: raw=0x%04x → xor=0x%04x",
                    ((uint16_t)out_report[3] << 8) | out_report[4],
                    dac_val);
            atto640d_write_reg(ATTO640D_REG_DAC_GFID, dac_val);
            break;

        case XI640_DAC_GSK:
            dac_val = ((uint16_t)out_report[3] << 8) | out_report[4];
            LOG_INF("DAC GSK: val=0x%04x", dac_val);
            atto640d_write_reg(ATTO640D_REG_DAC_GSK_A, dac_val);
            atto640d_write_reg(ATTO640D_REG_DAC_GSK_B, dac_val);
            break;

        default:
            LOG_WRN("DAC Register unbekannt: 0x%02x", dac_reg);
        }
        /* DAC SET: keine IN-Report-Response erwartet */
        break;
    }

    case XI640_CMD_FLAG_DIR:
        LOG_INF("FLAG_DIR: Richtung=0x%02x", out_report[2]);
        /* TODO: Shutter-GPIO steuern */
        break;

    case XI640_CMD_FLAG_CYCLE:
        LOG_INF("FLAG_CYCLE: 0x%02x 0x%02x", out_report[2], out_report[3]);
        /* TODO: Flag-Zyklus über GPIO */
        break;

    case XI640_CMD_PW_CONTROL:
        LOG_INF("PW_CONTROL: 0x%02x", out_report[2]);
        break;

    case XI640_CMD_RD_EEPROM:
        LOG_INF("RD_EEPROM: Adresse=0x%02x%02x", out_report[2], out_report[3]);
        /* Stub: EEPROM-Daten nicht verfügbar → leere Response */
        break;

    case XI640_CMD_WR_EEPROM:
        LOG_INF("WR_EEPROM: Adresse=0x%02x%02x", out_report[2], out_report[3]);
        break;

    case XI640_CMD_HKD_GET:
    case XI640_CMD_FW_CYP_VERS:
    case XI640_CMD_FW_MSP_VERS:
        xi640_hid_fill_housekeeping(in_report);
        LOG_DBG("Housekeeping Response gesendet (cmd=0x%02x)", cmd);
        break;

    default:
        LOG_DBG("Unbekanntes Kommando: 0x%02x", cmd);
        /* Unbekannte Kommandos: Housekeeping als sicherer Default */
        xi640_hid_fill_housekeeping(in_report);
        break;
    }
}

/* ── Kommando-Handler Thread ─────────────────────────────────────────────── */
/* NICHT im Hot Path — kein LOG-Limit nötig. */
static void xi640_hid_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (true) {
        k_sem_take(&xi640_hid_sem, K_FOREVER);

        if (!xi640_hid_iface_ready) {
            continue;
        }

        xi640_hid_process_command(xi640_hid_cmd, xi640_hid_rsp);

        int ret = hid_device_submit_report(xi640_hid_dev,
                                           XI640_HID_IN_SIZE,
                                           xi640_hid_rsp);
        if (ret != 0 && ret != -EAGAIN) {
            LOG_WRN("submit_report fehlgeschlagen: %d", ret);
        }
    }
}

/* ── HID-Callbacks ───────────────────────────────────────────────────────── */

static void xi640_hid_iface_ready_cb(const struct device *dev, const bool ready)
{
    ARG_UNUSED(dev);
    xi640_hid_iface_ready = ready;
    LOG_INF("HID Interface: %s", ready ? "bereit" : "nicht aktiv");
}

static void xi640_hid_output_report_cb(const struct device *dev,
                                        const uint16_t len,
                                        const uint8_t *const buf)
{
    ARG_UNUSED(dev);

    if (len > XI640_HID_OUT_SIZE) {
        LOG_WRN("OUT Report zu gross: %u > %u B", len, XI640_HID_OUT_SIZE);
        return;
    }

    memcpy(xi640_hid_cmd, buf, len);
    if (len < XI640_HID_OUT_SIZE) {
        memset(xi640_hid_cmd + len, 0, XI640_HID_OUT_SIZE - len);
    }

    k_sem_give(&xi640_hid_sem);
}

static int xi640_hid_get_report_cb(const struct device *dev,
                                    const uint8_t type, const uint8_t id,
                                    const uint16_t len, uint8_t *const buf)
{
    ARG_UNUSED(dev);

    /* GET_REPORT (Control-Pipe): Housekeeping auf Anfrage senden */
    if (type == HID_REPORT_TYPE_INPUT && len >= XI640_HID_IN_SIZE) {
        xi640_hid_fill_housekeeping(buf);
        return (int)XI640_HID_IN_SIZE;
    }
    return -ENOTSUP;
}

static int xi640_hid_set_report_cb(const struct device *dev,
                                    const uint8_t type, const uint8_t id,
                                    const uint16_t len, const uint8_t *const buf)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(id);

    /* SET_REPORT (Control-Pipe): Output Report über Control-Pipe empfangen */
    if (type == HID_REPORT_TYPE_OUTPUT && len >= XI640_HID_OUT_SIZE) {
        memcpy(xi640_hid_cmd, buf, XI640_HID_OUT_SIZE);
        k_sem_give(&xi640_hid_sem);
        return 0;
    }
    return -ENOTSUP;
}

static const struct hid_device_ops xi640_hid_ops = {
    .iface_ready    = xi640_hid_iface_ready_cb,
    .output_report  = xi640_hid_output_report_cb,
    .get_report     = xi640_hid_get_report_cb,
    .set_report     = xi640_hid_set_report_cb,
};

/* ── Public API ──────────────────────────────────────────────────────────── */

int xi640_hid_init(void)
{
    int ret;

    xi640_hid_dev = DEVICE_DT_GET(DT_NODELABEL(xi640_hid_dev));
    if (!device_is_ready(xi640_hid_dev)) {
        LOG_ERR("HID-Gerät '%s' nicht bereit", xi640_hid_dev->name);
        return -ENODEV;
    }

    ret = hid_device_register(xi640_hid_dev,
                              xi640_hid_rdesc,
                              sizeof(xi640_hid_rdesc),
                              &xi640_hid_ops);
    if (ret != 0) {
        LOG_ERR("hid_device_register fehlgeschlagen: %d", ret);
        return ret;
    }

    k_thread_create(&xi640_hid_thread,
                    xi640_hid_stack,
                    K_THREAD_STACK_SIZEOF(xi640_hid_stack),
                    xi640_hid_thread_fn,
                    NULL, NULL, NULL,
                    K_PRIO_PREEMPT(6), 0, K_NO_WAIT);
    k_thread_name_set(&xi640_hid_thread, "xi640_hid");

    LOG_INF("Xi640 HID: Vendor 0xFFA0 registriert "
            "(IN=%u B OUT=%u B Rdesc=%u B)",
            XI640_HID_IN_SIZE, XI640_HID_OUT_SIZE,
            (unsigned)sizeof(xi640_hid_rdesc));
    return 0;
}
