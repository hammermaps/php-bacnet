# FAQ

## Welche PHP-Versionen werden unterstützt?

Die Erweiterung benötigt PHP 8.4 oder 8.5 als NTS-Build unter Linux. Sie muss mit
derselben PHP-API gebaut werden wie die Runtime, die sie lädt. Prüfe das mit
`php -v` und `php --ri bacnet`.

## Wie prüfe ich, ob die Erweiterung geladen ist?

```bash
php -m | grep '^bacnet$'
php --ri bacnet
```

Die zweite Abfrage zeigt auch Version sowie Standardwerte für Interface, Port und
Timeout.

## Was übergebe ich als Interface?

Übergebe den Namen des Linux-Interfaces, nicht dessen IP-Adresse:

```php
$client = new Bacnet\Client('eth0', 47808, 5000);
```

Ermittle Namen und Adressen mit `ip -4 -o addr show`.

## Warum sollte der Client auf UDP 47808 laufen?

BACnet/IP verwendet standardmäßig UDP 47808 (`0xBAC0`). Manche Geräte senden ihre
I-Am-Antwort nicht an den Quellport der Who-Is-Anfrage, sondern an den
Broadcast-Port 47808. Ein Client auf einem alternativen Port kann solche Geräte
deshalb verpassen. Verwende für Discovery nach Möglichkeit 47808.

## Warum findet `whoIs()` zunächst keine Geräte?

Einige Geräte verpassen gelegentlich den ersten Broadcast nach dem Öffnen eines
Sockets. `Client::whoIs()` wiederholt eine leere Suche automatisch einmal nach
250 ms. Wähle trotzdem einen ausreichenden Timeout, zum Beispiel 5000 ms.

## Warum dauert das Lesen einer Objektliste lange?

Große Geräte können mehrere hundert Objekte enthalten. Einige Geräte brechen den
Abruf der vollständigen `OBJECT_LIST` ab; dann muss die Liste über den
`arrayIndex` einzeln gelesen werden. Das erzeugt einen BACnet-Request pro
Objekt und kann spürbar dauern. Lies zunächst den Index `0`, um die Anzahl zu
ermitteln, frage nur benötigte Bereiche ab und cache Objektmetadaten in der
Anwendung. Aktuelle `PRESENT_VALUE`-Werte sollten dagegen nicht lange gecacht
werden.

## Warum zeigt ein Wert direkt nach `writeProperty()` noch den alten Stand?

Ein erfolgreicher Write-ACK bestätigt zunächst nur, dass das Gerät den Auftrag
angenommen hat. Die Regelungslogik kann den Wert erst im nächsten Zyklus
übernehmen oder ihn durch eine höhere BACnet-Priorität ersetzen. Lies
`PRESENT_VALUE` daher für einige Sekunden wiederholt und bewerte eine Änderung
erst dann als wirksam. Ein realer Test zeigte nach einer Sekunde noch den alten
Wert, nach insgesamt etwa sechs Sekunden jedoch den geschriebenen Sollwert.
Für einen Sollwert etwa:

```php
$device->writeProperty($type, $instance, Bacnet\Property::PRESENT_VALUE,
    Bacnet\Value::real(22.0));
for ($attempt = 0; $attempt < 6; $attempt++) {
    sleep(1);
    $value = $device->readProperty($type, $instance, Bacnet\Property::PRESENT_VALUE);
    if ((float) $value === 22.0) {
        break;
    }
}
```

Vermeide wiederholte Schreibversuche ohne Prüfung: Sie können unnötige
Prioritäten oder unerwartete Anlagenzustände erzeugen.

## Wie finde ich den Betriebszustand oder einen Betriebsschalter?

BACnet standardisiert Objekttypen, nicht die Namen der Anlagenpunkte. Suche
zuerst in `OBJECT_NAME` und `DESCRIPTION` nach Begriffen wie `Betrieb`,
`Freigabe`, `Mode` oder `Wählgerät` und lies anschließend `PRESENT_VALUE`.
Ein Binary Output mit der Beschreibung „Freigabe“ kann einen einzelnen Antrieb
statt des Gesamtbetriebs steuern. Schreibe Werte erst, wenn die Anlagenlogik,
der korrekte Punkt und die verwendete BACnet-Priorität bestätigt sind.

## Wie finde ich typische Netzwerkprobleme?

BACnet/IP-Broadcasts bleiben im lokalen Subnetz. Prüfe Interface, VLAN und
Firewall sowie den Datenverkehr:

