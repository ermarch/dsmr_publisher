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

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include "p1_exporter.h"

const char *serial_device = SERIAL_DEV;

fd_ctx_t *serial_open(void)
{
    int fd = open(serial_device, O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return NULL;

    struct termios tio = {0};
    tcgetattr(fd, &tio);
    cfmakeraw(&tio);
    cfsetspeed(&tio, B115200);
    tcsetattr(fd, TCSANOW, &tio);

    fd_ctx_t *ctx = calloc(1, sizeof(*ctx));
    ctx->type = FD_SERIAL;
    ctx->fd   = fd;
    return ctx;
}

void serial_process(fd_ctx_t *ctx)
{
    while (1)
    {
        ssize_t n = read(ctx->fd,
                         ctx->rbuf + ctx->rlen,
                         sizeof(ctx->rbuf) - ctx->rlen);

        if (n > 0)
        {
            ctx->rlen += n;

            bool was_valid = sensor_valid;
            int used = dsmr_parse_stream(ctx->rbuf, ctx->rlen);

            if (used > 0)
            {
                memmove(ctx->rbuf, ctx->rbuf + used, ctx->rlen - used);
                ctx->rlen -= used;
            }

            /* On the very first valid telegram, store the transition so
               main() can trigger discovery + initial state publish. */
            ctx->first_valid = (!was_valid && sensor_valid);
        }
        else if (n < 0 && errno == EAGAIN)
            break;
        else
            break;
    }
}
