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
#include "bacnet/iam.h"
#include "bacnet/rp.h"
#include "bacnet/wp.h"
#include "bacnet/bacapp.h"
#include "bacnet/bacerror.h"
#include "bacnet/bacstr.h"
#include "bacnet/datetime.h"

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
zend_class_entry *bacnet_ce_server            = NULL;
zend_class_entry *bacnet_ce_schedule_entry    = NULL;
zend_class_entry *bacnet_ce_weekly_schedule   = NULL;
zend_class_entry *bacnet_ce_trend_log_record  = NULL;
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
static zend_object_handlers php_bacnet_value_handlers;
static zend_object_handlers php_bacnet_server_handlers;

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

/* ── Shared WriteProperty logic ──────────────────────────────────────────
 * Throws on error, returns 0 on success (void PHP method).
 */
static int bacnet_exec_write_property(
    php_bacnet_client  *client,
    BACNET_ADDRESS     *dest,
    BACNET_OBJECT_TYPE  obj_type,
    uint32_t            obj_inst,
    BACNET_PROPERTY_ID  prop_id,
    uint32_t            array_index,
    uint8_t             priority,
    zval               *value_zv,
    uint32_t            timeout_ms)
{
    php_bacnet_value_obj *val = Z_BACNET_VALUE_P(value_zv);

    /* Encode the application data into the WP struct's inline buffer */
    BACNET_WRITE_PROPERTY_DATA wpdata;
    memset(&wpdata, 0, sizeof(wpdata));
    wpdata.object_type     = obj_type;
    wpdata.object_instance = obj_inst;
    wpdata.object_property = prop_id;
    wpdata.array_index     = (BACNET_ARRAY_INDEX)array_index;
    wpdata.priority        = priority;

    int enc_len = bacapp_encode_application_data(
        wpdata.application_data, &val->appdata);
    if (enc_len <= 0) {
        zend_throw_exception(bacnet_ce_exception,
            "Failed to encode WriteProperty application data", 0);
        return -1;
    }
    wpdata.application_data_len = enc_len;

    /* Build WriteProperty APDU */
    uint8_t invoke_id = BACNET_G(next_invoke_id)++;
    uint8_t req_apdu[MAX_APDU];
    int req_len = wp_encode_apdu(req_apdu, invoke_id, &wpdata);
    if (req_len <= 0) {
        zend_throw_exception(bacnet_ce_exception,
            "Failed to encode WriteProperty APDU", 0);
        return -1;
    }

    /* Send and wait — Simple-ACK on success */
    uint8_t  resp_apdu[MAX_APDU];
    uint16_t resp_len = 0;
    int status = php_bacnet_send_and_wait(
        client, dest,
        req_apdu, (uint16_t)req_len,
        invoke_id, resp_apdu, &resp_len, timeout_ms);

    if (status == -1) {
        zend_throw_exception_ex(bacnet_ce_timeout_exception, 0,
            "WriteProperty timed out after %u ms", (unsigned)timeout_ms);
        return -1;
    }

    if (status == (int)PDU_TYPE_ERROR) {
        if (resp_len < 3) {
            zend_throw_exception(bacnet_ce_device_exception,
                "BACnet write error (malformed)", 0);
            return -1;
        }
        uint8_t                  err_iid = 0;
        BACNET_CONFIRMED_SERVICE svc    = SERVICE_CONFIRMED_WRITE_PROPERTY;
        BACNET_ERROR_CLASS       ec     = ERROR_CLASS_DEVICE;
        BACNET_ERROR_CODE        code   = ERROR_CODE_OTHER;
        bacerror_decode_service_request(
            resp_apdu + 1, resp_len - 1, &err_iid, &svc, &ec, &code);

        zend_throw_exception_ex(bacnet_ce_device_exception, 0,
            "BACnet write error: class=%d code=%d", (int)ec, (int)code);
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
            "BACnet %s PDU during write (reason %u)",
            (status == (int)PDU_TYPE_REJECT) ? "Reject" : "Abort",
            (unsigned)reason);
        return -1;
    }

    return 0; /* Simple-ACK → success */
}

/* ────────────────────────────────────────────────────────────────────── */
/*  Bacnet\Value — typed factory class                                    */
/* ────────────────────────────────────────────────────────────────────── */

static zend_object *php_bacnet_value_create_object(zend_class_entry *ce)
{
    php_bacnet_value_obj *obj =
        (php_bacnet_value_obj *)zend_object_alloc(sizeof(php_bacnet_value_obj), ce);
    memset(&obj->appdata, 0, sizeof(obj->appdata));
    zend_object_std_init(&obj->std, ce);
    object_properties_init(&obj->std, ce);
    obj->std.handlers = &php_bacnet_value_handlers;
    return &obj->std;
}

static void php_bacnet_value_free_object(zend_object *object)
{
    zend_object_std_dtor(object);
}

/* Helper: allocate new Value object, set tag and fill appdata, return it */
static php_bacnet_value_obj *bacnet_value_alloc(zval *out)
{
    object_init_ex(out, bacnet_ce_value);
    return Z_BACNET_VALUE_P(out);
}

/* Value::boolean(bool $v): self */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_value_boolean, 0, 1, Bacnet\\Value, 0)
    ZEND_ARG_TYPE_INFO(0, value, _IS_BOOL, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Value, boolean)
{
    bool v;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_BOOL(v) ZEND_PARSE_PARAMETERS_END();
    php_bacnet_value_obj *obj = bacnet_value_alloc(return_value);
    obj->appdata.tag = BACNET_APPLICATION_TAG_BOOLEAN;
    obj->appdata.type.Boolean = v;
}

/* Value::unsignedInt(int $v): self */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_value_unsigned_int, 0, 1, Bacnet\\Value, 0)
    ZEND_ARG_TYPE_INFO(0, value, IS_LONG, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Value, unsignedInt)
{
    zend_long v;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_LONG(v) ZEND_PARSE_PARAMETERS_END();
    php_bacnet_value_obj *obj = bacnet_value_alloc(return_value);
    obj->appdata.tag = BACNET_APPLICATION_TAG_UNSIGNED_INT;
    obj->appdata.type.Unsigned_Int = (BACNET_UNSIGNED_INTEGER)(v < 0 ? 0 : (uint32_t)v);
}

/* Value::signedInt(int $v): self */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_value_signed_int, 0, 1, Bacnet\\Value, 0)
    ZEND_ARG_TYPE_INFO(0, value, IS_LONG, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Value, signedInt)
{
    zend_long v;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_LONG(v) ZEND_PARSE_PARAMETERS_END();
    php_bacnet_value_obj *obj = bacnet_value_alloc(return_value);
    obj->appdata.tag = BACNET_APPLICATION_TAG_SIGNED_INT;
    obj->appdata.type.Signed_Int = (int32_t)v;
}

/* Value::real(float $v): self */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_value_real, 0, 1, Bacnet\\Value, 0)
    ZEND_ARG_TYPE_INFO(0, value, IS_DOUBLE, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Value, real)
{
    double v;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_DOUBLE(v) ZEND_PARSE_PARAMETERS_END();
    php_bacnet_value_obj *obj = bacnet_value_alloc(return_value);
    obj->appdata.tag = BACNET_APPLICATION_TAG_REAL;
    obj->appdata.type.Real = (float)v;
}

