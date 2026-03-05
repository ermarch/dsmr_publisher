// mqtt.c — minimal nonblocking MQTT client for unified epoll DSMR exporter

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "mqtt.h"
#include "state.h"
#include "systemd.h"

#define MQTT_KEEPALIVE 30
#define MQTT_RECONNECT_INITIAL 2
#define MQTT_RECONNECT_MAX 300

#define MQTT_BUF 4096

static int epoll_fd;
static int mqtt_fd = -1;
static int keepalive_fd = -1;
static int reconnect_fd = -1;

static bool mqtt_connected = false;
static bool mqtt_connecting = false;

static uint8_t txbuf[MQTT_BUF];
static size_t txlen = 0;

static uint8_t rxbuf[MQTT_BUF];

static uint32_t reconnect_delay = MQTT_RECONNECT_INITIAL;

static time_t last_tx = 0;
static time_t last_rx = 0;

static bool ha_birth_received = false;

static const char *client_id = "p1_exporter";
static const char *lwt_topic = "p1_exporter/availability";
static const char *state_topic = "dsmr/reading";

static const char *mqtt_host;
static int mqtt_port;
static const char *mqtt_user;
static const char *mqtt_pass;

static void epoll_add(int fd, uint32_t events);
static void epoll_mod(int fd, uint32_t events);
static void epoll_del(int fd);

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

static size_t dsmr_reader_render(char *buf, size_t sz)
{
    if (!state.telegram_valid)
        return 0;

    char ts[40];
    format_iso8601(ts, sizeof(ts), state.timestamp);

    return snprintf(buf, sz,
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

        state.energy_in_t1_kwh,
        state.energy_in_t2_kwh,
        state.energy_out_t1_kwh,
        state.energy_out_t2_kwh,

        state.power_in_w,
        state.power_out_w,

        state.gas_m3,

        state.power_l1_w,
        state.power_l2_w,
        state.power_l3_w,

        state.voltage_l1_v,
        state.voltage_l2_v,
        state.voltage_l3_v
    );
}

static void mqtt_queue(const void *data, size_t len)
{
    if (len > sizeof(txbuf) - txlen)
        return;

    memcpy(txbuf + txlen, data, len);
    txlen += len;

    epoll_mod(mqtt_fd, EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLERR);
}

static void mqtt_send_connect(void)
{
    uint8_t pkt[512];
    size_t p = 0;

    pkt[p++] = 0x10;

    uint8_t vh[512];
    size_t v = 0;

    vh[v++] = 0;
    vh[v++] = 4;
    memcpy(vh + v, "MQTT", 4); v += 4;
    vh[v++] = 4;

    uint8_t flags = 0x02;
    if (mqtt_user) flags |= 0x80;
    if (mqtt_pass) flags |= 0x40;
    flags |= 0x04 | 0x20;
    vh[v++] = flags;

    vh[v++] = MQTT_KEEPALIVE >> 8;
    vh[v++] = MQTT_KEEPALIVE & 0xFF;

    #define ADD_STR(s) \
        vh[v++] = strlen(s) >> 8; \
        vh[v++] = strlen(s) & 0xFF; \
        memcpy(vh + v, s, strlen(s)); \
        v += strlen(s);

    ADD_STR(client_id);
    ADD_STR(lwt_topic);
    ADD_STR("offline");

    if (mqtt_user) ADD_STR(mqtt_user);
    if (mqtt_pass) ADD_STR(mqtt_pass);

    pkt[p++] = v;
    memcpy(pkt + p, vh, v);
    p += v;

    mqtt_queue(pkt, p);
}

static void mqtt_send_ping(void)
{
    uint8_t pkt[2] = {0xC0, 0x00};
    mqtt_queue(pkt, 2);
}

static void mqtt_send_lwt_online(void)
{
    char msg[] = "online";
    mqtt_publish(lwt_topic, msg, strlen(msg), true);
}

static void mqtt_publish(const char *topic, const void *payload, size_t len, bool retain)
{
    uint8_t pkt[MQTT_BUF];
    size_t p = 0;

    pkt[p++] = 0x30 | (retain ? 1 : 0);

    size_t rl = 2 + strlen(topic) + len;
    pkt[p++] = rl;

    pkt[p++] = strlen(topic) >> 8;
    pkt[p++] = strlen(topic) & 0xFF;
    memcpy(pkt + p, topic, strlen(topic));
    p += strlen(topic);

    memcpy(pkt + p, payload, len);
    p += len;

    mqtt_queue(pkt, p);
}

