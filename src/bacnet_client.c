#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* php.h first — Zend Memory Manager (pemalloc/pefree/pestrdup/estrdup/efree) */
#include "php.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/time.h>

#ifndef BACDL_BIP
#define BACDL_BIP
#endif

#include "bacnet/bacdef.h"
#include "bacnet/bacenum.h"
#include "bacnet/bacaddr.h"
#include "bacnet/npdu.h"
#include "bacnet/apdu.h"
#include "bacnet/whois.h"
#include "bacnet/iam.h"
#include "bacnet/datalink/bip.h"

#include "bacnet_client.h"

/* Milliseconds since epoch, monotonic */
static uint64_t ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

php_bacnet_client *php_bacnet_client_create(
    const char *iface, uint16_t port, char **err_msg)
{
    /* Persistent allocation: client outlives a single PHP call frame */
    php_bacnet_client *client = pemalloc(sizeof(php_bacnet_client), 1);
    memset(client, 0, sizeof(*client));

    /*
     * bip_init() expects an interface *name* (e.g. "eth0"), not an IP address.
     * Pass NULL to let bacnet-stack auto-detect the first non-loopback interface.
     * "0.0.0.0" and "" are treated as the default (auto-detect).
     */
    const char *real_iface = NULL;
    if (iface && *iface && strcmp(iface, "0.0.0.0") != 0) {
        real_iface = iface;
    }
    /* pestrdup: persistent copy of the interface name — freed in destroy() */
    client->iface = real_iface ? pestrdup(real_iface, 1) : pestrdup("(auto)", 1);
    client->port  = port ? port : 47808;

    bip_set_port(client->port);
    /*
     * Always broadcast to the standard BACnet/IP port (47808).
     * bip_get_broadcast_port() falls back to BIP_Port when BIP_Broadcast_Port
     * is 0, so if the client is on a non-standard port (e.g. 47809), broadcasts
     * would go to that port instead of 47808 and no standard server would hear them.
     */
    bip_set_broadcast_port(0xBAC0); /* 47808 — standard BACnet/IP port */

    if (!bip_init(real_iface)) {
        if (err_msg) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "bip_init failed on interface '%s' port %u",
                client->iface, (unsigned)client->port);
            /* estrdup: temporary error string — caller frees with efree() */
            *err_msg = estrdup(buf);
        }
        pefree(client->iface, 1);
        pefree(client, 1);
        return NULL;
    }

    client->socket_fd  = bip_get_socket();
    client->initialized = true;

    return client;
}

void php_bacnet_client_destroy(php_bacnet_client *client)
{
    if (!client) return;
    if (client->initialized) {
        bip_cleanup();
        client->initialized = false;
    }
    pefree(client->iface, 1);
    pefree(client, 1);
}

/*
 * Send request_apdu as a broadcast (Who-Is) and collect I-Am responses
 * for timeout_ms milliseconds.
 */
