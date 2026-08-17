# Server-Sicherheit

`Bacnet\Server` und `Bacnet\MixedServer` verwenden dieselben Schutzfunktionen
für eingehende BACnet/IP-Pakete. Sie sind standardmäßig aktiv und für ein
moderates Automations-LAN vorkonfiguriert. Die Schutzschicht begrenzt Floods,
filtert IPv4-Quellen und verhindert die wiederholte Ausführung bereits
erfolgreicher WriteProperty-Anfragen.

## Sicherheitsmodell und Grenzen

Die Schutzschicht verarbeitet ein Paket in dieser Reihenfolge:

1. IPv4-Allow- und Denylisten; Deny hat immer Vorrang.
2. Eine bereits aktive temporäre Sperre der Quell-IP.
3. Grundlegende NPDU-/APDU-Strukturprüfung.
4. Globales und quellbezogenes Paketlimit.
5. Zusätzliches Who-Is- oder WriteProperty-Limit, falls zutreffend.
6. Normale Serververarbeitung beziehungsweise Aufnahme in die MixedServer-Queue.

ACL-, Block-, Rate- und malformed-Drops werden still verworfen. Gültige
BACnet-Anfragen erreichen weiterhin die vorhandene Protokollverarbeitung;
beispielsweise bleiben `Unknown Object`-Fehler unverändert.

Klassisches BACnet/IP ist weiterhin unverschlüsselt und unauthentifiziert.
VLAN und Firewall müssen deshalb die primäre Netzgrenze bilden. Die interne
ACL ist eine zusätzliche Begrenzung, kein Ersatz dafür. BACnet/SC wäre die
kryptografisch geschützte BACnet-Variante und gehört nicht zum Umfang dieser
Version.

## Standardwerte

| PHP-Optionsschlüssel | INI-Direktive | Standard | Bedeutung |
|---|---|---:|---|
| `enabled` | `bacnet.server_security_enabled` | `true` | Gesamte Schutzschicht aktivieren. |
| `per_source_rate` | `bacnet.server_per_source_rate` | `50` | Pakete pro Sekunde je Quell-IP. |
| `per_source_burst` | `bacnet.server_per_source_burst` | `100` | Kurzzeit-Burst je Quell-IP. |
| `global_rate` | `bacnet.server_global_rate` | `500` | Pakete pro Sekunde über alle Quellen. |
| `global_burst` | `bacnet.server_global_burst` | `1000` | Globaler Kurzzeit-Burst. |
| `who_is_rate` | `bacnet.server_who_is_rate` | `2` | Who-Is-Pakete pro Sekunde je Quelle. |
| `who_is_burst` | `bacnet.server_who_is_burst` | `5` | Who-Is-Kurzzeit-Burst je Quelle. |
| `write_rate` | `bacnet.server_write_rate` | `5` | WriteProperty-Pakete pro Sekunde je Quelle. |
| `write_burst` | `bacnet.server_write_burst` | `10` | WriteProperty-Kurzzeit-Burst je Quelle. |
| `flood_violations` | `bacnet.server_flood_violations` | `20` | Limitverletzungen bis zur Quellsperre. |
| `flood_window_seconds` | `bacnet.server_flood_window_seconds` | `10` | Fenster für gezählte Limitverletzungen. |
| `block_duration_seconds` | `bacnet.server_block_duration_seconds` | `60` | Dauer der temporären Quellsperre. |
| `duplicate_window_seconds` | `bacnet.server_duplicate_window_seconds` | `5` | Gültigkeit erfolgreicher Write-Fingerprints. |
| `allowed_networks` | `bacnet.server_allowed_networks` | `[]` | Erlaubte IPv4-CIDRs; leer erlaubt alle. |
| `denied_networks` | `bacnet.server_denied_networks` | `[]` | Gesperrte IPv4-CIDRs; Deny gewinnt. |
| `max_sources` | `bacnet.server_max_sources` | `1024` | Maximale Anzahl verwalteter Quellzustände. |
| `source_ttl_seconds` | `bacnet.server_source_ttl_seconds` | `300` | Ablaufzeit inaktiver Quellzustände. |
| `log_interval_seconds` | `bacnet.server_log_interval_seconds` | `60` | Mindestabstand aggregierter Warnungen. |

