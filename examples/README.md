# Beispiele

Alle Beispiele benötigen die geladene Extension und direkten Zugriff auf das
BACnet/IP-Netz. Pro Prozess darf nur ein `Client`, `Server` oder `MixedServer`
existieren.

## Mixed-Daemon

`mixed_daemon.php` stellt `DEVICE:5` mit dem Namen `proxy` und
`ANALOG_VALUE:1` als wechselnden Testwert bereit. Gleichzeitig sucht der
Prozess regelmäßig nach externen Geräten.

```bash
BACNET_DEMO_INTERFACE=net3 \
BACNET_DEMO_DEVICE_ID=5 \
php examples/mixed_daemon.php
```

Optionale Variablen sind `BACNET_DEMO_PORT` (Standard `47808`),
`BACNET_DEMO_SWITCH_SECONDS` (Standard `6`) und
`BACNET_DEMO_DISCOVERY_SECONDS` (Standard `60`). Beenden mit `Ctrl+C`.

## Einzelwert lesen

`mixed_read.php` startet einen kurzlebigen MixedServer, entdeckt ein bestimmtes
Gerät und liest eine Eigenschaft. Beispiel:

```bash
BACNET_DEMO_INTERFACE=net3 BACNET_DEMO_DEVICE_ID=5 \
php examples/mixed_read.php 200 analog_value 10110 present_value
```

Unterstützt werden die grundlegenden Analog-, Binär- und Device-Objekttypen
sowie `object_name` und `present_value`.
