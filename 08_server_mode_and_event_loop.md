# Step 8: BACnet-Server / Device-Modus & Event-Loop

**Ziel:** Extension kann auch als BACnet-Server/Device agieren. Eingehende Requests werden an PHP-Callbacks delegiert. Ein non-blocking `poll()` ermöglicht Integration in PHP-Userland-Loops (ReactPHP, Fiber-Loop, etc.).

## Aktionen

1.  **Neue Klasse `Bacnet\Server`:**
    ```php
    namespace Bacnet;
    class Server {
        public function __construct(int $deviceId, string $bindInterface = '0.0.0.0', int $port = 47808);
        
        /** Registriert ein lokales Objekt, auf das ReadProperty-Anfragen beantwortet werden */
        public function addLocalObject(ObjectIdentifier $oid): void;
        
        /** Entfernt ein lokales Objekt */
        public function removeLocalObject(ObjectIdentifier $oid): void;
        
        /** Setzt Callback für ReadProperty-Anfragen an lokale Objekte */
        public function onReadProperty(callable $handler): void;
        // Handler-Signatur: function(ObjectIdentifier $oid, Property $property, ?int $arrayIndex): mixed
        
        /** Setzt Callback für WriteProperty-Anfragen */
        public function onWriteProperty(callable $handler): void;
        // Handler-Signatur: function(ObjectIdentifier $oid, Property $property, mixed $value, ?int $arrayIndex): void
        
        /** Verarbeitet eingehende BACnet-Nachrichten (non-blocking) */
        public function poll(int $timeoutMs = 0): void;
        
        /** Beantwortet automatisch Who-Is mit I-Am für diesen Server */
        public function setAutoIAm(bool $enabled): void;
    }
    ```

2.  **Interne Server-Struktur (`src/bacnet_server.h`):**
    ```c
    typedef struct {
        php_bacnet_client *client;     // teilt Socket/Datalink (wiederverwendbar aus client.h)
        uint32_t device_id;
        zend_fcall_info_cache *read_handler;
        zend_fcall_info_cache *write_handler;
        HashTable *local_objects;      // Key: uint32_t composite (type << 22 | instance), Value: true
        bool auto_iam;
        zend_object std;
    } php_bacnet_server_obj;
    ```
    > Hinweis: `php_bacnet_client` aus Step 3 muss so erweitert werden, dass `Server` und `Client` denselben Datalink/Socket nutzen können (z. B. `php_bacnet_datalink_init` einmalig, dann von Client und Server referenziert). Falls der Socket bereits vom Stack global gehalten wird, reicht ein `datalink_init()` einmal pro RINIT.

3.  **Who-Is Handling im Server:**
    - In `poll()`:
      - `datalink_receive(&src, pdu, MAX_APDU, timeoutMs)`.
      - Parse NPDU/APDU.
      - Wenn Unconfirmed-Request + Who-Is:
        - Prüfe `auto_iam` und ob eigene `device_id` im Range liegt.
        - Baue I-Am mit `Send_I_Am()` oder `iam_encode_apdu()`.
        - Sende direkt zurück an `src` (oder Broadcast je nach Erfordernis).

4.  **ReadProperty-Anfrage verarbeiten:**
    - Wenn Confirmed-Request + ReadProperty:
      - Prüfe, ob ObjectType|Instance in `local_objects` registriert.
      - Falls ja: Rufe PHP-Callback auf.
        - Baue `zval`-Args: `ObjectIdentifier`, `Property`, `arrayIndex` (oder null).
        - Nutze `zend_call_function` mit gespeichertem `read_handler`.
        - Konvertiere Rückgabe (`mixed` -> `BACNET_APPLICATION_DATA_VALUE`) via `zval_to_bacapp_value()` (invers zu Step 5).
        - Baue Complex-ACK mit `rp_ack_encode_apdu()`.
        - Sende an Anfrager.
      - Falls kein Handler registriert: Sende Error-PDU (`unknown-object` oder `unknown-property`).

5.  **Non-blocking Design:**
    - `poll(0)` gibt sofort zurück, wenn keine Daten da.
    - So kann der PHP-User eine Schleife bauen:
      ```php
      $server = new Bacnet\Server(1234);
      while (true) {
          $server->poll(50); // 50ms blockieren max
          // andere PHP-Logik...
      }
      ```

6.  **ZTS-Hinweis:**  
    Da NTS, brauchen wir keine globalen Thread-Locks. Aber: Der Callback-Aufruf in `poll()` muss den aktuellen `EG()`-Kontext korrekt nutzen (was bei NTS/Standard-Request-Cycle trivial ist).

## Akzeptanzkriterien

- [ ] Ein externes BACnet-Tool (z. B. YABE oder BACnet-Stack-WhoIs) findet den PHP-Server nach `whoIs`.
- [ ] `readProperty` auf ein `addLocalObject`-registriertes Objekt triggert den PHP-Callback und dessen Rückgabe wird korrekt als BACnet-Datum encodiert.
- [ ] `poll(0)` blockiert nicht endlos.
- [ ] Fehlt ein Callback, wird ein BACnet-Error an den Requester zurückgesendet (nicht stillschweigend verworfen).