/* Value::enumerated(int $v): self */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_value_enumerated, 0, 1, Bacnet\\Value, 0)
    ZEND_ARG_TYPE_INFO(0, value, IS_LONG, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Value, enumerated)
{
    zend_long v;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_LONG(v) ZEND_PARSE_PARAMETERS_END();
    php_bacnet_value_obj *obj = bacnet_value_alloc(return_value);
    obj->appdata.tag = BACNET_APPLICATION_TAG_ENUMERATED;
    obj->appdata.type.Enumerated = (uint32_t)v;
}

/* Value::characterString(string $v): self */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_value_character_string, 0, 1, Bacnet\\Value, 0)
    ZEND_ARG_TYPE_INFO(0, value, IS_STRING, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Value, characterString)
{
    zend_string *v;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_STR(v) ZEND_PARSE_PARAMETERS_END();
    php_bacnet_value_obj *obj = bacnet_value_alloc(return_value);
    obj->appdata.tag = BACNET_APPLICATION_TAG_CHARACTER_STRING;
    characterstring_init_ansi(&obj->appdata.type.Character_String, ZSTR_VAL(v));
}

/* Value::bitString(BitString $v): self */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_value_bit_string, 0, 1, Bacnet\\Value, 0)
    ZEND_ARG_OBJ_INFO(0, value, Bacnet\\BitString, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Value, bitString)
{
    zval *bsv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(bsv, bacnet_ce_bit_string)
    ZEND_PARSE_PARAMETERS_END();
    php_bacnet_bitstring_obj *bs = Z_BACNET_BITSTRING_P(bsv);
    php_bacnet_value_obj *obj = bacnet_value_alloc(return_value);
    obj->appdata.tag = BACNET_APPLICATION_TAG_BIT_STRING;
    obj->appdata.type.Bit_String.bits_used = bs->bits_used;
    memcpy(obj->appdata.type.Bit_String.value, bs->value,
           sizeof(obj->appdata.type.Bit_String.value));
}

/* Value::date(Date $v): self */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_value_date, 0, 1, Bacnet\\Value, 0)
    ZEND_ARG_OBJ_INFO(0, value, Bacnet\\Date, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Value, date)
{
    zval *dv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(dv, bacnet_ce_date)
    ZEND_PARSE_PARAMETERS_END();
    php_bacnet_date_obj *d = Z_BACNET_DATE_P(dv);
    php_bacnet_value_obj *obj = bacnet_value_alloc(return_value);
    obj->appdata.tag = BACNET_APPLICATION_TAG_DATE;
    obj->appdata.type.Date.year  = d->year;
    obj->appdata.type.Date.month = d->month;
    obj->appdata.type.Date.day   = d->day;
    obj->appdata.type.Date.wday  = d->weekday;
}

/* Value::time(Time $v): self */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_value_time, 0, 1, Bacnet\\Value, 0)
    ZEND_ARG_OBJ_INFO(0, value, Bacnet\\Time, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Value, time)
{
    zval *tv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(tv, bacnet_ce_time)
    ZEND_PARSE_PARAMETERS_END();
    php_bacnet_time_obj *t = Z_BACNET_TIME_P(tv);
    php_bacnet_value_obj *obj = bacnet_value_alloc(return_value);
    obj->appdata.tag = BACNET_APPLICATION_TAG_TIME;
    obj->appdata.type.Time.hour        = t->hour;
    obj->appdata.type.Time.min         = t->minute;
    obj->appdata.type.Time.sec         = t->second;
    obj->appdata.type.Time.hundredths  = t->hundredths;
}

/* Value::objectIdentifier(ObjectIdentifier $v): self */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_value_object_identifier, 0, 1, Bacnet\\Value, 0)
    ZEND_ARG_OBJ_INFO(0, value, Bacnet\\ObjectIdentifier, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Value, objectIdentifier)
{
    zval *oidv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(oidv, bacnet_ce_object_identifier)
    ZEND_PARSE_PARAMETERS_END();
    php_bacnet_object_identifier_obj *oid = Z_BACNET_OID_P(oidv);
    php_bacnet_value_obj *obj = bacnet_value_alloc(return_value);
    obj->appdata.tag = BACNET_APPLICATION_TAG_OBJECT_ID;
    obj->appdata.type.Object_Id.type     = oid->object_type;
    obj->appdata.type.Object_Id.instance = oid->instance;
}

static const zend_function_entry bacnet_value_methods[] = {
    PHP_ME(Bacnet_Value, boolean,          arginfo_value_boolean,          ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Bacnet_Value, unsignedInt,      arginfo_value_unsigned_int,     ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Bacnet_Value, signedInt,        arginfo_value_signed_int,       ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Bacnet_Value, real,             arginfo_value_real,             ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Bacnet_Value, enumerated,       arginfo_value_enumerated,       ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Bacnet_Value, characterString,  arginfo_value_character_string, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Bacnet_Value, bitString,        arginfo_value_bit_string,       ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Bacnet_Value, date,             arginfo_value_date,             ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Bacnet_Value, time,             arginfo_value_time,             ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Bacnet_Value, objectIdentifier, arginfo_value_object_identifier,ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};

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
        efree(err_msg);
        RETURN_THROWS();
    }
    /* err_msg is NULL on success — efree(NULL) is a no-op */
    efree(err_msg);
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

/* writeProperty(ObjectType, int, Property, Value, int $priority=16, ?int $arrayIndex=null): void */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_device_write_property, 0, 4, IS_VOID, 0)
    ZEND_ARG_OBJ_INFO(0, objectType, Bacnet\\ObjectType, 0)
    ZEND_ARG_TYPE_INFO(0, instance,  IS_LONG,           0)
    ZEND_ARG_OBJ_INFO(0, property,   Bacnet\\Property,  0)
    ZEND_ARG_OBJ_INFO(0, value,      Bacnet\\Value,     0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, priority,   IS_LONG, 0, "16")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, arrayIndex, IS_LONG, 1, "null")
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Device, writeProperty)
{
    zval     *ot_enum, *prop_enum, *value_zv;
    zend_long instance, priority = 16, array_index = 0;
    bool      aidx_null = true;

    ZEND_PARSE_PARAMETERS_START(4, 6)
        Z_PARAM_OBJECT_OF_CLASS(ot_enum,   bacnet_ce_object_type_enum)
        Z_PARAM_LONG(instance)
        Z_PARAM_OBJECT_OF_CLASS(prop_enum, bacnet_ce_property_enum)
        Z_PARAM_OBJECT_OF_CLASS(value_zv,  bacnet_ce_value)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(priority)
        Z_PARAM_LONG_OR_NULL(array_index, aidx_null)
    ZEND_PARSE_PARAMETERS_END();

    php_bacnet_device_obj *dev = Z_BACNET_DEVICE_P(ZEND_THIS);
    if (Z_TYPE(dev->client_zval) != IS_OBJECT) {
        zend_throw_exception(bacnet_ce_exception, "Device has no associated client", 0);
        RETURN_THROWS();
    }
    php_bacnet_client_obj *cl = Z_BACNET_CLIENT_P(&dev->client_zval);
    if (!cl->client) {
        zend_throw_exception(bacnet_ce_exception, "BACnet client not initialized", 0);
        RETURN_THROWS();
    }

    zval *ot_b = zend_enum_fetch_case_value(Z_OBJ_P(ot_enum));
    zval *pr_b = zend_enum_fetch_case_value(Z_OBJ_P(prop_enum));

    bacnet_exec_write_property(
        cl->client, &dev->address,
        (BACNET_OBJECT_TYPE)Z_LVAL_P(ot_b),
        (uint32_t)instance,
        (BACNET_PROPERTY_ID)Z_LVAL_P(pr_b),
        aidx_null ? BACNET_ARRAY_ALL : (uint32_t)array_index,
        (uint8_t)(priority < 1 ? 1 : priority > 16 ? 16 : priority),
        value_zv,
        (uint32_t)BACNET_G(default_timeout_ms));
}

