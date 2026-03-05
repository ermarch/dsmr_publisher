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
#include <sys/socket.h>

#include <stdio.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include "p1_exporter.h"

const char *bind_host = BIND_HOST;
int http_port = HTTP_PORT;

int prometheus_render_to_buffer(char *buf, size_t size)
{
    snprintf(buf, size,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain; version=0.0.4\r\n\r\n"

        "p1_active_tariff{equipment_id=\"%s\"} %i\n"
        "p1_power_failures{equipment_id=\"%s\"} %i\n"

        "p1_energy_total{direction=\"import\",unit=\"kWh\",equipment_id=\"%s\"} %.3f\n"
        "p1_energy_total{direction=\"import\",unit=\"kWh\",tariff=\"t1\",equipment_id=\"%s\"} %.3f\n"
        "p1_energy_total{direction=\"import\",unit=\"kWh\",tariff=\"t2\",equipment_id=\"%s\"} %.3f\n"

        "p1_energy{direction=\"export\",unit=\"kWh\",equipment_id=\"%s\"} %.3f\n"
        "p1_energy{direction=\"export\",unit=\"kWh\",tariff=\"t1\",equipment_id=\"%s\"} %.3f\n"
        "p1_energy{direction=\"export\",unit=\"kWh\",tariff=\"t2\",equipment_id=\"%s\"} %.3f\n"

        "p1_power{direction=\"import\",unit=\"W\",phase=\"all\",equipment_id=\"%s\"} %.2f\n"
        "p1_power{direction=\"import\",unit=\"W\",phase=\"l1\",equipment_id=\"%s\"} %.2f\n"
        "p1_power{direction=\"import\",unit=\"W\",phase=\"l2\",equipment_id=\"%s\"} %.2f\n"
        "p1_power{direction=\"import\",unit=\"W\",phase=\"l3\",equipment_id=\"%s\"} %.2f\n"

        "p1_power{direction=\"export\",unit=\"W\",phase=\"all\",equipment_id=\"%s\"} %.2f\n"
        "p1_power{direction=\"export\",unit=\"W\",phase=\"l1\",equipment_id=\"%s\"} %.2f\n"
        "p1_power{direction=\"export\",unit=\"W\",phase=\"l2\",equipment_id=\"%s\"} %.2f\n"
        "p1_power{direction=\"export\",unit=\"W\",phase=\"l3\",equipment_id=\"%s\"} %.2f\n"

        "p1_voltage{phase=\"l1\",unit=\"V\",equipment_id=\"%s\"} %.1f\n"
        "p1_voltage{phase=\"l2\",unit=\"V\",equipment_id=\"%s\"} %.1f\n"
        "p1_voltage{phase=\"l3\",unit=\"V\",equipment_id=\"%s\"} %.1f\n"

        "p1_current{phase=\"l1\",unit=\"A\",equipment_id=\"%s\"} %.1f\n"
        "p1_current{phase=\"l2\",unit=\"A\",equipment_id=\"%s\"} %.1f\n"
        "p1_current{phase=\"l3\",unit=\"A\",equipment_id=\"%s\"} %.1f\n"

        "p1_gas_total{unit=\"m3\",equipment_id=\"%s\"} %.3f\n"
        "p1_water_total{unit=\"m3\",equipment_id=\"%s\"} %.3f\n",

        sensor.equipment_id, sensor.tariff_indicator,
        sensor.equipment_id, sensor.power_failures,

        sensor.equipment_id, sensor.energy_import_kWh[0], // total
        sensor.equipment_id, sensor.energy_import_kWh[1], // T1
        sensor.equipment_id, sensor.energy_import_kWh[2], // T2

        sensor.equipment_id, sensor.energy_export_kWh[0], // total
        sensor.equipment_id, sensor.energy_export_kWh[1], // T1
        sensor.equipment_id, sensor.energy_export_kWh[2], // T2

        sensor.equipment_id, sensor.power_import_W[0], // total
        sensor.equipment_id, sensor.power_import_W[1], // phase 1
        sensor.equipment_id, sensor.power_import_W[2], // phase 2
        sensor.equipment_id, sensor.power_import_W[3], // phase 3

        sensor.equipment_id, sensor.power_export_W[0], // total
        sensor.equipment_id, sensor.power_export_W[1], // phase 1
        sensor.equipment_id, sensor.power_export_W[2], // phase 2
        sensor.equipment_id, sensor.power_export_W[3], // phase 2

        sensor.equipment_id, sensor.voltage_V[1], // phase 1
        sensor.equipment_id, sensor.voltage_V[2], // phase 2
        sensor.equipment_id, sensor.voltage_V[3], // phase 3

        sensor.equipment_id, sensor.current_A[1], // phase 1
        sensor.equipment_id, sensor.current_A[2], // phase 2
        sensor.equipment_id, sensor.current_A[3], // phase 3

        sensor.mbus_id[GAS_METER_ID], sensor.gas_total_m3,
        sensor.mbus_id[WATER_METER_ID], sensor.water_total_m3
    );

    return strlen(buf);
}

fd_ctx_t *make_listen_socket(void)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port   = htons(http_port)
    };

    inet_pton(AF_INET, bind_host, &a.sin_addr);

    bind(fd, (struct sockaddr*)&a, sizeof(a));
    listen(fd, 16);

    fd_ctx_t *ctx = calloc(1, sizeof(*ctx));
    ctx->type = FD_LISTEN;
    ctx->fd   = fd;
    return ctx;
}

fd_ctx_t *accept_client(int lfd)
{
    int c = accept4(lfd, NULL, NULL, SOCK_NONBLOCK);
    if (c < 0) return NULL;

    fd_ctx_t *ctx = calloc(1, sizeof(*ctx));
    ctx->type = FD_HTTP;
    ctx->fd   = c;
    return ctx;
}

static bool http_request_complete(fd_ctx_t *ctx)
{
    return memmem(ctx->rbuf, ctx->rlen, "\r\n\r\n", 4);
}

void http_read(fd_ctx_t *ctx, int ep)
{
    while (1)
    {
        ssize_t n = read(ctx->fd,
                         ctx->rbuf + ctx->rlen,
                         sizeof(ctx->rbuf) - ctx->rlen);

        if (n > 0)
            ctx->rlen += n;
        else if (n < 0 && errno == EAGAIN)
            break;
        else
            return;
    }

    if (http_request_complete(ctx))
    {
        ctx->wlen = prometheus_render_to_buffer(
                        (char *)ctx->wbuf, sizeof(ctx->wbuf));

        ctx->woff = 0;

        ep_mod(ep, ctx, EPOLLIN | EPOLLOUT | EPOLLRDHUP);
    }
}

void http_write(fd_ctx_t *ctx, int ep)
{
    while (ctx->woff < ctx->wlen)
    {
        ssize_t n = write(ctx->fd,
                          ctx->wbuf + ctx->woff,
                          ctx->wlen - ctx->woff);

        if (n > 0)
            ctx->woff += n;
        else if (errno == EAGAIN)
            return;
        else
            return;
    }

    ctx_free(ep, ctx);
}
