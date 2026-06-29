#ifndef PHP_BACNET_TYPES_H
#define PHP_BACNET_TYPES_H

#include "php.h"
#include "zend_types.h"
#include "zend_enum.h"

#ifndef BACDL_BIP
#define BACDL_BIP
#endif
#include "bacnet/bacdef.h"
#include "bacnet/bacenum.h"
#include "bacnet/bacstr.h"
#include "bacnet/datetime.h"
#include "bacnet/bacapp.h"
#include "bacnet_client.h"

/*
 * Embedded-struct pattern for all PHP objects with C state.
 * zend_object MUST be the last field so zend_object_alloc() works correctly.
 */

/* ── Bacnet\Client ─────────────────────────────────────────────────────── */
typedef struct {
    php_bacnet_client *client;  /* NULL until __construct succeeds */
    zend_object std;
} php_bacnet_client_obj;

static inline php_bacnet_client_obj *php_bacnet_client_from_obj(zend_object *obj)
{
    return (php_bacnet_client_obj *)((char *)obj - XtOffsetOf(php_bacnet_client_obj, std));
}
#define Z_BACNET_CLIENT_P(zv)  php_bacnet_client_from_obj(Z_OBJ_P(zv))
#define Z_BACNET_CLIENT(zv)    php_bacnet_client_from_obj(Z_OBJ(zv))

/* ── Bacnet\Device ─────────────────────────────────────────────────────── */
typedef struct {
    BACNET_ADDRESS address;
    uint32_t device_id;
    uint16_t max_apdu;
    uint16_t vendor_id;
    zval client_zval;           /* ZVAL_COPY of the parent Client */
    zend_object std;
} php_bacnet_device_obj;

static inline php_bacnet_device_obj *php_bacnet_device_from_obj(zend_object *obj)
{
    return (php_bacnet_device_obj *)((char *)obj - XtOffsetOf(php_bacnet_device_obj, std));
}
#define Z_BACNET_DEVICE_P(zv)  php_bacnet_device_from_obj(Z_OBJ_P(zv))
#define Z_BACNET_DEVICE(zv)    php_bacnet_device_from_obj(Z_OBJ(zv))

/* ── Bacnet\ObjectIdentifier — immutable value object ──────────────────── */
typedef struct {
    BACNET_OBJECT_TYPE object_type;
    uint32_t instance;
    zval type_zval;             /* ObjectType enum case (refcounted) */
    zend_object std;
} php_bacnet_object_identifier_obj;

static inline php_bacnet_object_identifier_obj *php_bacnet_oid_from_obj(zend_object *obj)
{
    return (php_bacnet_object_identifier_obj *)
        ((char *)obj - XtOffsetOf(php_bacnet_object_identifier_obj, std));
}
#define Z_BACNET_OID_P(zv)  php_bacnet_oid_from_obj(Z_OBJ_P(zv))
#define Z_BACNET_OID(zv)    php_bacnet_oid_from_obj(Z_OBJ(zv))

/* ── Bacnet\BitString ──────────────────────────────────────────────────── */
#define PHP_BACNET_BITSTRING_MAX_BYTES 15   /* mirrors MAX_BITSTRING_BYTES */

typedef struct {
    uint8_t bits_used;
    uint8_t value[PHP_BACNET_BITSTRING_MAX_BYTES];
    zend_object std;
} php_bacnet_bitstring_obj;

static inline php_bacnet_bitstring_obj *php_bacnet_bitstring_from_obj(zend_object *obj)
{
    return (php_bacnet_bitstring_obj *)
        ((char *)obj - XtOffsetOf(php_bacnet_bitstring_obj, std));
}
#define Z_BACNET_BITSTRING_P(zv)  php_bacnet_bitstring_from_obj(Z_OBJ_P(zv))
#define Z_BACNET_BITSTRING(zv)    php_bacnet_bitstring_from_obj(Z_OBJ(zv))

/* ── Bacnet\Date ───────────────────────────────────────────────────────── */
typedef struct {
    uint16_t year;    /* AD; 0xFF = any (wildcard) */
    uint8_t  month;   /* 1..12, 0xFF = any */
    uint8_t  day;     /* 1..31, 0xFF = any */
    uint8_t  weekday; /* 1=Mon..7=Sun, 0xFF = any */
    zend_object std;
} php_bacnet_date_obj;

static inline php_bacnet_date_obj *php_bacnet_date_from_obj(zend_object *obj)
{
    return (php_bacnet_date_obj *)
        ((char *)obj - XtOffsetOf(php_bacnet_date_obj, std));
}
#define Z_BACNET_DATE_P(zv)  php_bacnet_date_from_obj(Z_OBJ_P(zv))
#define Z_BACNET_DATE(zv)    php_bacnet_date_from_obj(Z_OBJ(zv))

/* ── Bacnet\Time ───────────────────────────────────────────────────────── */
typedef struct {
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t hundredths;
    zend_object std;
} php_bacnet_time_obj;

static inline php_bacnet_time_obj *php_bacnet_time_from_obj(zend_object *obj)
{
    return (php_bacnet_time_obj *)
        ((char *)obj - XtOffsetOf(php_bacnet_time_obj, std));
}
#define Z_BACNET_TIME_P(zv)  php_bacnet_time_from_obj(Z_OBJ_P(zv))
#define Z_BACNET_TIME(zv)    php_bacnet_time_from_obj(Z_OBJ(zv))

/* ── Bacnet\Value — typed write value ──────────────────────────────────── */
typedef struct {
    BACNET_APPLICATION_DATA_VALUE appdata;  /* the typed value to write */
    zend_object std;
} php_bacnet_value_obj;

static inline php_bacnet_value_obj *php_bacnet_value_from_obj(zend_object *obj)
{
    return (php_bacnet_value_obj *)
        ((char *)obj - XtOffsetOf(php_bacnet_value_obj, std));
}
#define Z_BACNET_VALUE_P(zv)  php_bacnet_value_from_obj(Z_OBJ_P(zv))
#define Z_BACNET_VALUE(zv)    php_bacnet_value_from_obj(Z_OBJ(zv))

/* ── Bacnet\ObjectRef ──────────────────────────────────────────────────── */
typedef struct {
    BACNET_OBJECT_TYPE object_type;
    uint32_t instance;
    zval device_zval;           /* Bacnet\Device reference */
    zend_object std;
} php_bacnet_objectref_obj;

static inline php_bacnet_objectref_obj *php_bacnet_objectref_from_obj(zend_object *obj)
{
    return (php_bacnet_objectref_obj *)
        ((char *)obj - XtOffsetOf(php_bacnet_objectref_obj, std));
}
#define Z_BACNET_OBJREF_P(zv)  php_bacnet_objectref_from_obj(Z_OBJ_P(zv))
#define Z_BACNET_OBJREF(zv)    php_bacnet_objectref_from_obj(Z_OBJ(zv))

#endif /* PHP_BACNET_TYPES_H */