static const zend_function_entry bacnet_device_methods[] = {
    PHP_ME(Bacnet_Device, getDeviceId,    arginfo_bacnet_device_get_device_id,    ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_Device, getAddress,     arginfo_bacnet_device_get_address,      ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_Device, getMaxApdu,     arginfo_bacnet_device_get_max_apdu,     ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_Device, getVendorId,    arginfo_bacnet_device_get_vendor_id,    ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_Device, readProperty,   arginfo_bacnet_device_read_property,    ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_Device, writeProperty,  arginfo_bacnet_device_write_property,   ZEND_ACC_PUBLIC)
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

/* writeProperty(Property, Value, int $priority=16, ?int $arrayIndex=null): void */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_objectref_write_property, 0, 2, IS_VOID, 0)
    ZEND_ARG_OBJ_INFO(0, property,   Bacnet\\Property, 0)
    ZEND_ARG_OBJ_INFO(0, value,      Bacnet\\Value,    0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, priority,   IS_LONG, 0, "16")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, arrayIndex, IS_LONG, 1, "null")
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ObjectRef, writeProperty)
{
    zval     *prop_enum, *value_zv;
    zend_long priority = 16, array_index = 0;
    bool      aidx_null = true;

    ZEND_PARSE_PARAMETERS_START(2, 4)
        Z_PARAM_OBJECT_OF_CLASS(prop_enum, bacnet_ce_property_enum)
        Z_PARAM_OBJECT_OF_CLASS(value_zv,  bacnet_ce_value)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(priority)
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

    zval *pr_b = zend_enum_fetch_case_value(Z_OBJ_P(prop_enum));

    bacnet_exec_write_property(
        cl->client, &dev->address,
        ref->object_type, ref->instance,
        (BACNET_PROPERTY_ID)Z_LVAL_P(pr_b),
        aidx_null ? BACNET_ARRAY_ALL : (uint32_t)array_index,
        (uint8_t)(priority < 1 ? 1 : priority > 16 ? 16 : priority),
        value_zv,
        (uint32_t)BACNET_G(default_timeout_ms));
}

/* ── ObjectRef convenience methods ───────────────────────────────────────
 * Shared helper: extract validated php_bacnet_client * from ObjectRef $this.
 */
static php_bacnet_client *objectref_get_client(
    php_bacnet_objectref_obj  *ref,
    BACNET_ADDRESS            **addr_out)
{
    if (Z_TYPE(ref->device_zval) != IS_OBJECT) {
        zend_throw_exception(bacnet_ce_exception, "ObjectRef has no associated device", 0);
        return NULL;
    }
    php_bacnet_device_obj *dev = Z_BACNET_DEVICE_P(&ref->device_zval);
    if (Z_TYPE(dev->client_zval) != IS_OBJECT) {
        zend_throw_exception(bacnet_ce_exception, "Device has no associated client", 0);
        return NULL;
    }
    php_bacnet_client_obj *cl = Z_BACNET_CLIENT_P(&dev->client_zval);
    if (!cl->client) {
        zend_throw_exception(bacnet_ce_exception, "BACnet client not initialized", 0);
        return NULL;
    }
    *addr_out = &dev->address;
    return cl->client;
}

/* writeActive(): void — PRESENT_VALUE = enumerated(1) */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_objectref_write_active, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ObjectRef, writeActive)
{
    ZEND_PARSE_PARAMETERS_NONE();
    php_bacnet_objectref_obj *ref = Z_BACNET_OBJREF_P(ZEND_THIS);
    BACNET_ADDRESS *addr;
    php_bacnet_client *client = objectref_get_client(ref, &addr);
    if (!client) RETURN_THROWS();

    zval vz;
    object_init_ex(&vz, bacnet_ce_value);
    php_bacnet_value_obj *v = Z_BACNET_VALUE_P(&vz);
    v->appdata.tag = BACNET_APPLICATION_TAG_ENUMERATED;
    v->appdata.type.Enumerated = 1;

    bacnet_exec_write_property(client, addr,
        ref->object_type, ref->instance,
        PROP_PRESENT_VALUE, BACNET_ARRAY_ALL, 16, &vz,
        (uint32_t)BACNET_G(default_timeout_ms));
    zval_ptr_dtor(&vz);
}

/* writeInactive(): void — PRESENT_VALUE = enumerated(0) */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_objectref_write_inactive, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ObjectRef, writeInactive)
{
    ZEND_PARSE_PARAMETERS_NONE();
    php_bacnet_objectref_obj *ref = Z_BACNET_OBJREF_P(ZEND_THIS);
    BACNET_ADDRESS *addr;
    php_bacnet_client *client = objectref_get_client(ref, &addr);
    if (!client) RETURN_THROWS();

    zval vz;
    object_init_ex(&vz, bacnet_ce_value);
    php_bacnet_value_obj *v = Z_BACNET_VALUE_P(&vz);
    v->appdata.tag = BACNET_APPLICATION_TAG_ENUMERATED;
    v->appdata.type.Enumerated = 0;

    bacnet_exec_write_property(client, addr,
        ref->object_type, ref->instance,
        PROP_PRESENT_VALUE, BACNET_ARRAY_ALL, 16, &vz,
        (uint32_t)BACNET_G(default_timeout_ms));
    zval_ptr_dtor(&vz);
}

/* writePresentValue(mixed $value): void */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_objectref_write_present_value, 0, 1, IS_VOID, 0)
    ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ObjectRef, writePresentValue)
{
    zval *value_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_ZVAL(value_zv) ZEND_PARSE_PARAMETERS_END();

    php_bacnet_objectref_obj *ref = Z_BACNET_OBJREF_P(ZEND_THIS);
    BACNET_ADDRESS *addr;
    php_bacnet_client *client = objectref_get_client(ref, &addr);
    if (!client) RETURN_THROWS();

    /* Accept Bacnet\Value directly or native PHP type */
    zval encoded;
    bool needs_dtor = false;
    if (Z_TYPE_P(value_zv) == IS_OBJECT
        && instanceof_function(Z_OBJCE_P(value_zv), bacnet_ce_value)) {
        ZVAL_COPY_VALUE(&encoded, value_zv);
    } else {
        BACNET_APPLICATION_DATA_VALUE appdata;
        if (!zval_to_bacapp_value(value_zv, &appdata)) {
            zend_throw_exception(bacnet_ce_exception,
                "writePresentValue: unsupported value type", 0);
            RETURN_THROWS();
        }
        /*
         * PRESENT_VALUE type depends on object type:
         *   Binary*         → ENUMERATED  (0=inactive, 1=active)
         *   Analog*         → REAL        (cast int to float)
         *   Multi-state*    → UNSIGNED_INT
         *   Others          → keep auto-detected tag
         */
        uint32_t ot = ref->object_type;
        if (appdata.tag == BACNET_APPLICATION_TAG_UNSIGNED_INT ||
            appdata.tag == BACNET_APPLICATION_TAG_SIGNED_INT) {
            if (ot == OBJECT_BINARY_INPUT || ot == OBJECT_BINARY_OUTPUT ||
                ot == OBJECT_BINARY_VALUE) {
                appdata.tag = BACNET_APPLICATION_TAG_ENUMERATED;
                appdata.type.Enumerated = (uint32_t)appdata.type.Unsigned_Int;
            } else if (ot == OBJECT_ANALOG_INPUT || ot == OBJECT_ANALOG_OUTPUT ||
                       ot == OBJECT_ANALOG_VALUE) {
                appdata.tag = BACNET_APPLICATION_TAG_REAL;
                appdata.type.Real = (float)appdata.type.Unsigned_Int;
            }
        }
        object_init_ex(&encoded, bacnet_ce_value);
        php_bacnet_value_obj *vo = Z_BACNET_VALUE_P(&encoded);
        vo->appdata = appdata;
        needs_dtor = true;
    }

    bacnet_exec_write_property(client, addr,
        ref->object_type, ref->instance,
        PROP_PRESENT_VALUE, BACNET_ARRAY_ALL, 16, &encoded,
        (uint32_t)BACNET_G(default_timeout_ms));
    if (needs_dtor) zval_ptr_dtor(&encoded);
}

