#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include "php.h"
#include "zend_enum.h"

#ifndef BACDL_BIP
#define BACDL_BIP
#endif
#include "bacnet/bacdef.h"
#include "bacnet/bacenum.h"
#include "bacnet/bacapp.h"
#include "bacnet/bacstr.h"
#include "bacnet/datetime.h"

#include "bacnet/bacapp.h"

#include "../php_bacnet.h"
#include "bacnet_types.h"
#include "bacnet_helpers.h"

/* ── ObjectType enum case lookup ─────────────────────────────────────────
 * Maps C BACNET_OBJECT_TYPE values to PHP enum case names.
 * Only includes types registered in Bacnet\ObjectType.
 */
static const struct {
    BACNET_OBJECT_TYPE type;
    const char *name;
} s_ot_map[] = {
    { OBJECT_ANALOG_INPUT,       "ANALOG_INPUT"       },
    { OBJECT_ANALOG_OUTPUT,      "ANALOG_OUTPUT"      },
    { OBJECT_ANALOG_VALUE,       "ANALOG_VALUE"       },
    { OBJECT_BINARY_INPUT,       "BINARY_INPUT"       },
    { OBJECT_BINARY_OUTPUT,      "BINARY_OUTPUT"      },
    { OBJECT_BINARY_VALUE,       "BINARY_VALUE"       },
    { OBJECT_DEVICE,             "DEVICE"             },
    { OBJECT_EVENT_ENROLLMENT,   "EVENT_ENROLLMENT"   },
    { OBJECT_MULTI_STATE_INPUT,  "MULTI_STATE_INPUT"  },
    { OBJECT_MULTI_STATE_OUTPUT, "MULTI_STATE_OUTPUT" },
    { OBJECT_NOTIFICATION_CLASS, "NOTIFICATION_CLASS" },
    { OBJECT_SCHEDULE,           "SCHEDULE"           },
    { OBJECT_MULTI_STATE_VALUE,  "MULTI_STATE_VALUE"  },
    { OBJECT_TRENDLOG,           "TREND_LOG"          },
    { MAX_BACNET_OBJECT_TYPE, NULL }
};

/* Find an ObjectType enum case by backing integer value.
 * Returns true and sets *out on success; leaves *out UNDEF on failure. */
static bool find_ot_case(BACNET_OBJECT_TYPE type, zval *out)
{
    ZVAL_UNDEF(out);
    for (int i = 0; s_ot_map[i].name != NULL; i++) {
        if (s_ot_map[i].type == type) {
            zend_object *obj =
                zend_enum_get_case_cstr(bacnet_ce_object_type_enum, s_ot_map[i].name);
            if (obj) {
                ZVAL_OBJ_COPY(out, obj);
                return true;
            }
            return false;
        }
    }
    return false;
}

/* ── php_bacnet_oid_from_c ───────────────────────────────────────────────
 * Create a Bacnet\ObjectIdentifier from raw C values.
 * If the type is not in the PHP enum, the 'type' property is set to null.
 */
void php_bacnet_oid_from_c(BACNET_OBJECT_TYPE type, uint32_t instance, zval *zv)
{
    object_init_ex(zv, bacnet_ce_object_identifier);
    php_bacnet_object_identifier_obj *obj = Z_BACNET_OID_P(zv);
    obj->object_type = type;
    obj->instance    = instance;

    zval type_zv;
    if (find_ot_case(type, &type_zv)) {
        ZVAL_COPY_VALUE(&obj->type_zval, &type_zv);
        zend_update_property(bacnet_ce_object_identifier,
            Z_OBJ_P(zv), "type", sizeof("type") - 1, &type_zv);
    } else {
        ZVAL_UNDEF(&obj->type_zval);
        zend_update_property_null(bacnet_ce_object_identifier,
            Z_OBJ_P(zv), "type", sizeof("type") - 1);
    }
    zend_update_property_long(bacnet_ce_object_identifier,
        Z_OBJ_P(zv), "instance", sizeof("instance") - 1, (zend_long)instance);
}

/* ── php_bacnet_bitstring_new ────────────────────────────────────────────
 * Wrap a BACNET_BIT_STRING into a PHP Bacnet\BitString.
 */
void php_bacnet_bitstring_new(BACNET_BIT_STRING *bs, zval *zv)
{
    object_init_ex(zv, bacnet_ce_bit_string);
    php_bacnet_bitstring_obj *obj = Z_BACNET_BITSTRING_P(zv);
    obj->bits_used = bitstring_bits_used(bs);
    memcpy(obj->value, bs->value,
           obj->bits_used > 0
               ? (size_t)((obj->bits_used + 7) / 8)
               : 0);
}

/* ── php_bacnet_date_new ─────────────────────────────────────────────────
 */
