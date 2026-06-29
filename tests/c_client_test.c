/*
 * Standalone C test for php_bacnet_client.
 * Sends Who-Is broadcast, collects I-Am responses.
 *
 * Build:
 *   gcc -I../deps/bacnet-stack/src -DBACDL_BIP -DBACNET_STACK_DEPRECATED_DISABLE \
 *       -o c_client_test c_client_test.c ../src/bacnet_client.c \
 *       -L../deps/bacnet-stack/build -lbacnet-stack -Wl,-rpath,../deps/bacnet-stack/build
 *
 * Run:
 *   ./c_client_test [interface] [timeout_ms]
 *   ./c_client_test eth0 3000
 *   ./c_client_test 0.0.0.0 1000
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BACDL_BIP
#define BACDL_BIP
#endif

#include "bacnet/bacdef.h"
#include "bacnet/bacenum.h"
#include "bacnet/whois.h"

#include "../src/bacnet_client.h"

int main(int argc, char *argv[])
{
    const char *iface      = argc > 1 ? argv[1] : "0.0.0.0";
    uint32_t    timeout_ms = argc > 2 ? (uint32_t)atoi(argv[2]) : 3000;

    printf("BACnet Who-Is test\n");
    printf("  interface : %s\n", iface);
    printf("  timeout   : %u ms\n", timeout_ms);
    printf("  device cap: %d\n\n", BACNET_MAX_COLLECTED_DEVICES);

    char *err_msg = NULL;
    php_bacnet_client *client = php_bacnet_client_create(iface, 47808, &err_msg);
    if (!client) {
        fprintf(stderr, "ERROR: %s\n", err_msg ? err_msg : "unknown");
        free(err_msg);
        return 1;
    }
    printf("Socket opened (fd=%d)\n", client->socket_fd);

    /* Build Who-Is APDU (wildcard range: 0..BACNET_MAX_INSTANCE) */
    uint8_t apdu[64];
    int apdu_len = whois_encode_apdu(apdu, 0, BACNET_MAX_INSTANCE);
    if (apdu_len <= 0) {
        fprintf(stderr, "ERROR: whois_encode_apdu failed\n");
        php_bacnet_client_destroy(client);
        return 1;
    }

    printf("Sending Who-Is broadcast (%d bytes APDU)...\n", apdu_len);

    php_bacnet_iam_entry entries[BACNET_MAX_COLLECTED_DEVICES];
    memset(entries, 0, sizeof(entries));

    int count = php_bacnet_broadcast_and_collect(
        client, apdu, (uint16_t)apdu_len, entries, timeout_ms);

    printf("\nCollected %d device(s):\n", count);
    for (int i = 0; i < count; i++) {
        char ipbuf[32];
        uint16_t port;
        php_bacnet_address_to_ipport(&entries[i].address, ipbuf, sizeof(ipbuf), &port);
        printf("  [%d] device_id=%u  addr=%s:%u  max_apdu=%u  vendor=%u\n",
            i, entries[i].device_id, ipbuf, (unsigned)port,
            entries[i].max_apdu, entries[i].vendor_id);
    }

    php_bacnet_client_destroy(client);
    printf("\nDone. Socket closed.\n");

    return (count > 0) ? 0 : 2; /* exit 2 = no devices found (not an error per se) */
}