int php_bacnet_broadcast_and_collect(
    php_bacnet_client *client,
    uint8_t *request_apdu,
    uint16_t request_apdu_len,
    php_bacnet_iam_entry *entries,
    uint32_t timeout_ms)
{
    if (!client || !client->initialized) return 0;

    /* Build broadcast BACNET_ADDRESS (mac_len=0 → BIP subnet broadcast) */
    BACNET_ADDRESS dest;
    memset(&dest, 0, sizeof(dest));
    dest.net     = BACNET_BROADCAST_NETWORK;
    dest.mac_len = 0;

    /* NPDU data for unconfirmed broadcast */
    BACNET_NPDU_DATA npdu_data;
    npdu_encode_npdu_data(&npdu_data, false, MESSAGE_PRIORITY_NORMAL);

    /*
     * bvlc_send_pdu ignores the npdu_data parameter — it expects the caller
     * to pass a fully-formed NPDU+APDU buffer, not just the APDU.
     * Encode NPDU first, then append the APDU (mirrors Send_WhoIs_To_Network).
     */
    uint8_t pdu_buf[MAX_APDU + MAX_NPDU];
    BACNET_ADDRESS my_address;
    bip_get_my_address(&my_address);
    int npdu_hdrlen = npdu_encode_pdu(pdu_buf, &dest, &my_address, &npdu_data);
    if (npdu_hdrlen < 0 ||
        (size_t)npdu_hdrlen + request_apdu_len > sizeof(pdu_buf)) return 0;
    memcpy(pdu_buf + npdu_hdrlen, request_apdu, request_apdu_len);
    unsigned total_len = (unsigned)(npdu_hdrlen + request_apdu_len);

    bip_send_pdu(&dest, &npdu_data, pdu_buf, total_len);

    /* Collect I-Am responses until timeout */
    uint64_t deadline = ms_now() + timeout_ms;
    int count = 0;

    while (ms_now() < deadline && count < BACNET_MAX_COLLECTED_DEVICES) {
        uint64_t remaining = deadline - ms_now();
        if (remaining == 0) break;

        uint8_t pdu[MAX_APDU + MAX_NPDU];
        BACNET_ADDRESS src;
        memset(&src, 0, sizeof(src));

        uint16_t pdu_len = bip_receive(
            &src, pdu, (uint16_t)sizeof(pdu), (unsigned)remaining);

        if (pdu_len == 0) break; /* timeout */

        /* Decode NPDU header */
        BACNET_ADDRESS npdu_dest;
        BACNET_ADDRESS npdu_src;
        BACNET_NPDU_DATA npdu_hdr;
        int npdu_len = bacnet_npdu_decode(pdu, pdu_len, &npdu_dest, &npdu_src, &npdu_hdr);
        if (npdu_len < 0) continue;

        /* Skip network layer messages */
        if (npdu_hdr.network_layer_message) continue;

        uint8_t *apdu    = pdu + npdu_len;
        uint16_t apdu_len = pdu_len - (uint16_t)npdu_len;
        if (apdu_len < 2) continue;

        /* Unconfirmed service request? */
        if ((apdu[0] & 0xF0) != PDU_TYPE_UNCONFIRMED_SERVICE_REQUEST) continue;

        /* I-Am? */
        if (apdu[1] != SERVICE_UNCONFIRMED_I_AM) continue;

        uint32_t device_id  = 0;
        unsigned max_apdu   = 0;
        int segmentation    = 0;
        uint16_t vendor_id  = 0;

        int rc = iam_decode_service_request(
            apdu + 2, &device_id, &max_apdu, &segmentation, &vendor_id);
        if (rc < 0) continue;

        /* Deduplicate by device_id */
        bool found = false;
        for (int i = 0; i < count; i++) {
            if (entries[i].device_id == device_id) { found = true; break; }
        }
        if (found) continue;

        entries[count].device_id = device_id;
        entries[count].max_apdu  = (uint16_t)max_apdu;
        entries[count].vendor_id = vendor_id;
        memcpy(&entries[count].address, &src, sizeof(BACNET_ADDRESS));
        count++;
    }

    return count;
}

/*
 * Unicast request/response with invoke-ID matching.
 * Returns 0 on success (out_apdu filled), -1 on timeout.
 */