/* readTrendLog(): TrendLogRecord[] — reads LOG_BUFFER and wraps entries */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_objectref_read_trend_log, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ObjectRef, readTrendLog)
{
    ZEND_PARSE_PARAMETERS_NONE();
    php_bacnet_objectref_obj *ref = Z_BACNET_OBJREF_P(ZEND_THIS);
    BACNET_ADDRESS *addr;
    php_bacnet_client *client = objectref_get_client(ref, &addr);
    if (!client) RETURN_THROWS();

    zval raw;
    ZVAL_NULL(&raw);
    int rc = bacnet_exec_read_property(
        client, addr,
        ref->object_type, ref->instance,
        PROP_LOG_BUFFER, BACNET_ARRAY_ALL,
        (uint32_t)BACNET_G(default_timeout_ms), &raw);
    if (rc != 0) return;

    array_init(return_value);

    /* Wrap each decoded element in a TrendLogRecord */
    zval *entry;
    if (Z_TYPE(raw) == IS_ARRAY) {
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL(raw), entry) {
            zval rec;
            object_init_ex(&rec, bacnet_ce_trend_log_record);
            zend_update_property_null(bacnet_ce_trend_log_record,
                Z_OBJ(rec), "timestamp", sizeof("timestamp") - 1);
            zend_update_property(bacnet_ce_trend_log_record,
                Z_OBJ(rec), "value", sizeof("value") - 1, entry);
            zend_update_property_long(bacnet_ce_trend_log_record,
                Z_OBJ(rec), "statusFlags", sizeof("statusFlags") - 1, 0);
            add_next_index_zval(return_value, &rec);
        } ZEND_HASH_FOREACH_END();
    } else if (Z_TYPE(raw) != IS_NULL) {
        zval rec;
        object_init_ex(&rec, bacnet_ce_trend_log_record);
        zend_update_property_null(bacnet_ce_trend_log_record,
            Z_OBJ(rec), "timestamp", sizeof("timestamp") - 1);
        zend_update_property(bacnet_ce_trend_log_record,
            Z_OBJ(rec), "value", sizeof("value") - 1, &raw);
        zend_update_property_long(bacnet_ce_trend_log_record,
            Z_OBJ(rec), "statusFlags", sizeof("statusFlags") - 1, 0);
        add_next_index_zval(return_value, &rec);
    }
    zval_ptr_dtor(&raw);
}

/* readWeeklySchedule(): WeeklySchedule */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_objectref_read_weekly_schedule, 0, 0, Bacnet\\WeeklySchedule, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ObjectRef, readWeeklySchedule)
{
    ZEND_PARSE_PARAMETERS_NONE();
    php_bacnet_objectref_obj *ref = Z_BACNET_OBJREF_P(ZEND_THIS);
    BACNET_ADDRESS *addr;
    php_bacnet_client *client = objectref_get_client(ref, &addr);
    if (!client) RETURN_THROWS();

    /* WEEKLY_SCHEDULE is a BACnet sequence — the generic APDU decoder
     * won't parse it. We issue the read for completeness; if it fails
     * or returns null we still return an empty WeeklySchedule. */
    zval raw;
    ZVAL_NULL(&raw);
    bacnet_exec_read_property(
        client, addr,
        ref->object_type, ref->instance,
        PROP_WEEKLY_SCHEDULE, BACNET_ARRAY_ALL,
        (uint32_t)BACNET_G(default_timeout_ms), &raw);
    if (EG(exception)) { zval_ptr_dtor(&raw); return; }
    zval_ptr_dtor(&raw);

    /* Return an empty WeeklySchedule (full sequence decoding is TODO) */
    object_init_ex(return_value, bacnet_ce_weekly_schedule);
    static const char *day_names[] = {
        "monday","tuesday","wednesday","thursday","friday","saturday","sunday"
    };
    for (int i = 0; i < 7; i++) {
        zval empty_arr;
        array_init(&empty_arr);
        zend_update_property(bacnet_ce_weekly_schedule, Z_OBJ_P(return_value),
            day_names[i], strlen(day_names[i]), &empty_arr);
        zval_ptr_dtor(&empty_arr);
    }
}

/* writeWeeklySchedule(WeeklySchedule $schedule): void */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_objectref_write_weekly_schedule, 0, 1, IS_VOID, 0)
    ZEND_ARG_OBJ_INFO(0, schedule, Bacnet\\WeeklySchedule, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ObjectRef, writeWeeklySchedule)
{
    zval *schedule_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(schedule_zv, bacnet_ce_weekly_schedule)
    ZEND_PARSE_PARAMETERS_END();
    (void)schedule_zv;

    zend_throw_exception(bacnet_ce_exception,
        "writeWeeklySchedule: BACnet sequence encoding not yet implemented", 0);
    RETURN_THROWS();
}

static const zend_function_entry bacnet_objectref_methods[] = {
    PHP_ME(Bacnet_ObjectRef, __construct,        arginfo_bacnet_objectref_construct,        ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_ObjectRef, readProperty,       arginfo_bacnet_objectref_read_property,    ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_ObjectRef, writeProperty,      arginfo_bacnet_objectref_write_property,   ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_ObjectRef, writeActive,        arginfo_objectref_write_active,            ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_ObjectRef, writeInactive,      arginfo_objectref_write_inactive,          ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_ObjectRef, writePresentValue,  arginfo_objectref_write_present_value,     ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_ObjectRef, readTrendLog,       arginfo_objectref_read_trend_log,          ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_ObjectRef, readWeeklySchedule, arginfo_objectref_read_weekly_schedule,    ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_ObjectRef, writeWeeklySchedule,arginfo_objectref_write_weekly_schedule,   ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* ────────────────────────────────────────────────────────────────────── */
/*  Bacnet\ScheduleEntry — (Time, mixed) value pair for weekly schedules  */
/* ────────────────────────────────────────────────────────────────────── */

ZEND_BEGIN_ARG_INFO_EX(arginfo_schedule_entry_construct, 0, 0, 2)
    ZEND_ARG_OBJ_INFO(0, startTime, Bacnet\\Time, 0)
    ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ScheduleEntry, __construct)
{
    zval *time_zv, *value_zv;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(time_zv, bacnet_ce_time)
        Z_PARAM_ZVAL(value_zv)
    ZEND_PARSE_PARAMETERS_END();

    zend_update_property(bacnet_ce_schedule_entry, Z_OBJ_P(ZEND_THIS),
        "startTime", sizeof("startTime") - 1, time_zv);
    zend_update_property(bacnet_ce_schedule_entry, Z_OBJ_P(ZEND_THIS),
        "value", sizeof("value") - 1, value_zv);
}

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_schedule_entry_get_start_time, 0, 0, Bacnet\\Time, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ScheduleEntry, getStartTime)
{
    ZEND_PARSE_PARAMETERS_NONE();
    zval rv;
    zval *prop = zend_read_property(bacnet_ce_schedule_entry,
        Z_OBJ_P(ZEND_THIS), "startTime", sizeof("startTime") - 1, 0, &rv);
    ZVAL_COPY(return_value, prop);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_schedule_entry_get_value, 0, 0, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ScheduleEntry, getValue)
{
    ZEND_PARSE_PARAMETERS_NONE();
    zval rv;
    zval *prop = zend_read_property(bacnet_ce_schedule_entry,
        Z_OBJ_P(ZEND_THIS), "value", sizeof("value") - 1, 0, &rv);
    ZVAL_COPY(return_value, prop);
}