void php_bacnet_date_new(BACNET_DATE *d, zval *zv)
{
    object_init_ex(zv, bacnet_ce_date);
    php_bacnet_date_obj *obj = Z_BACNET_DATE_P(zv);
    obj->year    = d->year;
    obj->month   = d->month;
    obj->day     = d->day;
    obj->weekday = d->wday;

    zend_update_property_long(bacnet_ce_date, Z_OBJ_P(zv),
        "year",    sizeof("year")    - 1, (zend_long)d->year);
    zend_update_property_long(bacnet_ce_date, Z_OBJ_P(zv),
        "month",   sizeof("month")   - 1, (zend_long)d->month);
    zend_update_property_long(bacnet_ce_date, Z_OBJ_P(zv),
        "day",     sizeof("day")     - 1, (zend_long)d->day);
    zend_update_property_long(bacnet_ce_date, Z_OBJ_P(zv),
        "weekday", sizeof("weekday") - 1, (zend_long)d->wday);
}

/* ── php_bacnet_time_new ─────────────────────────────────────────────────
 */
void php_bacnet_time_new(BACNET_TIME *t, zval *zv)
{
    object_init_ex(zv, bacnet_ce_time);
    php_bacnet_time_obj *obj = Z_BACNET_TIME_P(zv);
    obj->hour        = t->hour;
    obj->minute      = t->min;
    obj->second      = t->sec;
    obj->hundredths  = t->hundredths;

    zend_update_property_long(bacnet_ce_time, Z_OBJ_P(zv),
        "hour",       sizeof("hour")       - 1, (zend_long)t->hour);
    zend_update_property_long(bacnet_ce_time, Z_OBJ_P(zv),
        "minute",     sizeof("minute")     - 1, (zend_long)t->min);
    zend_update_property_long(bacnet_ce_time, Z_OBJ_P(zv),
        "second",     sizeof("second")     - 1, (zend_long)t->sec);
    zend_update_property_long(bacnet_ce_time, Z_OBJ_P(zv),
        "hundredths", sizeof("hundredths") - 1, (zend_long)t->hundredths);
}

/* ── bacapp_value_to_zval ────────────────────────────────────────────────
 * Map one decoded BACnet application-data value to a PHP zval.
 */
void bacapp_value_to_zval(BACNET_APPLICATION_DATA_VALUE *val, zval *zv)
{
    ZVAL_NULL(zv);

    switch (val->tag) {
        case BACNET_APPLICATION_TAG_NULL:
            /* ZVAL_NULL already set */
            break;

        case BACNET_APPLICATION_TAG_BOOLEAN:
            ZVAL_BOOL(zv, val->type.Boolean ? 1 : 0);
            break;

        case BACNET_APPLICATION_TAG_UNSIGNED_INT:
            ZVAL_LONG(zv, (zend_long)val->type.Unsigned_Int);
            break;

        case BACNET_APPLICATION_TAG_SIGNED_INT:
            ZVAL_LONG(zv, (zend_long)val->type.Signed_Int);
            break;

        case BACNET_APPLICATION_TAG_REAL:
            ZVAL_DOUBLE(zv, (double)val->type.Real);
            break;

        case BACNET_APPLICATION_TAG_DOUBLE:
            ZVAL_DOUBLE(zv, val->type.Double);
            break;

        case BACNET_APPLICATION_TAG_ENUMERATED:
            ZVAL_LONG(zv, (zend_long)val->type.Enumerated);
            break;

        case BACNET_APPLICATION_TAG_CHARACTER_STRING: {
            char   *s = characterstring_value(&val->type.Character_String);
            size_t  l = characterstring_length(&val->type.Character_String);
            ZVAL_STRINGL(zv, s ? s : "", s ? l : 0);
            break;
        }

        case BACNET_APPLICATION_TAG_OCTET_STRING: {
            uint8_t *s = octetstring_value(&val->type.Octet_String);
            size_t   l = octetstring_length(&val->type.Octet_String);
            ZVAL_STRINGL(zv, (char *)(s ? s : (uint8_t *)""), s ? l : 0);
            break;
        }

        case BACNET_APPLICATION_TAG_BIT_STRING:
            php_bacnet_bitstring_new(&val->type.Bit_String, zv);
            break;

        case BACNET_APPLICATION_TAG_DATE:
            php_bacnet_date_new(&val->type.Date, zv);
            break;

        case BACNET_APPLICATION_TAG_TIME:
            php_bacnet_time_new(&val->type.Time, zv);
            break;

        case BACNET_APPLICATION_TAG_OBJECT_ID:
            php_bacnet_oid_from_c(
                val->type.Object_Id.type,
                val->type.Object_Id.instance,
                zv);
            break;

        default:
            /* Unknown/complex tag → null */
            break;
    }
}

/* ── bacapp_values_to_zval ───────────────────────────────────────────────
 * Decode all application-data values from apdu[0..apdu_len-1].
 * Single value → scalar.  Multiple → IS_ARRAY.
 */
/* ── php_bacnet_value_encode ─────────────────────────────────────────────
 * Encode the BACNET_APPLICATION_DATA_VALUE stored in a Bacnet\Value PHP object
 * into apdu[].  Returns bytes written (>0) or -1 on type mismatch.
 */
