#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include "php.h"
#include "zend_exceptions.h"
#include "zend_interfaces.h"
#include "zend_enum.h"

#ifndef BACDL_BIP
#define BACDL_BIP
#endif
#include "bacnet/bacdef.h"
#include "bacnet/bacenum.h"
#include "bacnet/whois.h"
#include "bacnet/rp.h"
#include "bacnet/bacapp.h"
#include "bacnet/bacerror.h"

#include "../php_bacnet.h"
#include "bacnet_classes.h"
#include "bacnet_types.h"
#include "bacnet_client.h"
#include "bacnet_helpers.h"

/* ── Class entry globals ─────────────────────────────────────────────── */

zend_class_entry *bacnet_ce_client            = NULL;
zend_class_entry *bacnet_ce_device            = NULL;
zend_class_entry *bacnet_ce_object_ref        = NULL;
zend_class_entry *bacnet_ce_object_identifier = NULL;
zend_class_entry *bacnet_ce_bit_string        = NULL;
zend_class_entry *bacnet_ce_date              = NULL;
zend_class_entry *bacnet_ce_time              = NULL;
zend_class_entry *bacnet_ce_value             = NULL;
zend_class_entry *bacnet_ce_object_type_enum  = NULL;
zend_class_entry *bacnet_ce_property_enum     = NULL;
zend_class_entry *bacnet_ce_exception         = NULL;
zend_class_entry *bacnet_ce_timeout_exception = NULL;
zend_class_entry *bacnet_ce_device_exception  = NULL;

/* ── Object handlers ─────────────────────────────────────────────────── */

static zend_object_handlers php_bacnet_client_handlers;
static zend_object_handlers php_bacnet_device_handlers;
static zend_object_handlers php_bacnet_oid_handlers;
static zend_object_handlers php_bacnet_bitstring_handlers;
static zend_object_handlers php_bacnet_date_handlers;
static zend_object_handlers php_bacnet_time_handlers;
static zend_object_handlers php_bacnet_objectref_handlers;

/* ── Shared ReadProperty logic ───────────────────────────────────────────
 * Called by both Device::readProperty and ObjectRef::readProperty.
 * Throws an exception on error.  Returns 0 on success.
 */
static int bacnet_exec_read_property(
    php_bacnet_client  *client,
    BACNET_ADDRESS     *dest,
    BACNET_OBJECT_TYPE  obj_type,
    uint32_t            obj_inst,
    BACNET_PROPERTY_ID  prop_id,
    uint32_t            array_index,   /* BACNET_ARRAY_ALL = no index */
    uint32_t            timeout_ms,
    zval               *return_value)
{
    /* Build ReadProperty APDU */
    uint8_t invoke_id = BACNET_G(next_invoke_id)++;

    BACNET_READ_PROPERTY_DATA rpdata;
    memset(&rpdata, 0, sizeof(rpdata));
    rpdata.object_type     = obj_type;
    rpdata.object_instance = obj_inst;
    rpdata.object_property = prop_id;
    rpdata.array_index     = (BACNET_ARRAY_INDEX)array_index;

    uint8_t req_apdu[MAX_APDU];
    int req_len = rp_encode_apdu(req_apdu, invoke_id, &rpdata);
    if (req_len <= 0) {
        zend_throw_exception(bacnet_ce_exception, "Failed to encode ReadProperty APDU", 0);
        return -1;
    }

    /* Send and wait for response */
    uint8_t  resp_apdu[MAX_APDU];
    uint16_t resp_len = 0;
    int status = php_bacnet_send_and_wait(
        client, dest,
        req_apdu, (uint16_t)req_len,
        invoke_id, resp_apdu, &resp_len, timeout_ms);

    if (status == -1) {
        zend_throw_exception_ex(bacnet_ce_timeout_exception, 0,
            "ReadProperty timed out after %u ms", (unsigned)timeout_ms);
        return -1;
    }

    if (status == (int)PDU_TYPE_ERROR) {
        if (resp_len < 3) {
            zend_throw_exception(bacnet_ce_device_exception,
                "BACnet error PDU (malformed)", 0);
            return -1;
        }
        uint8_t                  err_iid = 0;
        BACNET_CONFIRMED_SERVICE svc    = SERVICE_CONFIRMED_READ_PROPERTY;
        BACNET_ERROR_CLASS       ec     = ERROR_CLASS_DEVICE;
        BACNET_ERROR_CODE        code   = ERROR_CODE_OTHER;
        /* Error PDU: [pdu_type][invoke_id][service][class][code] — pass from byte 1 */
        bacerror_decode_service_request(
            resp_apdu + 1, resp_len - 1, &err_iid, &svc, &ec, &code);

        zend_throw_exception_ex(bacnet_ce_device_exception, 0,
            "BACnet error: class=%d code=%d", (int)ec, (int)code);
        /* Patch errorClass / errorCode onto the thrown exception object */
        if (EG(exception)) {
            zend_update_property_long(bacnet_ce_device_exception, EG(exception),
                "errorClass", sizeof("errorClass") - 1, (zend_long)ec);
            zend_update_property_long(bacnet_ce_device_exception, EG(exception),
                "errorCode",  sizeof("errorCode")  - 1, (zend_long)code);
        }
        return -1;
    }

    if (status == (int)PDU_TYPE_REJECT || status == (int)PDU_TYPE_ABORT) {
        uint8_t reason = (resp_len > 2) ? resp_apdu[2] : 0;
        zend_throw_exception_ex(bacnet_ce_device_exception, 0,
            "BACnet %s PDU (reason %u)",
            (status == (int)PDU_TYPE_REJECT) ? "Reject" : "Abort",
            (unsigned)reason);
        return -1;
    }

    /* Decode Complex-ACK — bytes 0=type, 1=invoke_id, 2=service, 3+=data */
    if (resp_len < 4) {
        zend_throw_exception(bacnet_ce_exception,
            "Malformed ReadProperty-ACK (too short)", 0);
        return -1;
    }

    BACNET_READ_PROPERTY_DATA rp_ack;
    memset(&rp_ack, 0, sizeof(rp_ack));
    int decoded = rp_ack_decode_service_request(
        resp_apdu + 3, (int)(resp_len - 3), &rp_ack);
    if (decoded < 0 || !rp_ack.application_data || rp_ack.application_data_len <= 0) {
        zend_throw_exception(bacnet_ce_exception,
            "Failed to decode ReadProperty-ACK", 0);
        return -1;
    }

    bacapp_values_to_zval(
        rp_ack.application_data,
        (unsigned)rp_ack.application_data_len,
        return_value);

    return 0;
}