static const zend_function_entry bacnet_schedule_entry_methods[] = {
    PHP_ME(Bacnet_ScheduleEntry, __construct,  arginfo_schedule_entry_construct,       ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_ScheduleEntry, getStartTime, arginfo_schedule_entry_get_start_time,  ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_ScheduleEntry, getValue,     arginfo_schedule_entry_get_value,       ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* ────────────────────────────────────────────────────────────────────── */
/*  Bacnet\WeeklySchedule — 7-day schedule (array of ScheduleEntry/day)   */
/* ────────────────────────────────────────────────────────────────────── */

ZEND_BEGIN_ARG_INFO_EX(arginfo_weekly_schedule_construct, 0, 0, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, monday,    IS_ARRAY, 0, "[]")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, tuesday,   IS_ARRAY, 0, "[]")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, wednesday, IS_ARRAY, 0, "[]")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, thursday,  IS_ARRAY, 0, "[]")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, friday,    IS_ARRAY, 0, "[]")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, saturday,  IS_ARRAY, 0, "[]")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, sunday,    IS_ARRAY, 0, "[]")
ZEND_END_ARG_INFO()

static const char *s_day_names[7] = {
    "monday","tuesday","wednesday","thursday","friday","saturday","sunday"
};

PHP_METHOD(Bacnet_WeeklySchedule, __construct)
{
    zval *days[7];
    zval empty;
    array_init(&empty);
    for (int i = 0; i < 7; i++) days[i] = &empty;

    ZEND_PARSE_PARAMETERS_START(0, 7)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(days[0]) Z_PARAM_ARRAY(days[1]) Z_PARAM_ARRAY(days[2])
        Z_PARAM_ARRAY(days[3]) Z_PARAM_ARRAY(days[4]) Z_PARAM_ARRAY(days[5])
        Z_PARAM_ARRAY(days[6])
    ZEND_PARSE_PARAMETERS_END();

    for (int i = 0; i < 7; i++) {
        zend_update_property(bacnet_ce_weekly_schedule, Z_OBJ_P(ZEND_THIS),
            s_day_names[i], strlen(s_day_names[i]), days[i]);
    }
    zval_ptr_dtor(&empty);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_weekly_schedule_get_day, 0, 1, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, weekday, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_WeeklySchedule, getDay)
{
    zend_long weekday;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_LONG(weekday) ZEND_PARSE_PARAMETERS_END();

    if (weekday < 1 || weekday > 7) {
        zend_throw_exception_ex(bacnet_ce_exception, 0,
            "weekday must be 1 (Mon) to 7 (Sun), got %ld", (long)weekday);
        RETURN_THROWS();
    }
    const char *name = s_day_names[weekday - 1];
    zval rv;
    zval *prop = zend_read_property(bacnet_ce_weekly_schedule,
        Z_OBJ_P(ZEND_THIS), name, strlen(name), 0, &rv);
    ZVAL_COPY(return_value, prop);
}

