/*
 * DSMR P1 Prometheus Exporter
 * Target: Raspberry Pi
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
#include <sys/socket.h>

#include "config.h"
#include "p1_exporter.h"

sensor_t sensor;
bool sensor_valid = false;

void ctx_free(int ep, fd_ctx_t *ctx)
{
    epoll_ctl(ep, EPOLL_CTL_DEL, ctx->fd, NULL);
    close(ctx->fd);
    free(ctx);
}

static void ep_add(int ep, fd_ctx_t *ctx, uint32_t ev)
{
    struct epoll_event e = {.events = ev, .data.ptr = ctx};
    epoll_ctl(ep, EPOLL_CTL_ADD, ctx->fd, &e);
}

void ep_mod(int ep, fd_ctx_t *ctx, uint32_t ev)
{
    struct epoll_event e = {.events = ev, .data.ptr = ctx};
    epoll_ctl(ep, EPOLL_CTL_MOD, ctx->fd, &e);
}

void read_config(void)
{
    Config *cfg = load_config(CONFIG_FILE);
    if (!cfg) return;

    serial_device = get_config_string(cfg, "serial", "device", SERIAL_DEV);

    bind_host = get_config_string(cfg, "prometheus", "hostname", BIND_HOST);
    http_port = get_config_int(cfg, "prometheus", "port", HTTP_PORT);

    mqtt_host = get_config_string(cfg, "mqtt", "hostname", MQTT_HOST);
    mqtt_port = get_config_int(cfg, "mqtt", "port", MQTT_PORT);
    mqtt_username = get_config_string(cfg, "mqtt", "username", MQTT_USERNAME);
    mqtt_password = get_config_string(cfg, "mqtt", "password", MQTT_PASSWORD);
    mqtt_keepalive = get_config_int(cfg, "mqtt", "keepalive", MQTT_KEEPALIVE);

    mqtt_reconnect_initial = get_config_int(cfg, "mqtt", "reconnect_initial", MQTT_RECONNECT_INITIAL);
    mqtt_reconnect_max = get_config_int(cfg, "mqtt", "reconnect_max", MQTT_RECONNECT_MAX);
    mqtt_reconnect_jitter = get_config_int(cfg, "mqtt", "reconnect_jitter", MQTT_RECONNECT_JITTER);

    free_config(cfg);
}

int main(void)
{
    read_config();

    systemd_notify_init();
    systemd_status("Starting P1 exporter");

    int ep = epoll_create1(0);

    fd_ctx_t *listen_ctx = make_listen_socket();
    if (listen_ctx) {
         ep_add(ep, listen_ctx, EPOLLIN);
    }

    fd_ctx_t *serial_ctx = serial_open();
    if (serial_ctx) {
        ep_add(ep, serial_ctx, EPOLLIN);
    }

    fd_ctx_t *mqtt_ctx = mqtt_open();
    if (mqtt_ctx)
    {
        mqtt_start_connect(mqtt_ctx);
        ep_add(ep, mqtt_ctx, EPOLLIN | EPOLLOUT);
    }

    systemd_ready();

    struct epoll_event events[MAX_EVENTS];

    while (1)
    {
        int n = epoll_wait(ep, events, MAX_EVENTS, 1000);

        for (int i = 0; i < n; i++)
        {
            fd_ctx_t *ctx = events[i].data.ptr;
            uint32_t ev = events[i].events;

            if (ev & (EPOLLHUP | EPOLLERR | EPOLLRDHUP))
            {
                ctx_free(ep, ctx);
                continue;
            }

            if (ctx->type == FD_LISTEN)
            {
                while (1)
                {
                    fd_ctx_t *c = accept_client(ctx->fd);
                    if (!c) break;
                    ep_add(ep, c, EPOLLIN | EPOLLRDHUP);
                }
            }
            else if (ctx->type == FD_SERIAL)
            {
                serial_process(ctx);
                if (ctx->first_valid && mqtt_ctx) {
                    /* First valid telegram — send discovery then state */
                    ha_publish_all(mqtt_ctx);
                }
                if (mqtt_ctx) mqtt_publish_state(mqtt_ctx);
                systemd_watchdog_ping();
            }
            else if (ctx->type == FD_HTTP)
            {
                if (ev & EPOLLIN) {
                    http_read(ctx, ep);
                }
                if (ev & EPOLLOUT) {
                    http_write(ctx, ep);
                }
            }
            else if (ctx->type == FD_MQTT_RECONNECT && mqtt_ctx)
            {
                uint64_t expirations;
                read(ctx->fd, &expirations, sizeof(expirations));

                epoll_ctl(ep, EPOLL_CTL_DEL, mqtt_ctx->fd, NULL);

                int newfd = mqtt_start_connect(mqtt_ctx);

                struct epoll_event mqtt_ev = {
                    .events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLERR,
                    .data.ptr = mqtt_ctx
                };
                epoll_ctl(ep, EPOLL_CTL_ADD, newfd, &mqtt_ev);
            }
            else if (ctx->type == FD_MQTT && mqtt_ctx)
            {
                if (ev & EPOLLIN) {
                    mqtt_io_read(mqtt_ctx);
                }
                if (ev & EPOLLOUT)
                {
                    if (mqtt_ctx->status & MQTT_CONNECTING)
                    {
                        mqtt_connect(ctx, mqtt_ctx);
                        continue;
                    }

                    /* normal write flush */
                    mqtt_io_write(mqtt_ctx);
                }
            }
        }

        systemd_watchdog_ping();
    }

    free(mqtt_ctx);
    free(serial_ctx);
    free(listen_ctx);
}

