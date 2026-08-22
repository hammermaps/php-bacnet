# Client-Cache

`Bacnet\Client` und die Client-Seite von `Bacnet\MixedServer` verwenden bei
aktivierter Cache-Funktion immer einen in C implementierten Shared-Memory-Cache
als L1. Dadurch teilen sich PHP-FPM-Worker bzw. FastCGI-Prozesse desselben Hosts
die Einträge. Das standardmäßige L2 ist LMDB. Der reine `Bacnet\Server` und
seine Callbacks werden nicht gecacht.

## Windows (NTS)

Windows-NTS-Builds verwenden einen begrenzten L1-Cache (maximal 256 Einträge)
in benanntem Shared Memory. Ein benannter Mutex schützt die Daten; damit
teilen alle FastCGI-Prozesse desselben Windows-Hosts die Einträge. Das
mitgelieferte LMDB stellt darüber ein persistentes L2 bereit. Das mit
`bacnet.cache_lmdb_path` konfigurierte Verzeichnis muss bereits existieren und
für den PHP-Prozess beschreibbar sein. `getCacheOptions()` meldet bei aktivem
Cache `l1_backend=shared_memory` und `l2_backend=lmdb`. Bei deaktiviertem Cache
bleibt die konfigurierte L2-Auswahl sichtbar, während `l2_available` in den
Statistiken `false` meldet, solange LMDB nicht geöffnet ist.

Die PHP-Laufzeitoptionen für Namespace, LMDB-Pfad, Map-Größe, L2-Größe und
`*_max_entries` sind auch unter Windows NTS verfügbar. Eine Änderung von
Namespace, LMDB-Pfad oder Map-Größe schließt die betroffene Ebene, leert den
Instanzcache und öffnet sie mit der neuen Konfiguration erneut.

`setCacheBackend()` ist unter Windows NTS ebenfalls verfügbar und ersetzt LMDB
instanzweise durch ein `Bacnet\CacheBackendInterface`.

## Installation

LMDB wird aus der fest gepinnten Projektquelle mitgebaut. Das voreingestellte
Verzeichnis `/var/cache/php-bacnet` muss vor dem Start angelegt und für den
PHP-Prozess beschreibbar gemacht werden. Unter Windows ist ein entsprechender
Pfad über `bacnet.cache_lmdb_path` zu setzen. Die
Erweiterung legt es aus Sicherheitsgründen nicht selbst an. Ist es nicht
verfügbar, bleibt L1 aktiv und eine aggregierte Warnung wird ausgegeben.

```ini
bacnet.cache_enabled = 1
bacnet.cache_l2_backend = lmdb
bacnet.cache_lmdb_path = /var/cache/php-bacnet
bacnet.cache_namespace = building-a

bacnet.cache_state_enabled = 0
bacnet.cache_object_enabled = 1
bacnet.cache_object_list_enabled = 1
bacnet.cache_device_enabled = 1
bacnet.cache_ip_enabled = 1
bacnet.cache_negative_enabled = 0
```

Der automatische Namespace besteht aus Interface und UDP-Port. Für mehrere
Deployments auf demselben Host sollte ein expliziter Namespace gesetzt werden.

### Direktiven und Standardwerte

| Direktive | Standard | Bedeutung |
|---|---:|---|
| `bacnet.cache_enabled` | `1` | Gesamten Client-Cache aktivieren |
| `bacnet.cache_l2_backend` | `lmdb` | `lmdb` oder `"none"`; ein PHP-Adapter setzt intern `callback` |
| `bacnet.cache_namespace` | leer | Automatisch `Interface:Port` |
| `bacnet.cache_shm_name` | leer | Automatisch gehashter POSIX-Segmentname |
| `bacnet.cache_lmdb_path` | `/var/cache/php-bacnet` | Vorhandenes, beschreibbares LMDB-Verzeichnis |
| `bacnet.cache_l1_max_bytes` | `16777216` | Logische L1-Speichergrenze |
| `bacnet.cache_l2_max_bytes` | `16777216` | Logische L2-Speichergrenze |
| `bacnet.cache_lmdb_map_size` | `67108864` | LMDB-Map-Größe |
| `bacnet.cache_coherence_interval_ms` | `1000` | Maximales Prüfintervall externer Generationen |
| `bacnet.cache_log_interval_seconds` | `60` | Mindestabstand aggregierter Warnungen |
| `bacnet.cache_state_enabled` | `0` | Dynamische State-Properties |
| `bacnet.cache_object_enabled` | `1` | Statische Objekt-Metadaten |
| `bacnet.cache_object_list_enabled` | `1` | `OBJECT_LIST` getrennt cachen |
| `bacnet.cache_device_enabled` | `1` | Who-Is-/I-Am-Ergebnisse |
| `bacnet.cache_ip_enabled` | `1` | Aktuelle Device-ID-zu-Adresse-Zuordnung |
| `bacnet.cache_negative_enabled` | `0` | Leere Discovery und Read-Timeouts |

