# Beispiele

Alle Beispiele benötigen die geladene Extension und direkten Zugriff auf das
BACnet/IP-Netz. Pro Prozess darf nur ein `Client`, `Server` oder `MixedServer`
existieren.

`cache_backend.php` zeigt, wie ein beliebiges PHP-Backend LMDB als L2 ersetzt.
`redis_cache_backend.php` implementiert dasselbe Interface beispielhaft mit
`ext-redis`, ohne Redis zur Abhängigkeit der Extension zu machen.
Ohne Override verwenden Client und MixedServer POSIX Shared Memory als L1 und
LMDB als persistentes Standard-L2.

## Abgesicherter Server

`security_server.php` zeigt eine VLAN-Allowlist, eine optionale Denylist,
effektive Optionswerte und die periodische Ausgabe aggregierter
Sicherheitsstatistiken. Standardmäßig bindet es `net3:47808`, stellt
`ANALOG_VALUE:1` lesbar bereit und akzeptiert nur `192.168.202.0/24`:

```bash
BACNET_DEMO_INTERFACE=net3 php examples/security_server.php
```

Vollständig konfiguriertes Beispiel:

```bash
BACNET_DEMO_INTERFACE=net3 \
BACNET_DEMO_DEVICE_ID=9001 \
BACNET_DEMO_PORT=47808 \
BACNET_DEMO_ALLOWED_NETWORKS=192.168.202.0/24,10.20.30.15/32 \
BACNET_DEMO_DENIED_NETWORKS=192.168.202.250/32 \
BACNET_DEMO_STATS_SECONDS=10 \
BACNET_DEMO_INCLUDE_SOURCES=1 \
php examples/security_server.php
```

| Variable | Standard | Bedeutung |
|---|---:|---|
| `BACNET_DEMO_INTERFACE` | `net3` | Linux-Interface-Name. |
| `BACNET_DEMO_DEVICE_ID` | `9001` | Lokale BACnet-Geräteinstanz. |
| `BACNET_DEMO_PORT` | `47808` | Lokaler UDP-Port. |
| `BACNET_DEMO_ALLOWED_NETWORKS` | `192.168.202.0/24` | Kommagetrennte IPv4-CIDRs; leer erlaubt alle. |
| `BACNET_DEMO_DENIED_NETWORKS` | leer | Kommagetrennte gesperrte IPv4-CIDRs. |
| `BACNET_DEMO_STATS_SECONDS` | `10` | Intervall der JSON-Statistikausgabe. |
| `BACNET_DEMO_INCLUDE_SOURCES` | `0` | Bei `1` quellbezogene Diagnosewerte ausgeben. |
| `BACNET_DEMO_ALLOW_WRITE` | `0` | Bei `1` WriteProperty für `ANALOG_VALUE:1` registrieren. |

Writes sind absichtlich standardmäßig deaktiviert, weil erst das Registrieren
von `onWriteProperty()` Anwendungsschreibzugriff freigibt. Mit
`BACNET_DEMO_ALLOW_WRITE=1` aktualisiert die Demo `PRESENT_VALUE` nur bei
BACnet-REAL-Werten von 10,0 bis 30,0. Die Demo ist kein Ersatz für eine
anlagenspezifische Autorisierung.

Die erste JSON-Zeile enthält `getSecurityOptions()`, spätere Zeilen enthalten
`getSecurityStats()`. Bedeutung und Tuning aller Felder beschreibt
[`docs/server-security.md`](../docs/server-security.md).

## Mixed-Daemon

`mixed_daemon.php` stellt `DEVICE:5` mit dem Namen `proxy` und
`ANALOG_VALUE:1` als wechselnden Testwert bereit. Die Demo beantwortet für
beide Objekte `Object_Identifier`, `Object_Name`, `Object_Type`,
`Description` und für den Analogwert `Present_Value`; sie eignet sich daher
auch zum Testen von `ReadPropertyMultiple` mit `PROP_ALL`. Gleichzeitig sucht
der Prozess regelmäßig nach externen Geräten.

```bash
BACNET_DEMO_INTERFACE=net3 \
BACNET_DEMO_DEVICE_ID=5 \
BACNET_DEMO_VENDOR_ID=123 \
BACNET_DEMO_VENDOR_NAME='Ihr Herstellername' \
BACNET_DEMO_MODEL_NAME='Ihr Modellname' \
php examples/mixed_daemon.php
```

`BACNET_DEMO_VENDOR_ID`, `BACNET_DEMO_VENDOR_NAME` und `BACNET_DEMO_MODEL_NAME`
sind erforderlich und müssen die eigenen, registrierten Herstellerdaten
enthalten. Optionale Variablen sind `BACNET_DEMO_PORT` (Standard `47808`),
`BACNET_DEMO_SWITCH_SECONDS` (Standard `6`) und
`BACNET_DEMO_DISCOVERY_SECONDS` (Standard `60`).
`BACNET_DEMO_SECURITY_STATS_SECONDS` steuert die Ausgabe geerbter
Sicherheitsstatistiken (Standard `60`). Mit `BACNET_DEMO_ALLOWED_NETWORKS` und
`BACNET_DEMO_DENIED_NETWORKS` können kommaseparierte CIDR-Overrides gesetzt
werden; bleiben die Variablen ungesetzt, gelten ausschließlich die INI-Werte.
Beenden mit `Ctrl+C`.

Der MixedServer erbt die gesamte Sicherheits-API. Ohne explizite Overrides
verwendet diese Demo die aktuellen `bacnet.server_*`-INI-Werte. Eingehende
Pakete werden vor Aufnahme in die Mixed-Queue geprüft.

## Einzelwert lesen

`mixed_read.php` startet einen kurzlebigen MixedServer, entdeckt ein bestimmtes
Gerät und liest eine Eigenschaft. Beispiel:

```bash
BACNET_DEMO_INTERFACE=net3 BACNET_DEMO_DEVICE_ID=5 \
php examples/mixed_read.php 200 analog_value 10110 present_value
```

Unterstützt werden die grundlegenden Analog-, Binär- und Device-Objekttypen
sowie `object_identifier`, `object_name`, `object_type`, `description` und
`present_value`.
