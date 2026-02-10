/*
 * DSMR P1 Prometheus Exporter
 * Target: Raspberry Pi
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <stdint.h>

/* ---------------- Config ---------------- */

#define SERIAL_DEVICE "/dev/ttyUSB0"
#define BAUDRATE B115200
#define HTTP_PORT 8888
#define MAX_LINE 512

/* ---------------- Metrics ---------------- */

struct {
    double power_import_w;
    double power_export_w;

    double power_import_l[3];
    double power_export_l[3];

    double voltage_l[3];
    double current_l[3];

    double energy_import_kwh;
    double energy_export_kwh;

    double gas_total_m3;

    char equipment_id[32];
} m;

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------------- CRC16 (DSMR) ---------------- */

static uint16_t crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
    return crc;
}

/* ---------------- DSMR Parsing ---------------- */

static void parse_line(const char *l) {
    double v;

    pthread_mutex_lock(&lock);

    sscanf(l, "0-0:96.1.1(%31[^)])", m.equipment_id);

    if (sscanf(l, "1-0:1.7.0(%lf*kW)", &v) == 1) m.power_import_w = v*1000;
    else if (sscanf(l, "1-0:2.7.0(%lf*kW)", &v) == 1) m.power_export_w = v*1000;

    else if (sscanf(l, "1-0:21.7.0(%lf*kW)", &v) == 1) m.power_import_l[0] = v*1000;
    else if (sscanf(l, "1-0:22.7.0(%lf*kW)", &v) == 1) m.power_export_l[0] = v*1000;

    else if (sscanf(l, "1-0:41.7.0(%lf*kW)", &v) == 1) m.power_import_l[1] = v*1000;
    else if (sscanf(l, "1-0:42.7.0(%lf*kW)", &v) == 1) m.power_export_l[1] = v*1000;

    else if (sscanf(l, "1-0:61.7.0(%lf*kW)", &v) == 1) m.power_import_l[2] = v*1000;
    else if (sscanf(l, "1-0:62.7.0(%lf*kW)", &v) == 1) m.power_export_l[2] = v*1000;

    else if (sscanf(l, "1-0:32.7.0(%lf*V)", &v) == 1) m.voltage_l[0] = v;
    else if (sscanf(l, "1-0:52.7.0(%lf*V)", &v) == 1) m.voltage_l[1] = v;
    else if (sscanf(l, "1-0:72.7.0(%lf*V)", &v) == 1) m.voltage_l[2] = v;

    else if (sscanf(l, "1-0:31.7.0(%lf*A)", &v) == 1) m.current_l[0] = v;
    else if (sscanf(l, "1-0:51.7.0(%lf*A)", &v) == 1) m.current_l[1] = v;
    else if (sscanf(l, "1-0:71.7.0(%lf*A)", &v) == 1) m.current_l[2] = v;

    else if (sscanf(l, "1-0:1.8.0(%lf*kWh)", &v) == 1) m.energy_import_kwh = v;
    else if (sscanf(l, "1-0:2.8.0(%lf*kWh)", &v) == 1) m.energy_export_kwh = v;

    else if (sscanf(l, "0-1:24.2.1(%*[^)])(%lf*m3)", &v) == 1) m.gas_total_m3 = v;

    pthread_mutex_unlock(&lock);
}

/* ---------------- Serial Thread ---------------- */

void *serial_thread(void *arg) {
    int fd = open(SERIAL_DEVICE, O_RDONLY | O_NOCTTY);
    if (fd < 0) exit(1);

    struct termios t = {0};
    tcgetattr(fd, &t);
    cfsetispeed(&t, BAUDRATE);
    t.c_cflag = CS8 | CREAD | CLOCAL;
    t.c_iflag = IGNPAR;
    tcsetattr(fd, TCSANOW, &t);

    char buf[256], line[MAX_LINE];
    size_t idx = 0;

    while (1) {
        int n = read(fd, buf, sizeof(buf));
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                line[idx] = 0;
                parse_line(line);
                idx = 0;
            } else if (idx < MAX_LINE-1)
                line[idx++] = buf[i];
        }
    }
}

/* ---------------- HTTP Server ---------------- */

void serve(int c) {
    char out[4096];
    pthread_mutex_lock(&lock);

    snprintf(out, sizeof(out),
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain; version=0.0.4\r\n\r\n"

        "p1_power_watts{direction=\"import\",equipment_id=\"%s\"} %.2f\n"
        "p1_power_watts{direction=\"export\",equipment_id=\"%s\"} %.2f\n"

        "p1_power_phase_watts{direction=\"import\",phase=\"l1\",equipment_id=\"%s\"} %.2f\n"
        "p1_power_phase_watts{direction=\"export\",phase=\"l1\",equipment_id=\"%s\"} %.2f\n"
        "p1_power_phase_watts{direction=\"import\",phase=\"l2\",equipment_id=\"%s\"} %.2f\n"
        "p1_power_phase_watts{direction=\"export\",phase=\"l2\",equipment_id=\"%s\"} %.2f\n"
        "p1_power_phase_watts{direction=\"import\",phase=\"l3\",equipment_id=\"%s\"} %.2f\n"
        "p1_power_phase_watts{direction=\"export\",phase=\"l3\",equipment_id=\"%s\"} %.2f\n"

        "p1_voltage_volts{phase=\"l1\",equipment_id=\"%s\"} %.1f\n"
        "p1_voltage_volts{phase=\"l2\",equipment_id=\"%s\"} %.1f\n"
        "p1_voltage_volts{phase=\"l3\",equipment_id=\"%s\"} %.1f\n"

        "p1_current_amps{phase=\"l1\",equipment_id=\"%s\"} %.1f\n"
        "p1_current_amps{phase=\"l2\",equipment_id=\"%s\"} %.1f\n"
        "p1_current_amps{phase=\"l3\",equipment_id=\"%s\"} %.1f\n"

        "p1_energy_kwh{direction=\"import\",equipment_id=\"%s\"} %.3f\n"
        "p1_energy_kwh{direction=\"export\",equipment_id=\"%s\"} %.3f\n"

        "p1_gas_m3_total{equipment_id=\"%s\"} %.3f\n",

        m.equipment_id, m.power_import_w,
        m.equipment_id, m.power_export_w,

        m.equipment_id, m.power_import_l[0],
        m.equipment_id, m.power_export_l[0],
        m.equipment_id, m.power_import_l[1],
        m.equipment_id, m.power_export_l[1],
        m.equipment_id, m.power_import_l[2],
        m.equipment_id, m.power_export_l[2],

        m.equipment_id, m.voltage_l[0],
        m.equipment_id, m.voltage_l[1],
        m.equipment_id, m.voltage_l[2],

        m.equipment_id, m.current_l[0],
        m.equipment_id, m.current_l[1],
        m.equipment_id, m.current_l[2],

        m.equipment_id, m.energy_import_kwh,
        m.equipment_id, m.energy_export_kwh,

        m.equipment_id, m.gas_total_m3
    );

    pthread_mutex_unlock(&lock);
    write(c, out, strlen(out));
}

void http_server(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in a = {.sin_family=AF_INET,.sin_port=htons(HTTP_PORT),.sin_addr.s_addr=INADDR_ANY};
    bind(s,(struct sockaddr*)&a,sizeof(a));
    listen(s,5);

    while (1) {
        int c = accept(s,NULL,NULL);
        serve(c);
        close(c);
    }
}

int main(void) {
    pthread_t t;
    pthread_create(&t,NULL,serial_thread,NULL);
    http_server();
    return 0;
}

