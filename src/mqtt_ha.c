/*
 * Unified MQTT + Home Assistant discovery/state publisher
 * Production-style, epoll-friendly, no heap fragmentation in hot path.
 */

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

#include <stdio.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <arpa/inet.h>
#include "p1_exporter.h"

int mqtt_port = MQTT_PORT;
const char *mqtt_host = MQTT_HOST;
const char *mqtt_username = MQTT_USERNAME;
const char *mqtt_password = MQTT_PASSWORD;
int mqtt_keepalive = MQTT_KEEPALIVE;

int mqtt_reconnect_initial = MQTT_RECONNECT_INITIAL;
int mqtt_reconnect_max = MQTT_RECONNECT_MAX;
int mqtt_reconnect_jitter = MQTT_RECONNECT_JITTER;

mqtt_format_t mqtt_format = MQTT_FORMAT_HA_NATIVE;

/* =========================
   LOW LEVEL WRITE QUEUE
   ========================= */

static void mqtt_queue_raw(fd_ctx_t *m, const void *data, size_t len)
{
    if (m->wlen + len > WBUF)
        return;

    bool was_empty = (m->wlen == 0);

    memcpy(m->wbuf + m->wlen, data, len);
    m->wlen += len;

    /* Re-arm EPOLLOUT now that there is data to send */
    if (was_empty && m->fd >= 0 && m->ep_fd >= 0) {
        struct epoll_event e = {
            .events  = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLERR,
            .data.ptr = m
        };
        epoll_ctl(m->ep_fd, EPOLL_CTL_MOD, m->fd, &e);
    }
}

/* =========================
   MQTT STRING ENCODER
   ========================= */

static uint8_t *mqtt_put_string(uint8_t *p, const char *s)
{
    uint16_t len = strlen(s);
    *p++ = len >> 8;
    *p++ = len & 0xff;
    memcpy(p, s, len);
    return p + len;
}


/* =========================
   MQTT CONNECT
   ========================= */
void mqtt_connect(fd_ctx_t *mqtt_ctx)
{
    int err = 0;
    socklen_t len = sizeof(err);

    if (getsockopt(mqtt_ctx->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0)
    {
        /* CONNECT FAILED */
        schedule_reconnect(mqtt_ctx->ep_fd, mqtt_ctx);
        return;
    }

    /* CONNECT SUCCESS */
    mqtt_ctx->status &= ~MQTT_CONNECTING;

    // send MQTT CONNECT packet
    mqtt_send_connect(mqtt_ctx);
}

/* =========================
   MQTT SUBSCRIBE
   ========================= */

static size_t mqtt_build_subscribe(uint8_t *buf, uint16_t pkt_id, const char *topic)
{
    uint8_t payload[256];
    uint8_t *p = payload;

    *p++ = pkt_id >> 8;
    *p++ = pkt_id & 0xff;

    p = mqtt_put_string(p, topic);
    *p++ = 0x00; // QoS0

    size_t remaining = p - payload;

    /* Build fixed header with variable-length remaining field */
    uint8_t fixed[5];
    int fixed_len = 0;
    fixed[fixed_len++] = 0x82; // SUBSCRIBE

    size_t rem = remaining;
    do {
        uint8_t byte = rem % 128;
        rem /= 128;
        if (rem) byte |= 0x80;
        fixed[fixed_len++] = byte;
    } while (rem);

    memcpy(buf, fixed, fixed_len);
    memcpy(buf + fixed_len, payload, remaining);

    return fixed_len + remaining;
}

static void mqtt_subscribe(fd_ctx_t *m, const char *topic)
{
    uint8_t pkt[256];
    uint16_t id = ++m->pkt_id;
    size_t len = mqtt_build_subscribe(pkt, id, topic);
    mqtt_queue_raw(m, pkt, len);
}

/* =========================
   BUILD CONNECT PACKET
   ========================= */

static size_t mqtt_build_connect(uint8_t *buf)
{
    uint8_t *p = buf + 2;

    p = mqtt_put_string(p, "MQTT");
    *p++ = 0x04;

    uint8_t flags = 0x02 | 0x80 | 0x40 | 0x04 | 0x20;
    *p++ = flags;

    *p++ = (uint8_t)(mqtt_keepalive >> 8);
    *p++ = (uint8_t)(mqtt_keepalive & 0xff);

    p = mqtt_put_string(p, MQTT_CLIENT_ID);

    /* LWT */
    p = mqtt_put_string(p, MQTT_AVAIL_TOPIC);
    p = mqtt_put_string(p, "offline");

    /* Username / password */
    p = mqtt_put_string(p, mqtt_username);
    p = mqtt_put_string(p, mqtt_password);

    size_t remaining = p - (buf + 2);

    buf[0] = 0x10;
    buf[1] = remaining;

    return remaining + 2;
}

/* =========================
   TCP CONNECT
   ========================= */

int mqtt_start_connect(fd_ctx_t *m)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(mqtt_port),
    };

    inet_pton(AF_INET, mqtt_host, &addr.sin_addr);

    m->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (m->fd < 0)
        return -1;

    int r = connect(m->fd, (struct sockaddr *)&addr, sizeof(addr));
    if (r < 0 && errno != EINPROGRESS) {
        close(m->fd);
        m->fd = -1;
        return -1;
    }

    m->status |= MQTT_CONNECTING;
    return m->fd;
}

