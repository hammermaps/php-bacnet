#ifndef PHP_BACNET_TYPES_H
#define PHP_BACNET_TYPES_H

#include "php.h"
#include "zend_types.h"

#ifndef BACDL_BIP
#define BACDL_BIP
#endif
#include "bacnet/bacdef.h"
#include "bacnet_client.h"

/*
 * Embedded-struct pattern for all PHP objects with C state.
 * zend_object MUST be the last field so zend_object_alloc() works correctly.
 */

/* Bacnet\Client */
typedef struct {
    php_bacnet_client *client;  /* NULL until __construct succeeds */
    zend_object std;            /* MUST be last */
} php_bacnet_client_obj;

static inline php_bacnet_client_obj *php_bacnet_client_from_obj(zend_object *obj)
{
    return (php_bacnet_client_obj *)((char *)obj - XtOffsetOf(php_bacnet_client_obj, std));
}
#define Z_BACNET_CLIENT_P(zv)  php_bacnet_client_from_obj(Z_OBJ_P(zv))
#define Z_BACNET_CLIENT(zv)    php_bacnet_client_from_obj(Z_OBJ(zv))

/* Bacnet\Device */
typedef struct {
    BACNET_ADDRESS address;
    uint32_t device_id;
    uint16_t max_apdu;
    uint16_t vendor_id;
    zval client_zval;           /* ZVAL_COPY of the parent Client — refcount kept */
    zend_object std;            /* MUST be last */
} php_bacnet_device_obj;

static inline php_bacnet_device_obj *php_bacnet_device_from_obj(zend_object *obj)
{
    return (php_bacnet_device_obj *)((char *)obj - XtOffsetOf(php_bacnet_device_obj, std));
}
#define Z_BACNET_DEVICE_P(zv)  php_bacnet_device_from_obj(Z_OBJ_P(zv))
#define Z_BACNET_DEVICE(zv)    php_bacnet_device_from_obj(Z_OBJ(zv))

#endif /* PHP_BACNET_TYPES_H */
