#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "zend_exceptions.h"
#include "zend_interfaces.h"

#ifndef BACDL_BIP
#define BACDL_BIP
#endif
#include "bacnet/bacdef.h"
#include "bacnet/whois.h"

#include "../php_bacnet.h"
#include "bacnet_classes.h"
#include "bacnet_types.h"
#include "bacnet_client.h"

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

/* ── Bacnet\Client::__construct(?string $interface, ?int $port, ?int $timeoutMs) */

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
    uint16_t p = port_null       ? (uint16_t)BACNET_G(default_port)       : (uint16_t)port;

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

/* ── Bacnet\Client::whoIs(?int $lowLimit, ?int $highLimit, ?int $timeoutMs): array */

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
        zend_throw_exception(bacnet_ce_exception,
            "BACnet client is not initialized", 0);
        RETURN_THROWS();
    }

    int32_t lo = low_null  ? 0                  : (int32_t)low_limit;
    int32_t hi = high_null ? BACNET_MAX_INSTANCE : (int32_t)high_limit;
    uint32_t tms = tms_null
        ? (uint32_t)BACNET_G(default_timeout_ms)
        : (uint32_t)timeout_ms;

    /* Clamp range */
    if (lo < 0) lo = 0;
    if (hi > BACNET_MAX_INSTANCE) hi = BACNET_MAX_INSTANCE;

    uint8_t apdu[64];
    int apdu_len = whois_encode_apdu(apdu, lo, hi);
    if (apdu_len <= 0) {
        zend_throw_exception(bacnet_ce_exception,
            "Failed to encode Who-Is APDU", 0);
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

/* ── Bacnet\Device::getDeviceId(): int */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_device_get_device_id, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Device, getDeviceId)
{
    ZEND_PARSE_PARAMETERS_NONE();
    php_bacnet_device_obj *obj = Z_BACNET_DEVICE_P(ZEND_THIS);
    RETURN_LONG((zend_long)obj->device_id);
}

/* ── Bacnet\Device::getAddress(): string  (dotted-IP:port, for debugging) */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_device_get_address, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Device, getAddress)
{
    ZEND_PARSE_PARAMETERS_NONE();
    php_bacnet_device_obj *obj = Z_BACNET_DEVICE_P(ZEND_THIS);
    char ipbuf[32];
    uint16_t port;
    php_bacnet_address_to_ipport(&obj->address, ipbuf, sizeof(ipbuf), &port);
    char result[64];
    snprintf(result, sizeof(result), "%s:%u", ipbuf, (unsigned)port);
    RETURN_STRING(result);
}

/* ── Bacnet\Device::getMaxApdu(): int */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_device_get_max_apdu, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Device, getMaxApdu)
{
    ZEND_PARSE_PARAMETERS_NONE();
    php_bacnet_device_obj *obj = Z_BACNET_DEVICE_P(ZEND_THIS);
    RETURN_LONG((zend_long)obj->max_apdu);
}

/* ── Bacnet\Device::getVendorId(): int */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_device_get_vendor_id, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Device, getVendorId)
{
    ZEND_PARSE_PARAMETERS_NONE();
    php_bacnet_device_obj *obj = Z_BACNET_DEVICE_P(ZEND_THIS);
    RETURN_LONG((zend_long)obj->vendor_id);
}

static const zend_function_entry bacnet_device_methods[] = {
    PHP_ME(Bacnet_Device, getDeviceId, arginfo_bacnet_device_get_device_id, ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_Device, getAddress,  arginfo_bacnet_device_get_address,   ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_Device, getMaxApdu,  arginfo_bacnet_device_get_max_apdu,  ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_Device, getVendorId, arginfo_bacnet_device_get_vendor_id, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* ────────────────────────────────────────────────────────────────────── */
/*  Registration                                                          */
/* ────────────────────────────────────────────────────────────────────── */

void php_bacnet_register_classes(void)
{
    zend_class_entry ce;

    /* Bacnet\Exception — extends \Exception */
    INIT_CLASS_ENTRY(ce, "Bacnet\\Exception", NULL);
    bacnet_ce_exception = zend_register_internal_class_ex(&ce, zend_ce_exception);

    /* Bacnet\TimeoutException — extends Bacnet\Exception */
    INIT_CLASS_ENTRY(ce, "Bacnet\\TimeoutException", NULL);
    bacnet_ce_timeout_exception =
        zend_register_internal_class_ex(&ce, bacnet_ce_exception);

    /* Bacnet\DeviceException — extends Bacnet\Exception */
    INIT_CLASS_ENTRY(ce, "Bacnet\\DeviceException", NULL);
    bacnet_ce_device_exception =
        zend_register_internal_class_ex(&ce, bacnet_ce_exception);
    zend_declare_property_long(bacnet_ce_device_exception, "errorClass", 10, 0,
        ZEND_ACC_PUBLIC);
    zend_declare_property_long(bacnet_ce_device_exception, "errorCode",  9,  0,
        ZEND_ACC_PUBLIC);

    /* Bacnet\Client */
    memcpy(&php_bacnet_client_handlers,
           zend_get_std_object_handlers(),
           sizeof(zend_object_handlers));
    php_bacnet_client_handlers.offset    = XtOffsetOf(php_bacnet_client_obj, std);
    php_bacnet_client_handlers.free_obj  = php_bacnet_client_free_object;
    php_bacnet_client_handlers.clone_obj = NULL; /* cloning not allowed */

    INIT_CLASS_ENTRY(ce, "Bacnet\\Client", bacnet_client_methods);
    bacnet_ce_client = zend_register_internal_class(&ce);
    bacnet_ce_client->create_object = php_bacnet_client_create_object;

    /* Bacnet\Device */
    memcpy(&php_bacnet_device_handlers,
           zend_get_std_object_handlers(),
           sizeof(zend_object_handlers));
    php_bacnet_device_handlers.offset    = XtOffsetOf(php_bacnet_device_obj, std);
    php_bacnet_device_handlers.free_obj  = php_bacnet_device_free_object;
    php_bacnet_device_handlers.clone_obj = NULL;

    INIT_CLASS_ENTRY(ce, "Bacnet\\Device", bacnet_device_methods);
    bacnet_ce_device = zend_register_internal_class(&ce);
    bacnet_ce_device->create_object = php_bacnet_device_create_object;

    /* Remaining classes: stubs for Phase 3/4/5 */
    INIT_CLASS_ENTRY(ce, "Bacnet\\ObjectIdentifier", NULL);
    bacnet_ce_object_identifier = zend_register_internal_class(&ce);

    INIT_CLASS_ENTRY(ce, "Bacnet\\ObjectRef", NULL);
    bacnet_ce_object_ref = zend_register_internal_class(&ce);

    INIT_CLASS_ENTRY(ce, "Bacnet\\Value", NULL);
    bacnet_ce_value = zend_register_internal_class(&ce);

    INIT_CLASS_ENTRY(ce, "Bacnet\\BitString", NULL);
    bacnet_ce_bit_string = zend_register_internal_class(&ce);

    INIT_CLASS_ENTRY(ce, "Bacnet\\Date", NULL);
    bacnet_ce_date = zend_register_internal_class(&ce);

    INIT_CLASS_ENTRY(ce, "Bacnet\\Time", NULL);
    bacnet_ce_time = zend_register_internal_class(&ce);
}