/* ────────────────────────────────────────────────────────────────────── */
/*  Bacnet\Client                                                         */
/* ────────────────────────────────────────────────────────────────────── */

static zend_object *php_bacnet_client_create_object(zend_class_entry *ce)
{
    php_bacnet_client_obj *obj =
        (php_bacnet_client_obj *)zend_object_alloc(sizeof(php_bacnet_client_obj), ce);
    obj->client = NULL;
    zend_object_std_init(&obj->std, ce);
    object_properties_init(&obj->std, ce);
    obj->std.handlers = &php_bacnet_client_handlers;
    return &obj->std;
}

static void php_bacnet_client_free_object(zend_object *object)
{
    php_bacnet_client_obj *obj = php_bacnet_client_from_obj(object);
    if (obj->client) {
        php_bacnet_client_destroy(obj->client);
        obj->client = NULL;
        BACNET_G(client_initialized) = 0;
    }
    zend_object_std_dtor(object);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_bacnet_client_construct, 0, 0, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, interface, IS_STRING, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, port,      IS_LONG,   1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeoutMs, IS_LONG,   1, "null")
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Client, __construct)
{
    zend_string *iface_str  = NULL;
    zend_long    port       = 0;
    zend_long    timeout_ms = 0;
    bool         port_null       = true;
    bool         timeout_ms_null = true;

    ZEND_PARSE_PARAMETERS_START(0, 3)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR_OR_NULL(iface_str)
        Z_PARAM_LONG_OR_NULL(port, port_null)
        Z_PARAM_LONG_OR_NULL(timeout_ms, timeout_ms_null)
    ZEND_PARSE_PARAMETERS_END();

    if (BACNET_G(client_initialized)) {
        zend_throw_error(NULL,
            "Only one Bacnet\\Client may exist per PHP process "
            "(bacnet-stack uses a process-global UDP socket)");
        RETURN_THROWS();
    }

    php_bacnet_client_obj *obj = Z_BACNET_CLIENT_P(ZEND_THIS);

    const char *iface = (iface_str && ZSTR_LEN(iface_str) > 0)
                        ? ZSTR_VAL(iface_str) : NULL;
    uint16_t p = port_null ? (uint16_t)BACNET_G(default_port) : (uint16_t)port;

    char *err_msg = NULL;
    obj->client   = php_bacnet_client_create(iface, p, &err_msg);
    if (!obj->client) {
        zend_throw_exception_ex(bacnet_ce_exception, 0,
            "Failed to initialize BACnet client: %s",
            err_msg ? err_msg : "unknown error");
        free(err_msg);
        RETURN_THROWS();
    }
    free(err_msg);
    BACNET_G(client_initialized) = 1;
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_client_whois, 0, 0, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, lowLimit,  IS_LONG, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, highLimit, IS_LONG, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeoutMs, IS_LONG, 1, "null")
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Client, whoIs)
{
    zend_long low_limit  = 0;
    zend_long high_limit = 0;
    zend_long timeout_ms = 0;
    bool low_null  = true;
    bool high_null = true;
    bool tms_null  = true;

    ZEND_PARSE_PARAMETERS_START(0, 3)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG_OR_NULL(low_limit,  low_null)
        Z_PARAM_LONG_OR_NULL(high_limit, high_null)
        Z_PARAM_LONG_OR_NULL(timeout_ms, tms_null)
    ZEND_PARSE_PARAMETERS_END();

    php_bacnet_client_obj *obj = Z_BACNET_CLIENT_P(ZEND_THIS);
    if (!obj->client) {
        zend_throw_exception(bacnet_ce_exception, "BACnet client not initialized", 0);
        RETURN_THROWS();
    }

    int32_t lo   = low_null  ? 0                  : (int32_t)low_limit;
    int32_t hi   = high_null ? BACNET_MAX_INSTANCE : (int32_t)high_limit;
    uint32_t tms = tms_null
        ? (uint32_t)BACNET_G(default_timeout_ms)
        : (uint32_t)timeout_ms;

    if (lo < 0) lo = 0;
    if (hi > BACNET_MAX_INSTANCE) hi = BACNET_MAX_INSTANCE;

    uint8_t apdu[64];
    int apdu_len = whois_encode_apdu(apdu, lo, hi);
    if (apdu_len <= 0) {
        zend_throw_exception(bacnet_ce_exception, "Failed to encode Who-Is APDU", 0);
        RETURN_THROWS();
    }

    php_bacnet_iam_entry entries[BACNET_MAX_COLLECTED_DEVICES];
    int count = php_bacnet_broadcast_and_collect(
        obj->client, apdu, (uint16_t)apdu_len, entries, tms);

    array_init(return_value);

    for (int i = 0; i < count; i++) {
        zval device_zval;
        object_init_ex(&device_zval, bacnet_ce_device);
        php_bacnet_device_obj *dev = Z_BACNET_DEVICE_P(&device_zval);

        dev->device_id = entries[i].device_id;
        dev->max_apdu  = entries[i].max_apdu;
        dev->vendor_id = entries[i].vendor_id;
        memcpy(&dev->address, &entries[i].address, sizeof(BACNET_ADDRESS));
        ZVAL_COPY(&dev->client_zval, ZEND_THIS);

        add_next_index_zval(return_value, &device_zval);
    }
}