static const zend_function_entry bacnet_weekly_schedule_methods[] = {
    PHP_ME(Bacnet_WeeklySchedule, __construct, arginfo_weekly_schedule_construct, ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_WeeklySchedule, getDay,      arginfo_weekly_schedule_get_day,   ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* ────────────────────────────────────────────────────────────────────── */
/*  Bacnet\TrendLogRecord — one entry from a TrendLog LOG_BUFFER          */
/* ────────────────────────────────────────────────────────────────────── */

ZEND_BEGIN_ARG_INFO_EX(arginfo_trend_log_record_construct, 0, 0, 3)
    ZEND_ARG_INFO(0, timestamp)
    ZEND_ARG_INFO(0, value)
    ZEND_ARG_TYPE_INFO(0, statusFlags, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_TrendLogRecord, __construct)
{
    zval *ts_zv, *value_zv;
    zend_long status_flags = 0;
    ZEND_PARSE_PARAMETERS_START(3, 3)
        Z_PARAM_ZVAL(ts_zv)
        Z_PARAM_ZVAL(value_zv)
        Z_PARAM_LONG(status_flags)
    ZEND_PARSE_PARAMETERS_END();

    zend_update_property(bacnet_ce_trend_log_record, Z_OBJ_P(ZEND_THIS),
        "timestamp", sizeof("timestamp") - 1, ts_zv);
    zend_update_property(bacnet_ce_trend_log_record, Z_OBJ_P(ZEND_THIS),
        "value", sizeof("value") - 1, value_zv);
    zend_update_property_long(bacnet_ce_trend_log_record, Z_OBJ_P(ZEND_THIS),
        "statusFlags", sizeof("statusFlags") - 1, status_flags);
}

static const zend_function_entry bacnet_trend_log_record_methods[] = {
    PHP_ME(Bacnet_TrendLogRecord, __construct, arginfo_trend_log_record_construct, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* ────────────────────────────────────────────────────────────────────── */
/*  Bacnet\Server — server / device mode with PHP callbacks              */
/* ────────────────────────────────────────────────────────────────────── */

static zend_object *php_bacnet_server_create_object(zend_class_entry *ce)
{
    php_bacnet_server_obj *obj =
        (php_bacnet_server_obj *)zend_object_alloc(sizeof(php_bacnet_server_obj), ce);
    obj->client           = NULL;
    obj->device_id        = 0;
    obj->auto_iam         = true;
    obj->read_handler_set = false;
    obj->write_handler_set= false;
    obj->local_objects    = NULL;
    ZVAL_UNDEF(&obj->read_handler_zv);
    ZVAL_UNDEF(&obj->write_handler_zv);
    zend_object_std_init(&obj->std, ce);
    object_properties_init(&obj->std, ce);
    obj->std.handlers = &php_bacnet_server_handlers;
    return &obj->std;
}

static void php_bacnet_server_free_object(zend_object *object)
{
    php_bacnet_server_obj *obj = php_bacnet_server_from_obj(object);
    if (obj->client) {
        php_bacnet_client_destroy(obj->client);
        obj->client = NULL;
        BACNET_G(client_initialized) = 0;
    }
    if (obj->local_objects) {
        zend_hash_destroy(obj->local_objects);
        efree(obj->local_objects);
        obj->local_objects = NULL;
    }
    if (obj->read_handler_set) {
        zval_ptr_dtor(&obj->read_handler_zv);
        obj->read_handler_set = false;
    }
    if (obj->write_handler_set) {
        zval_ptr_dtor(&obj->write_handler_zv);
        obj->write_handler_set = false;
    }
    zend_object_std_dtor(object);
}

/* __construct(int $deviceId, string $bindInterface='0.0.0.0', int $port=47808) */
ZEND_BEGIN_ARG_INFO_EX(arginfo_bacnet_server_construct, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, deviceId,      IS_LONG,   0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, bindInterface, IS_STRING, 0, "\"0.0.0.0\"")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, port,          IS_LONG,   0, "47808")
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Server, __construct)
{
    zend_long      device_id = 0;
    zend_string   *iface_str = NULL;
    zend_long      port      = 47808;

    ZEND_PARSE_PARAMETERS_START(1, 3)
        Z_PARAM_LONG(device_id)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR(iface_str)
        Z_PARAM_LONG(port)
    ZEND_PARSE_PARAMETERS_END();

    if (device_id < 0 || device_id > BACNET_MAX_INSTANCE) {
        zend_throw_exception_ex(bacnet_ce_exception, 0,
            "Invalid BACnet device ID %ld", (long)device_id);
        RETURN_THROWS();
    }
    if (BACNET_G(client_initialized)) {
        zend_throw_error(NULL,
            "Only one BACnet socket (Client or Server) may exist per PHP process");
        RETURN_THROWS();
    }

    php_bacnet_server_obj *srv = Z_BACNET_SERVER_P(ZEND_THIS);

    /* Allocate local_objects HashTable */
    srv->local_objects = (HashTable *)emalloc(sizeof(HashTable));
    zend_hash_init(srv->local_objects, 16, NULL, NULL, 0);

    const char *iface = (iface_str && ZSTR_LEN(iface_str) > 0
                        && strcmp(ZSTR_VAL(iface_str), "0.0.0.0") != 0)
                      ? ZSTR_VAL(iface_str) : NULL;

    char *err_msg = NULL;
    srv->client = php_bacnet_client_create(iface, (uint16_t)port, &err_msg);
    if (!srv->client) {
        char buf[256];
        snprintf(buf, sizeof(buf), "BACnet Server init failed: %s",
            err_msg ? err_msg : "unknown error");
        efree(err_msg);
        zend_hash_destroy(srv->local_objects);
        efree(srv->local_objects);
        srv->local_objects = NULL;
        zend_throw_exception(bacnet_ce_exception, buf, 0);
        RETURN_THROWS();
    }
    /* err_msg is NULL on success — efree(NULL) is a no-op */
    efree(err_msg);

    srv->device_id = (uint32_t)device_id;
    srv->auto_iam  = true;
    BACNET_G(client_initialized) = 1;
}

/* addLocalObject(ObjectIdentifier $oid): void */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_server_add_local_object, 0, 1, IS_VOID, 0)
    ZEND_ARG_OBJ_INFO(0, oid, Bacnet\\ObjectIdentifier, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Server, addLocalObject)
{
    zval *oid_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(oid_zv, bacnet_ce_object_identifier)
    ZEND_PARSE_PARAMETERS_END();

    php_bacnet_server_obj *srv = Z_BACNET_SERVER_P(ZEND_THIS);
    if (!srv->local_objects) return;

    php_bacnet_object_identifier_obj *oid = Z_BACNET_OID_P(oid_zv);
    zend_ulong key = ((zend_ulong)oid->object_type << 22) | (zend_ulong)oid->instance;
    zval tval;
    ZVAL_TRUE(&tval);
    zend_hash_index_update(srv->local_objects, key, &tval);
}

/* removeLocalObject(ObjectIdentifier $oid): void */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_server_remove_local_object, 0, 1, IS_VOID, 0)
    ZEND_ARG_OBJ_INFO(0, oid, Bacnet\\ObjectIdentifier, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Server, removeLocalObject)
{
    zval *oid_zv;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(oid_zv, bacnet_ce_object_identifier)
    ZEND_PARSE_PARAMETERS_END();

    php_bacnet_server_obj *srv = Z_BACNET_SERVER_P(ZEND_THIS);
    if (!srv->local_objects) return;

    php_bacnet_object_identifier_obj *oid = Z_BACNET_OID_P(oid_zv);
    zend_ulong key = ((zend_ulong)oid->object_type << 22) | (zend_ulong)oid->instance;
    zend_hash_index_del(srv->local_objects, key);
}

/* onReadProperty(callable $handler): void */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_server_on_read_property, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, handler, IS_CALLABLE, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Server, onReadProperty)
{
    zval *handler;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(handler)
    ZEND_PARSE_PARAMETERS_END();

    php_bacnet_server_obj *srv = Z_BACNET_SERVER_P(ZEND_THIS);
    if (srv->read_handler_set) {
        zval_ptr_dtor(&srv->read_handler_zv);
        srv->read_handler_set = false;
    }

    char *err_str = NULL;
    if (!zend_is_callable_ex(handler, NULL, 0, NULL, &srv->read_fcc, &err_str)) {
        zend_throw_exception_ex(bacnet_ce_exception, 0,
            "onReadProperty: not callable: %s", err_str ? err_str : "?");
        efree(err_str);
        return;
    }
    efree(err_str);

    ZVAL_COPY(&srv->read_handler_zv, handler);
    srv->read_handler_set = true;
}

/* onWriteProperty(callable $handler): void */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_server_on_write_property, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, handler, IS_CALLABLE, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Server, onWriteProperty)
{
    zval *handler;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(handler)
    ZEND_PARSE_PARAMETERS_END();

    php_bacnet_server_obj *srv = Z_BACNET_SERVER_P(ZEND_THIS);
    if (srv->write_handler_set) {
        zval_ptr_dtor(&srv->write_handler_zv);
        srv->write_handler_set = false;
    }

    char *err_str = NULL;
    if (!zend_is_callable_ex(handler, NULL, 0, NULL, &srv->write_fcc, &err_str)) {
        zend_throw_exception_ex(bacnet_ce_exception, 0,
            "onWriteProperty: not callable: %s", err_str ? err_str : "?");
        efree(err_str);
        return;
    }
    efree(err_str);

    ZVAL_COPY(&srv->write_handler_zv, handler);
    srv->write_handler_set = true;
}

/* setAutoIAm(bool $enabled): void */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_server_set_auto_iam, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, enabled, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Server, setAutoIAm)
{
    bool enabled;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_BOOL(enabled) ZEND_PARSE_PARAMETERS_END();
    Z_BACNET_SERVER_P(ZEND_THIS)->auto_iam = enabled;
}

