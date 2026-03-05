/*
 * Public Domain (www.unlicense.org)
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or distribute
 * this software, either in source code form or as a compiled binary, for any
 * purpose, commercial or non-commercial, and by any means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors of
 * this software dedicate any and all copyright interest in the software to
 * the public domain. We make this dedication for the benefit of the public at
 * large and to the detriment of our heirs and successors. We intend this
 * dedication to be an overt act of relinquishment in perpetuity of all
 * present and future rights to this software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include "p1_exporter.h"

/**
 * Custom multi-byte memtok_r
 * @param s         Buffer to parse (NULL for subsequent calls)
 * @param len       Remaining length of the buffer
 * @param delim     The delimiter sequence (e.g., "--")
 * @param delim_len Length of the delimiter sequence
 * @param saveptr   Tracks progress
 * @param out_len   Length of the found token
 */
static void* memtok_r(void *s, size_t *len, const void *delim, size_t delim_len, void **saveptr, size_t *out_len)
{
    unsigned char *start = s ? (unsigned char *)s : (unsigned char *)*saveptr;

    if (*len < delim_len || start == NULL) {
        if (*len > 0 && start != NULL) { // Handle final token
            *out_len = *len;
            *len = 0;
            *saveptr = NULL;
            return start;
        }
        return NULL;
    }

    // Find the multi-byte sequence in the buffer
    unsigned char *next = memmem(start, *len, delim, delim_len);

    if (next) {
        *out_len = (size_t)(next - start);
        *saveptr = next + delim_len;
        *len -= (*out_len + delim_len);
    } else {
        *out_len = *len;
        *saveptr = NULL;
        *len = 0;
    }

    return start;
}