int php_bacnet_value_encode(zval *value_zv, uint8_t *apdu, int apdu_size)
{
    if (Z_TYPE_P(value_zv) != IS_OBJECT ||
        !instanceof_function(Z_OBJCE_P(value_zv), bacnet_ce_value)) {
        return -1;
    }
    php_bacnet_value_obj *val = Z_BACNET_VALUE_P(value_zv);
    int n = bacapp_encode_application_data(apdu, &val->appdata);
    return (n > 0 && n <= apdu_size) ? n : -1;
}

/* ── zval_to_bacapp_value ────────────────────────────────────────────────
 * Convert a PHP value to a BACNET_APPLICATION_DATA_VALUE for server-side
 * ReadProperty ACK encoding.  Returns true on success.
 */
bool zval_to_bacapp_value(zval *zv, BACNET_APPLICATION_DATA_VALUE *val)
{
    memset(val, 0, sizeof(*val));

    switch (Z_TYPE_P(zv)) {
        case IS_NULL:
            val->tag = BACNET_APPLICATION_TAG_NULL;
            return true;

        case IS_TRUE:
            val->tag = BACNET_APPLICATION_TAG_BOOLEAN;
            val->type.Boolean = true;
            return true;

        case IS_FALSE:
            val->tag = BACNET_APPLICATION_TAG_BOOLEAN;
            val->type.Boolean = false;
            return true;

        case IS_LONG:
            if (Z_LVAL_P(zv) >= 0) {
                val->tag = BACNET_APPLICATION_TAG_UNSIGNED_INT;
                val->type.Unsigned_Int = (BACNET_UNSIGNED_INTEGER)Z_LVAL_P(zv);
            } else {
                val->tag = BACNET_APPLICATION_TAG_SIGNED_INT;
                val->type.Signed_Int = (int32_t)Z_LVAL_P(zv);
            }
            return true;

        case IS_DOUBLE:
            val->tag = BACNET_APPLICATION_TAG_REAL;
            val->type.Real = (float)Z_DVAL_P(zv);
            return true;

        case IS_STRING:
            val->tag = BACNET_APPLICATION_TAG_CHARACTER_STRING;
            characterstring_init_ansi(&val->type.Character_String, Z_STRVAL_P(zv));
            return true;

        case IS_OBJECT: {
            zend_class_entry *ce = Z_OBJCE_P(zv);
            if (instanceof_function(ce, bacnet_ce_value)) {
                php_bacnet_value_obj *v = Z_BACNET_VALUE_P(zv);
                *val = v->appdata;
                return true;
            }
            if (instanceof_function(ce, bacnet_ce_bit_string)) {
                php_bacnet_bitstring_obj *bs = Z_BACNET_BITSTRING_P(zv);
                val->tag = BACNET_APPLICATION_TAG_BIT_STRING;
                val->type.Bit_String.bits_used = bs->bits_used;
                memcpy(val->type.Bit_String.value, bs->value, sizeof(bs->value));
                return true;
            }
            if (instanceof_function(ce, bacnet_ce_date)) {
                php_bacnet_date_obj *d = Z_BACNET_DATE_P(zv);
                val->tag = BACNET_APPLICATION_TAG_DATE;
                val->type.Date.year  = d->year;
                val->type.Date.month = d->month;
                val->type.Date.day   = d->day;
                val->type.Date.wday  = d->weekday;
                return true;
            }
            if (instanceof_function(ce, bacnet_ce_time)) {
                php_bacnet_time_obj *t = Z_BACNET_TIME_P(zv);
                val->tag = BACNET_APPLICATION_TAG_TIME;
                val->type.Time.hour       = t->hour;
                val->type.Time.min        = t->minute;
                val->type.Time.sec        = t->second;
                val->type.Time.hundredths = t->hundredths;
                return true;
            }
            if (instanceof_function(ce, bacnet_ce_object_identifier)) {
                php_bacnet_object_identifier_obj *oid = Z_BACNET_OID_P(zv);
                val->tag = BACNET_APPLICATION_TAG_OBJECT_ID;
                val->type.Object_Id.type     = oid->object_type;
                val->type.Object_Id.instance = oid->instance;
                return true;
            }
            return false;
        }

        default:
            return false;
    }
}

void bacapp_values_to_zval(uint8_t *apdu, unsigned apdu_len, zval *out)
{
    zval arr;
    array_init(&arr);
    int count = 0;
    unsigned offset = 0;

    while (offset < apdu_len) {
        BACNET_APPLICATION_DATA_VALUE val;
        memset(&val, 0, sizeof(val));
        int n = bacapp_decode_application_data(
            apdu + offset, apdu_len - offset, &val);
        if (n <= 0) break;
        offset += (unsigned)n;

        zval item;
        ZVAL_NULL(&item);
        bacapp_value_to_zval(&val, &item);
        add_next_index_zval(&arr, &item);
        count++;
    }

    if (count == 1) {
        zval *first = zend_hash_index_find(Z_ARRVAL(arr), 0);
        if (first) {
            ZVAL_COPY(out, first);
        } else {
            ZVAL_NULL(out);
        }
        zval_ptr_dtor(&arr);
    } else if (count > 1) {
        ZVAL_COPY_VALUE(out, &arr);
    } else {
        zval_ptr_dtor(&arr);
        ZVAL_NULL(out);
    }
}
