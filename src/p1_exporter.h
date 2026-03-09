/*
 * DSMR P1 Prometheus and Home Assistant/MQTT publisher
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

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#define CONFIG_FILE		"/etc/dsmr_publisher.conf"

#define HTTP_PORT		8888
#define BIND_HOST		"127.0.0.1"
#define SERIAL_DEV		"/dev/ttyUSB0"
#define MBUS_CHANNELS		16

#define MAX_EVENTS		64
#define RBUF			8192
#define WBUF			8192


#define MQTT_CLIENT_ID		"dsmr"
#define MQTT_JSON_TOPIC		MQTT_CLIENT_ID"/reading"
#define MQTT_STATE_TOPIC	MQTT_CLIENT_ID"/state"
#define MQTT_AVAIL_TOPIC	MQTT_CLIENT_ID"/availability"

#define HA_DISCOVERY_PREFIX	"homeassistant"
#define HA_STATUS_TOPIC		HA_DISCOVERY_PREFIX"/state"

#define MQTT_HOST		"192.168.1.206"
#define MQTT_PORT		1883
#define MQTT_USERNAME		"mqtt_user"
#define MQTT_PASSWORD		""
#define MQTT_KEEPALIVE		60

#define MQTT_RECONNECT_INITIAL	 1 // seconds
#define MQTT_RECONNECT_MAX	60 // seconds
#define MQTT_RECONNECT_JITTER	20 // percent

#define MQTT_CONNECTED		0x1
#define MQTT_CONNECTING		0x2
#define MQTT_TELEGRAM_VALID	0x4

typedef enum {
    MQTT_FORMAT_HA_NATIVE = 0,
    MQTT_FORMAT_DSMR_READER = 1
} mqtt_format_t;

extern const char *serial_device;

extern const char *bind_host;
extern int http_port;

extern const char *mqtt_host;
extern int mqtt_port;
extern const char *mqtt_username;
extern const char *mqtt_password;
extern int mqtt_keepalive;
extern int mqtt_reconnect_initial;
extern int mqtt_reconnect_max;
extern int mqtt_reconnect_jitter;

extern mqtt_format_t mqtt_format;

/* ================= Metrics ================= */

// Device type mapping
//  DSMR / M-Bus standard
//
// channel  Meaning
// 003      Gas meter
// 004      Heat meter
// 007      Water meter
// 008      Heat cost allocator
// 002      Electricity (extra)
enum
{
    EQUIPMENT_ID = 0,
    ELECTRICITY_METER_EXTRA_ID = 2,
    GAS_METER_ID = 3,
    THERMAL_METER_ID = 4,
    WATER_METER_ID = 7,
    THERMAL_COST_ALLOCATOR = 8
};

typedef struct {
    char equipment_id[32];

    int tariff_indicator;
    uint32_t power_failures;

    double energy_import_kWh[3];
    double energy_export_kWh[3];

    double power_import_W[4];
    double power_export_W[4];

    double voltage_V[4];
    double current_A[4];

    int channel;
    int mbus_type[MBUS_CHANNELS];
    char mbus_id[MBUS_CHANNELS][32];
    double electricity_extra_kWh;
    double gas_total_m3;
    double water_total_m3;
    double thermal_total_GJ;
//  double thermal_cost;
} sensor_t;

extern sensor_t sensor;
extern bool sensor_valid;

/* ===================== FD CONTEXT ===================== */

typedef enum {
    FD_LISTEN,
    FD_HTTP,
    FD_SERIAL,
    FD_MQTT,
    FD_MQTT_RECONNECT
} fd_type_t;

typedef struct
{
    fd_type_t type;
    int fd;

    uint8_t rbuf[RBUF];
    size_t  rlen;

    uint8_t wbuf[WBUF];
    size_t  wlen;
    size_t  woff;

    int status;	 // mqtt
    int counter; // mqtt
    int timer_fd; // mqtt
    uint16_t pkt_id; // mqtt
    bool first_valid; // serial: set for one cycle when the first valid telegram arrives
    bool io_error;    // serial: set when the device returns EOF or an unrecoverable error
    int  ep_fd;       // epoll fd, stored so write-arming can be done from queue functions

} fd_ctx_t;

/* ============== DECLARATIONS =============== */

// main
void ctx_free(int, fd_ctx_t*);
void ep_mod(int, fd_ctx_t*, uint32_t);

// serial
fd_ctx_t *serial_open(void);
void serial_process(fd_ctx_t*);

// systemd
void systemd_ready(void);
void systemd_notify_init(void);
void systemd_status(const char*);
void systemd_watchdog_ping(void);

// dsmr
int  dsmr_parse_stream(uint8_t*, size_t);

// mqtt_ha
fd_ctx_t *mqtt_open(void);
void mqtt_connect(fd_ctx_t*);
int mqtt_start_connect(fd_ctx_t*);
void mqtt_publish_state(fd_ctx_t*);
void mqtt_send_connect(fd_ctx_t*);
void ha_publish_all(fd_ctx_t*);
ssize_t mqtt_io_read(fd_ctx_t*);
ssize_t mqtt_io_write(fd_ctx_t*);
void schedule_reconnect(int ep, fd_ctx_t*);

// prometheus
fd_ctx_t *make_listen_socket(void);
fd_ctx_t *accept_client(int);
void http_read(fd_ctx_t*, int);
void http_write(fd_ctx_t*, int);