static uint16_t crc16(uint16_t crc, uint8_t *data, size_t length)
{
    for (size_t i=0; i<length; i++)
    {
        crc ^= (uint16_t)((uint8_t)data[i]);
        for (int j=0; j<8; j++)
        {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static void dsmr_parse_line(const char *l, size_t line_len)
{
    int type;
    double v;

    // 0-0:96.1.1: Equipment Identifier of the electricity meter
    sscanf(l, "0-0:96.1.1(%31s)", sensor.equipment_id);

    // 1-0:1.8.0: Total cumulative energy consumed (imported)
    // 1-0:1.8.1: Meter Reading electricity delivered to client (low tariff)
    // 1-0:1.8.2: Meter Reading electricity delivered to client (normal tariff)
    if (sscanf(l, "1-0:1.8.0(%lf*kWh)", &v) == 1) {
        sensor.energy_import_kWh[0] = v;
    } else if (sscanf(l, "1-0:1.8.1(%lf*kWh)", &v) == 1) {
        sensor.energy_import_kWh[1] = v;
    } else if (sscanf(l, "1-0:1.8.2(%lf*kWh)", &v) == 1) {
        sensor.energy_import_kWh[2] = v;

    // 1-0:2.8.0: Total cumulative solar energy fed back into the grid
    // 1-0:2.8.1: Meter Reading electricity delivered by client (low tariff)
    // 1-0:2.8.2: Meter Reading electricity delivered by client (normal tariff)
    } else if (sscanf(l, "1-0:2.8.0(%lf*kWh)", &v) == 1) {
        sensor.energy_export_kWh[0] = v;
    } else if (sscanf(l, "1-0:2.8.1(%lf*kWh)", &v) == 1) {
        sensor.energy_export_kWh[1] = v;
    } else if (sscanf(l, "1-0:2.8.2(%lf*kWh)", &v) == 1) {
        sensor.energy_export_kWh[2] = v;

    // 0-0:96.14.0: Tariff indicator electricity.
    // The tariff indicator can be used to switch tariff dependent loads
    // e.g boilers. This is responsibility of the P1 user
    } else if (sscanf(l, "0-0:96.14.0(%lf)", &v) == 1) {
        sensor.tariff_indicator = v;

    // 1-0:1.7.0: Actual electricity power delivered (+P)
    // 1-0:2.7.0: Actual electricity power received (-P)
    } else if (sscanf(l, "1-0:1.7.0(%lf*kW)", &v) == 1) {
        sensor.power_import_W[0] = v*1000;
    } else if (sscanf(l, "1-0:2.7.0(%lf*kW)", &v) == 1) {
        sensor.power_export_W[0] = v*1000;

    // 1-0:32.7.0: Instantaneous voltage L1
    // 1-0:52.7.0: Instantaneous voltage L2
    // 1-0:72.7.0: Instantaneous voltage L3
    } else if (sscanf(l, "1-0:32.7.0(%lf*V)", &v) == 1) {
        sensor.voltage_V[1] = v;
    } else if (sscanf(l, "1-0:52.7.0(%lf*V)", &v) == 1) {
        sensor.voltage_V[2] = v;
    } else if (sscanf(l, "1-0:72.7.0(%lf*V)", &v) == 1) {
        sensor.voltage_V[3] = v;

    // 1-0:31.7.0: Instantaneous current L1
    // 1-0:51.7.0: Instantaneous current L2
    // 1-0:71.7.0: Instantaneous current L3
    } else if (sscanf(l, "1-0:31.7.0(%lf*A)", &v) == 1) {
        sensor.current_A[1] = v;
    } else if (sscanf(l, "1-0:51.7.0(%lf*A)", &v) == 1) {
        sensor.current_A[2] = v;
    } else if (sscanf(l, "1-0:71.7.0(%lf*A)", &v) == 1) {
        sensor.current_A[3] = v;

    // 1-0:21.7.0: Instantaneous active power L1 (+P)
    // 1-0:41.7.0: Instantaneous active power L2 (+P)
    // 1-0:61.7.0: Instantaneous active power L3 (+P)
    } else if (sscanf(l, "1-0:21.7.0(%lf*kW)", &v) == 1) {
        sensor.power_import_W[1] = v*1000;
    } else if (sscanf(l, "1-0:41.7.0(%lf*kW)", &v) == 1) {
        sensor.power_import_W[2] = v*1000;
    } else if (sscanf(l, "1-0:61.7.0(%lf*kW)", &v) == 1) {
        sensor.power_import_W[3] = v*1000;

    // 1-0:22.7.0: Instantaneous active power L1 (-P)
    // 1-0:42.7.0: Instantaneous active power L2 (-P)
    // 1-0:62.7.0: Instantaneous active power L3 (-P)
    } else if (sscanf(l, "1-0:22.7.0(%lf*kW)", &v) == 1) {
        sensor.power_export_W[1] = v*1000;
    } else if (sscanf(l, "1-0:42.7.0(%lf*kW)", &v) == 1) {
        sensor.power_export_W[2] = v*1000;
    } else if (sscanf(l, "1-0:62.7.0(%lf*kW)", &v) == 1) {
        sensor.power_export_W[3] = v*1000;

    // ===== MBUS devices =====
    // 0-n:24.1.0: Device-Type
    } else if (sscanf(l, "0-%d:24.1.0(%d)", &sensor.channel, &type) == 2) {
        if (type < 0) type = 0;
        else if (type > MBUS_CHANNELS) type = MBUS_CHANNELS;
        sensor.mbus_type[sensor.channel] = type;
    }

   // 0-n:96.1.0: Equipment identifier
   else if (sscanf(l, "0-%d:96.1.0(%31[^)])", &sensor.channel, sensor.mbus_id[sensor.mbus_type[sensor.channel]]) == 2) {
    }

    // 0-n:24.2.1: Last 5-minute Meter reading
    else if (sscanf(l, "0-%d:24.2.1(%*[^)])(%lf*m3)", &sensor.channel, &v) == 2)
    {
        switch(sensor.mbus_type[sensor.channel])
        {
        case GAS_METER_ID: sensor.gas_total_m3 = v; break;
        case WATER_METER_ID: sensor.water_total_m3 = v; break;
        default: break;
        }
    }
    else if (sscanf(l, "0-%d:24.2.1(%*[^)])(%lf*GJ)", &sensor.channel, &v) == 2) {
        sensor.thermal_total_GJ = v;
    }
}

static void dsmr_parse_telegram(char *telegram, size_t len)
{
    size_t line_len;
    void *saveptr;
    char *line;

    line = memtok_r(telegram, &len, "\r\n", 2, &saveptr, &line_len);
    while(line != NULL)
    {
        dsmr_parse_line(line, line_len);
        line = memtok_r(NULL, &len, "\r\n", 2, &saveptr, &line_len);
    }
}

int dsmr_parse_stream(uint8_t *buf, size_t len)
{
    size_t pos = 0;

    while (pos < len)
    {
        /* Find telegram start */
        uint8_t *start = memchr(buf + pos, '/', len - pos);
        if (!start)
            return pos;

        size_t start_off = start - buf;

        /* Find end marker '!' */
        uint8_t *bang = memchr(start, '!', len - start_off);
        if (!bang)
            return start_off;   // incomplete telegram

        size_t telegram_len = bang - start + 1;

        /* Need 4 CRC hex chars + newline */
        if (start_off + telegram_len + 5 > len)
            return start_off;   // incomplete CRC

        char crc_str[5] = {0};
        memcpy(crc_str, bang + 1, 4);

        uint16_t expected_crc = strtoul(crc_str, NULL, 16);

        uint16_t actual_crc = crc16(0x0000, start, telegram_len);

        if (actual_crc == expected_crc)
        {
            /* TEMP NULL-TERMINATE FOR OLD PARSER */
            char save = start[telegram_len];
            start[telegram_len] = 0;

            dsmr_parse_telegram((char *)start, telegram_len);

            start[telegram_len] = save;
        }
        else
        {
            /* optional: crc_error_counter++ */
        }

        pos = start_off + telegram_len + 5;
    }

    return pos;
}