void mqtt_publish_telegram(void)
{
    if (!mqtt_connected || !state.telegram_valid)
        return;

    char buf[1024];
    size_t len = dsmr_reader_render(buf, sizeof(buf));

    if (len)
        mqtt_publish(state_topic, buf, len, false);
}

static void mqtt_handle_connack(void)
{
    mqtt_connected = true;
    mqtt_connecting = false;
    reconnect_delay = MQTT_RECONNECT_INITIAL;

    mqtt_send_lwt_online();

    systemd_log("MQTT connected");

    struct itimerspec its = {
        .it_interval = {MQTT_KEEPALIVE, 0},
        .it_value = {MQTT_KEEPALIVE, 0}
    };
    timerfd_settime(keepalive_fd, 0, &its, NULL);
}

static void mqtt_process_rx(size_t len)
{
    last_rx = time(NULL);

    if (rxbuf[0] == 0x20)
        mqtt_handle_connack();

    if (rxbuf[0] == 0xD0)
        return;

    if ((rxbuf[0] & 0xF0) == 0x30)
    {
        if (strstr((char *)rxbuf, "homeassistant/status"))
        {
            if (strstr((char *)rxbuf, "online"))
            {
                ha_birth_received = true;
                ha_discovery_publish();
            }
        }
    }
}

static void mqtt_flush_tx(void)
{
    if (!txlen) return;

    ssize_t n = write(mqtt_fd, txbuf, txlen);
    if (n < 0)
    {
        if (errno == EAGAIN) return;
        mqtt_schedule_reconnect();
        return;
    }

    memmove(txbuf, txbuf + n, txlen - n);
    txlen -= n;

    if (!txlen)
        epoll_mod(mqtt_fd, EPOLLIN | EPOLLRDHUP | EPOLLERR);

    last_tx = time(NULL);
}

static void mqtt_read_rx(void)
{
    ssize_t n = read(mqtt_fd, rxbuf, sizeof(rxbuf));
    if (n <= 0)
    {
        mqtt_schedule_reconnect();
        return;
    }

    mqtt_process_rx(n);
}

static int create_socket(void)
{
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(mqtt_port);
    inet_pton(AF_INET, mqtt_host, &addr.sin_addr);

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    return fd;
}

void mqtt_start_connect(void)
{
    mqtt_fd = create_socket();
    mqtt_connecting = true;

    epoll_add(mqtt_fd, EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLERR);
}

void mqtt_schedule_reconnect(void)
{
    if (mqtt_fd != -1)
    {
        epoll_del(mqtt_fd);
        close(mqtt_fd);
        mqtt_fd = -1;
    }

    mqtt_connected = false;
    mqtt_connecting = false;

    struct itimerspec its = {
        .it_value = {reconnect_delay, 0}
    };
    timerfd_settime(reconnect_fd, 0, &its, NULL);

    reconnect_delay = reconnect_delay < MQTT_RECONNECT_MAX ?
                      reconnect_delay * 2 : MQTT_RECONNECT_MAX;
}

void mqtt_on_timer(int fd)
{
    uint64_t expirations;
    read(fd, &expirations, sizeof(expirations));

    if (fd == keepalive_fd)
    {
        if (time(NULL) - last_tx >= MQTT_KEEPALIVE)
            mqtt_send_ping();
    }
    else if (fd == reconnect_fd)
    {
        mqtt_start_connect();
    }
}

void mqtt_on_epoll(uint32_t events)
{
    if (events & EPOLLOUT)
    {
        if (mqtt_connecting)
        {
            mqtt_send_connect();
            mqtt_connecting = false;
        }
        mqtt_flush_tx();
    }

    if (events & EPOLLIN)
        mqtt_read_rx();

    if (events & (EPOLLERR | EPOLLRDHUP | EPOLLHUP))
        mqtt_schedule_reconnect();
}

void mqtt_init(int epfd)
{
    epoll_fd = epfd;

    mqtt_host = getenv("MQTT_HOST");
    mqtt_port = atoi(getenv("MQTT_PORT"));
    mqtt_user = getenv("MQTT_USER");
    mqtt_pass = getenv("MQTT_PASS");

    keepalive_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    reconnect_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);

    epoll_add(keepalive_fd, EPOLLIN);
    epoll_add(reconnect_fd, EPOLLIN);
}
