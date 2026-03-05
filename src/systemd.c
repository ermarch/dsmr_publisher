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
#include <stddef.h>
#include <time.h>
#include <sys/un.h>
#include <sys/socket.h>
#include "p1_exporter.h"

static int watchdog_usec = 0;

static void systemd_send(const char *msg)
{
    const char *sock = getenv("NOTIFY_SOCKET");
    if (!sock) return;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;

    strncpy(addr.sun_path, sock, sizeof(addr.sun_path)-1);

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return;

    sendto(fd, msg, strlen(msg), 0,
           (struct sockaddr *)&addr, sizeof(addr));

    close(fd);
}

void systemd_ready(void)
{
    systemd_send("READY=1");

    const char *wd = getenv("WATCHDOG_USEC");
    if (wd) watchdog_usec = atoi(wd);
}

void systemd_status(const char *txt)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "STATUS=%s", txt);
    systemd_send(buf);
}

static int notify_fd = -1;
static struct sockaddr_un notify_addr;
static socklen_t notify_addr_len;

void systemd_notify_init(void)
{
    const char *path = getenv("NOTIFY_SOCKET");
    if (!path || !*path)
        return;

    notify_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (notify_fd < 0)
        return;

    memset(&notify_addr, 0, sizeof(notify_addr));
    notify_addr.sun_family = AF_UNIX;

    if (path[0] == '@') {
        /* abstract namespace */
        notify_addr.sun_path[0] = '\0';
        strncpy(notify_addr.sun_path + 1, path + 1,
                sizeof(notify_addr.sun_path) - 2);
    } else {
        strncpy(notify_addr.sun_path, path,
                sizeof(notify_addr.sun_path) - 1);
    }

    notify_addr_len =
        offsetof(struct sockaddr_un, sun_path) +
        strlen(path) + 1;
}

void systemd_watchdog_ping(void)
{
    if (!watchdog_usec) return;

    static time_t last = 0;
    time_t now = time(NULL);

    int interval = (watchdog_usec/1000000)/2;
    if (interval <= 0) interval = 1;

    if (now-last >= interval)
    {
        systemd_send("WATCHDOG=1");
        last = now;
    }
}
