# Step 7: Who-Is & Device Discovery

**Ziel:** Geräte im BACnet-Netzwerk per Broadcast finden. `Client::whoIs()` sendet Broadcasts, empfängt `I-Am`-Antworten und returned deduplizierte `Bacnet\Device`-Instanzen.

## Aktionen

1.  **PHP-Methodensignatur:**
    ```php
    namespace Bacnet;
    class Client {
        /**
         * @param int|null $lowLimit  Geräte-Instance-ID von (null = wildcard)
         * @param int|null $highLimit Geräte-Instance-ID bis (null = wildcard)
         * @param int|null $timeoutMs Wartezeit in ms; null = bacnet.default_timeout_ms
         * @return Device[]
         * @throws TimeoutException wenn keine Antworten innerhalb der Zeit
         */
        public function whoIs(?int $lowLimit = null, ?int $highLimit = null, ?int $timeoutMs = null): array;
    }
    ```

2.  **Request bauen:**
    - Nutze `WhoIs_encode_apdu()` aus `bacnet/whois.h`.
    - Falls beide Limits null: Unicast-Range `0` bis `4194303` (BACNET_MAX_INSTANCE).
    - `BACNET_CONFIRMED_SERVICE` ist **nicht** nötig – Who-Is ist *Unconfirmed Request*.

3.  **Broadcast senden:**
    - `dest` auf `BROADCAST_NETWORK` (0xFFFF) und MAC-Len 0 setzen (BIP-Broadcast).
    - NPDU-Daten: `data_expecting_reply = false`, `network_layer_message = false`.
    - `datalink_send_pdu()` mit dem gebauten APDU.

4.  **Collector-Loop (`php_bacnet_broadcast_and_collect`):**
    - Lese vom Socket für `timeoutMs` via `select()` oder `datalink_receive()` im Loop.
    - Jede empfangene PDU: Prüfe NPDU (Discarded-Flag ignorieren), dann APDU-Type.
    - Nur `PDU_TYPE_UNCONFIRMED_SERVICE` mit `SERVICE_CHOICE_I_AM` akzeptieren.

5.  **I-Am decodieren:**
    - `iam_decode_service_request()` liefert `device_id`, `max_apdu`, `segmentation`, `vendor_id`.
    - Quell-Adresse (`src`) aus `datalink_receive()` als `BACNET_ADDRESS` speichern.

6.  **Deduplizierung & Objekt-Erzeugung:**
    - Temporäres `HashTable` (oder einfaches C-Array) mit Key = `device_id`.
    - Für jede neue `device_id`: Erzeuge `Bacnet\Device`-ZObjekt:
      - Setze intern `device_id`.
      - Kopiere `BACNET_ADDRESS` in `php_bacnet_device_obj`.
      - ZVAL_COPY des `Client`-ZObjekts in `client_zval`.
    - Sammle alle in ein PHP-Array (`zend_new_array` + `zend_hash_next_index_insert`).

7.  **Edge Cases:**
    - Keine Antworten innerhalb Timeout: Leeres Array returnen (kein Exception-Wurf nötig, da Broadcast unzuverlässig ist). Oder optional nach 2x Retry? Für MVP: einfach warten und was kommt, kommt.
    - Parse-Fehler bei I-Am: Eintrag überspringen, restliche Antworten weiter verarbeiten (nicht alles abbrechen).

## Akzeptanzkriterien

- [ ] `whoIs()` returned `array` (ggf. leer).
- [ ] Jedes Array-Element ist `instanceof Bacnet\Device`.
- [ ] `$device->getDeviceId()` (oder Property) liefert die aus I-Am extrahierte Instance-ID.
- [ ] `$device->readProperty(...)` ist direkt im Anschluss aufrufbar (Socket wiederverwendbar).
- [ ] Broadcast funktioniert über `255.255.255.255:47808` bzw. Subnet-Broadcast.
