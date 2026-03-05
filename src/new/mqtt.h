#pragma once
#include <stdint.h>

void mqtt_init(int epoll_fd);
void mqtt_start_connect(void);
void mqtt_on_epoll(uint32_t events);
void mqtt_on_timer(int fd);
void mqtt_publish_telegram(void);
void mqtt_schedule_reconnect(void);