/* =========================
   SEND CONNECT AFTER TCP UP
   ========================= */

void mqtt_send_connect(fd_ctx_t *m)
{
    uint8_t pkt[256];
    size_t len = mqtt_build_connect(pkt);
    mqtt_queue_raw(m, pkt, len);
}

/* =========================
   VERY SMALL MQTT ENCODER
   (QoS0 publish only)
   ========================= */

static void mqtt_encode_publish(fd_ctx_t *m,
                                const char *topic,
                                const char *payload,
                                bool retain)
{
    uint8_t header = 0x30 | (retain ? 0x01 : 0x00);

    uint16_t topic_len = strlen(topic);
    uint32_t payload_len = strlen(payload);

    uint32_t remaining = 2 + topic_len + payload_len;

    uint8_t fixed[5];
    int fixed_len = 0;

    fixed[fixed_len++] = header;

    do {
        uint8_t byte = remaining % 128;
        remaining /= 128;
        if (remaining) byte |= 0x80;
        fixed[fixed_len++] = byte;
    } while (remaining);

    mqtt_queue_raw(m, fixed, fixed_len);

    uint8_t tl[2] = { topic_len >> 8, topic_len & 0xff };
    mqtt_queue_raw(m, tl, 2);
    mqtt_queue_raw(m, topic, topic_len);
    mqtt_queue_raw(m, payload, payload_len);
}

static void mqtt_publish(fd_ctx_t *m,
                         const char *topic,
                         const char *payload,
                         bool retain)
{
    if (!(m->status & MQTT_CONNECTED))
        return;

    mqtt_encode_publish(m, topic, payload, retain);
}

/* =========================
   HOME ASSISTANT DISCOVERY
   ========================= */

static void ha_publish_sensor(fd_ctx_t *m,
                              const char *serial,
                              const char *object_id,
                              const char *name,
                              const char *unit,
                              const char *dev_class,
                              const char *state_class,
                              const char *value_tpl)
{
    char topic[256];
    char payload[1024];

    snprintf(topic, sizeof(topic),
             HA_DISCOVERY_PREFIX "/sensor/%s/config", object_id);

    snprintf(payload, sizeof(payload),
        "{"
        "\"name\":\"%s\","
        "\"uniq_id\":\"%s_%s\","
        "\"stat_t\":\"" MQTT_STATE_TOPIC "\","
        "\"avty_t\":\"" MQTT_AVAIL_TOPIC "\","
        "\"pl_avail\":\"online\","
        "\"pl_not_avail\":\"offline\","
        "%s%s%s"
        "%s%s%s"
        "%s%s%s"
        "\"val_tpl\":\"%s\","
        "\"dev\":{"
            "\"ids\":[\"dsmr_%s\"],"
            "\"name\":\"DSMR Smart Meter\","
            "\"mf\":\"dsmr_publisher\","
            "\"mdl\":\"DSMR\""
        "}"
        "}",
        name,
        serial, object_id,

        unit ? "\"unit_of_meas\":\"" : "",
        unit ? unit : "",
        unit ? "\"," : "",

        dev_class ? "\"dev_cla\":\"" : "",
        dev_class ? dev_class : "",
        dev_class ? "\"," : "",

        state_class ? "\"stat_cla\":\"" : "",
        state_class ? state_class : "",
        state_class ? "\"," : "",

        value_tpl,
        serial
    );

    mqtt_publish(m, topic, payload, true);
}