Jede positive Partition besitzt zusätzlich `*_ttl_seconds` und
`*_max_entries`. Die Defaults sind State `1/4096`, Object `300/4096`,
Object-List `300/256`, Device `60/256` und IP `300/256`. Negative Einträge
verwenden `bacnet.cache_negative_whois_ttl_seconds=2`,
`bacnet.cache_negative_read_ttl_seconds=1` und maximal 1024 Einträge.

Der State-Cache umfasst `PRESENT_VALUE`, `STATUS_FLAGS`, `EVENT_STATE`,
`OUT_OF_SERVICE`, `RELIABILITY`, `PRIORITY_ARRAY` und Datensatz-Zähler. Der
Objekt-Cache nimmt nur statische Metadaten wie Kennung, Name, Typ, Beschreibung,
Einheiten, Zustandsnamen und Notification-Metadaten auf. Objektlisten liegen in
einer eigenen Partition. Logbuffer, Zeitpläne, Empfängerlisten und unbekannte
Properties werden nicht gecacht.

## PHP-Konfiguration

```php
$client->setCacheOptions([
    'state_enabled' => true,
    'state_ttl' => 1.0,
    'negative_enabled' => true,
]);

$options = $client->getCacheOptions();
$stats = $client->getCacheStats(includeEntries: false, reset: false);
$client->clearCache('object');
```

Die PHP-Schlüssel entsprechen den Direktiven ohne `bacnet.cache_`; TTL-Schlüssel
heißen kurz `state_ttl`, `object_ttl` usw. Änderungen wirken sofort, leeren
betroffene Zustände und lassen kumulierte Statistiken bestehen. Unbekannte oder
ungültige Werte lösen `ValueError` aus.

`whoIs(refresh: true)` sowie der letzte Parameter von `readProperty()` umgehen
L1 und L2. Ein erfolgreicher Write invalidiert betroffene Read-Einträge. Der
Negative-Cache speichert nur leere Who-Is-Ergebnisse und Transport-Timeouts,
keine BACnet-Fehler.

Die C-Schicht speichert die binären BACnet-Anwendungsdaten und erzeugt daraus
bei jedem Treffer neue PHP-Werte. Requestgebundene `zval`-Zeiger gelangen weder
in Shared Memory noch in LMDB. Einträge tragen Formatversion und Ablaufzeit;
unpassende, beschädigte oder abgelaufene LMDB-Daten werden anhand von Länge,
Formatkennung und Prüfsumme verworfen. Nach jedem LMDB-Schreibvorgang entfernt
ein Pruning abgelaufene Einträge und begrenzt sowohl die Partition als auch die
L2-Größe. `clearCache()` löscht namespace- und partitionsweise; gezielte
Invalidierungen löschen nur den betroffenen Schlüsselpräfix. `getCacheStats()` liefert
unter anderem `hits`, `misses`, `stores`, `refreshes`, `expirations`,
`evictions`, `invalidations`, `negative_hits`, `backend_errors` und die
Verfügbarkeit beider Ebenen.

## Eigenes L2-Backend

Ein Objekt mit `Bacnet\CacheBackendInterface` ersetzt LMDB für diese Instanz.
Damit lassen sich Redis, Memcached, APCu oder anwendungsspezifische Speicher
verwenden. Adapterfehler werden fail-open behandelt: BACnet-Zugriffe laufen
weiter, der Fehler erscheint in den Statistiken und höchstens einmal je
Logintervall als Warnung. Siehe `examples/cache_backend.php`.

Cache-Inhalte sind weder authentifiziert noch verschlüsselt. VLAN und Firewall
bleiben die primäre Netzgrenze für klassisches BACnet/IP.