/* ── poll() — process one pending PDU (non-blocking if timeoutMs=0) ──── */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_server_poll, 0, 0, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeoutMs, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Server, poll)
{
    zend_long timeout_ms_arg = 0;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(timeout_ms_arg)
    ZEND_PARSE_PARAMETERS_END();

    if (timeout_ms_arg < 0) timeout_ms_arg = 0;

    php_bacnet_server_obj *srv = Z_BACNET_SERVER_P(ZEND_THIS);
    if (!srv->client || !srv->client->initialized) {
        zend_throw_exception(bacnet_ce_exception, "Server not initialized", 0);
        RETURN_THROWS();
    }

    uint8_t pdu[MAX_APDU + MAX_NPDU];
    BACNET_ADDRESS src;
    memset(&src, 0, sizeof(src));

    uint16_t pdu_len = bip_receive(&src, pdu, (uint16_t)sizeof(pdu),
                                   (unsigned)timeout_ms_arg);
    if (pdu_len == 0) return;

    BACNET_ADDRESS npdu_dest, npdu_src_addr;
    BACNET_NPDU_DATA npdu_hdr;
    int npdu_len = bacnet_npdu_decode(pdu, pdu_len, &npdu_dest, &npdu_src_addr, &npdu_hdr);
    if (npdu_len < 0 || npdu_hdr.network_layer_message) return;

    uint8_t  *apdu     = pdu + npdu_len;
    uint16_t  apdu_len = pdu_len - (uint16_t)npdu_len;
    if (apdu_len < 2) return;

    uint8_t pdu_type = apdu[0] & 0xF0;

    /* ── Macro: send BACnet Error PDU back to src ─────────────────────── */
#define BACNET_SEND_ERROR(iid, svc, ec, code) do { \
    uint8_t _ea[32]; \
    int _el = bacerror_encode_apdu(_ea, (iid), (svc), (ec), (code)); \
    if (_el > 0) { \
        BACNET_NPDU_DATA _nd; \
        npdu_encode_npdu_data(&_nd, false, MESSAGE_PRIORITY_NORMAL); \
        bip_send_pdu(&src, &_nd, _ea, (uint16_t)_el); \
    } \
} while (0)

    /* ── Who-Is → I-Am ─────────────────────────────────────────────────── */
    if (pdu_type == PDU_TYPE_UNCONFIRMED_SERVICE_REQUEST) {
        if (apdu[1] == SERVICE_UNCONFIRMED_WHO_IS && srv->auto_iam) {
            int32_t low = -1, high = -1;
            if (apdu_len > 2) {
                whois_decode_service_request(apdu + 2, apdu_len - 2, &low, &high);
            }
            uint32_t did = srv->device_id;
            bool in_range = ((low < 0)  || ((int32_t)did >= low))
                         && ((high < 0) || ((int32_t)did <= high));
            if (in_range) {
                uint8_t iam_apdu[MAX_APDU];
                int iam_len = iam_encode_apdu(iam_apdu, srv->device_id,
                                              480, SEGMENTATION_NONE, 0);
                if (iam_len > 0) {
                    BACNET_NPDU_DATA resp_npdu;
                    npdu_encode_npdu_data(&resp_npdu, false, MESSAGE_PRIORITY_NORMAL);
                    BACNET_ADDRESS bcast;
                    memset(&bcast, 0, sizeof(bcast));
                    bcast.net = BACNET_BROADCAST_NETWORK;
                    bip_send_pdu(&bcast, &resp_npdu, iam_apdu, (uint16_t)iam_len);
                }
            }
        }
        goto poll_done;
    }

    /* ── Confirmed request ──────────────────────────────────────────────── */
    if (pdu_type == PDU_TYPE_CONFIRMED_SERVICE_REQUEST) {
        if (apdu_len < 4) goto poll_done;
        uint8_t invoke_id = apdu[2];
        uint8_t service   = apdu[3];

        /* ── ReadProperty ─────────────────────────────────────────────── */
        if (service == SERVICE_CONFIRMED_READ_PROPERTY) {
            BACNET_READ_PROPERTY_DATA rpdata;
            memset(&rpdata, 0, sizeof(rpdata));
            if (rp_decode_service_request(apdu + 4, apdu_len - 4, &rpdata) < 0)
                goto poll_done;

            zend_ulong obj_key = ((zend_ulong)rpdata.object_type << 22)
                               | (zend_ulong)rpdata.object_instance;
            if (!srv->local_objects
                || !zend_hash_index_exists(srv->local_objects, obj_key)
                || !srv->read_handler_set) {
                BACNET_SEND_ERROR(invoke_id, SERVICE_CONFIRMED_READ_PROPERTY,
                    ERROR_CLASS_OBJECT, ERROR_CODE_UNKNOWN_OBJECT);
                goto poll_done;
            }

            /* Build PHP args: (ObjectIdentifier, Property|int, ?int) */
            zval oid_zv, prop_zv, aidx_zv;
            php_bacnet_oid_from_c(rpdata.object_type, rpdata.object_instance, &oid_zv);

            zval prop_int_zv;
            ZVAL_LONG(&prop_int_zv, (zend_long)rpdata.object_property);
            ZVAL_UNDEF(&prop_zv);
            zend_call_method_with_1_params(NULL, bacnet_ce_property_enum,
                NULL, "tryfrom", &prop_zv, &prop_int_zv);
            if (EG(exception)) { zval_ptr_dtor(&oid_zv); goto poll_done; }
            if (Z_TYPE(prop_zv) != IS_OBJECT) {
                /* Unknown property: pass raw int */
                ZVAL_COPY_VALUE(&prop_zv, &prop_int_zv);
            }

            if (rpdata.array_index == BACNET_ARRAY_ALL) {
                ZVAL_NULL(&aidx_zv);
            } else {
                ZVAL_LONG(&aidx_zv, (zend_long)rpdata.array_index);
            }

            zval args[3], retval;
            ZVAL_COPY_VALUE(&args[0], &oid_zv);
            ZVAL_COPY_VALUE(&args[1], &prop_zv);
            ZVAL_COPY_VALUE(&args[2], &aidx_zv);
            ZVAL_UNDEF(&retval);

            zend_fcall_info fci;
            memset(&fci, 0, sizeof(fci));
            fci.size        = sizeof(fci);
            fci.retval      = &retval;
            fci.param_count = 3;
            fci.params      = args;

            int call_rc = zend_call_function(&fci, &srv->read_fcc);
            zval_ptr_dtor(&oid_zv);
            zval_ptr_dtor(&prop_zv);

            if (call_rc == FAILURE || EG(exception)) { goto poll_done; }

            /* Encode return value → BACNET_APPLICATION_DATA_VALUE */
            BACNET_APPLICATION_DATA_VALUE appval;
            if (!zval_to_bacapp_value(&retval, &appval)) {
                zval_ptr_dtor(&retval);
                BACNET_SEND_ERROR(invoke_id, SERVICE_CONFIRMED_READ_PROPERTY,
                    ERROR_CLASS_PROPERTY, ERROR_CODE_DATATYPE_NOT_SUPPORTED);
                goto poll_done;
            }
            zval_ptr_dtor(&retval);

            uint8_t app_buf[MAX_APDU];
            int app_len = bacapp_encode_application_data(app_buf, &appval);
            if (app_len <= 0) {
                BACNET_SEND_ERROR(invoke_id, SERVICE_CONFIRMED_READ_PROPERTY,
                    ERROR_CLASS_PROPERTY, ERROR_CODE_DATATYPE_NOT_SUPPORTED);
                goto poll_done;
            }

            rpdata.application_data     = app_buf;
            rpdata.application_data_len = app_len;

            uint8_t ack_apdu[MAX_APDU];
            int ack_len = rp_ack_encode_apdu(ack_apdu, invoke_id, &rpdata);
            if (ack_len > 0) {
                BACNET_NPDU_DATA resp_npdu;
                npdu_encode_npdu_data(&resp_npdu, false, MESSAGE_PRIORITY_NORMAL);
                bip_send_pdu(&src, &resp_npdu, ack_apdu, (uint16_t)ack_len);
            }

        /* ── WriteProperty ────────────────────────────────────────────── */
        } else if (service == SERVICE_CONFIRMED_WRITE_PROPERTY) {
            BACNET_WRITE_PROPERTY_DATA wpdata;
            memset(&wpdata, 0, sizeof(wpdata));
            if (wp_decode_service_request(apdu + 4, apdu_len - 4, &wpdata) < 0)
                goto poll_done;

            zend_ulong obj_key = ((zend_ulong)wpdata.object_type << 22)
                               | (zend_ulong)wpdata.object_instance;
            if (!srv->local_objects
                || !zend_hash_index_exists(srv->local_objects, obj_key)
                || !srv->write_handler_set) {
                BACNET_SEND_ERROR(invoke_id, SERVICE_CONFIRMED_WRITE_PROPERTY,
                    ERROR_CLASS_OBJECT, ERROR_CODE_UNKNOWN_OBJECT);
                goto poll_done;
            }

            /* Decode application_data → PHP value */
            zval value_zv;
            ZVAL_NULL(&value_zv);
            if (wpdata.application_data && wpdata.application_data_len > 0) {
                bacapp_values_to_zval(wpdata.application_data,
                                      (unsigned)wpdata.application_data_len,
                                      &value_zv);
            }

            /* Property enum via tryFrom */
            zval prop_int_zv, prop_zv;
            ZVAL_LONG(&prop_int_zv, (zend_long)wpdata.object_property);
            ZVAL_UNDEF(&prop_zv);
            zend_call_method_with_1_params(NULL, bacnet_ce_property_enum,
                NULL, "tryfrom", &prop_zv, &prop_int_zv);
            if (EG(exception)) { zval_ptr_dtor(&value_zv); goto poll_done; }
            if (!Z_OBJ_P(&prop_zv)) {
                ZVAL_COPY_VALUE(&prop_zv, &prop_int_zv);
            }

            zval oid_zv, aidx_zv;
            php_bacnet_oid_from_c(wpdata.object_type, wpdata.object_instance, &oid_zv);
            if (wpdata.array_index == BACNET_ARRAY_ALL) {
                ZVAL_NULL(&aidx_zv);
            } else {
                ZVAL_LONG(&aidx_zv, (zend_long)wpdata.array_index);
            }

            zval args[4], retval;
            ZVAL_COPY_VALUE(&args[0], &oid_zv);
            ZVAL_COPY_VALUE(&args[1], &prop_zv);
            ZVAL_COPY_VALUE(&args[2], &value_zv);
            ZVAL_COPY_VALUE(&args[3], &aidx_zv);
            ZVAL_UNDEF(&retval);

            zend_fcall_info fci;
            memset(&fci, 0, sizeof(fci));
            fci.size        = sizeof(fci);
            fci.retval      = &retval;
            fci.param_count = 4;
            fci.params      = args;

            int call_rc = zend_call_function(&fci, &srv->write_fcc);
            zval_ptr_dtor(&oid_zv);
            zval_ptr_dtor(&prop_zv);
            zval_ptr_dtor(&value_zv);
            zval_ptr_dtor(&retval);

            if (call_rc == FAILURE || EG(exception)) goto poll_done;

            /* Simple-ACK */
            uint8_t ack_apdu[3];
            ack_apdu[0] = PDU_TYPE_SIMPLE_ACK;
            ack_apdu[1] = invoke_id;
            ack_apdu[2] = SERVICE_CONFIRMED_WRITE_PROPERTY;
            BACNET_NPDU_DATA resp_npdu;
            npdu_encode_npdu_data(&resp_npdu, false, MESSAGE_PRIORITY_NORMAL);
            bip_send_pdu(&src, &resp_npdu, ack_apdu, 3);
        }
    }

