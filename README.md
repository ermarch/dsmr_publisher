# dsmr_publisher

A Dutch Smart Meter Requirements (DSMR) P1 port publisher for
[Prometheus](https://prometheus.io) and [Home Assistant](https://www.home-assistant.io)
via MQTT.

Reads OBIS data from the P1 serial port of a Dutch smart meter and
simultaneously exposes a Prometheus scrape endpoint and publishes sensor
discovery + state messages to an MQTT broker (Home Assistant auto-discovery
included). Designed to run as a systemd service on a Raspberry Pi.

---

## Building

```bash
cmake -B build
cmake --build build
sudo cp build/dsmr_publisher /usr/local/bin/dsmr_publisher
```

---

## Configuration

Create `/etc/dsmr_publisher.conf`:

```ini
[serial]
device: /dev/ttyUSB0

[prometheus]
hostname: 127.0.0.1
port: 8888

[mqtt]
hostname: 192.168.1.206
port: 1883
username: mqtt_user
password: password
keepalive: 60

reconnect_initial: 1
reconnect_max: 60
reconnect_jitter: 20
```

All values shown are the built-in defaults and can be omitted if unchanged.

| Section      | Key                 | Default         | Description                                      |
|--------------|---------------------|-----------------|--------------------------------------------------|
| `serial`     | `device`            | `/dev/ttyUSB0`  | Serial device connected to the P1 port           |
| `prometheus` | `hostname`          | `127.0.0.1`     | Address the HTTP scrape server binds to          |
| `prometheus` | `port`              | `8888`          | TCP port for the Prometheus scrape endpoint      |
| `mqtt`       | `hostname`          | `192.168.1.206` | MQTT broker address                              |
| `mqtt`       | `port`              | `1883`          | MQTT broker port                                 |
| `mqtt`       | `username`          | `mqtt_user`     | MQTT username                                    |
| `mqtt`       | `password`          |                 | MQTT password                                    |
| `mqtt`       | `keepalive`         | `60`            | MQTT keepalive interval in seconds               |
| `mqtt`       | `reconnect_initial` | `1`             | Initial reconnect back-off in seconds            |
| `mqtt`       | `reconnect_max`     | `60`            | Maximum reconnect back-off in seconds            |
| `mqtt`       | `reconnect_jitter`  | `20`            | Reconnect jitter as a percentage of back-off     |

---

## systemd Service

Copy the unit file and create a dedicated user:

```bash
sudo cp admin/p1_exporter.service /etc/systemd/system/dsmr_publisher.service
sudo useradd -r p1
sudo usermod -aG dialout p1
sudo systemctl daemon-reload
sudo systemctl enable --now dsmr_publisher
```

The service file (`admin/p1_exporter.service`):

```ini
[Unit]
Description=DSMR P1 Prometheus and Home Assistant publisher
After=network.target

[Service]
Type=notify
WatchdogSec=30s
NotifyAccess=main
ExecStart=/usr/local/bin/dsmr_publisher
Restart=always
User=p1
Group=dialout
NoNewPrivileges=yes
PrivateTmp=yes
ProtectSystem=strict
ProtectHome=yes

[Install]
WantedBy=multi-user.target
```

The `p1` user needs to be in the `dialout` group to access the serial device.

---

## Prometheus

Add the following to `/etc/prometheus/prometheus.yml`
(see also `admin/prometheus.yml`):

```yaml
scrape_configs:
  - job_name: 'dsmr_publisher'
    static_configs:
      - targets: ['localhost:8888']
```

### Exposed Metrics

All metrics carry an `equipment_id` label populated from the meter's OBIS
identifier (`0-0:96.1.1`).

| Metric | Labels | Unit | Description |
|--------|--------|------|-------------|
| `p1_active_tariff` | `equipment_id` | — | Current tariff indicator (1 or 2) |
| `p1_power_failures` | `equipment_id` | — | Cumulative power failure count |
| `p1_energy_total` | `direction`, `tariff`, `equipment_id` | kWh | Cumulative energy (import); `tariff` is `t1`, `t2`, or omitted for the combined total |
| `p1_energy` | `direction`, `tariff`, `equipment_id` | kWh | Cumulative energy (export); same label structure |
| `p1_power` | `direction`, `phase`, `equipment_id` | W | Instantaneous power per phase (`l1`–`l3`) and `all` |
| `p1_voltage` | `phase`, `equipment_id` | V | Instantaneous voltage per phase |
| `p1_current` | `phase`, `equipment_id` | A | Instantaneous current per phase |
| `p1_gas_total` | `equipment_id` | m³ | Cumulative gas consumption |
| `p1_water_total` | `equipment_id` | m³ | Cumulative water consumption |

---

## Home Assistant (MQTT)

The publisher uses Home Assistant MQTT auto-discovery on the
`homeassistant/` prefix. Once connected to the broker, sensors appear
automatically in HA under a device named **DSMR Smart Meter**.

MQTT topics used:

| Topic | Purpose |
|-------|---------|
| `dsmr/state` | JSON state payload (published after every telegram) |
| `dsmr/availability` | LWT — `online` / `offline` |
| `homeassistant/state` | Subscribed — triggers re-discovery on HA restart |
| `homeassistant/sensor/<id>/config` | Discovery config per sensor (retained) |

---

## Notes

- **Gas meter** readings update at most once per hour per the DSMR spec.
  Use `increase()` or `delta()` in PromQL rather than `rate()`.
- **Total power ≠ sum of phases** — the meter reports these independently;
  this is normal DSMR behaviour.
- **Per-phase export** is available and useful for diagnosing solar PV
  phase imbalance.
- The CRC-16 of each telegram is verified before parsing; corrupt telegrams
  are silently discarded.