int php_bacnet_send_and_wait(
    php_bacnet_client *client,
    BACNET_ADDRESS *dest,
    uint8_t *request_apdu,
    uint16_t request_apdu_len,
    uint8_t expected_invoke_id,
    uint8_t *out_apdu,
    uint16_t *out_apdu_len,
    uint32_t timeout_ms)
{
    if (!client || !client->initialized) return -1;

    BACNET_NPDU_DATA npdu_data;
    npdu_encode_npdu_data(&npdu_data, true, MESSAGE_PRIORITY_NORMAL);

    /* Encode NPDU before APDU — bvlc_send_pdu ignores npdu_data */
    uint8_t pdu_buf[MAX_APDU + MAX_NPDU];
    BACNET_ADDRESS my_address;
    bip_get_my_address(&my_address);
    int npdu_hdrlen = npdu_encode_pdu(pdu_buf, dest, &my_address, &npdu_data);
    if (npdu_hdrlen < 0 ||
        (size_t)npdu_hdrlen + request_apdu_len > sizeof(pdu_buf)) return -1;
    memcpy(pdu_buf + npdu_hdrlen, request_apdu, request_apdu_len);
    unsigned total_len = (unsigned)(npdu_hdrlen + request_apdu_len);

    bip_send_pdu(dest, &npdu_data, pdu_buf, total_len);

    uint64_t deadline = ms_now() + timeout_ms;

    while (ms_now() < deadline) {
        uint64_t remaining = deadline - ms_now();
        if (remaining == 0) break;

        uint8_t pdu[MAX_APDU + MAX_NPDU];
        BACNET_ADDRESS src;
        memset(&src, 0, sizeof(src));

        uint16_t pdu_len = bip_receive(
            &src, pdu, (uint16_t)sizeof(pdu), (unsigned)remaining);

        if (pdu_len == 0) break; /* timeout */

        BACNET_ADDRESS npdu_dest;
        BACNET_ADDRESS npdu_src_addr;
        BACNET_NPDU_DATA npdu_hdr;
        int npdu_len = bacnet_npdu_decode(
            pdu, pdu_len, &npdu_dest, &npdu_src_addr, &npdu_hdr);
        if (npdu_len < 0) continue;
        if (npdu_hdr.network_layer_message) continue;

        uint8_t *apdu    = pdu + npdu_len;
        uint16_t apdu_len = pdu_len - (uint16_t)npdu_len;
        if (apdu_len < 3) continue;

        uint8_t pdu_type = apdu[0] & 0xF0;

        /* Simple-ACK (WriteProperty success): byte[0]=0x20, byte[1]=invoke_id */
        if (pdu_type == PDU_TYPE_SIMPLE_ACK) {
            if (apdu[1] != expected_invoke_id) continue;
            uint16_t copy_len = apdu_len;
            if (copy_len > MAX_APDU) copy_len = MAX_APDU;
            memcpy(out_apdu, apdu, copy_len);
            *out_apdu_len = copy_len;
            return 0;
        }

        /* Complex-ACK: byte[0]=type|seg, byte[1]=invoke_id */
        if (pdu_type == PDU_TYPE_COMPLEX_ACK) {
            if (apdu[1] != expected_invoke_id) continue;
            uint16_t copy_len = apdu_len;
            if (copy_len > MAX_APDU) copy_len = MAX_APDU;
            memcpy(out_apdu, apdu, copy_len);
            *out_apdu_len = copy_len;
            return 0;
        }

        /* Error / Reject / Abort: also carry invoke_id at byte[1] */
        if (pdu_type == PDU_TYPE_ERROR ||
            pdu_type == PDU_TYPE_REJECT ||
            pdu_type == PDU_TYPE_ABORT) {
            if (apdu[1] != expected_invoke_id) continue;
            uint16_t copy_len = apdu_len;
            if (copy_len > MAX_APDU) copy_len = MAX_APDU;
            memcpy(out_apdu, apdu, copy_len);
            *out_apdu_len = copy_len;
            /* Return the raw PDU type so the PHP layer can throw the right exception */
            return (int)pdu_type;
        }
    }

    return -1; /* timeout */
}

void php_bacnet_address_from_ipport(
    const char *ip, uint16_t port, BACNET_ADDRESS *addr)
{
    memset(addr, 0, sizeof(BACNET_ADDRESS));
    struct in_addr ia;
    if (inet_pton(AF_INET, ip, &ia) == 1) {
        uint32_t ip_be = ia.s_addr; /* already network byte order */
        addr->mac[0] = (ip_be >>  0) & 0xFF;
        addr->mac[1] = (ip_be >>  8) & 0xFF;
        addr->mac[2] = (ip_be >> 16) & 0xFF;
        addr->mac[3] = (ip_be >> 24) & 0xFF;
        addr->mac[4] = (port >> 8) & 0xFF;
        addr->mac[5] = (port >> 0) & 0xFF;
        addr->mac_len = 6;
    }
    addr->net = 0; /* local */
    addr->len = 0;
}

void php_bacnet_address_to_ipport(
    const BACNET_ADDRESS *addr, char *ipbuf, size_t ipbuflen, uint16_t *port)
{
    if (addr->mac_len >= 6) {
        snprintf(ipbuf, ipbuflen, "%u.%u.%u.%u",
            addr->mac[0], addr->mac[1], addr->mac[2], addr->mac[3]);
        *port = ((uint16_t)addr->mac[4] << 8) | addr->mac[5];
    } else {
        snprintf(ipbuf, ipbuflen, "0.0.0.0");
        *port = 0;
    }
}