poll_done:
#undef BACNET_SEND_ERROR
    return;
}

static const zend_function_entry bacnet_server_methods[] = {
    PHP_ME(Bacnet_Server, __construct,      arginfo_bacnet_server_construct,            ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_Server, addLocalObject,   arginfo_bacnet_server_add_local_object,     ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_Server, removeLocalObject,arginfo_bacnet_server_remove_local_object,  ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_Server, onReadProperty,   arginfo_bacnet_server_on_read_property,     ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_Server, onWriteProperty,  arginfo_bacnet_server_on_write_property,    ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_Server, setAutoIAm,       arginfo_bacnet_server_set_auto_iam,         ZEND_ACC_PUBLIC)
    PHP_ME(Bacnet_Server, poll,             arginfo_bacnet_server_poll,                 ZEND_ACC_PUBLIC)
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

    /* ── Bacnet\Value ────────────────────────────────────────────────── */

    memcpy(&php_bacnet_value_handlers,
           zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    php_bacnet_value_handlers.offset    = XtOffsetOf(php_bacnet_value_obj, std);
    php_bacnet_value_handlers.free_obj  = php_bacnet_value_free_object;
    php_bacnet_value_handlers.clone_obj = NULL;

    INIT_CLASS_ENTRY(ce, "Bacnet\\Value", bacnet_value_methods);
    bacnet_ce_value = zend_register_internal_class(&ce);
    bacnet_ce_value->create_object = php_bacnet_value_create_object;

    /* ── Bacnet\Server ──────────────────────────────────────────────── */

    memcpy(&php_bacnet_server_handlers,
           zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    php_bacnet_server_handlers.offset    = XtOffsetOf(php_bacnet_server_obj, std);
    php_bacnet_server_handlers.free_obj  = php_bacnet_server_free_object;
    php_bacnet_server_handlers.clone_obj = NULL;

    INIT_CLASS_ENTRY(ce, "Bacnet\\Server", bacnet_server_methods);
    bacnet_ce_server = zend_register_internal_class(&ce);
    bacnet_ce_server->create_object = php_bacnet_server_create_object;

    /* ── Bacnet\ScheduleEntry ─────────────────────────────────────── */

    INIT_CLASS_ENTRY(ce, "Bacnet\\ScheduleEntry", bacnet_schedule_entry_methods);
    bacnet_ce_schedule_entry = zend_register_internal_class(&ce);
    zend_declare_property_null(bacnet_ce_schedule_entry,
        "startTime", sizeof("startTime") - 1, ZEND_ACC_PUBLIC);
    zend_declare_property_null(bacnet_ce_schedule_entry,
        "value", sizeof("value") - 1, ZEND_ACC_PUBLIC);

    /* ── Bacnet\WeeklySchedule ───────────────────────────────────── */

    INIT_CLASS_ENTRY(ce, "Bacnet\\WeeklySchedule", bacnet_weekly_schedule_methods);
    bacnet_ce_weekly_schedule = zend_register_internal_class(&ce);
    for (int i = 0; i < 7; i++) {
        zend_declare_property_null(bacnet_ce_weekly_schedule,
            s_day_names[i], strlen(s_day_names[i]), ZEND_ACC_PUBLIC);
    }

    /* ── Bacnet\TrendLogRecord ───────────────────────────────────── */

    INIT_CLASS_ENTRY(ce, "Bacnet\\TrendLogRecord", bacnet_trend_log_record_methods);
    bacnet_ce_trend_log_record = zend_register_internal_class(&ce);
    zend_declare_property_null(bacnet_ce_trend_log_record,
        "timestamp", sizeof("timestamp") - 1, ZEND_ACC_PUBLIC);
    zend_declare_property_null(bacnet_ce_trend_log_record,
        "value", sizeof("value") - 1, ZEND_ACC_PUBLIC);
    zend_declare_property_long(bacnet_ce_trend_log_record,
        "statusFlags", sizeof("statusFlags") - 1, 0, ZEND_ACC_PUBLIC);
}
