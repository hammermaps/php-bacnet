#ifndef PHP_BACNET_HELPERS_H
#define PHP_BACNET_HELPERS_H

#include "php.h"

#ifndef BACDL_BIP
#define BACDL_BIP
#endif
#include "bacnet/bacdef.h"
#include "bacnet/bacenum.h"
#include "bacnet/bacapp.h"
#include "bacnet/bacstr.h"
#include "bacnet/datetime.h"

/* Map one decoded BACnet application-data value to a PHP zval. */
void bacapp_value_to_zval(BACNET_APPLICATION_DATA_VALUE *val, zval *zv);

/*
 * Decode all application-data bytes in apdu[0..apdu_len-1] into zv.
 * Single value → scalar.  Multiple values → IS_ARRAY.
 */
void bacapp_values_to_zval(uint8_t *apdu, unsigned apdu_len, zval *zv);

/* Create a Bacnet\ObjectIdentifier from C type+instance (ObjectType may be unknown). */
void php_bacnet_oid_from_c(BACNET_OBJECT_TYPE type, uint32_t instance, zval *zv);

/* Create a Bacnet\BitString from a BACNET_BIT_STRING. */
void php_bacnet_bitstring_new(BACNET_BIT_STRING *bs, zval *zv);

/* Create a Bacnet\Date from a BACNET_DATE. */
void php_bacnet_date_new(BACNET_DATE *d, zval *zv);

/* Create a Bacnet\Time from a BACNET_TIME. */
void php_bacnet_time_new(BACNET_TIME *t, zval *zv);

/*
 * Encode a Bacnet\Value object's application data into apdu[].
 * Returns bytes written (>0) or -1 on error.
 * apdu must have room for at least MAX_APDU bytes.
 */
int php_bacnet_value_encode(zval *value_zv, uint8_t *apdu, int apdu_size);

/*
 * Convert a PHP zval to a BACNET_APPLICATION_DATA_VALUE.
 * Handles: null, bool, int, float, string, and BACnet objects (Value, BitString,
 * Date, Time, ObjectIdentifier).  Returns true on success.
 */
bool zval_to_bacapp_value(zval *zv, BACNET_APPLICATION_DATA_VALUE *val);

#endif /* PHP_BACNET_HELPERS_H */
