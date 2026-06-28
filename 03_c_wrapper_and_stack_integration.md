# Step 3: C-Wrapper für BACnet-Stack (Datalink & synchrone Kommunikation)

**Ziel:** Internen C-Layer bauen, der `bacnet-stack` initialisiert, den BIP-Datalink hält und synchrone Request/Response-Operationen ermöglicht. Der Socket bleibt persistent pro `Client`-Objekt offen, um späteren Server-Modus zu ermöglichen.

## Aktionen

1.  **Interne Client-Struktur (`src/bacnet_client.h`):**
    ```c
    #ifndef PHP_BACNET_CLIENT_H
    #define PHP_BACNET_CLIENT_H

    #include <stdint.h>
    #include <bacnet/bacdef.h>
    #include <bacnet/bacenum.h>
    #include <bacnet/bacaddr.h>

    typedef struct {
        int socket;                  // UDP-Socket (von bip_get_socket() oder datalink)
        uint16_t port;
        BACNET_ADDRESS bind_addr;    // eigene Adresse
    } php_bacnet_client;

    php_bacnet_client *php_bacnet_client_create(const char *iface, uint16_t port);
    void php_bacnet_client_destroy(php_bacnet_client *client);

    /* Synchroner Request/Response 
     * out_apdu muss mindestens MAX_APDU Bytes haben.
     */
    int php_bacnet_send_and_wait(
        php_bacnet_client *client,
        BACNET_ADDRESS *dest,
        uint8_t *request_apdu, uint16_t request_apdu_len,
        uint8_t expected_invoke_id,
        uint8_t *out_apdu, uint16_t *out_apdu_len,
        uint32_t timeout_ms
    );

    int php_bacnet_broadcast_and_collect(
        php_bacnet_client *client,
        uint8_t *request_apdu, uint16_t request_apdu_len,
        uint8_t **response_buffer, size_t *response_buffer_len, // dynamisch wachsender Buffer
        uint32_t timeout_ms
    );

    void php_bacnet_address_from_ipport(const char *ip, uint16_t port, BACNET_ADDRESS *addr);
    void php_bacnet_address_to_ipport(BACNET_ADDRESS *addr, char *ipbuf, size_t ipbuflen, uint16_t *port);

    #endif
    ```

2.  **Datalink-Initialisierung (`src/bacnet_client.c`):**
    - Nutze `bip_init(NULL)` (oder `bip_init(iface)` wenn Interface gebunden werden soll).
    - Die Funktion öffnet den Socket intern im bacnet-stack.
    - Hole den Socket-FD mit `bip_get_socket()` (falls verfügbar) oder speichere ihn nicht, wenn `datalink_receive()` direkt genutzt wird.
    - Setze die eigene Adresse via `bip_get_addr(&bind_addr)` und `bip_get_port()`.

3.  **Socket-Schleife für synchronen Empfang:**
    Da `datalink_receive(BACNET_ADDRESS *src, uint8_t *pdu, uint16_t max_pdu, unsigned timeout)` bereits blockierend mit Timeout arbeitet, nutzen wir diese Funktion.
    
    `php_bacnet_send_and_wait`:
    - Baue NPDU-Header selbst oder nutze `datalink_send_pdu(dest, &npdu_data, apdu, apdu_len)`.
      > Die NPDU-Daten (`BACNET_NPDU_DATA`) müssen initialisiert werden: `npdu_encode_npdu_data(&npdu_data, false, MESSAGE_PRIORITY_NORMAL)`; `dest->net` und Adressen müssen korrekt gesetzt sein.
    - Sende die PDU.
    - Starte Schleife:
      ```c
      uint16_t pdu_len;
      uint8_t pdu[MAX_APDU];
      BACNET_ADDRESS src;
      pdu_len = datalink_receive(&src, pdu, MAX_APDU, timeout_ms);
      if (pdu_len == 0) return -1; // Timeout
      ```
    - Parse APDU-Header mit `apdu_decode()` (aus `bacnet/apdu.h`):
      - Prüfe `apdu_type` (Complex-ACK, Error, Reject, Abort).
      - Prüfe `invoke_id`.
      - Kopiere Service-Daten in `out_apdu`.
    - Bei Match: Return 0.
    - Bei Timeout oder falscher Invoke-ID: Weiter empfangen bis Timeout abgelaufen.

4.  **Broadcast-Collector (`php_bacnet_broadcast_and_collect`):**
    - Ähnlich, aber für unbestätigte Dienste (Who-Is -> I-Am).
    - Sammelt alle Antworten in einem wachsenden Buffer (z. B. Array von Structs).
    - Für MVP reicht ein einfacher fixed-size Array mit 64 Einträgen.

5.  **Aufräumen:**
    - `php_bacnet_client_destroy` ruft `datalink_cleanup()` auf.

6.  **Hilfsfunktionen:**
    - `php_bacnet_address_from_ipport`: Konvertiert IP:Port String in BACNET_ADDRESS (`mac[0..3]=IP octets`, `mac[4..5]=Port big-endian`).
    - `php_bacnet_address_to_ipport`: Umkehrung.

## Akzeptanzkriterien

- [ ] `php_bacnet_client_create`/`destroy` kompilieren und linken ohne Fehler.
- [ ] Ein standalone C-Test `tests/c_client_test.c` (nicht in Extension gelinkt, nur intern) sendet Who-Is Broadcast und empfängt mindestens ein I-Am (gegen BACnet-Simulator oder echtes Gerät).
- [ ] Keine Speicherlecks bei `create`/`destroy` (prüfbar mit Valgrind auf dem C-Test).