static const zend_function_entry bacnet_client_methods[] = {
    PHP_ME(Bacnet_Client, __construct, arginfo_bacnet_client_construct, ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_Client, whoIs,       arginfo_bacnet_client_whois,     ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* ────────────────────────────────────────────────────────────────────── */
/*  Bacnet\Device                                                         */
/* ────────────────────────────────────────────────────────────────────── */

static zend_object *php_bacnet_device_create_object(zend_class_entry *ce)
{
    php_bacnet_device_obj *obj =
        (php_bacnet_device_obj *)zend_object_alloc(sizeof(php_bacnet_device_obj), ce);
    obj->device_id = 0;
    obj->max_apdu  = 0;
    obj->vendor_id = 0;
    memset(&obj->address, 0, sizeof(obj->address));
    ZVAL_UNDEF(&obj->client_zval);
    zend_object_std_init(&obj->std, ce);
    object_properties_init(&obj->std, ce);
    obj->std.handlers = &php_bacnet_device_handlers;
    return &obj->std;
}

static void php_bacnet_device_free_object(zend_object *object)
{
    php_bacnet_device_obj *obj = php_bacnet_device_from_obj(object);
    zval_ptr_dtor(&obj->client_zval);
    zend_object_std_dtor(object);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_device_get_device_id, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Device, getDeviceId)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG((zend_long)Z_BACNET_DEVICE_P(ZEND_THIS)->device_id);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_device_get_address, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Device, getAddress)
{
    ZEND_PARSE_PARAMETERS_NONE();
    php_bacnet_device_obj *obj = Z_BACNET_DEVICE_P(ZEND_THIS);
    char ipbuf[32]; uint16_t port;
    php_bacnet_address_to_ipport(&obj->address, ipbuf, sizeof(ipbuf), &port);
    char result[64];
    snprintf(result, sizeof(result), "%s:%u", ipbuf, (unsigned)port);
    RETURN_STRING(result);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_device_get_max_apdu, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Device, getMaxApdu)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG((zend_long)Z_BACNET_DEVICE_P(ZEND_THIS)->max_apdu);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_device_get_vendor_id, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Device, getVendorId)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG((zend_long)Z_BACNET_DEVICE_P(ZEND_THIS)->vendor_id);
}

/* readProperty(ObjectType $objectType, int $instance, Property $property, ?int $arrayIndex = null): mixed */
ZEND_BEGIN_ARG_INFO_EX(arginfo_bacnet_device_read_property, 0, 0, 3)
    ZEND_ARG_OBJ_INFO(0, objectType, Bacnet\\ObjectType, 0)
    ZEND_ARG_TYPE_INFO(0, instance,  IS_LONG,           0)
    ZEND_ARG_OBJ_INFO(0, property,   Bacnet\\Property,  0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, arrayIndex, IS_LONG, 1, "null")
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Device, readProperty)
{
    zval     *ot_enum;
    zend_long instance;
    zval     *prop_enum;
    zend_long array_index  = 0;
    bool      aidx_null    = true;

    ZEND_PARSE_PARAMETERS_START(3, 4)
        Z_PARAM_OBJECT_OF_CLASS(ot_enum,   bacnet_ce_object_type_enum)
        Z_PARAM_LONG(instance)
        Z_PARAM_OBJECT_OF_CLASS(prop_enum, bacnet_ce_property_enum)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG_OR_NULL(array_index, aidx_null)
    ZEND_PARSE_PARAMETERS_END();

    php_bacnet_device_obj *dev = Z_BACNET_DEVICE_P(ZEND_THIS);

    if (Z_TYPE(dev->client_zval) != IS_OBJECT) {
        zend_throw_exception(bacnet_ce_exception,
            "Device has no associated client", 0);
        RETURN_THROWS();
    }

    php_bacnet_client_obj *cl = Z_BACNET_CLIENT_P(&dev->client_zval);
    if (!cl->client) {
        zend_throw_exception(bacnet_ce_exception, "BACnet client not initialized", 0);
        RETURN_THROWS();
    }

    zval *ot_backing   = zend_enum_fetch_case_value(Z_OBJ_P(ot_enum));
    zval *prop_backing = zend_enum_fetch_case_value(Z_OBJ_P(prop_enum));

    uint32_t aidx = aidx_null
        ? BACNET_ARRAY_ALL
        : (uint32_t)array_index;
    uint32_t tms  = (uint32_t)BACNET_G(default_timeout_ms);

    bacnet_exec_read_property(
        cl->client,
        &dev->address,
        (BACNET_OBJECT_TYPE)Z_LVAL_P(ot_backing),
        (uint32_t)instance,
        (BACNET_PROPERTY_ID)Z_LVAL_P(prop_backing),
        aidx, tms,
        return_value);
}