```bash
sudo tcpdump -ni eth0 'udp port 47808'
```

Erwartet werden Who-Is- und I-Am-Pakete. Fehlen I-Am-Antworten bereits am Host,
liegt die Ursache außerhalb des PHP-Prozesses.

## Ersetzen die Server-Schutzfunktionen VLAN oder Firewall?

Nein. ACLs, Rate-Limits, temporäre Sperren und Write-Deduplizierung reduzieren
die Wirkung von Floods und Wiederholungen innerhalb des erreichbaren Netzes.
Klassisches BACnet/IP authentisiert und verschlüsselt Pakete jedoch nicht.
Begrenze den Zugriff primär über ein eigenes VLAN und eine Firewall. BACnet/SC
ist für kryptografisch geschützte Verbindungen vorgesehen, wird in dieser
Version aber nicht implementiert.

`onWriteProperty()` bleibt eine bewusste Schreibfreigabe: Ohne registrierten
Callback werden Writes nicht an Anwendungscode weitergereicht. Für feinere
Berechtigungen prüft der Callback Objekt, Property und Anlagenzustand selbst.

Details stehen im Leitfaden [Server-Sicherheit](./server-security.md).

## Warum reagiert der Server auf einzelne Pakete nicht?

ACL-, Rate-, Block- und malformed-Pakete werden absichtlich still verworfen.
Prüfe zuerst die aggregierten Zähler:

```php
var_export($server->getSecurityStats(includeSources: true));
```

- `acl_drops` steigt: Allow-/Deny-CIDRs und die tatsächliche Quell-IP prüfen.
- `rate_drops` steigt: Quell-, Global- und dienstspezifische Raten prüfen.
- `blocked_drops` steigt: Die Quelle hat zuvor im Flood-Fenster zu viele
  Limitverletzungen erzeugt und bleibt bis zum Ablauf der Sperrdauer blockiert.
- `malformed_pdus` steigt: NPDU/APDU oder Read-/Write-Service-Payload ist ungültig.
- `queue_overflows` steigt: Der MixedServer-Event-Loop leert seine Queue nicht
  schnell genug; Timeouts verkürzen und `poll()` häufiger aufrufen.

`getSecurityOptions()` zeigt die tatsächlich aus Defaults, INI und
Instanz-Overrides entstandenen Werte. Für Diagnose-Snapshots kann
`getSecurityStats(includeSources: true, reset: true)` den bisherigen Stand
zurückgeben und die Zähler anschließend zurücksetzen.

## Warum läuft ein Write-Callback bei einem Retransmit nur einmal?

Ein bereits erfolgreich verarbeiteter WriteProperty-Request wird standardmäßig
fünf Sekunden dedupliziert. Der Server sendet dem wiederholenden Client erneut
den Simple-ACK, ruft den PHP-Callback aber nicht nochmals auf. Der Zähler
`deduplicated_writes` bestätigt dieses Verhalten. Der Fingerprint enthält
Quelle, Invoke-ID, Objekt, Property und Payload.

Das Fenster kann über `bacnet.server_duplicate_window_seconds` oder den
PHP-Schlüssel `duplicate_window_seconds` angepasst werden.

## Wie deaktiviere ich die Schutzschicht für eine Diagnose?

Global per INI:

```ini
bacnet.server_security_enabled = 0
```

Oder für eine bereits erzeugte Instanz:

```php
$server->setSecurityOptions(['enabled' => false]);
```

Damit entfallen ACLs, Limits, temporäre Sperren, Write-Deduplizierung und beim
MixedServer die Queue-Vorfilterung. Nutze dies nur kurzzeitig in einem bereits
durch VLAN und Firewall isolierten Testnetz.

## Warum erscheint „Bacnet\Client or Server already initialized“?

Der eingebettete BACnet-Stack verwendet pro PHP-Prozess einen UDP-Socket. Lege
pro Request genau einen `Bacnet\Client` oder `Bacnet\Server` an und reiche dieses
Objekt weiter. In PHP-FPM wird die Sperre zu Beginn des nächsten Requests
zurückgesetzt.

## Wie baue ich nach einer PHP-Aktualisierung neu?

Baue die Erweiterung mit dem passenden `phpize` und `php-config` neu, etwa:

```bash
phpize8.5 --clean && phpize8.5
./configure --with-bacnet --with-php-config=php-config8.5
make -j$(nproc)
```

Installiere anschließend das erzeugte `modules/bacnet.so` in das passende
Extension-Verzeichnis und starte den verwendeten PHP-Dienst neu.