void ha_publish_all(fd_ctx_t *m)
{
    const char *id = sensor.equipment_id;

    ha_publish_sensor(m,id,"energy_import","Energy import","kWh","energy","total_increasing","{{ value_json.energy_import }}");
    ha_publish_sensor(m,id,"energy_export","Energy export","kWh","energy","total_increasing","{{ value_json.energy_export }}");

    ha_publish_sensor(m,id,"power_import","Power consumption","W","power","measurement","{{ value_json.power_import }}");
    ha_publish_sensor(m,id,"power_export","Power injection","W","power","measurement","{{ value_json.power_export }}");

    ha_publish_sensor(m,id,"voltage_l1","Voltage L1","V","voltage","measurement","{{ value_json.voltage_l1 }}");
    ha_publish_sensor(m,id,"voltage_l2","Voltage L2","V","voltage","measurement","{{ value_json.voltage_l2 }}");
    ha_publish_sensor(m,id,"voltage_l3","Voltage L3","V","voltage","measurement","{{ value_json.voltage_l3 }}");

    ha_publish_sensor(m,id,"current_l1","Current L1","A","current","measurement","{{ value_json.current_l1 }}");
    ha_publish_sensor(m,id,"current_l2","Current L2","A","current","measurement","{{ value_json.current_l2 }}");
    ha_publish_sensor(m,id,"current_l3","Current L3","A","current","measurement","{{ value_json.current_l3 }}");

    ha_publish_sensor(m,id,"tariff_indicator","Tariff",NULL,NULL,NULL,"{{ value_json.tariff_indicator }}");
    ha_publish_sensor(m,id,"power_failures","Power failures",NULL,NULL,"total_increasing","{{ value_json.power_failures }}");

    id = sensor.mbus_id[GAS_METER_ID];
    ha_publish_sensor(m,id,"gas","Gas consumption","m³","gas","total_increasing","{{ value_json.gas }}");

    id = sensor.mbus_id[WATER_METER_ID];
    ha_publish_sensor(m,id,"water","Water consumption","m³","water","total_increasing","{{ value_json.water }}");
}

/* =========================
   STATE PUBLISH
   ========================= */

void mqtt_publish_state(fd_ctx_t *m)
{
    if (!sensor_valid)
        return;
    char json[1024];

    snprintf(json, sizeof(json),
        "{"
        "\"energy_import\":%.3f,"
        "\"energy_export\":%.3f,"
        "\"power_import\":%.1f,"
        "\"power_export\":%.1f,"
        "\"voltage_l1\":%.1f,"
        "\"voltage_l2\":%.1f,"
        "\"voltage_l3\":%.1f,"
        "\"current_l1\":%.1f,"
        "\"current_l2\":%.1f,"
        "\"current_l3\":%.1f,"
        "\"tariff_indicator\":%d,"
        "\"power_failures\":%u,"
        "\"water\":%.3f,"
        "\"gas\":%.3f"
        "}",
        sensor.energy_import_kWh[1]+sensor.energy_import_kWh[2],
        sensor.energy_export_kWh[1]+sensor.energy_export_kWh[2],
        sensor.power_import_W[0],
        sensor.power_export_W[0],
        sensor.voltage_V[1],
        sensor.voltage_V[2],
        sensor.voltage_V[3],
        sensor.current_A[1],
        sensor.current_A[2],
        sensor.current_A[3],
        sensor.tariff_indicator,
        sensor.power_failures,
        sensor.water_total_m3,
        sensor.gas_total_m3
    );

    mqtt_publish(m, MQTT_STATE_TOPIC, json, false);
}