static const zend_function_entry bacnet_device_methods[] = {
    PHP_ME(Bacnet_Device, getDeviceId,   arginfo_bacnet_device_get_device_id,   ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_Device, getAddress,    arginfo_bacnet_device_get_address,     ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_Device, getMaxApdu,    arginfo_bacnet_device_get_max_apdu,    ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_Device, getVendorId,   arginfo_bacnet_device_get_vendor_id,   ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_Device, readProperty,  arginfo_bacnet_device_read_property,   ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* ────────────────────────────────────────────────────────────────────── */
/*  Bacnet\ObjectIdentifier                                               */
/* ────────────────────────────────────────────────────────────────────── */

static zend_object *php_bacnet_oid_create_object(zend_class_entry *ce)
{
    php_bacnet_object_identifier_obj *obj =
        (php_bacnet_object_identifier_obj *)
            zend_object_alloc(sizeof(php_bacnet_object_identifier_obj), ce);
    obj->object_type = MAX_BACNET_OBJECT_TYPE;
    obj->instance    = 0;
    ZVAL_UNDEF(&obj->type_zval);
    zend_object_std_init(&obj->std, ce);
    object_properties_init(&obj->std, ce);
    obj->std.handlers = &php_bacnet_oid_handlers;
    return &obj->std;
}

static void php_bacnet_oid_free_object(zend_object *object)
{
    php_bacnet_object_identifier_obj *obj = php_bacnet_oid_from_obj(object);
    zval_ptr_dtor(&obj->type_zval);
    zend_object_std_dtor(object);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_bacnet_oid_construct, 0, 0, 2)
    ZEND_ARG_OBJ_INFO(0, type,     Bacnet\\ObjectType, 0)
    ZEND_ARG_TYPE_INFO(0, instance, IS_LONG,           0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ObjectIdentifier, __construct)
{
    zval *type_enum;
    zend_long instance;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(type_enum, bacnet_ce_object_type_enum)
        Z_PARAM_LONG(instance)
    ZEND_PARSE_PARAMETERS_END();

    if (instance < 0 || instance > BACNET_MAX_INSTANCE) {
        zend_throw_exception_ex(bacnet_ce_exception, 0,
            "Invalid BACnet object instance %ld (must be 0..%u)",
            (long)instance, BACNET_MAX_INSTANCE);
        RETURN_THROWS();
    }

    php_bacnet_object_identifier_obj *obj = Z_BACNET_OID_P(ZEND_THIS);
    zval *backing = zend_enum_fetch_case_value(Z_OBJ_P(type_enum));
    obj->object_type = (BACNET_OBJECT_TYPE)Z_LVAL_P(backing);
    obj->instance    = (uint32_t)instance;
    ZVAL_COPY(&obj->type_zval, type_enum);

    zend_update_property(bacnet_ce_object_identifier, Z_OBJ_P(ZEND_THIS),
        "type", sizeof("type") - 1, type_enum);
    zend_update_property_long(bacnet_ce_object_identifier, Z_OBJ_P(ZEND_THIS),
        "instance", sizeof("instance") - 1, instance);
}

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_bacnet_oid_get_type, 0, 0, Bacnet\\ObjectType, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_ObjectIdentifier, getType)
{
    ZEND_PARSE_PARAMETERS_NONE();
    php_bacnet_object_identifier_obj *obj = Z_BACNET_OID_P(ZEND_THIS);
    if (Z_TYPE(obj->type_zval) == IS_UNDEF) { RETURN_NULL(); }
    RETURN_ZVAL(&obj->type_zval, 1, 0);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_oid_get_instance, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_ObjectIdentifier, getInstance)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG((zend_long)Z_BACNET_OID_P(ZEND_THIS)->instance);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_oid_to_string, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_ObjectIdentifier, __toString)
{
    ZEND_PARSE_PARAMETERS_NONE();
    php_bacnet_object_identifier_obj *obj = Z_BACNET_OID_P(ZEND_THIS);
    char buf[64];
    snprintf(buf, sizeof(buf), "%u:%u", (unsigned)obj->object_type, obj->instance);
    RETURN_STRING(buf);
}

static const zend_function_entry bacnet_oid_methods[] = {
    PHP_ME(Bacnet_ObjectIdentifier, __construct, arginfo_bacnet_oid_construct,    ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_ObjectIdentifier, getType,     arginfo_bacnet_oid_get_type,     ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_ObjectIdentifier, getInstance, arginfo_bacnet_oid_get_instance, ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_ObjectIdentifier, __toString,  arginfo_bacnet_oid_to_string,    ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* ────────────────────────────────────────────────────────────────────── */
/*  Bacnet\BitString                                                      */
/* ────────────────────────────────────────────────────────────────────── */

static zend_object *php_bacnet_bitstring_create_object(zend_class_entry *ce)
{
    php_bacnet_bitstring_obj *obj =
        (php_bacnet_bitstring_obj *)zend_object_alloc(sizeof(php_bacnet_bitstring_obj), ce);
    obj->bits_used = 0;
    memset(obj->value, 0, sizeof(obj->value));
    zend_object_std_init(&obj->std, ce);
    object_properties_init(&obj->std, ce);
    obj->std.handlers = &php_bacnet_bitstring_handlers;
    return &obj->std;
}

static void php_bacnet_bitstring_free_object(zend_object *object)
{
    zend_object_std_dtor(object);
}

/* __construct(array $bits) — e.g. [true, false, true] */
ZEND_BEGIN_ARG_INFO_EX(arginfo_bacnet_bitstring_construct, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, bits, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_BitString, __construct)
{
    zval *bits_arr;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ARRAY(bits_arr)
    ZEND_PARSE_PARAMETERS_END();

    php_bacnet_bitstring_obj *obj = Z_BACNET_BITSTRING_P(ZEND_THIS);
    memset(obj->value, 0, sizeof(obj->value));

    zend_long n = (zend_long)zend_hash_num_elements(Z_ARRVAL_P(bits_arr));
    if (n > PHP_BACNET_BITSTRING_MAX_BYTES * 8) {
        n = PHP_BACNET_BITSTRING_MAX_BYTES * 8;
    }
    obj->bits_used = (uint8_t)n;

    zend_long idx = 0;
    zval *elem;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(bits_arr), elem) {
        if (idx >= n) break;
        if (zend_is_true(elem)) {
            unsigned byte_no  = (unsigned)idx / 8;
            unsigned bit_shift = 7u - ((unsigned)idx % 8u);
            obj->value[byte_no] |= (uint8_t)(1u << bit_shift);
        }
        idx++;
    } ZEND_HASH_FOREACH_END();

    zend_update_property_long(bacnet_ce_bit_string, Z_OBJ_P(ZEND_THIS),
        "length", sizeof("length") - 1, (zend_long)obj->bits_used);
}

/* getBit(int $index): bool */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_bitstring_get_bit, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, index, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_BitString, getBit)
{
    zend_long index;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(index)
    ZEND_PARSE_PARAMETERS_END();

    php_bacnet_bitstring_obj *obj = Z_BACNET_BITSTRING_P(ZEND_THIS);
    if (index < 0 || index >= (zend_long)obj->bits_used) {
        RETURN_FALSE;
    }
    unsigned byte_no  = (unsigned)index / 8;
    unsigned bit_shift = 7u - ((unsigned)index % 8u);
    RETURN_BOOL((obj->value[byte_no] >> bit_shift) & 1u);
}

/* getLength(): int */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_bitstring_get_length, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_BitString, getLength)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG((zend_long)Z_BACNET_BITSTRING_P(ZEND_THIS)->bits_used);
}

