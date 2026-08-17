#ifndef PHP_BACNET_SECURITY_H
#define PHP_BACNET_SECURITY_H

#include "php.h"
#include "bacnet/bacaddr.h"

typedef struct php_bacnet_security php_bacnet_security;

typedef enum {
    PHP_BACNET_PACKET_OTHER = 0,
    PHP_BACNET_PACKET_WHO_IS,
    PHP_BACNET_PACKET_WRITE
} php_bacnet_packet_kind;

php_bacnet_security *php_bacnet_security_create(void);
void php_bacnet_security_destroy(php_bacnet_security *security);
bool php_bacnet_security_configure(php_bacnet_security *security, HashTable *overrides);
void php_bacnet_security_options_to_array(php_bacnet_security *security, zval *result);
void php_bacnet_security_stats_to_array(
    php_bacnet_security *security, bool include_sources, bool reset, zval *result);
bool php_bacnet_security_accept(
    php_bacnet_security *security,
    const BACNET_ADDRESS *source,
    const uint8_t *pdu,
    uint16_t pdu_len,
    php_bacnet_packet_kind *kind);
void php_bacnet_security_malformed(
    php_bacnet_security *security, const BACNET_ADDRESS *source);
void php_bacnet_security_queue_overflow(php_bacnet_security *security);
bool php_bacnet_security_is_duplicate_write(
    php_bacnet_security *security, const BACNET_ADDRESS *source,
    const uint8_t *apdu, uint16_t apdu_len);
void php_bacnet_security_record_write(
    php_bacnet_security *security, const BACNET_ADDRESS *source,
    const uint8_t *apdu, uint16_t apdu_len);

#endif