Raten werden mit monoton gemessener Zeit als Token-Buckets umgesetzt. Ein
Burst ist die sofort verfügbare Token-Kapazität; die zugehörige Rate füllt sie
kontinuierlich wieder auf.

## Konfigurationspriorität

Die effektiven Werte entstehen in folgender Reihenfolge:

1. eingebaute Standardwerte,
2. aktuelle INI-Werte beim Aufruf des Konstruktors,
3. partielle Overrides durch `setSecurityOptions()`.

```ini
; php.ini oder conf.d/30-bacnet.ini
bacnet.server_security_enabled = 1
bacnet.server_allowed_networks = 192.168.202.0/24
bacnet.server_denied_networks = 192.168.202.250/32
bacnet.server_write_rate = 3
bacnet.server_write_burst = 6
```

INI-Listen sind kommaseparierte IPv4-CIDRs. PHP-Overrides verwenden Arrays:

```php
$server->setSecurityOptions([
    'per_source_rate' => 40,
    'per_source_burst' => 80,
    'write_rate' => 3,
    'write_burst' => 6,
    'allowed_networks' => ['192.168.202.0/24'],
    'denied_networks' => ['192.168.202.250/32'],
]);
```

Ein Override ist partiell: Nicht genannte Werte bleiben unverändert. Die
Änderung wirkt sofort und setzt Token-, Verletzungs-, Sperr- und
Duplicate-Zustände zurück. Kumulierte Statistiken bleiben erhalten. Unbekannte
Schlüssel, falsche Typen, nicht positive Grenzwerte und ungültige CIDRs lösen
`ValueError` aus.

`getSecurityOptions()` liefert sämtliche effektiven Werte. Damit lässt sich
nach INI- und PHP-Konfiguration die tatsächlich aktive Einstellung prüfen:

```php
var_export($server->getSecurityOptions());
```

## Allow- und Denylisten

Nur IPv4-CIDR-Notation wird akzeptiert. Einzelne Hosts werden mit `/32`
angegeben. Eine leere Allowlist erlaubt alle Quellen. Sobald mindestens ein
Allow-Netz vorhanden ist, werden Quellen außerhalb dieser Netze verworfen.
Deny wird danach mit höherer Priorität angewendet.

```php
$server->setSecurityOptions([
    'allowed_networks' => [
        '192.168.202.0/24',
        '10.20.30.15/32',
    ],
    'denied_networks' => [
        '192.168.202.250/32',
    ],
]);
```

## Temporäre Quellsperren

Rate-Limit-Verletzungen werden pro Quell-IP gezählt. Erreicht eine Quelle
innerhalb von `flood_window_seconds` den Wert `flood_violations`, wird sie für
`block_duration_seconds` gesperrt. Weitere Pakete dieser IP zählen als
`blocked_drops`. Nach Ablauf darf die Quelle wieder Pakete senden.

Es werden höchstens `max_sources` Quellzustände gehalten. Inaktive Einträge
laufen nach `source_ttl_seconds` ab. Ist die Tabelle vorher voll, ersetzt der
Server den am längsten nicht verwendeten Eintrag.

## WriteProperty-Deduplizierung

Nur erfolgreich durch den PHP-Callback verarbeitete Writes werden gespeichert.
Der Fingerprint umfasst Quell-IP, Invoke-ID, Objekt, Property und Payload.
Kommt derselbe Write innerhalb von `duplicate_window_seconds` erneut an,
sendet der Server nochmals den BACnet Simple-ACK, führt den Callback aber nicht
erneut aus. Dadurch bleiben Retransmits für den Client erfolgreich, ohne eine
Anlagenaktion zu wiederholen.

