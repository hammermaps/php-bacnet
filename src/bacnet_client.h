#ifndef PHP_BACNET_CLIENT_H
#define PHP_BACNET_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* BACnet stack — define BIP datalink before including headers */
#ifndef BACDL_BIP
#define BACDL_BIP
#endif

#include "bacnet/bacdef.h"
#include "bacnet/bacenum.h"
#include "bacnet/bacaddr.h"
#include "bacnet/npdu.h"
#include "bacnet/datalink/bip.h"

/* Maximum devices collected by whoIs broadcast. Hard limit for v0.1.0. */
#define BACNET_MAX_COLLECTED_DEVICES 64

/*
 * Internal client state. One per Bacnet\Client object.
 *
 * IMPORTANT: The socket fd opened by bip_init() is process-global in
 * bacnet-stack. Only one php_bacnet_client may exist per PHP process.
 * bip_cleanup() is called only in free_obj — never between requests —
 * so the fd stays persistent for future COV subscription support (v0.2.0).
 */
typedef struct {
    int socket_fd;       /* fd from bip_get_socket() — kept open until destroy */
    uint16_t port;
    char *iface;         /* interface string (pemalloc'd) */
    bool initialized;
} php_bacnet_client;

/* Collected I-Am response */
typedef struct {
    BACNET_ADDRESS address;
    uint32_t device_id;
    uint16_t max_apdu;
    uint16_t vendor_id;
} php_bacnet_iam_entry;

php_bacnet_client *php_bacnet_client_create(const char *iface, uint16_t port, char **err_msg);
void php_bacnet_client_destroy(php_bacnet_client *client);

/*
 * Synchronous unicast request/response with invoke-ID matching.
 * Returns 0 on success, -1 on timeout.
 * out_apdu must be at least MAX_APDU bytes.
 */
int php_bacnet_send_and_wait(
    php_bacnet_client *client,
    BACNET_ADDRESS *dest,
    uint8_t *request_apdu,
    uint16_t request_apdu_len,
    uint8_t expected_invoke_id,
    uint8_t *out_apdu,
    uint16_t *out_apdu_len,
    uint32_t timeout_ms);

/*
 * Broadcast collector for Who-Is / I-Am.
 * Sends request_apdu as broadcast, then collects I-Am responses for timeout_ms.
 * entries[] must have room for BACNET_MAX_COLLECTED_DEVICES entries.
 * Returns number of unique devices found (0..64).
 */
int php_bacnet_broadcast_and_collect(
    php_bacnet_client *client,
    uint8_t *request_apdu,
    uint16_t request_apdu_len,
    php_bacnet_iam_entry *entries,
    uint32_t timeout_ms);

/* Convert "192.168.1.100" + port into a BACNET_ADDRESS (mac[0..3]=IP, mac[4..5]=port BE) */
void php_bacnet_address_from_ipport(const char *ip, uint16_t port, BACNET_ADDRESS *addr);

/* Reverse: extract dotted-IP into ipbuf (must be >=16 bytes) and port */
void php_bacnet_address_to_ipport(
    const BACNET_ADDRESS *addr, char *ipbuf, size_t ipbuflen, uint16_t *port);

#endif /* PHP_BACNET_CLIENT_H */