/* =========================
   HA ENERGY DASHBOARD INTEGRATION
   ========================= */

// Publish on: dsmr/json
// Field				Unit
// ---------------------------		----
// electricity_delivered_1/2		kWh
// electricity_returned_1/2 		kWh
// electricity_currently_delivered	W
// electricity_currently_returned	W
// gas_delivered			m³
//
// phase_currently_delivered_l1/2/3	W
// electricity_voltage_l1/2/3		V
// electricity_current_l1/2/3		A
//
// identification		e.g. "KFM5KAIFA-METER"
// p1_version			e.g. "50"
// electricity_tariff
// power_failure_count
// long_power_failure_count

static void format_iso8601(char *dst, size_t sz, time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);

    strftime(dst, sz, "%Y-%m-%dT%H:%M:%S%z", &tm);

    // turn +0100 into +01:00
    size_t l = strlen(dst);
    if (l > 2) {
        memmove(dst + l + 1, dst + l - 2, 3);
        dst[l - 2] = ':';
    }
}

size_t dsmr_reader_render(fd_ctx_t *m)
{
//  if (!(m->status & MQTT_TELEGRAM_VALID))
//      return 0;

    char ts[40];
    format_iso8601(ts, sizeof(ts), time(NULL));

    char json[1024];

    snprintf(json, sizeof(json),
        "{"
        "\"timestamp\":\"%s\","

        "\"electricity_delivered_1\":%.3f,"
        "\"electricity_delivered_2\":%.3f,"
        "\"electricity_returned_1\":%.3f,"
        "\"electricity_returned_2\":%.3f,"

        "\"electricity_currently_delivered\":%.0f,"
        "\"electricity_currently_returned\":%.0f,"

        "\"gas_delivered\":%.3f,"

        "\"phase_currently_delivered_l1\":%.0f,"
        "\"phase_currently_delivered_l2\":%.0f,"
        "\"phase_currently_delivered_l3\":%.0f,"

        "\"electricity_voltage_l1\":%.1f,"
        "\"electricity_voltage_l2\":%.1f,"
        "\"electricity_voltage_l3\":%.1f"

        "}\n",
        ts,
        sensor.energy_import_kWh[1],
        sensor.energy_import_kWh[2],
        sensor.energy_export_kWh[1],
        sensor.energy_export_kWh[2],

        sensor.power_import_W[0],
        sensor.power_export_W[0],

        sensor.gas_total_m3,

        sensor.power_import_W[1],
        sensor.power_import_W[2],
        sensor.power_import_W[3],

        sensor.voltage_V[1],
        sensor.voltage_V[2],
        sensor.voltage_V[3]
    );

    mqtt_publish(m, MQTT_JSON_TOPIC, json, false);
    return strlen(json);
}

/* =========================
   LWT + CONNECT HOOK
   ========================= */

void mqtt_on_connect(fd_ctx_t *m)
{
    m->counter = 0;

    mqtt_publish(m, MQTT_AVAIL_TOPIC, "online", true);
    mqtt_subscribe(m, HA_STATUS_TOPIC);

    if (sensor_valid) {
        ha_publish_all(m);
        mqtt_publish_state(m);
    }
}

fd_ctx_t *mqtt_open(void)
{
    fd_ctx_t *ctx = calloc(1, sizeof(*ctx));
    ctx->type     = FD_MQTT;
    ctx->timer_fd = -1;
    ctx->ep_fd    = -1;

    return ctx;
}

/* =========================
   EPOLL INTEGRATION
   ========================= */