Die Registrierung von `onWriteProperty()` bleibt die bewusste Schreibfreigabe.
Die Schutzschicht ergänzt keinen fachlichen Autorisierungs-Callback. Prüfe im
Handler weiterhin explizit Objekt, Property, Datentyp, Wertebereich und
Anlagenzustand.

## Statistiken

```php
$stats = $server->getSecurityStats(
    includeSources: false,
    reset: false,
);
```

| Schlüssel | Bedeutung |
|---|---|
| `accepted_packets` | Von der Schutzschicht akzeptierte Pakete. |
| `rate_drops` | Durch einen Token-Bucket verworfene Pakete. |
| `acl_drops` | Durch Allow- oder Denylisten verworfene Pakete. |
| `blocked_drops` | Pakete bereits temporär gesperrter Quellen. |
| `malformed_pdus` | Strukturell ungültige NPDU/APDU- oder Service-PDUs. |
| `queue_overflows` | Nicht mehr in die MixedServer-Queue passende Pakete. |
| `deduplicated_writes` | Erneut bestätigte Writes ohne Callback-Ausführung. |
| `active_sources` | Noch nicht abgelaufene Quellzustände. |
| `blocked_sources` | Davon aktuell temporär gesperrte Quellen. |

Mit `includeSources: true` enthält das Ergebnis zusätzlich `sources`. Diese
Map ist auf `max_sources` begrenzt und je IPv4-Adresse aufgebaut. Ein Eintrag
enthält `accepted_packets`, `rate_drops`, `acl_drops`, `blocked_drops`,
`malformed_pdus`, `blocked` und `idle_seconds`.

`reset: true` gibt zuerst den aktuellen Stand zurück und setzt danach die
kumulierten globalen und quellbezogenen Diagnosezähler zurück. Aktive Token-
und Sperrzustände werden dabei nicht verändert.

```php
$snapshot = $server->getSecurityStats(includeSources: true, reset: true);
fwrite(STDOUT, json_encode($snapshot, JSON_THROW_ON_ERROR) . PHP_EOL);
```

Drops erzeugen höchstens einmal pro `log_interval_seconds` eine aggregierte
PHP-Warnung. Einzelne verworfene Pakete werden nicht protokolliert, damit ein
Flood nicht selbst einen Log-Flood verursacht.

## MixedServer-Queue

Während synchroner `whoIs()`-, ReadProperty- oder WriteProperty-Aufrufe legt
`MixedServer` eingehende Server-PDUs in einer Queue mit 32 Einträgen ab. Die
Sicherheitsprüfung läuft vor der Queue. ACL-, Rate-, Block- und malformed-Drops
belegen daher keinen Queue-Platz und können zulässige Anfragen nicht verdrängen.

## Betriebsempfehlungen

- BACnet/IP in ein eigenes VLAN legen und UDP 47808 an der Firewall nur für
  benötigte Kommunikationspartner freigeben.
- `allowed_networks` auf die tatsächlich verwendeten Automationsnetze setzen.
- Deny-Einträge nur als zusätzliche Ausnahme verwenden; Deny gewinnt immer.
- Raten zunächst mit den Defaults betreiben und anhand von Statistiken messen,
  bevor sie abgesenkt werden.
- `includeSources: true` nur für Diagnose-Snapshots verwenden, nicht in jedem
  Event-Loop-Durchlauf.
- Write-Callbacks fachlich validieren und echte Anlagenwerte nicht ungeprüft
  aus dem Netzwerk übernehmen.
- `bacnet.server_security_enabled=0` nur für kontrollierte Fehlersuche nutzen;
  damit entfallen auch Queue-Vorfilterung, ACLs, Limits und Deduplizierung.

Eine ausführbare Konfiguration befindet sich in
[`examples/security_server.php`](../examples/security_server.php).