/* toArray(): bool[] */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_bitstring_to_array, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_BitString, toArray)
{
    ZEND_PARSE_PARAMETERS_NONE();
    php_bacnet_bitstring_obj *obj = Z_BACNET_BITSTRING_P(ZEND_THIS);
    array_init(return_value);
    for (uint8_t i = 0; i < obj->bits_used; i++) {
        unsigned byte_no  = i / 8;
        unsigned bit_shift = 7u - (i % 8u);
        add_next_index_bool(return_value, (obj->value[byte_no] >> bit_shift) & 1u);
    }
}

static const zend_function_entry bacnet_bitstring_methods[] = {
    PHP_ME(Bacnet_BitString, __construct, arginfo_bacnet_bitstring_construct,  ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_BitString, getBit,      arginfo_bacnet_bitstring_get_bit,    ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_BitString, getLength,   arginfo_bacnet_bitstring_get_length, ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_BitString, toArray,     arginfo_bacnet_bitstring_to_array,   ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* ────────────────────────────────────────────────────────────────────── */
/*  Bacnet\Date                                                           */
/* ────────────────────────────────────────────────────────────────────── */

static zend_object *php_bacnet_date_create_object(zend_class_entry *ce)
{
    php_bacnet_date_obj *obj =
        (php_bacnet_date_obj *)zend_object_alloc(sizeof(php_bacnet_date_obj), ce);
    obj->year = 0; obj->month = 0; obj->day = 0; obj->weekday = 0;
    zend_object_std_init(&obj->std, ce);
    object_properties_init(&obj->std, ce);
    obj->std.handlers = &php_bacnet_date_handlers;
    return &obj->std;
}

static void php_bacnet_date_free_object(zend_object *object)
{
    zend_object_std_dtor(object);
}

/* __construct(int $year, int $month, int $day, int $weekday) */
ZEND_BEGIN_ARG_INFO_EX(arginfo_bacnet_date_construct, 0, 0, 4)
    ZEND_ARG_TYPE_INFO(0, year,    IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, month,   IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, day,     IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, weekday, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Date, __construct)
{
    zend_long year, month, day, weekday;
    ZEND_PARSE_PARAMETERS_START(4, 4)
        Z_PARAM_LONG(year)
        Z_PARAM_LONG(month)
        Z_PARAM_LONG(day)
        Z_PARAM_LONG(weekday)
    ZEND_PARSE_PARAMETERS_END();

    php_bacnet_date_obj *obj = Z_BACNET_DATE_P(ZEND_THIS);
    obj->year    = (uint16_t)year;
    obj->month   = (uint8_t)month;
    obj->day     = (uint8_t)day;
    obj->weekday = (uint8_t)weekday;

    zend_object *zo = Z_OBJ_P(ZEND_THIS);
    zend_update_property_long(bacnet_ce_date, zo, "year",    sizeof("year")    - 1, year);
    zend_update_property_long(bacnet_ce_date, zo, "month",   sizeof("month")   - 1, month);
    zend_update_property_long(bacnet_ce_date, zo, "day",     sizeof("day")     - 1, day);
    zend_update_property_long(bacnet_ce_date, zo, "weekday", sizeof("weekday") - 1, weekday);
}

static const zend_function_entry bacnet_date_methods[] = {
    PHP_ME(Bacnet_Date, __construct, arginfo_bacnet_date_construct, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* ────────────────────────────────────────────────────────────────────── */
/*  Bacnet\Time                                                           */
/* ────────────────────────────────────────────────────────────────────── */

static zend_object *php_bacnet_time_create_object(zend_class_entry *ce)
{
    php_bacnet_time_obj *obj =
        (php_bacnet_time_obj *)zend_object_alloc(sizeof(php_bacnet_time_obj), ce);
    obj->hour = 0; obj->minute = 0; obj->second = 0; obj->hundredths = 0;
    zend_object_std_init(&obj->std, ce);
    object_properties_init(&obj->std, ce);
    obj->std.handlers = &php_bacnet_time_handlers;
    return &obj->std;
}

static void php_bacnet_time_free_object(zend_object *object)
{
    zend_object_std_dtor(object);
}

/* __construct(int $hour, int $minute, int $second, int $hundredths) */
ZEND_BEGIN_ARG_INFO_EX(arginfo_bacnet_time_construct, 0, 0, 4)
    ZEND_ARG_TYPE_INFO(0, hour,       IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, minute,     IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, second,     IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, hundredths, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Time, __construct)
{
    zend_long hour, minute, second, hundredths;
    ZEND_PARSE_PARAMETERS_START(4, 4)
        Z_PARAM_LONG(hour)
        Z_PARAM_LONG(minute)
        Z_PARAM_LONG(second)
        Z_PARAM_LONG(hundredths)
    ZEND_PARSE_PARAMETERS_END();

    php_bacnet_time_obj *obj = Z_BACNET_TIME_P(ZEND_THIS);
    obj->hour       = (uint8_t)hour;
    obj->minute     = (uint8_t)minute;
    obj->second     = (uint8_t)second;
    obj->hundredths = (uint8_t)hundredths;

    zend_object *zo = Z_OBJ_P(ZEND_THIS);
    zend_update_property_long(bacnet_ce_time, zo, "hour",       sizeof("hour")       - 1, hour);
    zend_update_property_long(bacnet_ce_time, zo, "minute",     sizeof("minute")     - 1, minute);
    zend_update_property_long(bacnet_ce_time, zo, "second",     sizeof("second")     - 1, second);
    zend_update_property_long(bacnet_ce_time, zo, "hundredths", sizeof("hundredths") - 1, hundredths);
}

static const zend_function_entry bacnet_time_methods[] = {
    PHP_ME(Bacnet_Time, __construct, arginfo_bacnet_time_construct, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* ────────────────────────────────────────────────────────────────────── */
/*  Bacnet\ObjectRef                                                      */
/* ────────────────────────────────────────────────────────────────────── */

static zend_object *php_bacnet_objectref_create_object(zend_class_entry *ce)
{
    php_bacnet_objectref_obj *obj =
        (php_bacnet_objectref_obj *)zend_object_alloc(sizeof(php_bacnet_objectref_obj), ce);
    obj->object_type = MAX_BACNET_OBJECT_TYPE;
    obj->instance    = 0;
    ZVAL_UNDEF(&obj->device_zval);
    zend_object_std_init(&obj->std, ce);
    object_properties_init(&obj->std, ce);
    obj->std.handlers = &php_bacnet_objectref_handlers;
    return &obj->std;
}

static void php_bacnet_objectref_free_object(zend_object *object)
{
    php_bacnet_objectref_obj *obj = php_bacnet_objectref_from_obj(object);
    zval_ptr_dtor(&obj->device_zval);
    zend_object_std_dtor(object);
}

/* __construct(Device $device, ObjectType $type, int $instance) */
ZEND_BEGIN_ARG_INFO_EX(arginfo_bacnet_objectref_construct, 0, 0, 3)
    ZEND_ARG_OBJ_INFO(0, device,   Bacnet\\Device,     0)
    ZEND_ARG_OBJ_INFO(0, type,     Bacnet\\ObjectType, 0)
    ZEND_ARG_TYPE_INFO(0, instance, IS_LONG,           0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ObjectRef, __construct)
{
    zval     *dev_zv;
    zval     *ot_enum;
    zend_long instance;

    ZEND_PARSE_PARAMETERS_START(3, 3)
        Z_PARAM_OBJECT_OF_CLASS(dev_zv, bacnet_ce_device)
        Z_PARAM_OBJECT_OF_CLASS(ot_enum, bacnet_ce_object_type_enum)
        Z_PARAM_LONG(instance)
    ZEND_PARSE_PARAMETERS_END();

    if (instance < 0 || instance > BACNET_MAX_INSTANCE) {
        zend_throw_exception_ex(bacnet_ce_exception, 0,
            "Invalid BACnet object instance %ld", (long)instance);
        RETURN_THROWS();
    }

    php_bacnet_objectref_obj *obj = Z_BACNET_OBJREF_P(ZEND_THIS);
    zval *backing = zend_enum_fetch_case_value(Z_OBJ_P(ot_enum));
    obj->object_type = (BACNET_OBJECT_TYPE)Z_LVAL_P(backing);
    obj->instance    = (uint32_t)instance;
    ZVAL_COPY(&obj->device_zval, dev_zv);
}

/* readProperty(Property $property, ?int $arrayIndex = null): mixed */
ZEND_BEGIN_ARG_INFO_EX(arginfo_bacnet_objectref_read_property, 0, 0, 1)
    ZEND_ARG_OBJ_INFO(0, property,   Bacnet\\Property, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, arrayIndex, IS_LONG, 1, "null")
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ObjectRef, readProperty)
{
    zval     *prop_enum;
    zend_long array_index = 0;
    bool      aidx_null   = true;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_OBJECT_OF_CLASS(prop_enum, bacnet_ce_property_enum)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG_OR_NULL(array_index, aidx_null)
    ZEND_PARSE_PARAMETERS_END();

    php_bacnet_objectref_obj *ref = Z_BACNET_OBJREF_P(ZEND_THIS);
    if (Z_TYPE(ref->device_zval) != IS_OBJECT) {
        zend_throw_exception(bacnet_ce_exception, "ObjectRef has no associated device", 0);
        RETURN_THROWS();
    }

    php_bacnet_device_obj *dev = Z_BACNET_DEVICE_P(&ref->device_zval);
    if (Z_TYPE(dev->client_zval) != IS_OBJECT) {
        zend_throw_exception(bacnet_ce_exception, "Device has no associated client", 0);
        RETURN_THROWS();
    }

    php_bacnet_client_obj *cl = Z_BACNET_CLIENT_P(&dev->client_zval);
    if (!cl->client) {
        zend_throw_exception(bacnet_ce_exception, "BACnet client not initialized", 0);
        RETURN_THROWS();
    }

    zval *prop_backing = zend_enum_fetch_case_value(Z_OBJ_P(prop_enum));
    uint32_t aidx = aidx_null ? BACNET_ARRAY_ALL : (uint32_t)array_index;
    uint32_t tms  = (uint32_t)BACNET_G(default_timeout_ms);

    bacnet_exec_read_property(
        cl->client, &dev->address,
        ref->object_type, ref->instance,
        (BACNET_PROPERTY_ID)Z_LVAL_P(prop_backing),
        aidx, tms,
        return_value);
}

static const zend_function_entry bacnet_objectref_methods[] = {
    PHP_ME(Bacnet_ObjectRef, __construct,  arginfo_bacnet_objectref_construct,    ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_ObjectRef, readProperty, arginfo_bacnet_objectref_read_property, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* ────────────────────────────────────────────────────────────────────── */
/*  Helper: register one int-backed enum case                            */
/* ────────────────────────────────────────────────────────────────────── */

static void bacnet_enum_add_long(zend_class_entry *ce, const char *name, zend_long val)
{
    zval v;
    ZVAL_LONG(&v, val);
    zend_enum_add_case_cstr(ce, name, &v);
}

/* ────────────────────────────────────────────────────────────────────── */
/*  Registration                                                          */
/* ────────────────────────────────────────────────────────────────────── */

void php_bacnet_register_classes(void)
{
    zend_class_entry ce;

    /* ── Exceptions ─────────────────────────────────────────────────── */

    INIT_CLASS_ENTRY(ce, "Bacnet\\Exception", NULL);
    bacnet_ce_exception = zend_register_internal_class_ex(&ce, zend_ce_exception);

    INIT_CLASS_ENTRY(ce, "Bacnet\\TimeoutException", NULL);
    bacnet_ce_timeout_exception =
        zend_register_internal_class_ex(&ce, bacnet_ce_exception);

    INIT_CLASS_ENTRY(ce, "Bacnet\\DeviceException", NULL);
    bacnet_ce_device_exception =
        zend_register_internal_class_ex(&ce, bacnet_ce_exception);
    zend_declare_property_long(bacnet_ce_device_exception,
        "errorClass", sizeof("errorClass") - 1, 0, ZEND_ACC_PUBLIC);
    zend_declare_property_long(bacnet_ce_device_exception,
        "errorCode",  sizeof("errorCode")  - 1, 0, ZEND_ACC_PUBLIC);

    /* ── enum Bacnet\ObjectType: int ────────────────────────────────── */

    bacnet_ce_object_type_enum =
        zend_register_internal_enum("Bacnet\\ObjectType", IS_LONG, NULL);

    bacnet_enum_add_long(bacnet_ce_object_type_enum, "ANALOG_INPUT",       0);
    bacnet_enum_add_long(bacnet_ce_object_type_enum, "ANALOG_OUTPUT",      1);
    bacnet_enum_add_long(bacnet_ce_object_type_enum, "ANALOG_VALUE",       2);
    bacnet_enum_add_long(bacnet_ce_object_type_enum, "BINARY_INPUT",       3);
    bacnet_enum_add_long(bacnet_ce_object_type_enum, "BINARY_OUTPUT",      4);
    bacnet_enum_add_long(bacnet_ce_object_type_enum, "BINARY_VALUE",       5);
    bacnet_enum_add_long(bacnet_ce_object_type_enum, "DEVICE",             8);
    bacnet_enum_add_long(bacnet_ce_object_type_enum, "EVENT_ENROLLMENT",   9);
    bacnet_enum_add_long(bacnet_ce_object_type_enum, "MULTI_STATE_INPUT",  13);
    bacnet_enum_add_long(bacnet_ce_object_type_enum, "MULTI_STATE_OUTPUT", 14);
    bacnet_enum_add_long(bacnet_ce_object_type_enum, "NOTIFICATION_CLASS", 15);
    bacnet_enum_add_long(bacnet_ce_object_type_enum, "SCHEDULE",           17);
    bacnet_enum_add_long(bacnet_ce_object_type_enum, "MULTI_STATE_VALUE",  19);
    bacnet_enum_add_long(bacnet_ce_object_type_enum, "TREND_LOG",          20);

    /* ── enum Bacnet\Property: int ──────────────────────────────────── */

    bacnet_ce_property_enum =
        zend_register_internal_enum("Bacnet\\Property", IS_LONG, NULL);

    bacnet_enum_add_long(bacnet_ce_property_enum, "OBJECT_IDENTIFIER",           75);
    bacnet_enum_add_long(bacnet_ce_property_enum, "OBJECT_NAME",                 77);
    bacnet_enum_add_long(bacnet_ce_property_enum, "OBJECT_TYPE",                 79);
    bacnet_enum_add_long(bacnet_ce_property_enum, "DESCRIPTION",                 28);
    bacnet_enum_add_long(bacnet_ce_property_enum, "OBJECT_LIST",                 76);
    bacnet_enum_add_long(bacnet_ce_property_enum, "PRESENT_VALUE",               85);
    bacnet_enum_add_long(bacnet_ce_property_enum, "STATUS_FLAGS",               111);
    bacnet_enum_add_long(bacnet_ce_property_enum, "EVENT_STATE",                 36);
    bacnet_enum_add_long(bacnet_ce_property_enum, "OUT_OF_SERVICE",              81);
    bacnet_enum_add_long(bacnet_ce_property_enum, "UNITS",                      117);
    bacnet_enum_add_long(bacnet_ce_property_enum, "PRIORITY_ARRAY",              87);
    bacnet_enum_add_long(bacnet_ce_property_enum, "RELINQUISH_DEFAULT",         104);
    bacnet_enum_add_long(bacnet_ce_property_enum, "NUMBER_OF_STATES",            74);
    bacnet_enum_add_long(bacnet_ce_property_enum, "STATE_TEXT",                 110);
    bacnet_enum_add_long(bacnet_ce_property_enum, "NOTIFICATION_CLASS",          17);
    bacnet_enum_add_long(bacnet_ce_property_enum, "ACK_REQUIRED",                 1);
    bacnet_enum_add_long(bacnet_ce_property_enum, "NOTIFY_TYPE",                 72);
    bacnet_enum_add_long(bacnet_ce_property_enum, "EVENT_TYPE",                  37);
    bacnet_enum_add_long(bacnet_ce_property_enum, "EVENT_PARAMETERS",            83);
    bacnet_enum_add_long(bacnet_ce_property_enum, "OBJECT_PROPERTY_REFERENCE",   78);
    bacnet_enum_add_long(bacnet_ce_property_enum, "RECIPIENT_LIST",             102);
    bacnet_enum_add_long(bacnet_ce_property_enum, "WEEKLY_SCHEDULE",            123);
    bacnet_enum_add_long(bacnet_ce_property_enum, "EXCEPTION_SCHEDULE",          38);
    bacnet_enum_add_long(bacnet_ce_property_enum, "SCHEDULE_DEFAULT",           174);
    bacnet_enum_add_long(bacnet_ce_property_enum, "EFFECTIVE_PERIOD",            32);
    bacnet_enum_add_long(bacnet_ce_property_enum, "LOG_BUFFER",                 131);
    bacnet_enum_add_long(bacnet_ce_property_enum, "LOG_DEVICE_OBJECT_PROPERTY", 132);
    bacnet_enum_add_long(bacnet_ce_property_enum, "RECORD_COUNT",               141);
    bacnet_enum_add_long(bacnet_ce_property_enum, "TOTAL_RECORD_COUNT",         145);
    bacnet_enum_add_long(bacnet_ce_property_enum, "RELIABILITY",                103);

    /* ── Bacnet\Client ───────────────────────────────────────────────── */

    memcpy(&php_bacnet_client_handlers,
           zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    php_bacnet_client_handlers.offset    = XtOffsetOf(php_bacnet_client_obj, std);
    php_bacnet_client_handlers.free_obj  = php_bacnet_client_free_object;
    php_bacnet_client_handlers.clone_obj = NULL;

    INIT_CLASS_ENTRY(ce, "Bacnet\\Client", bacnet_client_methods);
    bacnet_ce_client = zend_register_internal_class(&ce);
    bacnet_ce_client->create_object = php_bacnet_client_create_object;

    /* ── Bacnet\Device ───────────────────────────────────────────────── */

    memcpy(&php_bacnet_device_handlers,
           zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    php_bacnet_device_handlers.offset    = XtOffsetOf(php_bacnet_device_obj, std);
    php_bacnet_device_handlers.free_obj  = php_bacnet_device_free_object;
    php_bacnet_device_handlers.clone_obj = NULL;

    INIT_CLASS_ENTRY(ce, "Bacnet\\Device", bacnet_device_methods);
    bacnet_ce_device = zend_register_internal_class(&ce);
    bacnet_ce_device->create_object = php_bacnet_device_create_object;

    /* ── Bacnet\ObjectIdentifier ─────────────────────────────────────── */

    memcpy(&php_bacnet_oid_handlers,
           zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    php_bacnet_oid_handlers.offset    = XtOffsetOf(php_bacnet_object_identifier_obj, std);
    php_bacnet_oid_handlers.free_obj  = php_bacnet_oid_free_object;
    php_bacnet_oid_handlers.clone_obj = NULL;

    INIT_CLASS_ENTRY(ce, "Bacnet\\ObjectIdentifier", bacnet_oid_methods);
    bacnet_ce_object_identifier = zend_register_internal_class(&ce);
    bacnet_ce_object_identifier->create_object = php_bacnet_oid_create_object;

    zend_declare_property_null(bacnet_ce_object_identifier,
        "type",     sizeof("type")     - 1, ZEND_ACC_PUBLIC);
    zend_declare_property_long(bacnet_ce_object_identifier,
        "instance", sizeof("instance") - 1, 0, ZEND_ACC_PUBLIC);

    /* ── Bacnet\BitString ────────────────────────────────────────────── */

    memcpy(&php_bacnet_bitstring_handlers,
           zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    php_bacnet_bitstring_handlers.offset    = XtOffsetOf(php_bacnet_bitstring_obj, std);
    php_bacnet_bitstring_handlers.free_obj  = php_bacnet_bitstring_free_object;
    php_bacnet_bitstring_handlers.clone_obj = NULL;

    INIT_CLASS_ENTRY(ce, "Bacnet\\BitString", bacnet_bitstring_methods);
    bacnet_ce_bit_string = zend_register_internal_class(&ce);
    bacnet_ce_bit_string->create_object = php_bacnet_bitstring_create_object;

    zend_declare_property_long(bacnet_ce_bit_string,
        "length", sizeof("length") - 1, 0, ZEND_ACC_PUBLIC);

    /* ── Bacnet\Date ─────────────────────────────────────────────────── */

    memcpy(&php_bacnet_date_handlers,
           zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    php_bacnet_date_handlers.offset    = XtOffsetOf(php_bacnet_date_obj, std);
    php_bacnet_date_handlers.free_obj  = php_bacnet_date_free_object;
    php_bacnet_date_handlers.clone_obj = NULL;

    INIT_CLASS_ENTRY(ce, "Bacnet\\Date", bacnet_date_methods);
    bacnet_ce_date = zend_register_internal_class(&ce);
    bacnet_ce_date->create_object = php_bacnet_date_create_object;

    zend_declare_property_long(bacnet_ce_date, "year",    sizeof("year")    - 1, 0, ZEND_ACC_PUBLIC);
    zend_declare_property_long(bacnet_ce_date, "month",   sizeof("month")   - 1, 0, ZEND_ACC_PUBLIC);
    zend_declare_property_long(bacnet_ce_date, "day",     sizeof("day")     - 1, 0, ZEND_ACC_PUBLIC);
    zend_declare_property_long(bacnet_ce_date, "weekday", sizeof("weekday") - 1, 0, ZEND_ACC_PUBLIC);

    /* ── Bacnet\Time ─────────────────────────────────────────────────── */

    memcpy(&php_bacnet_time_handlers,
           zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    php_bacnet_time_handlers.offset    = XtOffsetOf(php_bacnet_time_obj, std);
    php_bacnet_time_handlers.free_obj  = php_bacnet_time_free_object;
    php_bacnet_time_handlers.clone_obj = NULL;

    INIT_CLASS_ENTRY(ce, "Bacnet\\Time", bacnet_time_methods);
    bacnet_ce_time = zend_register_internal_class(&ce);
    bacnet_ce_time->create_object = php_bacnet_time_create_object;

    zend_declare_property_long(bacnet_ce_time, "hour",       sizeof("hour")       - 1, 0, ZEND_ACC_PUBLIC);
    zend_declare_property_long(bacnet_ce_time, "minute",     sizeof("minute")     - 1, 0, ZEND_ACC_PUBLIC);
    zend_declare_property_long(bacnet_ce_time, "second",     sizeof("second")     - 1, 0, ZEND_ACC_PUBLIC);
    zend_declare_property_long(bacnet_ce_time, "hundredths", sizeof("hundredths") - 1, 0, ZEND_ACC_PUBLIC);

    /* ── Bacnet\ObjectRef ────────────────────────────────────────────── */

    memcpy(&php_bacnet_objectref_handlers,
           zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    php_bacnet_objectref_handlers.offset    = XtOffsetOf(php_bacnet_objectref_obj, std);
    php_bacnet_objectref_handlers.free_obj  = php_bacnet_objectref_free_object;
    php_bacnet_objectref_handlers.clone_obj = NULL;

    INIT_CLASS_ENTRY(ce, "Bacnet\\ObjectRef", bacnet_objectref_methods);
    bacnet_ce_object_ref = zend_register_internal_class(&ce);
    bacnet_ce_object_ref->create_object = php_bacnet_objectref_create_object;

    /* ── Bacnet\Value (Phase 5 stub) ─────────────────────────────────── */

    INIT_CLASS_ENTRY(ce, "Bacnet\\Value", NULL);
    bacnet_ce_value = zend_register_internal_class(&ce);
}