ssize_t mqtt_io_write(fd_ctx_t *m)
{
    if (!m->wlen)
        return 0;

    ssize_t n = write(m->fd, m->wbuf, m->wlen);
    if (n > 0) {
        memmove(m->wbuf, m->wbuf + n, m->wlen - n);
        m->wlen -= n;
    } else if (n < 0 && errno == EAGAIN) {
        return 0;
    } else {
        return -1; /* connection lost */
    }
    return n;
}

ssize_t mqtt_io_read(fd_ctx_t *m)
{
    ssize_t n = read(m->fd, m->rbuf + m->rlen,
                     sizeof(m->rbuf) - m->rlen);

    if (n == 0 || (n < 0 && errno != EAGAIN)) {
        return -1; /* connection lost */
    }

    if (n > 0) {
        m->rlen += n;
    }

    /* --- CONNACK --- */
    if (m->rlen >= 4 && (m->rbuf[0] & 0xF0) == 0x20)
    {
        if (m->rbuf[3] == 0x00)
        {
            m->status |= MQTT_CONNECTED;
            m->status &= ~MQTT_CONNECTING;
            m->type = FD_MQTT;
        }

        memmove(m->rbuf, m->rbuf + 4, m->rlen - 4);
        m->rlen -= 4;

        mqtt_on_connect(m);
    }

    /* --- PUBLISH (for HA birth) --- */
    while (m->rlen > 2)
    {
        uint8_t type = m->rbuf[0] & 0xF0;
        uint8_t rem  = m->rbuf[1];

        if (m->rlen < rem + 2)
            break;

        if (type == 0x30)
        {
            uint16_t topic_len = (m->rbuf[2] << 8) | m->rbuf[3];
            char topic[128] = {0};
            memcpy(topic, &m->rbuf[4], topic_len);

            char *payload = (char*)&m->rbuf[4 + topic_len];
            size_t payload_len = rem - topic_len - 2;

            if (strcmp(topic, HA_STATUS_TOPIC) == 0)
            {
                if (payload_len == 6 && memcmp(payload, "online", 6) == 0)
                {
                    /* HA restarted: resend discovery and state */
                    ha_publish_all(m);
                    mqtt_publish_state(m);
//                  dsmr_reader_render(m);
                }
            }
        }

        memmove(m->rbuf, m->rbuf + rem + 2, m->rlen - rem - 2);
        m->rlen -= rem + 2;
    }

    return n;
}

/* =========================
   COMPUTE BACKOFF
   ========================= */

static int mqtt_next_backoff(int attempts)
{
    int base = mqtt_reconnect_initial << attempts;

    if (base > mqtt_reconnect_max)
        base = mqtt_reconnect_max;

    int jitter = (base * mqtt_reconnect_jitter) / 100;
    int delta  = rand() % (jitter + 1);

    return base - (jitter / 2) + delta;
}

/* =========================
   SCHEDULE RCONNECT
   ========================= */

void schedule_reconnect(int ep, fd_ctx_t *m)
{
    if (m->fd != -1)
    {
        epoll_ctl(ep, EPOLL_CTL_DEL, m->fd, NULL);
        close(m->fd);
        m->fd = -1;
    }

    m->status &= ~MQTT_CONNECTED;
    m->status &= ~MQTT_CONNECTING;

    int delay = mqtt_next_backoff(m->counter++);
    printf("MQTT reconnect in %d sec\n", delay);

    struct epoll_event ev = {
        .events = EPOLLIN,
        .data.ptr = m
    };

    if (m->timer_fd == -1)
    {
        m->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        m->type = FD_MQTT_RECONNECT;
        epoll_ctl(ep, EPOLL_CTL_ADD, m->timer_fd, &ev);
    }
    else
    {
        /* Timer already registered — just update it */
        epoll_ctl(ep, EPOLL_CTL_MOD, m->timer_fd, &ev);
    }

    struct itimerspec ts = {
        .it_value.tv_sec = delay
    };

    timerfd_settime(m->timer_fd, 0, &ts, NULL);
}
