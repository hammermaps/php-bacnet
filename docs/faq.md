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

## Wie finde ich typische Netzwerkprobleme?

BACnet/IP-Broadcasts bleiben im lokalen Subnetz. Prüfe Interface, VLAN und
Firewall sowie den Datenverkehr:

```bash
sudo tcpdump -ni eth0 'udp port 47808'
```

Erwartet werden Who-Is- und I-Am-Pakete. Fehlen I-Am-Antworten bereits am Host,
liegt die Ursache außerhalb des PHP-Prozesses.

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
