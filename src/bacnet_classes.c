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
#include "bacnet/rpm.h"
#include "bacnet/wp.h"
#include "bacnet/abort.h"
#include "bacnet/reject.h"
#include "bacnet/bacapp.h"
#include "bacnet/bacerror.h"
#include "bacnet/bacstr.h"
#include "bacnet/datetime.h"
#include "bacnet/datalink/bip.h"
#include "bacnet/cov.h"

#include "../php_bacnet.h"
#include "bacnet_classes.h"
#include "bacnet_types.h"
#include "bacnet_client.h"
#include "bacnet_helpers.h"
#include "bacnet_security.h"
#include "bacnet_transport.h"
#include "bacnet_cache.h"

/* ── Class entry globals ─────────────────────────────────────────────── */

zend_class_entry *bacnet_ce_client = NULL;
zend_class_entry *bacnet_ce_device = NULL;
zend_class_entry *bacnet_ce_object_ref = NULL;
zend_class_entry *bacnet_ce_object_identifier = NULL;
zend_class_entry *bacnet_ce_bit_string = NULL;
zend_class_entry *bacnet_ce_date = NULL;
zend_class_entry *bacnet_ce_time = NULL;
zend_class_entry *bacnet_ce_value = NULL;
zend_class_entry *bacnet_ce_server = NULL;
zend_class_entry *bacnet_ce_mixed = NULL;
zend_class_entry *bacnet_ce_schedule_entry = NULL;
zend_class_entry *bacnet_ce_weekly_schedule = NULL;
zend_class_entry *bacnet_ce_trend_log_record = NULL;
zend_class_entry *bacnet_ce_object_type_enum = NULL;
zend_class_entry *bacnet_ce_property_enum = NULL;
zend_class_entry *bacnet_ce_exception = NULL;
zend_class_entry *bacnet_ce_timeout_exception = NULL;
zend_class_entry *bacnet_ce_device_exception = NULL;
zend_class_entry *bacnet_ce_cache_backend = NULL;

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

static bool php_bacnet_cov_dispatch(php_bacnet_client *client, const BACNET_ADDRESS *source,
									uint8_t *apdu, uint16_t apdu_len, bool confirmed);
static bool php_bacnet_server_send_apdu(BACNET_ADDRESS *dest, const uint8_t *apdu,
										uint16_t apdu_len);

static void php_bacnet_cov_handle_unsolicited(void *context, const BACNET_ADDRESS *source,
											  uint8_t *pdu, uint16_t pdu_len) {
	php_bacnet_client *client = (php_bacnet_client *)context;
	BACNET_ADDRESS dest;
	BACNET_ADDRESS npdu_source;
	BACNET_NPDU_DATA npdu;
	int offset = bacnet_npdu_decode(pdu, pdu_len, &dest, &npdu_source, &npdu);
	if (offset < 0 || npdu.network_layer_message || (uint16_t)offset + 2 > pdu_len) {
		return;
	}
	uint8_t *apdu = pdu + offset;
	uint16_t apdu_len = pdu_len - (uint16_t)offset;
	if ((apdu[0] & 0xF0) == PDU_TYPE_UNCONFIRMED_SERVICE_REQUEST &&
		apdu[1] == SERVICE_UNCONFIRMED_COV_NOTIFICATION) {
		(void)php_bacnet_cov_dispatch(client, source, apdu, apdu_len, false);
		return;
	}
	if ((apdu[0] & 0xF0) == PDU_TYPE_CONFIRMED_SERVICE_REQUEST && apdu_len >= 5 &&
		apdu[3] == SERVICE_CONFIRMED_COV_NOTIFICATION) {
		(void)php_bacnet_cov_dispatch(client, source, apdu, apdu_len, true);
		uint8_t ack[3];
		int ack_len = encode_simple_ack(ack, apdu[2], SERVICE_CONFIRMED_COV_NOTIFICATION);
		if (ack_len > 0) {
			(void)php_bacnet_server_send_apdu((BACNET_ADDRESS *)source, ack, (uint16_t)ack_len);
		}
	}
}

static php_bacnet_client *php_bacnet_transport_from_owner(zval *owner) {
	if (Z_TYPE_P(owner) != IS_OBJECT)
		return NULL;

	if (bacnet_ce_mixed && instanceof_function(Z_OBJCE_P(owner), bacnet_ce_mixed)) {
		return Z_BACNET_MIXED_P(owner)->client;
	}
	if (instanceof_function(Z_OBJCE_P(owner), bacnet_ce_client)) {
		return Z_BACNET_CLIENT_P(owner)->client;
	}
	return NULL;
}

static void php_bacnet_mixed_queue_unsolicited(void *context, const BACNET_ADDRESS *source,
											   uint8_t *pdu, uint16_t pdu_len) {
	php_bacnet_server_obj *srv = (php_bacnet_server_obj *)context;
	php_bacnet_packet_kind kind;
	if (!php_bacnet_security_accept(srv->security, source, pdu, pdu_len, &kind)) {
		return;
	}
	if (!php_bacnet_client_queue_pdu(srv->client, source, pdu, pdu_len)) {
		php_bacnet_security_queue_overflow(srv->security);
	}
}

static bool php_bacnet_cov_dispatch(php_bacnet_client *client, const BACNET_ADDRESS *source,
									uint8_t *apdu, uint16_t apdu_len, bool confirmed) {
	if (!client || !client->cov_handler_set || apdu_len < (confirmed ? 5 : 3))
		return false;
	BACNET_PROPERTY_VALUE values[8];
	BACNET_COV_DATA data;
	memset(&data, 0, sizeof(data));
	bacapp_property_value_list_init(values, 8);
	data.listOfValues = values;
	unsigned offset = confirmed ? 4 : 2;
	if (cov_notify_decode_service_request(apdu + offset, apdu_len - offset, &data) <= 0)
		return false;
	zval event, properties;
	array_init(&event);
	array_init(&properties);
	add_assoc_long(&event, "subscriberProcessId", (zend_long)data.subscriberProcessIdentifier);
	add_assoc_long(&event, "deviceId", (zend_long)data.initiatingDeviceIdentifier);
	add_assoc_long(&event, "objectType", (zend_long)data.monitoredObjectIdentifier.type);
	add_assoc_long(&event, "instance", (zend_long)data.monitoredObjectIdentifier.instance);
	add_assoc_long(&event, "timeRemaining", (zend_long)data.timeRemaining);
	for (BACNET_PROPERTY_VALUE *item = data.listOfValues; item; item = item->next) {
		zval property, value;
		array_init(&property);
		add_assoc_long(&property, "property", (zend_long)item->propertyIdentifier);
		add_assoc_long(&property, "arrayIndex", (zend_long)item->propertyArrayIndex);
		bacapp_value_to_zval(&item->value, &value);
		add_assoc_zval(&property, "value", &value);
		add_next_index_zval(&properties, &property);
	}
	add_assoc_zval(&event, "properties", &properties);
	zval retval;
	ZVAL_UNDEF(&retval);
	zend_call_known_function(client->cov_fcc.function_handler, client->cov_fcc.object,
							 client->cov_fcc.called_scope, &retval, 1, &event, NULL);
	zval_ptr_dtor(&event);
	if (!Z_ISUNDEF(retval))
		zval_ptr_dtor(&retval);
	return EG(exception) == NULL;
}

/* ── Shared ReadProperty logic ───────────────────────────────────────────
 * Called by both Device::readProperty and ObjectRef::readProperty.
 * Throws an exception on error.  Returns 0 on success.
 */
static int php_bacnet_exec_read_property(php_bacnet_client *client, BACNET_ADDRESS *dest,
										 BACNET_OBJECT_TYPE obj_type, uint32_t obj_inst,
										 BACNET_PROPERTY_ID prop_id,
										 uint32_t array_index, /* BACNET_ARRAY_ALL = no index */
										 uint32_t timeout_ms, zval *return_value, bool refresh) {
	char ip[16], cache_key[192], negative_key[208];
	uint16_t port = 0;
	php_bacnet_address_to_ipport(dest, ip, sizeof(ip), &port);
	snprintf(cache_key, sizeof(cache_key), "%s:%u|%u|%u|%u|%u", ip, port, (unsigned)obj_type,
			 (unsigned)obj_inst, (unsigned)prop_id, (unsigned)array_index);
	snprintf(negative_key, sizeof(negative_key), "read|%s", cache_key);
	php_bacnet_cache_partition partition = PHP_BACNET_CACHE_OBJECT;
	switch (prop_id) {
	case PROP_PRESENT_VALUE:
	case PROP_STATUS_FLAGS:
	case PROP_EVENT_STATE:
	case PROP_OUT_OF_SERVICE:
	case PROP_RELIABILITY:
	case PROP_PRIORITY_ARRAY:
	case PROP_RECORD_COUNT:
	case PROP_TOTAL_RECORD_COUNT:
		partition = PHP_BACNET_CACHE_STATE;
		break;
	case PROP_OBJECT_LIST:
		partition = PHP_BACNET_CACHE_OBJECT_LIST;
		break;
	case PROP_LOG_BUFFER:
	case PROP_WEEKLY_SCHEDULE:
	case PROP_EXCEPTION_SCHEDULE:
	case PROP_RECIPIENT_LIST:
	case PROP_EVENT_PARAMETERS:
		partition = PHP_BACNET_CACHE_PARTITION_COUNT;
		break;
	case PROP_OBJECT_IDENTIFIER:
	case PROP_OBJECT_NAME:
	case PROP_OBJECT_TYPE:
	case PROP_DESCRIPTION:
	case PROP_UNITS:
	case PROP_NUMBER_OF_STATES:
	case PROP_STATE_TEXT:
	case PROP_NOTIFICATION_CLASS:
	case PROP_ACK_REQUIRED:
	case PROP_NOTIFY_TYPE:
	case PROP_EVENT_TYPE:
	case PROP_OBJECT_PROPERTY_REFERENCE:
	case PROP_LOG_DEVICE_OBJECT_PROPERTY:
	case PROP_RELINQUISH_DEFAULT:
		partition = PHP_BACNET_CACHE_OBJECT;
		break;
	default:
		partition = PHP_BACNET_CACHE_PARTITION_COUNT;
		break;
	}
	if (refresh) {
		if (partition < PHP_BACNET_CACHE_PARTITION_COUNT)
			php_bacnet_cache_refresh(client->cache, partition, cache_key);
		php_bacnet_cache_refresh(client->cache, PHP_BACNET_CACHE_NEGATIVE, negative_key);
	} else {
		uint8_t marker[1];
		uint32_t marker_len = sizeof(marker);
		if (php_bacnet_cache_get(client->cache, PHP_BACNET_CACHE_NEGATIVE, negative_key, marker,
								 &marker_len)) {
			zend_throw_exception(bacnet_ce_timeout_exception,
								 "ReadProperty timed out (negative cache)", 0);
			return -1;
		}
		if (partition < PHP_BACNET_CACHE_PARTITION_COUNT) {
			uint8_t cached[MAX_APDU];
			uint32_t cached_len = sizeof(cached);
			if (php_bacnet_cache_get(client->cache, partition, cache_key, cached, &cached_len)) {
				bacapp_values_to_zval(cached, cached_len, return_value);
				return 0;
			}
		}
	}
	/* Build ReadProperty APDU */
	uint8_t invoke_id = BACNET_G(next_invoke_id)++;

	BACNET_READ_PROPERTY_DATA rpdata;
	memset(&rpdata, 0, sizeof(rpdata));
	rpdata.object_type = obj_type;
	rpdata.object_instance = obj_inst;
	rpdata.object_property = prop_id;
	rpdata.array_index = (BACNET_ARRAY_INDEX)array_index;

	uint8_t req_apdu[MAX_APDU];
	int req_len = rp_encode_apdu(req_apdu, invoke_id, &rpdata);
	if (req_len <= 0) {
		zend_throw_exception(bacnet_ce_exception, "Failed to encode ReadProperty APDU", 0);
		return -1;
	}

	/* Send and wait for response */
	uint8_t resp_apdu[MAX_APDU];
	uint16_t resp_len = 0;
	int status = php_bacnet_send_and_wait(client, dest, req_apdu, (uint16_t)req_len, invoke_id,
										  resp_apdu, &resp_len, timeout_ms);

	if (status == -2 && EG(exception)) {
		return -1;
	}

	if (status == -1) {
		uint8_t marker = 1;
		php_bacnet_cache_put(
			client->cache, PHP_BACNET_CACHE_NEGATIVE, negative_key, &marker, 1,
			php_bacnet_cache_partition_ttl(client->cache, PHP_BACNET_CACHE_NEGATIVE));
		zend_throw_exception_ex(bacnet_ce_timeout_exception, 0,
								"ReadProperty timed out after %u ms", (unsigned)timeout_ms);
		return -1;
	}

	if (status == (int)PDU_TYPE_ERROR) {
		if (resp_len < 3) {
			zend_throw_exception(bacnet_ce_device_exception, "BACnet error PDU (malformed)", 0);
			return -1;
		}
		uint8_t err_iid = 0;
		BACNET_CONFIRMED_SERVICE svc = SERVICE_CONFIRMED_READ_PROPERTY;
		BACNET_ERROR_CLASS ec = ERROR_CLASS_DEVICE;
		BACNET_ERROR_CODE code = ERROR_CODE_OTHER;
		/* Error PDU: [pdu_type][invoke_id][service][class][code] — pass from byte 1 */
		bacerror_decode_service_request(resp_apdu + 1, resp_len - 1, &err_iid, &svc, &ec, &code);

		zend_throw_exception_ex(bacnet_ce_device_exception, 0, "BACnet error: class=%d code=%d",
								(int)ec, (int)code);
		/* Patch errorClass / errorCode onto the thrown exception object */
		if (EG(exception)) {
			zend_update_property_long(bacnet_ce_device_exception, EG(exception), "errorClass",
									  strlen("errorClass"), (zend_long)ec);
			zend_update_property_long(bacnet_ce_device_exception, EG(exception), "errorCode",
									  strlen("errorCode"), (zend_long)code);
		}
		return -1;
	}

	if (status == (int)PDU_TYPE_REJECT || status == (int)PDU_TYPE_ABORT) {
		uint8_t reason = (resp_len > 2) ? resp_apdu[2] : 0;
		zend_throw_exception_ex(bacnet_ce_device_exception, 0, "BACnet %s PDU (reason %u)",
								(status == (int)PDU_TYPE_REJECT) ? "Reject" : "Abort",
								(unsigned)reason);
		return -1;
	}

	/* Decode Complex-ACK — bytes 0=type, 1=invoke_id, 2=service, 3+=data */
	if (resp_len < 4) {
		zend_throw_exception(bacnet_ce_exception, "Malformed ReadProperty-ACK (too short)", 0);
		return -1;
	}

	BACNET_READ_PROPERTY_DATA rp_ack;
	memset(&rp_ack, 0, sizeof(rp_ack));
	int decoded = rp_ack_decode_service_request(resp_apdu + 3, (int)(resp_len - 3), &rp_ack);
	if (decoded < 0 || !rp_ack.application_data || rp_ack.application_data_len <= 0) {
		zend_throw_exception(bacnet_ce_exception, "Failed to decode ReadProperty-ACK", 0);
		return -1;
	}

	bacapp_values_to_zval(rp_ack.application_data, (unsigned)rp_ack.application_data_len,
						  return_value);

	if (!EG(exception) && partition < PHP_BACNET_CACHE_PARTITION_COUNT) {
		php_bacnet_cache_put(client->cache, partition, cache_key, rp_ack.application_data,
							 (uint32_t)rp_ack.application_data_len,
							 php_bacnet_cache_partition_ttl(client->cache, partition));
	}

	return 0;
}

/* ── Shared WriteProperty logic ──────────────────────────────────────────
 * Throws on error, returns 0 on success (void PHP method).
 */
static int php_bacnet_exec_write_property(php_bacnet_client *client, BACNET_ADDRESS *dest,
										  BACNET_OBJECT_TYPE obj_type, uint32_t obj_inst,
										  BACNET_PROPERTY_ID prop_id, uint32_t array_index,
										  uint8_t priority, zval *value_zv, uint32_t timeout_ms) {
	php_bacnet_value_obj *val = Z_BACNET_VALUE_P(value_zv);

	/* Encode the application data into the WP struct's inline buffer */
	BACNET_WRITE_PROPERTY_DATA wpdata;
	memset(&wpdata, 0, sizeof(wpdata));
	wpdata.object_type = obj_type;
	wpdata.object_instance = obj_inst;
	wpdata.object_property = prop_id;
	wpdata.array_index = (BACNET_ARRAY_INDEX)array_index;
	wpdata.priority = priority;

	int enc_len = bacapp_encode_application_data(wpdata.application_data, &val->appdata);
	if (enc_len <= 0) {
		zend_throw_exception(bacnet_ce_exception, "Failed to encode WriteProperty application data",
							 0);
		return -1;
	}
	wpdata.application_data_len = enc_len;

	/* Build WriteProperty APDU */
	uint8_t invoke_id = BACNET_G(next_invoke_id)++;
	uint8_t req_apdu[MAX_APDU];
	int req_len = wp_encode_apdu(req_apdu, invoke_id, &wpdata);
	if (req_len <= 0) {
		zend_throw_exception(bacnet_ce_exception, "Failed to encode WriteProperty APDU", 0);
		return -1;
	}

	/* Send and wait — Simple-ACK on success */
	uint8_t resp_apdu[MAX_APDU];
	uint16_t resp_len = 0;
	int status = php_bacnet_send_and_wait(client, dest, req_apdu, (uint16_t)req_len, invoke_id,
										  resp_apdu, &resp_len, timeout_ms);

	if (status == -2 && EG(exception)) {
		return -1;
	}

	if (status == -1) {
		zend_throw_exception_ex(bacnet_ce_timeout_exception, 0,
								"WriteProperty timed out after %u ms", (unsigned)timeout_ms);
		return -1;
	}

	if (status == (int)PDU_TYPE_ERROR) {
		if (resp_len < 3) {
			zend_throw_exception(bacnet_ce_device_exception, "BACnet write error (malformed)", 0);
			return -1;
		}
		uint8_t err_iid = 0;
		BACNET_CONFIRMED_SERVICE svc = SERVICE_CONFIRMED_WRITE_PROPERTY;
		BACNET_ERROR_CLASS ec = ERROR_CLASS_DEVICE;
		BACNET_ERROR_CODE code = ERROR_CODE_OTHER;
		bacerror_decode_service_request(resp_apdu + 1, resp_len - 1, &err_iid, &svc, &ec, &code);

		zend_throw_exception_ex(bacnet_ce_device_exception, 0,
								"BACnet write error: class=%d code=%d", (int)ec, (int)code);
		if (EG(exception)) {
			zend_update_property_long(bacnet_ce_device_exception, EG(exception), "errorClass",
									  strlen("errorClass"), (zend_long)ec);
			zend_update_property_long(bacnet_ce_device_exception, EG(exception), "errorCode",
									  strlen("errorCode"), (zend_long)code);
		}
		return -1;
	}

	if (status == (int)PDU_TYPE_REJECT || status == (int)PDU_TYPE_ABORT) {
		uint8_t reason = (resp_len > 2) ? resp_apdu[2] : 0;
		zend_throw_exception_ex(
			bacnet_ce_device_exception, 0, "BACnet %s PDU during write (reason %u)",
			(status == (int)PDU_TYPE_REJECT) ? "Reject" : "Abort", (unsigned)reason);
		return -1;
	}

	char ip[16], scope[192];
	uint16_t port = 0;
	php_bacnet_address_to_ipport(dest, ip, sizeof(ip), &port);
	snprintf(scope, sizeof(scope), "%s:%u|%u|%u|%u", ip, port, (unsigned)obj_type,
			 (unsigned)obj_inst, (unsigned)prop_id);
	php_bacnet_cache_invalidate(client->cache, PHP_BACNET_CACHE_STATE, scope);
	php_bacnet_cache_invalidate(client->cache, PHP_BACNET_CACHE_OBJECT, scope);
	php_bacnet_cache_invalidate(client->cache, PHP_BACNET_CACHE_OBJECT_LIST, scope);
	php_bacnet_cache_invalidate(client->cache, PHP_BACNET_CACHE_NEGATIVE, scope);

	return 0; /* Simple-ACK → success */
}

/* ────────────────────────────────────────────────────────────────────── */
/*  Bacnet\Value — typed factory class                                    */
/* ────────────────────────────────────────────────────────────────────── */

static zend_object *php_bacnet_value_create_object(zend_class_entry *ce) {
	php_bacnet_value_obj *obj =
		(php_bacnet_value_obj *)zend_object_alloc(sizeof(php_bacnet_value_obj), ce);
	memset(&obj->appdata, 0, sizeof(obj->appdata));
	zend_object_std_init(&obj->std, ce);
	object_properties_init(&obj->std, ce);
	obj->std.handlers = &php_bacnet_value_handlers;
	return &obj->std;
}

static void php_bacnet_value_free_object(zend_object *object) {
	zend_object_std_dtor(object);
}

/* Helper: allocate new Value object, set tag and fill appdata, return it */
static php_bacnet_value_obj *php_bacnet_value_alloc(zval *out) {
	object_init_ex(out, bacnet_ce_value);
	return Z_BACNET_VALUE_P(out);
}

/* Value::boolean(bool $v): self */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_value_boolean, 0, 1, Bacnet\\Value, 0)
ZEND_ARG_TYPE_INFO(0, value, _IS_BOOL, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Value, boolean) {
	bool v;
	ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_BOOL(v) ZEND_PARSE_PARAMETERS_END();
	php_bacnet_value_obj *obj = php_bacnet_value_alloc(return_value);
	obj->appdata.tag = BACNET_APPLICATION_TAG_BOOLEAN;
	obj->appdata.type.Boolean = v;
}

/* Value::unsignedInt(int $v): self */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_value_unsigned_int, 0, 1, Bacnet\\Value, 0)
ZEND_ARG_TYPE_INFO(0, value, IS_LONG, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Value, unsignedInt) {
	zend_long v;
	ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_LONG(v) ZEND_PARSE_PARAMETERS_END();
	php_bacnet_value_obj *obj = php_bacnet_value_alloc(return_value);
	obj->appdata.tag = BACNET_APPLICATION_TAG_UNSIGNED_INT;
	obj->appdata.type.Unsigned_Int = (BACNET_UNSIGNED_INTEGER)(v < 0 ? 0 : (uint32_t)v);
}

/* Value::signedInt(int $v): self */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_value_signed_int, 0, 1, Bacnet\\Value, 0)
ZEND_ARG_TYPE_INFO(0, value, IS_LONG, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Value, signedInt) {
	zend_long v;
	ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_LONG(v) ZEND_PARSE_PARAMETERS_END();
	php_bacnet_value_obj *obj = php_bacnet_value_alloc(return_value);
	obj->appdata.tag = BACNET_APPLICATION_TAG_SIGNED_INT;
	obj->appdata.type.Signed_Int = (int32_t)v;
}

/* Value::real(float $v): self */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_value_real, 0, 1, Bacnet\\Value, 0)
ZEND_ARG_TYPE_INFO(0, value, IS_DOUBLE, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Value, real) {
	double v;
	ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_DOUBLE(v) ZEND_PARSE_PARAMETERS_END();
	php_bacnet_value_obj *obj = php_bacnet_value_alloc(return_value);
	obj->appdata.tag = BACNET_APPLICATION_TAG_REAL;
	obj->appdata.type.Real = (float)v;
}

/* Value::enumerated(int $v): self */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_value_enumerated, 0, 1, Bacnet\\Value, 0)
ZEND_ARG_TYPE_INFO(0, value, IS_LONG, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Value, enumerated) {
	zend_long v;
	ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_LONG(v) ZEND_PARSE_PARAMETERS_END();
	php_bacnet_value_obj *obj = php_bacnet_value_alloc(return_value);
	obj->appdata.tag = BACNET_APPLICATION_TAG_ENUMERATED;
	obj->appdata.type.Enumerated = (uint32_t)v;
}

/* Value::characterString(string $v): self */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_value_character_string, 0, 1, Bacnet\\Value, 0)
ZEND_ARG_TYPE_INFO(0, value, IS_STRING, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Value, characterString) {
	zend_string *v;
	ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_STR(v) ZEND_PARSE_PARAMETERS_END();
	php_bacnet_value_obj *obj = php_bacnet_value_alloc(return_value);
	obj->appdata.tag = BACNET_APPLICATION_TAG_CHARACTER_STRING;
	characterstring_init_ansi(&obj->appdata.type.Character_String, ZSTR_VAL(v));
}

/* Value::bitString(BitString $v): self */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_value_bit_string, 0, 1, Bacnet\\Value, 0)
ZEND_ARG_OBJ_INFO(0, value, Bacnet\\BitString, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Value, bitString) {
	zval *bsv;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_OBJECT_OF_CLASS(bsv, bacnet_ce_bit_string)
	ZEND_PARSE_PARAMETERS_END();
	php_bacnet_bitstring_obj *bs = Z_BACNET_BITSTRING_P(bsv);
	php_bacnet_value_obj *obj = php_bacnet_value_alloc(return_value);
	obj->appdata.tag = BACNET_APPLICATION_TAG_BIT_STRING;
	obj->appdata.type.Bit_String.bits_used = bs->bits_used;
	memcpy(obj->appdata.type.Bit_String.value, bs->value,
		   sizeof(obj->appdata.type.Bit_String.value));
}

/* Value::date(Date $v): self */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_value_date, 0, 1, Bacnet\\Value, 0)
ZEND_ARG_OBJ_INFO(0, value, Bacnet\\Date, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Value, date) {
	zval *dv;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_OBJECT_OF_CLASS(dv, bacnet_ce_date)
	ZEND_PARSE_PARAMETERS_END();
	php_bacnet_date_obj *d = Z_BACNET_DATE_P(dv);
	php_bacnet_value_obj *obj = php_bacnet_value_alloc(return_value);
	obj->appdata.tag = BACNET_APPLICATION_TAG_DATE;
	obj->appdata.type.Date.year = d->year;
	obj->appdata.type.Date.month = d->month;
	obj->appdata.type.Date.day = d->day;
	obj->appdata.type.Date.wday = d->weekday;
}

/* Value::time(Time $v): self */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_value_time, 0, 1, Bacnet\\Value, 0)
ZEND_ARG_OBJ_INFO(0, value, Bacnet\\Time, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Value, time) {
	zval *tv;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_OBJECT_OF_CLASS(tv, bacnet_ce_time)
	ZEND_PARSE_PARAMETERS_END();
	php_bacnet_time_obj *t = Z_BACNET_TIME_P(tv);
	php_bacnet_value_obj *obj = php_bacnet_value_alloc(return_value);
	obj->appdata.tag = BACNET_APPLICATION_TAG_TIME;
	obj->appdata.type.Time.hour = t->hour;
	obj->appdata.type.Time.min = t->minute;
	obj->appdata.type.Time.sec = t->second;
	obj->appdata.type.Time.hundredths = t->hundredths;
}

/* Value::objectIdentifier(ObjectIdentifier $v): self */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_value_object_identifier, 0, 1, Bacnet\\Value, 0)
ZEND_ARG_OBJ_INFO(0, value, Bacnet\\ObjectIdentifier, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Value, objectIdentifier) {
	zval *oidv;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_OBJECT_OF_CLASS(oidv, bacnet_ce_object_identifier)
	ZEND_PARSE_PARAMETERS_END();
	php_bacnet_object_identifier_obj *oid = Z_BACNET_OID_P(oidv);
	php_bacnet_value_obj *obj = php_bacnet_value_alloc(return_value);
	obj->appdata.tag = BACNET_APPLICATION_TAG_OBJECT_ID;
	obj->appdata.type.Object_Id.type = oid->object_type;
	obj->appdata.type.Object_Id.instance = oid->instance;
}

static const zend_function_entry bacnet_value_methods[] = {
	PHP_ME(Bacnet_Value, boolean, arginfo_value_boolean, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC) PHP_ME(
		Bacnet_Value, unsignedInt, arginfo_value_unsigned_int,
		ZEND_ACC_PUBLIC | ZEND_ACC_STATIC) PHP_ME(Bacnet_Value, signedInt, arginfo_value_signed_int,
												  ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
		PHP_ME(Bacnet_Value, real, arginfo_value_real, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC) PHP_ME(
			Bacnet_Value, enumerated, arginfo_value_enumerated, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
			PHP_ME(Bacnet_Value, characterString, arginfo_value_character_string,
				   ZEND_ACC_PUBLIC | ZEND_ACC_STATIC) PHP_ME(Bacnet_Value, bitString,
															 arginfo_value_bit_string,
															 ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
				PHP_ME(Bacnet_Value, date, arginfo_value_date, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
					PHP_ME(Bacnet_Value, time, arginfo_value_time,
						   ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
						PHP_ME(Bacnet_Value, objectIdentifier, arginfo_value_object_identifier,
							   ZEND_ACC_PUBLIC | ZEND_ACC_STATIC) PHP_FE_END};

/* ────────────────────────────────────────────────────────────────────── */
/*  Bacnet\Client                                                         */
/* ────────────────────────────────────────────────────────────────────── */

static zend_object *php_bacnet_client_create_object(zend_class_entry *ce) {
	php_bacnet_client_obj *obj =
		(php_bacnet_client_obj *)zend_object_alloc(sizeof(php_bacnet_client_obj), ce);
	obj->client = NULL;
	zend_object_std_init(&obj->std, ce);
	object_properties_init(&obj->std, ce);
	obj->std.handlers = &php_bacnet_client_handlers;
	return &obj->std;
}

static void php_bacnet_client_free_object(zend_object *object) {
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
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, port, IS_LONG, 1, "null")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeoutMs, IS_LONG, 1, "null")
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Client, __construct) {
	zend_string *iface_str = NULL;
	zend_long port = 0;
	zend_long timeout_ms = 0;
	bool port_null = true;
	bool timeout_ms_null = true;

	ZEND_PARSE_PARAMETERS_START(0, 3)
	Z_PARAM_OPTIONAL
	Z_PARAM_STR_OR_NULL(iface_str)
	Z_PARAM_LONG_OR_NULL(port, port_null)
	Z_PARAM_LONG_OR_NULL(timeout_ms, timeout_ms_null)
	ZEND_PARSE_PARAMETERS_END();

	if (BACNET_G(client_initialized)) {
		zend_throw_error(NULL, "Only one Bacnet\\Client may exist per PHP process "
							   "(bacnet-stack uses a process-global UDP socket)");
		RETURN_THROWS();
	}

	php_bacnet_client_obj *obj = Z_BACNET_CLIENT_P(ZEND_THIS);

	const char *iface = (iface_str && ZSTR_LEN(iface_str) > 0) ? ZSTR_VAL(iface_str) : NULL;
	uint16_t p = port_null ? (uint16_t)BACNET_G(default_port) : (uint16_t)port;

	char *err_msg = NULL;
	obj->client = php_bacnet_client_create(iface, p, &err_msg);
	if (!obj->client) {
		zend_throw_exception_ex(bacnet_ce_exception, 0, "Failed to initialize BACnet client: %s",
								err_msg ? err_msg : "unknown error");
		efree(err_msg);
		RETURN_THROWS();
	}
	/* err_msg is NULL on success — efree(NULL) is a no-op */
	efree(err_msg);
	php_bacnet_client_enable_cache(obj->client);
	BACNET_G(client_initialized) = 1;
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_client_whois, 0, 0, IS_ARRAY, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, lowLimit, IS_LONG, 1, "null")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, highLimit, IS_LONG, 1, "null")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeoutMs, IS_LONG, 1, "null")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, refresh, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

typedef struct {
	uint32_t count;
	php_bacnet_iam_entry entries[PHP_BACNET_MAX_COLLECTED_DEVICES];
} php_bacnet_cached_whois;

static void php_bacnet_whois_result(zval *owner, php_bacnet_iam_entry *entries, int count,
									zval *return_value) {
	php_bacnet_client *transport = php_bacnet_transport_from_owner(owner);
	array_init(return_value);
	for (int i = 0; i < count; i++) {
		if (transport) {
			char route_key[48];
			snprintf(route_key, sizeof(route_key), "device|%u", entries[i].device_id);
			php_bacnet_cache_put(
				transport->cache, PHP_BACNET_CACHE_IP, route_key, (uint8_t *)&entries[i].address,
				sizeof(BACNET_ADDRESS),
				php_bacnet_cache_partition_ttl(transport->cache, PHP_BACNET_CACHE_IP));
		}
		zval device_zval;
		object_init_ex(&device_zval, bacnet_ce_device);
		php_bacnet_device_obj *dev = Z_BACNET_DEVICE_P(&device_zval);
		dev->device_id = entries[i].device_id;
		dev->max_apdu = entries[i].max_apdu;
		dev->vendor_id = entries[i].vendor_id;
		memcpy(&dev->address, &entries[i].address, sizeof(BACNET_ADDRESS));
		ZVAL_COPY(&dev->client_zval, owner);
		add_next_index_zval(return_value, &device_zval);
	}
}

static void php_bacnet_device_refresh_route(php_bacnet_device_obj *device) {
	php_bacnet_client *transport = php_bacnet_transport_from_owner(&device->client_zval);
	if (!transport)
		return;
	char key[48];
	snprintf(key, sizeof(key), "device|%u", device->device_id);
	BACNET_ADDRESS address;
	uint32_t length = sizeof(address);
	if (php_bacnet_cache_get(transport->cache, PHP_BACNET_CACHE_IP, key, (uint8_t *)&address,
							 &length) &&
		length == sizeof(address))
		memcpy(&device->address, &address, sizeof(address));
}

static php_bacnet_cache *php_bacnet_cache_from_this(zval *object) {
	if (bacnet_ce_mixed && instanceof_function(Z_OBJCE_P(object), bacnet_ce_mixed)) {
		php_bacnet_server_obj *srv = Z_BACNET_MIXED_P(object);
		return srv->client ? srv->client->cache : NULL;
	}
	php_bacnet_client_obj *client = Z_BACNET_CLIENT_P(object);
	return client->client ? client->client->cache : NULL;
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_cache_set_options, 0, 1, IS_VOID, 0)
ZEND_ARG_TYPE_INFO(0, options, IS_ARRAY, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_cache_get_options, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_cache_get_stats, 0, 0, IS_ARRAY, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, includeEntries, _IS_BOOL, 0, "false")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, reset, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_cache_clear, 0, 0, IS_VOID, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, partition, IS_STRING, 1, "null")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_cache_set_backend, 0, 1, IS_VOID, 0)
ZEND_ARG_OBJ_INFO(0, backend, Bacnet\\CacheBackendInterface, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_cache_backend_get, 0, 3,
										MAY_BE_STRING | MAY_BE_NULL)
ZEND_ARG_TYPE_INFO(0, namespace, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, partition, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_cache_backend_set, 0, 6, IS_VOID, 0)
ZEND_ARG_TYPE_INFO(0, namespace, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, partition, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, payload, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, expiresAtMs, IS_LONG, 0)
ZEND_ARG_TYPE_INFO(0, maxEntries, IS_LONG, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_cache_backend_invalidate, 0, 3, IS_VOID, 0)
ZEND_ARG_TYPE_INFO(0, namespace, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, partition, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, scope, IS_STRING, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_cache_backend_clear, 0, 2, IS_VOID, 0)
ZEND_ARG_TYPE_INFO(0, namespace, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, partition, IS_STRING, 1)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_cache_backend_generation, 0, 2, IS_LONG, 0)
ZEND_ARG_TYPE_INFO(0, namespace, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, partition, IS_STRING, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry bacnet_cache_backend_methods[] = {
	ZEND_ABSTRACT_ME(Bacnet_CacheBackendInterface, get, arginfo_cache_backend_get) ZEND_ABSTRACT_ME(
		Bacnet_CacheBackendInterface, set, arginfo_cache_backend_set)
		ZEND_ABSTRACT_ME(Bacnet_CacheBackendInterface, invalidate, arginfo_cache_backend_invalidate)
			ZEND_ABSTRACT_ME(Bacnet_CacheBackendInterface, clear, arginfo_cache_backend_clear)
				ZEND_ABSTRACT_ME(Bacnet_CacheBackendInterface, getGeneration,
								 arginfo_cache_backend_generation)
					ZEND_ABSTRACT_ME(Bacnet_CacheBackendInterface, bumpGeneration,
									 arginfo_cache_backend_generation) PHP_FE_END};

static void php_bacnet_cache_method_set_options(INTERNAL_FUNCTION_PARAMETERS) {
	zval *options;
	ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_ARRAY(options) ZEND_PARSE_PARAMETERS_END();
	php_bacnet_cache *cache = php_bacnet_cache_from_this(ZEND_THIS);
	if (!cache) {
		zend_throw_exception(bacnet_ce_exception, "BACnet client not initialized", 0);
		RETURN_THROWS();
	}
	zend_string *error = NULL;
	if (!php_bacnet_cache_set_options(cache, Z_ARRVAL_P(options), &error)) {
		zend_value_error("%s", error ? ZSTR_VAL(error) : "Ungültige Cache-Option");
		if (error)
			zend_string_release(error);
		RETURN_THROWS();
	}
}
static void php_bacnet_cache_method_get_options(INTERNAL_FUNCTION_PARAMETERS) {
	ZEND_PARSE_PARAMETERS_NONE();
	php_bacnet_cache *cache = php_bacnet_cache_from_this(ZEND_THIS);
	if (!cache) {
		zend_throw_exception(bacnet_ce_exception, "BACnet client not initialized", 0);
		RETURN_THROWS();
	}
	php_bacnet_cache_get_options(cache, return_value);
}
static void php_bacnet_cache_method_get_stats(INTERNAL_FUNCTION_PARAMETERS) {
	bool include = false, reset = false;
	ZEND_PARSE_PARAMETERS_START(0, 2)
	Z_PARAM_OPTIONAL Z_PARAM_BOOL(include) Z_PARAM_BOOL(reset) ZEND_PARSE_PARAMETERS_END();
	php_bacnet_cache *cache = php_bacnet_cache_from_this(ZEND_THIS);
	if (!cache) {
		zend_throw_exception(bacnet_ce_exception, "BACnet client not initialized", 0);
		RETURN_THROWS();
	}
	php_bacnet_cache_get_stats(cache, include, reset, return_value);
}
static void php_bacnet_cache_method_clear(INTERNAL_FUNCTION_PARAMETERS) {
	zend_string *name = NULL;
	ZEND_PARSE_PARAMETERS_START(0, 1)
	Z_PARAM_OPTIONAL Z_PARAM_STR_OR_NULL(name) ZEND_PARSE_PARAMETERS_END();
	php_bacnet_cache *cache = php_bacnet_cache_from_this(ZEND_THIS);
	if (!cache) {
		zend_throw_exception(bacnet_ce_exception, "BACnet client not initialized", 0);
		RETURN_THROWS();
	}
	int partition = -1;
	if (name) {
		const char *names[] = {"state", "object", "object_list", "device", "ip", "negative"};
		partition = -2;
		for (int i = 0; i < 6; i++)
			if (!strcmp(ZSTR_VAL(name), names[i])) {
				partition = i;
				break;
			}
		if (partition == -2) {
			zend_value_error("Unbekannte Cache-Partition: %s", ZSTR_VAL(name));
			RETURN_THROWS();
		}
	}
	php_bacnet_cache_clear(cache, partition);
}
static void php_bacnet_cache_method_set_backend(INTERNAL_FUNCTION_PARAMETERS) {
	zval *backend;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_OBJECT_OF_CLASS(backend, bacnet_ce_cache_backend) ZEND_PARSE_PARAMETERS_END();
	php_bacnet_cache *cache = php_bacnet_cache_from_this(ZEND_THIS);
	if (!cache) {
		zend_throw_exception(bacnet_ce_exception, "BACnet client not initialized", 0);
		RETURN_THROWS();
	}
	php_bacnet_cache_set_backend(cache, backend);
}

PHP_METHOD(Bacnet_Client, setCacheOptions) {
	php_bacnet_cache_method_set_options(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
PHP_METHOD(Bacnet_Client, getCacheOptions) {
	php_bacnet_cache_method_get_options(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
PHP_METHOD(Bacnet_Client, getCacheStats) {
	php_bacnet_cache_method_get_stats(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
PHP_METHOD(Bacnet_Client, clearCache) {
	php_bacnet_cache_method_clear(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
PHP_METHOD(Bacnet_Client, setCacheBackend) {
	php_bacnet_cache_method_set_backend(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_cov_notification, 0, 1, IS_VOID, 0)
ZEND_ARG_TYPE_INFO(0, handler, IS_CALLABLE, 0)
ZEND_END_ARG_INFO()

static void php_bacnet_cov_set_handler(INTERNAL_FUNCTION_PARAMETERS) {
	zval *handler;
	ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_ZVAL(handler) ZEND_PARSE_PARAMETERS_END();
	php_bacnet_client *client = php_bacnet_transport_from_owner(ZEND_THIS);
	if (!client) {
		zend_throw_exception(bacnet_ce_exception, "BACnet client not initialized", 0);
		RETURN_THROWS();
	}
	if (client->cov_handler_set)
		zval_ptr_dtor(&client->cov_handler_zv);
	char *error = NULL;
	if (!zend_is_callable_ex(handler, NULL, 0, NULL, &client->cov_fcc, &error)) {
		zend_throw_exception_ex(bacnet_ce_exception, 0, "onCovNotification: not callable: %s",
								error ? error : "?");
		efree(error);
		RETURN_THROWS();
	}
	efree(error);
	ZVAL_COPY(&client->cov_handler_zv, handler);
	client->cov_handler_set = true;
	if (instanceof_function(Z_OBJCE_P(ZEND_THIS), bacnet_ce_client)) {
		client->unsolicited_handler = php_bacnet_cov_handle_unsolicited;
		client->unsolicited_context = client;
	}
}
PHP_METHOD(Bacnet_Client, onCovNotification) {
	php_bacnet_cov_set_handler(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
PHP_METHOD(Bacnet_MixedServer, onCovNotification) {
	php_bacnet_cov_set_handler(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_client_poll, 0, 0, IS_VOID, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeoutMs, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Client, poll) {
	zend_long timeout = 0;
	ZEND_PARSE_PARAMETERS_START(0, 1)
	Z_PARAM_OPTIONAL Z_PARAM_LONG(timeout) ZEND_PARSE_PARAMETERS_END();
	php_bacnet_client *client = Z_BACNET_CLIENT_P(ZEND_THIS)->client;
	if (!client) {
		zend_throw_exception(bacnet_ce_exception, "BACnet client not initialized", 0);
		RETURN_THROWS();
	}
	uint8_t pdu[MAX_APDU + MAX_NPDU];
	BACNET_ADDRESS source, dest, npdu_source;
	BACNET_NPDU_DATA npdu;
	uint16_t len = 0;
	if (!php_bacnet_client_pop_pdu(client, &source, pdu, &len)) {
		php_bacnet_transport_lock();
		len = bip_receive(&source, pdu, sizeof(pdu), (unsigned)(timeout < 0 ? 0 : timeout));
		php_bacnet_transport_unlock();
	}
	if (!len)
		return;
	int offset = bacnet_npdu_decode(pdu, len, &dest, &npdu_source, &npdu);
	if (offset < 0 || npdu.network_layer_message || (uint16_t)offset + 2 > len)
		return;
	uint8_t *apdu = pdu + offset;
	uint16_t apdu_len = len - (uint16_t)offset;
	if ((apdu[0] & 0xF0) == PDU_TYPE_UNCONFIRMED_SERVICE_REQUEST &&
		apdu[1] == SERVICE_UNCONFIRMED_COV_NOTIFICATION) {
		(void)php_bacnet_cov_dispatch(client, &source, apdu, apdu_len, false);
		return;
	}
	if ((apdu[0] & 0xF0) == PDU_TYPE_CONFIRMED_SERVICE_REQUEST && apdu_len >= 5 &&
		apdu[3] == SERVICE_CONFIRMED_COV_NOTIFICATION) {
		(void)php_bacnet_cov_dispatch(client, &source, apdu, apdu_len, true);
		uint8_t ack[3];
		int ack_len = encode_simple_ack(ack, apdu[2], SERVICE_CONFIRMED_COV_NOTIFICATION);
		if (ack_len > 0) {
			(void)php_bacnet_server_send_apdu(&source, ack, (uint16_t)ack_len);
		}
	}
}
PHP_METHOD(Bacnet_MixedServer, setCacheOptions) {
	php_bacnet_cache_method_set_options(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
PHP_METHOD(Bacnet_MixedServer, getCacheOptions) {
	php_bacnet_cache_method_get_options(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
PHP_METHOD(Bacnet_MixedServer, getCacheStats) {
	php_bacnet_cache_method_get_stats(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
PHP_METHOD(Bacnet_MixedServer, clearCache) {
	php_bacnet_cache_method_clear(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
PHP_METHOD(Bacnet_MixedServer, setCacheBackend) {
	php_bacnet_cache_method_set_backend(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}

PHP_METHOD(Bacnet_Client, whoIs) {
	zend_long low_limit = 0;
	zend_long high_limit = 0;
	zend_long timeout_ms = 0;
	bool low_null = true;
	bool high_null = true;
	bool tms_null = true;
	bool refresh = false;

	ZEND_PARSE_PARAMETERS_START(0, 4)
	Z_PARAM_OPTIONAL
	Z_PARAM_LONG_OR_NULL(low_limit, low_null)
	Z_PARAM_LONG_OR_NULL(high_limit, high_null)
	Z_PARAM_LONG_OR_NULL(timeout_ms, tms_null)
	Z_PARAM_BOOL(refresh)
	ZEND_PARSE_PARAMETERS_END();

	php_bacnet_client_obj *obj = Z_BACNET_CLIENT_P(ZEND_THIS);
	if (!obj->client) {
		zend_throw_exception(bacnet_ce_exception, "BACnet client not initialized", 0);
		RETURN_THROWS();
	}

	int32_t lo = low_null ? 0 : (int32_t)low_limit;
	int32_t hi = high_null ? BACNET_MAX_INSTANCE : (int32_t)high_limit;
	uint32_t tms = tms_null ? (uint32_t)BACNET_G(default_timeout_ms) : (uint32_t)timeout_ms;

	if (lo < 0)
		lo = 0;
	if (hi > BACNET_MAX_INSTANCE)
		hi = BACNET_MAX_INSTANCE;

	char discovery_key[64], negative_key[80];
	snprintf(discovery_key, sizeof(discovery_key), "whois|%d|%d", lo, hi);
	snprintf(negative_key, sizeof(negative_key), "empty|%s", discovery_key);
	if (refresh) {
		php_bacnet_cache_refresh(obj->client->cache, PHP_BACNET_CACHE_DEVICE, discovery_key);
		php_bacnet_cache_refresh(obj->client->cache, PHP_BACNET_CACHE_NEGATIVE, negative_key);
	} else {
		php_bacnet_cached_whois cached;
		uint32_t cached_len = sizeof(cached);
		if (php_bacnet_cache_get(obj->client->cache, PHP_BACNET_CACHE_DEVICE, discovery_key,
								 (uint8_t *)&cached, &cached_len) &&
			cached_len >= sizeof(uint32_t) && cached.count <= PHP_BACNET_MAX_COLLECTED_DEVICES) {
			php_bacnet_whois_result(ZEND_THIS, cached.entries, cached.count, return_value);
			return;
		}
		uint8_t marker;
		uint32_t marker_len = 1;
		if (php_bacnet_cache_get(obj->client->cache, PHP_BACNET_CACHE_NEGATIVE, negative_key,
								 &marker, &marker_len)) {
			array_init(return_value);
			return;
		}
	}

	uint8_t apdu[64];
	int apdu_len = whois_encode_apdu(apdu, lo, hi);
	if (apdu_len <= 0) {
		zend_throw_exception(bacnet_ce_exception, "Failed to encode Who-Is APDU", 0);
		RETURN_THROWS();
	}

	php_bacnet_iam_entry entries[PHP_BACNET_MAX_COLLECTED_DEVICES];
	int count =
		php_bacnet_broadcast_and_collect(obj->client, apdu, (uint16_t)apdu_len, entries, tms);

	/*
	 * Some BACnet/IP devices occasionally miss the first Who-Is broadcast
	 * after a client socket is opened. Retry an empty discovery once without
	 * changing the public API or duplicating already discovered devices.
	 */
	if (count == 0) {
		usleep(250000);
		count =
			php_bacnet_broadcast_and_collect(obj->client, apdu, (uint16_t)apdu_len, entries, tms);
	}

	if (count > 0) {
		php_bacnet_cached_whois cached;
		memset(&cached, 0, sizeof(cached));
		cached.count = count;
		memcpy(cached.entries, entries, count * sizeof(*entries));
		php_bacnet_cache_put(
			obj->client->cache, PHP_BACNET_CACHE_DEVICE, discovery_key, (uint8_t *)&cached,
			sizeof(uint32_t) + count * sizeof(*entries),
			php_bacnet_cache_partition_ttl(obj->client->cache, PHP_BACNET_CACHE_DEVICE));
	} else {
		uint8_t marker = 1;
		php_bacnet_cache_put(obj->client->cache, PHP_BACNET_CACHE_NEGATIVE, negative_key, &marker,
							 1, php_bacnet_cache_negative_whois_ttl(obj->client->cache));
	}
	php_bacnet_whois_result(ZEND_THIS, entries, count, return_value);
}

static const zend_function_entry bacnet_client_methods[] = {
	PHP_ME(Bacnet_Client, __construct, arginfo_bacnet_client_construct, ZEND_ACC_PUBLIC)
		PHP_ME(Bacnet_Client, whoIs, arginfo_bacnet_client_whois, ZEND_ACC_PUBLIC) PHP_ME(
			Bacnet_Client, setCacheOptions, arginfo_bacnet_cache_set_options, ZEND_ACC_PUBLIC)
			PHP_ME(Bacnet_Client, getCacheOptions, arginfo_bacnet_cache_get_options,
				   ZEND_ACC_PUBLIC) PHP_ME(Bacnet_Client, getCacheStats,
										   arginfo_bacnet_cache_get_stats, ZEND_ACC_PUBLIC)
				PHP_ME(Bacnet_Client, clearCache, arginfo_bacnet_cache_clear, ZEND_ACC_PUBLIC)
					PHP_ME(Bacnet_Client, setCacheBackend, arginfo_bacnet_cache_set_backend,
						   ZEND_ACC_PUBLIC) PHP_ME(Bacnet_Client, onCovNotification,
												   arginfo_bacnet_cov_notification, ZEND_ACC_PUBLIC)
						PHP_ME(Bacnet_Client, poll, arginfo_bacnet_client_poll, ZEND_ACC_PUBLIC)
							PHP_FE_END};

/* Client side of Bacnet\MixedServer. The class inherits all Server methods. */
PHP_METHOD(Bacnet_MixedServer, whoIs) {
	zend_long low_limit = 0;
	zend_long high_limit = 0;
	zend_long timeout_ms = 0;
	bool low_null = true;
	bool high_null = true;
	bool tms_null = true;
	bool refresh = false;

	ZEND_PARSE_PARAMETERS_START(0, 4)
	Z_PARAM_OPTIONAL
	Z_PARAM_LONG_OR_NULL(low_limit, low_null)
	Z_PARAM_LONG_OR_NULL(high_limit, high_null)
	Z_PARAM_LONG_OR_NULL(timeout_ms, tms_null)
	Z_PARAM_BOOL(refresh)
	ZEND_PARSE_PARAMETERS_END();

	php_bacnet_server_obj *srv = Z_BACNET_MIXED_P(ZEND_THIS);
	if (!srv->client) {
		zend_throw_exception(bacnet_ce_exception, "BACnet mixed server not initialized", 0);
		RETURN_THROWS();
	}

	int32_t lo = low_null ? 0 : (int32_t)low_limit;
	int32_t hi = high_null ? BACNET_MAX_INSTANCE : (int32_t)high_limit;
	uint32_t tms = tms_null ? (uint32_t)BACNET_G(default_timeout_ms) : (uint32_t)timeout_ms;

	if (lo < 0)
		lo = 0;
	if (hi > BACNET_MAX_INSTANCE)
		hi = BACNET_MAX_INSTANCE;

	char discovery_key[64], negative_key[80];
	snprintf(discovery_key, sizeof(discovery_key), "whois|%d|%d", lo, hi);
	snprintf(negative_key, sizeof(negative_key), "empty|%s", discovery_key);
	if (refresh) {
		php_bacnet_cache_refresh(srv->client->cache, PHP_BACNET_CACHE_DEVICE, discovery_key);
		php_bacnet_cache_refresh(srv->client->cache, PHP_BACNET_CACHE_NEGATIVE, negative_key);
	} else {
		php_bacnet_cached_whois cached;
		uint32_t cached_len = sizeof(cached);
		if (php_bacnet_cache_get(srv->client->cache, PHP_BACNET_CACHE_DEVICE, discovery_key,
								 (uint8_t *)&cached, &cached_len) &&
			cached_len >= sizeof(uint32_t) && cached.count <= PHP_BACNET_MAX_COLLECTED_DEVICES) {
			php_bacnet_whois_result(ZEND_THIS, cached.entries, cached.count, return_value);
			return;
		}
		uint8_t marker;
		uint32_t marker_len = 1;
		if (php_bacnet_cache_get(srv->client->cache, PHP_BACNET_CACHE_NEGATIVE, negative_key,
								 &marker, &marker_len)) {
			array_init(return_value);
			return;
		}
	}

	uint8_t apdu[64];
	int apdu_len = whois_encode_apdu(apdu, lo, hi);
	if (apdu_len <= 0) {
		zend_throw_exception(bacnet_ce_exception, "Failed to encode Who-Is APDU", 0);
		RETURN_THROWS();
	}

	php_bacnet_iam_entry entries[PHP_BACNET_MAX_COLLECTED_DEVICES];
	int count =
		php_bacnet_broadcast_and_collect(srv->client, apdu, (uint16_t)apdu_len, entries, tms);
	if (EG(exception))
		RETURN_THROWS();

	if (count == 0) {
		usleep(250000);
		count =
			php_bacnet_broadcast_and_collect(srv->client, apdu, (uint16_t)apdu_len, entries, tms);
		if (EG(exception))
			RETURN_THROWS();
	}

	if (count > 0) {
		php_bacnet_cached_whois cached;
		memset(&cached, 0, sizeof(cached));
		cached.count = count;
		memcpy(cached.entries, entries, count * sizeof(*entries));
		php_bacnet_cache_put(
			srv->client->cache, PHP_BACNET_CACHE_DEVICE, discovery_key, (uint8_t *)&cached,
			sizeof(uint32_t) + count * sizeof(*entries),
			php_bacnet_cache_partition_ttl(srv->client->cache, PHP_BACNET_CACHE_DEVICE));
	} else {
		uint8_t marker = 1;
		php_bacnet_cache_put(srv->client->cache, PHP_BACNET_CACHE_NEGATIVE, negative_key, &marker,
							 1, php_bacnet_cache_negative_whois_ttl(srv->client->cache));
	}
	php_bacnet_whois_result(ZEND_THIS, entries, count, return_value);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_mixed_pending_pdu_count, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_MixedServer, getPendingPduCount) {
	ZEND_PARSE_PARAMETERS_NONE();
	php_bacnet_server_obj *srv = Z_BACNET_MIXED_P(ZEND_THIS);
	RETURN_LONG(srv->client ? srv->client->pending_count : 0);
}

static const zend_function_entry bacnet_mixed_methods[] = {
	PHP_ME(Bacnet_MixedServer, whoIs, arginfo_bacnet_client_whois, ZEND_ACC_PUBLIC)
		PHP_ME(Bacnet_MixedServer, getPendingPduCount, arginfo_bacnet_mixed_pending_pdu_count,
			   ZEND_ACC_PUBLIC) PHP_ME(Bacnet_MixedServer, setCacheOptions,
									   arginfo_bacnet_cache_set_options, ZEND_ACC_PUBLIC)
			PHP_ME(Bacnet_MixedServer, getCacheOptions, arginfo_bacnet_cache_get_options,
				   ZEND_ACC_PUBLIC) PHP_ME(Bacnet_MixedServer, getCacheStats,
										   arginfo_bacnet_cache_get_stats, ZEND_ACC_PUBLIC)
				PHP_ME(Bacnet_MixedServer, clearCache, arginfo_bacnet_cache_clear, ZEND_ACC_PUBLIC)
					PHP_ME(Bacnet_MixedServer, setCacheBackend, arginfo_bacnet_cache_set_backend,
						   ZEND_ACC_PUBLIC) PHP_ME(Bacnet_MixedServer, onCovNotification,
												   arginfo_bacnet_cov_notification, ZEND_ACC_PUBLIC)
						PHP_FE_END};

/* ────────────────────────────────────────────────────────────────────── */
/*  Bacnet\Device                                                         */
/* ────────────────────────────────────────────────────────────────────── */

static zend_object *php_bacnet_device_create_object(zend_class_entry *ce) {
	php_bacnet_device_obj *obj =
		(php_bacnet_device_obj *)zend_object_alloc(sizeof(php_bacnet_device_obj), ce);
	obj->device_id = 0;
	obj->max_apdu = 0;
	obj->vendor_id = 0;
	memset(&obj->address, 0, sizeof(obj->address));
	ZVAL_UNDEF(&obj->client_zval);
	zend_object_std_init(&obj->std, ce);
	object_properties_init(&obj->std, ce);
	obj->std.handlers = &php_bacnet_device_handlers;
	return &obj->std;
}

static void php_bacnet_device_free_object(zend_object *object) {
	php_bacnet_device_obj *obj = php_bacnet_device_from_obj(object);
	zval_ptr_dtor(&obj->client_zval);
	zend_object_std_dtor(object);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_device_get_device_id, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Device, getDeviceId) {
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_LONG((zend_long)Z_BACNET_DEVICE_P(ZEND_THIS)->device_id);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_device_get_address, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Device, getAddress) {
	ZEND_PARSE_PARAMETERS_NONE();
	php_bacnet_device_obj *obj = Z_BACNET_DEVICE_P(ZEND_THIS);
	php_bacnet_device_refresh_route(obj);
	char ipbuf[32];
	uint16_t port;
	php_bacnet_address_to_ipport(&obj->address, ipbuf, sizeof(ipbuf), &port);
	char result[64];
	snprintf(result, sizeof(result), "%s:%u", ipbuf, (unsigned)port);
	RETURN_STRING(result);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_device_get_max_apdu, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Device, getMaxApdu) {
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_LONG((zend_long)Z_BACNET_DEVICE_P(ZEND_THIS)->max_apdu);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_device_get_vendor_id, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_Device, getVendorId) {
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_LONG((zend_long)Z_BACNET_DEVICE_P(ZEND_THIS)->vendor_id);
}

/* readProperty(ObjectType $objectType, int $instance, Property $property, ?int $arrayIndex = null):
 * mixed */
ZEND_BEGIN_ARG_INFO_EX(arginfo_bacnet_device_read_property, 0, 0, 3)
ZEND_ARG_OBJ_INFO(0, objectType, Bacnet\\ObjectType, 0)
ZEND_ARG_TYPE_INFO(0, instance, IS_LONG, 0)
ZEND_ARG_OBJ_INFO(0, property, Bacnet\\Property, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, arrayIndex, IS_LONG, 1, "null")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, refresh, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Device, readProperty) {
	zval *ot_enum;
	zend_long instance;
	zval *prop_enum;
	zend_long array_index = 0;
	bool aidx_null = true;
	bool refresh = false;

	ZEND_PARSE_PARAMETERS_START(3, 5)
	Z_PARAM_OBJECT_OF_CLASS(ot_enum, bacnet_ce_object_type_enum)
	Z_PARAM_LONG(instance)
	Z_PARAM_OBJECT_OF_CLASS(prop_enum, bacnet_ce_property_enum)
	Z_PARAM_OPTIONAL
	Z_PARAM_LONG_OR_NULL(array_index, aidx_null)
	Z_PARAM_BOOL(refresh)
	ZEND_PARSE_PARAMETERS_END();

	php_bacnet_device_obj *dev = Z_BACNET_DEVICE_P(ZEND_THIS);

	if (Z_TYPE(dev->client_zval) != IS_OBJECT) {
		zend_throw_exception(bacnet_ce_exception, "Device has no associated client", 0);
		RETURN_THROWS();
	}

	php_bacnet_client *client = php_bacnet_transport_from_owner(&dev->client_zval);
	if (!client) {
		zend_throw_exception(bacnet_ce_exception, "BACnet client not initialized", 0);
		RETURN_THROWS();
	}
	php_bacnet_device_refresh_route(dev);

	zval *ot_backing = zend_enum_fetch_case_value(Z_OBJ_P(ot_enum));
	zval *prop_backing = zend_enum_fetch_case_value(Z_OBJ_P(prop_enum));

	uint32_t aidx = aidx_null ? BACNET_ARRAY_ALL : (uint32_t)array_index;
	uint32_t tms = (uint32_t)BACNET_G(default_timeout_ms);

	php_bacnet_exec_read_property(client, &dev->address, (BACNET_OBJECT_TYPE)Z_LVAL_P(ot_backing),
								  (uint32_t)instance, (BACNET_PROPERTY_ID)Z_LVAL_P(prop_backing),
								  aidx, tms, return_value, refresh);
}

/* writeProperty(ObjectType, int, Property, Value, int $priority=16, ?int $arrayIndex=null): void */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_device_write_property, 0, 4, IS_VOID, 0)
ZEND_ARG_OBJ_INFO(0, objectType, Bacnet\\ObjectType, 0)
ZEND_ARG_TYPE_INFO(0, instance, IS_LONG, 0)
ZEND_ARG_OBJ_INFO(0, property, Bacnet\\Property, 0)
ZEND_ARG_OBJ_INFO(0, value, Bacnet\\Value, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, priority, IS_LONG, 0, "16")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, arrayIndex, IS_LONG, 1, "null")
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Device, writeProperty) {
	zval *ot_enum, *prop_enum, *value_zv;
	zend_long instance, priority = 16, array_index = 0;
	bool aidx_null = true;

	ZEND_PARSE_PARAMETERS_START(4, 6)
	Z_PARAM_OBJECT_OF_CLASS(ot_enum, bacnet_ce_object_type_enum)
	Z_PARAM_LONG(instance)
	Z_PARAM_OBJECT_OF_CLASS(prop_enum, bacnet_ce_property_enum)
	Z_PARAM_OBJECT_OF_CLASS(value_zv, bacnet_ce_value)
	Z_PARAM_OPTIONAL
	Z_PARAM_LONG(priority)
	Z_PARAM_LONG_OR_NULL(array_index, aidx_null)
	ZEND_PARSE_PARAMETERS_END();

	php_bacnet_device_obj *dev = Z_BACNET_DEVICE_P(ZEND_THIS);
	if (Z_TYPE(dev->client_zval) != IS_OBJECT) {
		zend_throw_exception(bacnet_ce_exception, "Device has no associated client", 0);
		RETURN_THROWS();
	}
	php_bacnet_client *client = php_bacnet_transport_from_owner(&dev->client_zval);
	if (!client) {
		zend_throw_exception(bacnet_ce_exception, "BACnet client not initialized", 0);
		RETURN_THROWS();
	}
	php_bacnet_device_refresh_route(dev);

	zval *ot_b = zend_enum_fetch_case_value(Z_OBJ_P(ot_enum));
	zval *pr_b = zend_enum_fetch_case_value(Z_OBJ_P(prop_enum));

	php_bacnet_exec_write_property(client, &dev->address, (BACNET_OBJECT_TYPE)Z_LVAL_P(ot_b),
								   (uint32_t)instance, (BACNET_PROPERTY_ID)Z_LVAL_P(pr_b),
								   aidx_null ? BACNET_ARRAY_ALL : (uint32_t)array_index,
								   (uint8_t)(priority < 1	 ? 1
											 : priority > 16 ? 16
															 : priority),
								   value_zv, (uint32_t)BACNET_G(default_timeout_ms));
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_device_subscribe_cov, 0, 3, IS_LONG, 0)
ZEND_ARG_OBJ_INFO(0, objectType, Bacnet\\ObjectType, 0)
ZEND_ARG_TYPE_INFO(0, instance, IS_LONG, 0)
ZEND_ARG_OBJ_INFO(0, property, Bacnet\\Property, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, lifetimeSeconds, IS_LONG, 0, "3600")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, covIncrement, IS_DOUBLE, 1, "null")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, subscriberProcessId, IS_LONG, 1, "null")
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Device, subscribeCOV) {
	zval *type_zv, *property_zv;
	zend_long instance, lifetime = 3600, process_id = 0;
	double increment = 0;
	bool increment_null = true, process_null = true;
	ZEND_PARSE_PARAMETERS_START(3, 6)
	Z_PARAM_OBJECT_OF_CLASS(type_zv, bacnet_ce_object_type_enum)
	Z_PARAM_LONG(instance)
	Z_PARAM_OBJECT_OF_CLASS(property_zv, bacnet_ce_property_enum)
	Z_PARAM_OPTIONAL Z_PARAM_LONG(lifetime) Z_PARAM_DOUBLE_OR_NULL(increment, increment_null)
		Z_PARAM_LONG_OR_NULL(process_id, process_null) ZEND_PARSE_PARAMETERS_END();
	if (instance < 0 || instance > BACNET_MAX_INSTANCE || lifetime < 1 || lifetime > 86400 ||
		(!process_null && process_id < 1)) {
		zend_value_error("Invalid COV subscription parameters");
		RETURN_THROWS();
	}
	php_bacnet_device_obj *device = Z_BACNET_DEVICE_P(ZEND_THIS);
	php_bacnet_client *client = php_bacnet_transport_from_owner(&device->client_zval);
	if (!client) {
		zend_throw_exception(bacnet_ce_exception, "Device has no associated client", 0);
		RETURN_THROWS();
	}
	php_bacnet_device_refresh_route(device);
	zval *type = zend_enum_fetch_case_value(Z_OBJ_P(type_zv));
	zval *property = zend_enum_fetch_case_value(Z_OBJ_P(property_zv));
	uint32_t pid = process_null ? client->next_cov_process_id++ : (uint32_t)process_id;
	if (pid == 0)
		pid = client->next_cov_process_id++;
	BACNET_SUBSCRIBE_COV_DATA data;
	memset(&data, 0, sizeof(data));
	data.subscriberProcessIdentifier = pid;
	data.monitoredObjectIdentifier.type = (BACNET_OBJECT_TYPE)Z_LVAL_P(type);
	data.monitoredObjectIdentifier.instance = (uint32_t)instance;
	data.issueConfirmedNotifications = false;
	data.lifetime = (uint32_t)lifetime;
	data.covSubscribeToProperty = true;
	data.monitoredProperty.property_identifier = (BACNET_PROPERTY_ID)Z_LVAL_P(property);
	data.monitoredProperty.property_array_index = BACNET_ARRAY_ALL;
	data.covIncrementPresent = !increment_null;
	data.covIncrement = (float)increment;
	uint8_t request[MAX_APDU], response[MAX_APDU];
	uint16_t response_len = 0;
	uint8_t invoke = (uint8_t)(pid & 0xFF);
	int request_len = cov_subscribe_property_encode_apdu(request, sizeof(request), invoke, &data);
	if (request_len <= 0 ||
		php_bacnet_send_and_wait(client, &device->address, request, (uint16_t)request_len, invoke,
								 response, &response_len,
								 (uint32_t)BACNET_G(default_timeout_ms)) != 0) {
		zend_throw_exception(bacnet_ce_timeout_exception, "SubscribeCOVProperty failed", 0);
		RETURN_THROWS();
	}
	RETURN_LONG((zend_long)pid);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_device_unsubscribe_cov, 0, 4, IS_VOID, 0)
ZEND_ARG_OBJ_INFO(0, objectType, Bacnet\\ObjectType, 0)
ZEND_ARG_TYPE_INFO(0, instance, IS_LONG, 0)
ZEND_ARG_OBJ_INFO(0, property, Bacnet\\Property, 0)
ZEND_ARG_TYPE_INFO(0, subscriberProcessId, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Device, unsubscribeCOV) {
	zval *type_zv, *property_zv;
	zend_long instance, process_id;
	ZEND_PARSE_PARAMETERS_START(4, 4)
	Z_PARAM_OBJECT_OF_CLASS(type_zv, bacnet_ce_object_type_enum)
	Z_PARAM_LONG(instance)
	Z_PARAM_OBJECT_OF_CLASS(property_zv, bacnet_ce_property_enum)
	Z_PARAM_LONG(process_id)
	ZEND_PARSE_PARAMETERS_END();
	if (instance < 0 || instance > BACNET_MAX_INSTANCE || process_id < 1) {
		zend_value_error("Invalid COV cancellation parameters");
		RETURN_THROWS();
	}
	php_bacnet_device_obj *device = Z_BACNET_DEVICE_P(ZEND_THIS);
	php_bacnet_client *client = php_bacnet_transport_from_owner(&device->client_zval);
	if (!client) {
		zend_throw_exception(bacnet_ce_exception, "Device has no associated client", 0);
		RETURN_THROWS();
	}
	php_bacnet_device_refresh_route(device);
	zval *type = zend_enum_fetch_case_value(Z_OBJ_P(type_zv));
	zval *property = zend_enum_fetch_case_value(Z_OBJ_P(property_zv));
	BACNET_SUBSCRIBE_COV_DATA data;
	memset(&data, 0, sizeof(data));
	data.subscriberProcessIdentifier = (uint32_t)process_id;
	data.monitoredObjectIdentifier.type = (BACNET_OBJECT_TYPE)Z_LVAL_P(type);
	data.monitoredObjectIdentifier.instance = (uint32_t)instance;
	data.cancellationRequest = true;
	data.covSubscribeToProperty = true;
	data.monitoredProperty.property_identifier = (BACNET_PROPERTY_ID)Z_LVAL_P(property);
	data.monitoredProperty.property_array_index = BACNET_ARRAY_ALL;
	uint8_t request[MAX_APDU], response[MAX_APDU];
	uint16_t response_len = 0;
	uint8_t invoke = (uint8_t)((uint32_t)process_id & 0xFF);
	int request_len = cov_subscribe_property_encode_apdu(request, sizeof(request), invoke, &data);
	if (request_len <= 0 ||
		php_bacnet_send_and_wait(client, &device->address, request, (uint16_t)request_len, invoke,
								 response, &response_len,
								 (uint32_t)BACNET_G(default_timeout_ms)) != 0) {
		zend_throw_exception(bacnet_ce_timeout_exception,
							 "SubscribeCOVProperty cancellation failed", 0);
		RETURN_THROWS();
	}
}

static const zend_function_entry bacnet_device_methods[] = {
	PHP_ME(Bacnet_Device, getDeviceId, arginfo_bacnet_device_get_device_id, ZEND_ACC_PUBLIC) PHP_ME(
		Bacnet_Device, getAddress, arginfo_bacnet_device_get_address, ZEND_ACC_PUBLIC)
		PHP_ME(Bacnet_Device, getMaxApdu, arginfo_bacnet_device_get_max_apdu, ZEND_ACC_PUBLIC)
			PHP_ME(Bacnet_Device, getVendorId, arginfo_bacnet_device_get_vendor_id,
				   ZEND_ACC_PUBLIC) PHP_ME(Bacnet_Device, readProperty,
										   arginfo_bacnet_device_read_property, ZEND_ACC_PUBLIC)
				PHP_ME(Bacnet_Device, writeProperty, arginfo_bacnet_device_write_property,
					   ZEND_ACC_PUBLIC) PHP_ME(Bacnet_Device, subscribeCOV,
											   arginfo_bacnet_device_subscribe_cov, ZEND_ACC_PUBLIC)
					PHP_ME(Bacnet_Device, unsubscribeCOV, arginfo_bacnet_device_unsubscribe_cov,
						   ZEND_ACC_PUBLIC) PHP_FE_END};

/* ────────────────────────────────────────────────────────────────────── */
/*  Bacnet\ObjectIdentifier                                               */
/* ────────────────────────────────────────────────────────────────────── */

static zend_object *php_bacnet_oid_create_object(zend_class_entry *ce) {
	php_bacnet_object_identifier_obj *obj = (php_bacnet_object_identifier_obj *)zend_object_alloc(
		sizeof(php_bacnet_object_identifier_obj), ce);
	obj->object_type = MAX_BACNET_OBJECT_TYPE;
	obj->instance = 0;
	ZVAL_UNDEF(&obj->type_zval);
	zend_object_std_init(&obj->std, ce);
	object_properties_init(&obj->std, ce);
	obj->std.handlers = &php_bacnet_oid_handlers;
	return &obj->std;
}

static void php_bacnet_oid_free_object(zend_object *object) {
	php_bacnet_object_identifier_obj *obj = php_bacnet_oid_from_obj(object);
	zval_ptr_dtor(&obj->type_zval);
	zend_object_std_dtor(object);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_bacnet_oid_construct, 0, 0, 2)
ZEND_ARG_OBJ_INFO(0, type, Bacnet\\ObjectType, 0)
ZEND_ARG_TYPE_INFO(0, instance, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ObjectIdentifier, __construct) {
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
	obj->instance = (uint32_t)instance;
	ZVAL_COPY(&obj->type_zval, type_enum);

	zend_update_property(bacnet_ce_object_identifier, Z_OBJ_P(ZEND_THIS), "type", strlen("type"),
						 type_enum);
	zend_update_property_long(bacnet_ce_object_identifier, Z_OBJ_P(ZEND_THIS), "instance",
							  strlen("instance"), instance);
}

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_bacnet_oid_get_type, 0, 0, Bacnet\\ObjectType, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_ObjectIdentifier, getType) {
	ZEND_PARSE_PARAMETERS_NONE();
	php_bacnet_object_identifier_obj *obj = Z_BACNET_OID_P(ZEND_THIS);
	if (Z_TYPE(obj->type_zval) == IS_UNDEF) {
		RETURN_NULL();
	}
	RETURN_ZVAL(&obj->type_zval, 1, 0);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_oid_get_instance, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_ObjectIdentifier, getInstance) {
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_LONG((zend_long)Z_BACNET_OID_P(ZEND_THIS)->instance);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_oid_to_string, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()
PHP_METHOD(Bacnet_ObjectIdentifier, __toString) {
	ZEND_PARSE_PARAMETERS_NONE();
	php_bacnet_object_identifier_obj *obj = Z_BACNET_OID_P(ZEND_THIS);
	char buf[64];
	snprintf(buf, sizeof(buf), "%u:%u", (unsigned)obj->object_type, obj->instance);
	RETURN_STRING(buf);
}

static const zend_function_entry bacnet_oid_methods[] = {
	PHP_ME(Bacnet_ObjectIdentifier, __construct, arginfo_bacnet_oid_construct, ZEND_ACC_PUBLIC)
		PHP_ME(Bacnet_ObjectIdentifier, getType, arginfo_bacnet_oid_get_type, ZEND_ACC_PUBLIC)
			PHP_ME(Bacnet_ObjectIdentifier, getInstance, arginfo_bacnet_oid_get_instance,
				   ZEND_ACC_PUBLIC) PHP_ME(Bacnet_ObjectIdentifier, __toString,
										   arginfo_bacnet_oid_to_string, ZEND_ACC_PUBLIC)
				PHP_FE_END};

/* ────────────────────────────────────────────────────────────────────── */
/*  Bacnet\BitString                                                      */
/* ────────────────────────────────────────────────────────────────────── */

static zend_object *php_bacnet_bitstring_create_object(zend_class_entry *ce) {
	php_bacnet_bitstring_obj *obj =
		(php_bacnet_bitstring_obj *)zend_object_alloc(sizeof(php_bacnet_bitstring_obj), ce);
	obj->bits_used = 0;
	memset(obj->value, 0, sizeof(obj->value));
	zend_object_std_init(&obj->std, ce);
	object_properties_init(&obj->std, ce);
	obj->std.handlers = &php_bacnet_bitstring_handlers;
	return &obj->std;
}

static void php_bacnet_bitstring_free_object(zend_object *object) {
	zend_object_std_dtor(object);
}

/* __construct(array $bits) — e.g. [true, false, true] */
ZEND_BEGIN_ARG_INFO_EX(arginfo_bacnet_bitstring_construct, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, bits, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_BitString, __construct) {
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
		if (idx >= n)
			break;
		if (zend_is_true(elem)) {
			unsigned byte_no = (unsigned)idx / 8;
			unsigned bit_shift = 7u - ((unsigned)idx % 8u);
			obj->value[byte_no] |= (uint8_t)(1u << bit_shift);
		}
		idx++;
	}
	ZEND_HASH_FOREACH_END();

	zend_update_property_long(bacnet_ce_bit_string, Z_OBJ_P(ZEND_THIS), "length", strlen("length"),
							  (zend_long)obj->bits_used);
}

/* getBit(int $index): bool */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_bitstring_get_bit, 0, 1, _IS_BOOL, 0)
ZEND_ARG_TYPE_INFO(0, index, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_BitString, getBit) {
	zend_long index;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_LONG(index)
	ZEND_PARSE_PARAMETERS_END();

	php_bacnet_bitstring_obj *obj = Z_BACNET_BITSTRING_P(ZEND_THIS);
	if (index < 0 || index >= (zend_long)obj->bits_used) {
		RETURN_FALSE;
	}
	unsigned byte_no = (unsigned)index / 8;
	unsigned bit_shift = 7u - ((unsigned)index % 8u);
	RETURN_BOOL((obj->value[byte_no] >> bit_shift) & 1u);
}

/* getLength(): int */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_bitstring_get_length, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_BitString, getLength) {
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_LONG((zend_long)Z_BACNET_BITSTRING_P(ZEND_THIS)->bits_used);
}

/* toArray(): bool[] */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_bitstring_to_array, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_BitString, toArray) {
	ZEND_PARSE_PARAMETERS_NONE();
	php_bacnet_bitstring_obj *obj = Z_BACNET_BITSTRING_P(ZEND_THIS);
	array_init(return_value);
	for (uint8_t i = 0; i < obj->bits_used; i++) {
		unsigned byte_no = i / 8;
		unsigned bit_shift = 7u - (i % 8u);
		add_next_index_bool(return_value, (obj->value[byte_no] >> bit_shift) & 1u);
	}
}

static const zend_function_entry bacnet_bitstring_methods[] = {
	PHP_ME(Bacnet_BitString, __construct, arginfo_bacnet_bitstring_construct, ZEND_ACC_PUBLIC)
		PHP_ME(Bacnet_BitString, getBit, arginfo_bacnet_bitstring_get_bit, ZEND_ACC_PUBLIC) PHP_ME(
			Bacnet_BitString, getLength, arginfo_bacnet_bitstring_get_length, ZEND_ACC_PUBLIC)
			PHP_ME(Bacnet_BitString, toArray, arginfo_bacnet_bitstring_to_array, ZEND_ACC_PUBLIC)
				PHP_FE_END};

/* ────────────────────────────────────────────────────────────────────── */
/*  Bacnet\Date                                                           */
/* ────────────────────────────────────────────────────────────────────── */

static zend_object *php_bacnet_date_create_object(zend_class_entry *ce) {
	php_bacnet_date_obj *obj =
		(php_bacnet_date_obj *)zend_object_alloc(sizeof(php_bacnet_date_obj), ce);
	obj->year = 0;
	obj->month = 0;
	obj->day = 0;
	obj->weekday = 0;
	zend_object_std_init(&obj->std, ce);
	object_properties_init(&obj->std, ce);
	obj->std.handlers = &php_bacnet_date_handlers;
	return &obj->std;
}

static void php_bacnet_date_free_object(zend_object *object) {
	zend_object_std_dtor(object);
}

/* __construct(int $year, int $month, int $day, int $weekday) */
ZEND_BEGIN_ARG_INFO_EX(arginfo_bacnet_date_construct, 0, 0, 4)
ZEND_ARG_TYPE_INFO(0, year, IS_LONG, 0)
ZEND_ARG_TYPE_INFO(0, month, IS_LONG, 0)
ZEND_ARG_TYPE_INFO(0, day, IS_LONG, 0)
ZEND_ARG_TYPE_INFO(0, weekday, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Date, __construct) {
	zend_long year, month, day, weekday;
	ZEND_PARSE_PARAMETERS_START(4, 4)
	Z_PARAM_LONG(year)
	Z_PARAM_LONG(month)
	Z_PARAM_LONG(day)
	Z_PARAM_LONG(weekday)
	ZEND_PARSE_PARAMETERS_END();

	php_bacnet_date_obj *obj = Z_BACNET_DATE_P(ZEND_THIS);
	obj->year = (uint16_t)year;
	obj->month = (uint8_t)month;
	obj->day = (uint8_t)day;
	obj->weekday = (uint8_t)weekday;

	zend_object *zo = Z_OBJ_P(ZEND_THIS);
	zend_update_property_long(bacnet_ce_date, zo, "year", strlen("year"), year);
	zend_update_property_long(bacnet_ce_date, zo, "month", strlen("month"), month);
	zend_update_property_long(bacnet_ce_date, zo, "day", strlen("day"), day);
	zend_update_property_long(bacnet_ce_date, zo, "weekday", strlen("weekday"), weekday);
}

static const zend_function_entry bacnet_date_methods[] = {
	PHP_ME(Bacnet_Date, __construct, arginfo_bacnet_date_construct, ZEND_ACC_PUBLIC) PHP_FE_END};

/* ────────────────────────────────────────────────────────────────────── */
/*  Bacnet\Time                                                           */
/* ────────────────────────────────────────────────────────────────────── */

static zend_object *php_bacnet_time_create_object(zend_class_entry *ce) {
	php_bacnet_time_obj *obj =
		(php_bacnet_time_obj *)zend_object_alloc(sizeof(php_bacnet_time_obj), ce);
	obj->hour = 0;
	obj->minute = 0;
	obj->second = 0;
	obj->hundredths = 0;
	zend_object_std_init(&obj->std, ce);
	object_properties_init(&obj->std, ce);
	obj->std.handlers = &php_bacnet_time_handlers;
	return &obj->std;
}

static void php_bacnet_time_free_object(zend_object *object) {
	zend_object_std_dtor(object);
}

/* __construct(int $hour, int $minute, int $second, int $hundredths) */
ZEND_BEGIN_ARG_INFO_EX(arginfo_bacnet_time_construct, 0, 0, 4)
ZEND_ARG_TYPE_INFO(0, hour, IS_LONG, 0)
ZEND_ARG_TYPE_INFO(0, minute, IS_LONG, 0)
ZEND_ARG_TYPE_INFO(0, second, IS_LONG, 0)
ZEND_ARG_TYPE_INFO(0, hundredths, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Time, __construct) {
	zend_long hour, minute, second, hundredths;
	ZEND_PARSE_PARAMETERS_START(4, 4)
	Z_PARAM_LONG(hour)
	Z_PARAM_LONG(minute)
	Z_PARAM_LONG(second)
	Z_PARAM_LONG(hundredths)
	ZEND_PARSE_PARAMETERS_END();

	php_bacnet_time_obj *obj = Z_BACNET_TIME_P(ZEND_THIS);
	obj->hour = (uint8_t)hour;
	obj->minute = (uint8_t)minute;
	obj->second = (uint8_t)second;
	obj->hundredths = (uint8_t)hundredths;

	zend_object *zo = Z_OBJ_P(ZEND_THIS);
	zend_update_property_long(bacnet_ce_time, zo, "hour", strlen("hour"), hour);
	zend_update_property_long(bacnet_ce_time, zo, "minute", strlen("minute"), minute);
	zend_update_property_long(bacnet_ce_time, zo, "second", strlen("second"), second);
	zend_update_property_long(bacnet_ce_time, zo, "hundredths", strlen("hundredths"), hundredths);
}

static const zend_function_entry bacnet_time_methods[] = {
	PHP_ME(Bacnet_Time, __construct, arginfo_bacnet_time_construct, ZEND_ACC_PUBLIC) PHP_FE_END};

/* ────────────────────────────────────────────────────────────────────── */
/*  Bacnet\ObjectRef                                                      */
/* ────────────────────────────────────────────────────────────────────── */

static zend_object *php_bacnet_objectref_create_object(zend_class_entry *ce) {
	php_bacnet_objectref_obj *obj =
		(php_bacnet_objectref_obj *)zend_object_alloc(sizeof(php_bacnet_objectref_obj), ce);
	obj->object_type = MAX_BACNET_OBJECT_TYPE;
	obj->instance = 0;
	ZVAL_UNDEF(&obj->device_zval);
	zend_object_std_init(&obj->std, ce);
	object_properties_init(&obj->std, ce);
	obj->std.handlers = &php_bacnet_objectref_handlers;
	return &obj->std;
}

static void php_bacnet_objectref_free_object(zend_object *object) {
	php_bacnet_objectref_obj *obj = php_bacnet_objectref_from_obj(object);
	zval_ptr_dtor(&obj->device_zval);
	zend_object_std_dtor(object);
}

/* __construct(Device $device, ObjectType $type, int $instance) */
ZEND_BEGIN_ARG_INFO_EX(arginfo_bacnet_objectref_construct, 0, 0, 3)
ZEND_ARG_OBJ_INFO(0, device, Bacnet\\Device, 0)
ZEND_ARG_OBJ_INFO(0, type, Bacnet\\ObjectType, 0)
ZEND_ARG_TYPE_INFO(0, instance, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ObjectRef, __construct) {
	zval *dev_zv;
	zval *ot_enum;
	zend_long instance;

	ZEND_PARSE_PARAMETERS_START(3, 3)
	Z_PARAM_OBJECT_OF_CLASS(dev_zv, bacnet_ce_device)
	Z_PARAM_OBJECT_OF_CLASS(ot_enum, bacnet_ce_object_type_enum)
	Z_PARAM_LONG(instance)
	ZEND_PARSE_PARAMETERS_END();

	if (instance < 0 || instance > BACNET_MAX_INSTANCE) {
		zend_throw_exception_ex(bacnet_ce_exception, 0, "Invalid BACnet object instance %ld",
								(long)instance);
		RETURN_THROWS();
	}

	php_bacnet_objectref_obj *obj = Z_BACNET_OBJREF_P(ZEND_THIS);
	zval *backing = zend_enum_fetch_case_value(Z_OBJ_P(ot_enum));
	obj->object_type = (BACNET_OBJECT_TYPE)Z_LVAL_P(backing);
	obj->instance = (uint32_t)instance;
	ZVAL_COPY(&obj->device_zval, dev_zv);
}

/* readProperty(Property $property, ?int $arrayIndex = null): mixed */
ZEND_BEGIN_ARG_INFO_EX(arginfo_bacnet_objectref_read_property, 0, 0, 1)
ZEND_ARG_OBJ_INFO(0, property, Bacnet\\Property, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, arrayIndex, IS_LONG, 1, "null")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, refresh, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ObjectRef, readProperty) {
	zval *prop_enum;
	zend_long array_index = 0;
	bool aidx_null = true;
	bool refresh = false;

	ZEND_PARSE_PARAMETERS_START(1, 3)
	Z_PARAM_OBJECT_OF_CLASS(prop_enum, bacnet_ce_property_enum)
	Z_PARAM_OPTIONAL
	Z_PARAM_LONG_OR_NULL(array_index, aidx_null)
	Z_PARAM_BOOL(refresh)
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

	php_bacnet_client *client = php_bacnet_transport_from_owner(&dev->client_zval);
	if (!client) {
		zend_throw_exception(bacnet_ce_exception, "BACnet client not initialized", 0);
		RETURN_THROWS();
	}
	php_bacnet_device_refresh_route(dev);

	zval *prop_backing = zend_enum_fetch_case_value(Z_OBJ_P(prop_enum));
	uint32_t aidx = aidx_null ? BACNET_ARRAY_ALL : (uint32_t)array_index;
	uint32_t tms = (uint32_t)BACNET_G(default_timeout_ms);

	php_bacnet_exec_read_property(client, &dev->address, ref->object_type, ref->instance,
								  (BACNET_PROPERTY_ID)Z_LVAL_P(prop_backing), aidx, tms,
								  return_value, refresh);
}

/* writeProperty(Property, Value, int $priority=16, ?int $arrayIndex=null): void */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_objectref_write_property, 0, 2, IS_VOID, 0)
ZEND_ARG_OBJ_INFO(0, property, Bacnet\\Property, 0)
ZEND_ARG_OBJ_INFO(0, value, Bacnet\\Value, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, priority, IS_LONG, 0, "16")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, arrayIndex, IS_LONG, 1, "null")
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ObjectRef, writeProperty) {
	zval *prop_enum, *value_zv;
	zend_long priority = 16, array_index = 0;
	bool aidx_null = true;

	ZEND_PARSE_PARAMETERS_START(2, 4)
	Z_PARAM_OBJECT_OF_CLASS(prop_enum, bacnet_ce_property_enum)
	Z_PARAM_OBJECT_OF_CLASS(value_zv, bacnet_ce_value)
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
	php_bacnet_client *client = php_bacnet_transport_from_owner(&dev->client_zval);
	if (!client) {
		zend_throw_exception(bacnet_ce_exception, "BACnet client not initialized", 0);
		RETURN_THROWS();
	}
	php_bacnet_device_refresh_route(dev);

	zval *pr_b = zend_enum_fetch_case_value(Z_OBJ_P(prop_enum));

	php_bacnet_exec_write_property(client, &dev->address, ref->object_type, ref->instance,
								   (BACNET_PROPERTY_ID)Z_LVAL_P(pr_b),
								   aidx_null ? BACNET_ARRAY_ALL : (uint32_t)array_index,
								   (uint8_t)(priority < 1	 ? 1
											 : priority > 16 ? 16
															 : priority),
								   value_zv, (uint32_t)BACNET_G(default_timeout_ms));
}

/* ── ObjectRef convenience methods ───────────────────────────────────────
 * Shared helper: extract validated php_bacnet_client * from ObjectRef $this.
 */
static php_bacnet_client *php_bacnet_objectref_get_client(php_bacnet_objectref_obj *ref,
														  BACNET_ADDRESS **addr_out) {
	if (Z_TYPE(ref->device_zval) != IS_OBJECT) {
		zend_throw_exception(bacnet_ce_exception, "ObjectRef has no associated device", 0);
		return NULL;
	}
	php_bacnet_device_obj *dev = Z_BACNET_DEVICE_P(&ref->device_zval);
	if (Z_TYPE(dev->client_zval) != IS_OBJECT) {
		zend_throw_exception(bacnet_ce_exception, "Device has no associated client", 0);
		return NULL;
	}
	php_bacnet_client *client = php_bacnet_transport_from_owner(&dev->client_zval);
	if (!client) {
		zend_throw_exception(bacnet_ce_exception, "BACnet client not initialized", 0);
		return NULL;
	}
	php_bacnet_device_refresh_route(dev);
	*addr_out = &dev->address;
	return client;
}

/* writeActive(): void — PRESENT_VALUE = enumerated(1) */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_objectref_write_active, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ObjectRef, writeActive) {
	ZEND_PARSE_PARAMETERS_NONE();
	php_bacnet_objectref_obj *ref = Z_BACNET_OBJREF_P(ZEND_THIS);
	BACNET_ADDRESS *addr;
	php_bacnet_client *client = php_bacnet_objectref_get_client(ref, &addr);
	if (!client)
		RETURN_THROWS();

	zval vz;
	object_init_ex(&vz, bacnet_ce_value);
	php_bacnet_value_obj *v = Z_BACNET_VALUE_P(&vz);
	v->appdata.tag = BACNET_APPLICATION_TAG_ENUMERATED;
	v->appdata.type.Enumerated = 1;

	php_bacnet_exec_write_property(client, addr, ref->object_type, ref->instance,
								   PROP_PRESENT_VALUE, BACNET_ARRAY_ALL, 16, &vz,
								   (uint32_t)BACNET_G(default_timeout_ms));
	zval_ptr_dtor(&vz);
}

/* writeInactive(): void — PRESENT_VALUE = enumerated(0) */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_objectref_write_inactive, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ObjectRef, writeInactive) {
	ZEND_PARSE_PARAMETERS_NONE();
	php_bacnet_objectref_obj *ref = Z_BACNET_OBJREF_P(ZEND_THIS);
	BACNET_ADDRESS *addr;
	php_bacnet_client *client = php_bacnet_objectref_get_client(ref, &addr);
	if (!client)
		RETURN_THROWS();

	zval vz;
	object_init_ex(&vz, bacnet_ce_value);
	php_bacnet_value_obj *v = Z_BACNET_VALUE_P(&vz);
	v->appdata.tag = BACNET_APPLICATION_TAG_ENUMERATED;
	v->appdata.type.Enumerated = 0;

	php_bacnet_exec_write_property(client, addr, ref->object_type, ref->instance,
								   PROP_PRESENT_VALUE, BACNET_ARRAY_ALL, 16, &vz,
								   (uint32_t)BACNET_G(default_timeout_ms));
	zval_ptr_dtor(&vz);
}

/* writePresentValue(mixed $value): void */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_objectref_write_present_value, 0, 1, IS_VOID, 0)
ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ObjectRef, writePresentValue) {
	zval *value_zv;
	ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_ZVAL(value_zv) ZEND_PARSE_PARAMETERS_END();

	php_bacnet_objectref_obj *ref = Z_BACNET_OBJREF_P(ZEND_THIS);
	BACNET_ADDRESS *addr;
	php_bacnet_client *client = php_bacnet_objectref_get_client(ref, &addr);
	if (!client)
		RETURN_THROWS();

	/* Accept Bacnet\Value directly or native PHP type */
	zval encoded;
	bool needs_dtor = false;
	if (Z_TYPE_P(value_zv) == IS_OBJECT &&
		instanceof_function(Z_OBJCE_P(value_zv), bacnet_ce_value)) {
		ZVAL_COPY_VALUE(&encoded, value_zv);
	} else {
		BACNET_APPLICATION_DATA_VALUE appdata;
		if (!zval_to_bacapp_value(value_zv, &appdata)) {
			zend_throw_exception(bacnet_ce_exception, "writePresentValue: unsupported value type",
								 0);
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

	php_bacnet_exec_write_property(client, addr, ref->object_type, ref->instance,
								   PROP_PRESENT_VALUE, BACNET_ARRAY_ALL, 16, &encoded,
								   (uint32_t)BACNET_G(default_timeout_ms));
	if (needs_dtor)
		zval_ptr_dtor(&encoded);
}

/* readTrendLog(): TrendLogRecord[] — reads LOG_BUFFER and wraps entries */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_objectref_read_trend_log, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ObjectRef, readTrendLog) {
	ZEND_PARSE_PARAMETERS_NONE();
	php_bacnet_objectref_obj *ref = Z_BACNET_OBJREF_P(ZEND_THIS);
	BACNET_ADDRESS *addr;
	php_bacnet_client *client = php_bacnet_objectref_get_client(ref, &addr);
	if (!client)
		RETURN_THROWS();

	zval raw;
	ZVAL_NULL(&raw);
	int rc = php_bacnet_exec_read_property(client, addr, ref->object_type, ref->instance,
										   PROP_LOG_BUFFER, BACNET_ARRAY_ALL,
										   (uint32_t)BACNET_G(default_timeout_ms), &raw, false);
	if (rc != 0)
		return;

	array_init(return_value);

	/* Wrap each decoded element in a TrendLogRecord */
	zval *entry;
	if (Z_TYPE(raw) == IS_ARRAY) {
		ZEND_HASH_FOREACH_VAL(Z_ARRVAL(raw), entry) {
			zval rec;
			object_init_ex(&rec, bacnet_ce_trend_log_record);
			zend_update_property_null(bacnet_ce_trend_log_record, Z_OBJ(rec), "timestamp",
									  strlen("timestamp"));
			zend_update_property(bacnet_ce_trend_log_record, Z_OBJ(rec), "value", strlen("value"),
								 entry);
			zend_update_property_long(bacnet_ce_trend_log_record, Z_OBJ(rec), "statusFlags",
									  strlen("statusFlags"), 0);
			add_next_index_zval(return_value, &rec);
		}
		ZEND_HASH_FOREACH_END();
	} else if (Z_TYPE(raw) != IS_NULL) {
		zval rec;
		object_init_ex(&rec, bacnet_ce_trend_log_record);
		zend_update_property_null(bacnet_ce_trend_log_record, Z_OBJ(rec), "timestamp",
								  strlen("timestamp"));
		zend_update_property(bacnet_ce_trend_log_record, Z_OBJ(rec), "value", strlen("value"),
							 &raw);
		zend_update_property_long(bacnet_ce_trend_log_record, Z_OBJ(rec), "statusFlags",
								  strlen("statusFlags"), 0);
		add_next_index_zval(return_value, &rec);
	}
	zval_ptr_dtor(&raw);
}

/* readWeeklySchedule(): WeeklySchedule */
ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_objectref_read_weekly_schedule, 0, 0,
									   Bacnet\\WeeklySchedule, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ObjectRef, readWeeklySchedule) {
	ZEND_PARSE_PARAMETERS_NONE();
	php_bacnet_objectref_obj *ref = Z_BACNET_OBJREF_P(ZEND_THIS);
	BACNET_ADDRESS *addr;
	php_bacnet_client *client = php_bacnet_objectref_get_client(ref, &addr);
	if (!client)
		RETURN_THROWS();

	/* WEEKLY_SCHEDULE is a BACnet sequence — the generic APDU decoder
	 * won't parse it. We issue the read for completeness; if it fails
	 * or returns null we still return an empty WeeklySchedule. */
	zval raw;
	ZVAL_NULL(&raw);
	php_bacnet_exec_read_property(client, addr, ref->object_type, ref->instance,
								  PROP_WEEKLY_SCHEDULE, BACNET_ARRAY_ALL,
								  (uint32_t)BACNET_G(default_timeout_ms), &raw, false);
	if (EG(exception)) {
		zval_ptr_dtor(&raw);
		return;
	}
	zval_ptr_dtor(&raw);

	/* Return an empty WeeklySchedule (full sequence decoding is TODO) */
	object_init_ex(return_value, bacnet_ce_weekly_schedule);
	static const char *day_names[] = {"monday", "tuesday",	"wednesday", "thursday",
									  "friday", "saturday", "sunday"};
	for (int i = 0; i < 7; i++) {
		zval empty_arr;
		array_init(&empty_arr);
		zend_update_property(bacnet_ce_weekly_schedule, Z_OBJ_P(return_value), day_names[i],
							 strlen(day_names[i]), &empty_arr);
		zval_ptr_dtor(&empty_arr);
	}
}

/* writeWeeklySchedule(WeeklySchedule $schedule): void */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_objectref_write_weekly_schedule, 0, 1, IS_VOID, 0)
ZEND_ARG_OBJ_INFO(0, schedule, Bacnet\\WeeklySchedule, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ObjectRef, writeWeeklySchedule) {
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
	PHP_ME(Bacnet_ObjectRef, __construct, arginfo_bacnet_objectref_construct, ZEND_ACC_PUBLIC)
		PHP_ME(Bacnet_ObjectRef, readProperty, arginfo_bacnet_objectref_read_property,
			   ZEND_ACC_PUBLIC) PHP_ME(Bacnet_ObjectRef, writeProperty,
									   arginfo_bacnet_objectref_write_property, ZEND_ACC_PUBLIC)
			PHP_ME(Bacnet_ObjectRef, writeActive, arginfo_objectref_write_active,
				   ZEND_ACC_PUBLIC) PHP_ME(Bacnet_ObjectRef, writeInactive,
										   arginfo_objectref_write_inactive, ZEND_ACC_PUBLIC)
				PHP_ME(Bacnet_ObjectRef, writePresentValue, arginfo_objectref_write_present_value,
					   ZEND_ACC_PUBLIC) PHP_ME(Bacnet_ObjectRef, readTrendLog,
											   arginfo_objectref_read_trend_log, ZEND_ACC_PUBLIC)
					PHP_ME(Bacnet_ObjectRef, readWeeklySchedule,
						   arginfo_objectref_read_weekly_schedule, ZEND_ACC_PUBLIC)
						PHP_ME(Bacnet_ObjectRef, writeWeeklySchedule,
							   arginfo_objectref_write_weekly_schedule, ZEND_ACC_PUBLIC)
							PHP_FE_END};

/* ────────────────────────────────────────────────────────────────────── */
/*  Bacnet\ScheduleEntry — (Time, mixed) value pair for weekly schedules  */
/* ────────────────────────────────────────────────────────────────────── */

ZEND_BEGIN_ARG_INFO_EX(arginfo_schedule_entry_construct, 0, 0, 2)
ZEND_ARG_OBJ_INFO(0, startTime, Bacnet\\Time, 0)
ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ScheduleEntry, __construct) {
	zval *time_zv, *value_zv;
	ZEND_PARSE_PARAMETERS_START(2, 2)
	Z_PARAM_OBJECT_OF_CLASS(time_zv, bacnet_ce_time)
	Z_PARAM_ZVAL(value_zv)
	ZEND_PARSE_PARAMETERS_END();

	zend_update_property(bacnet_ce_schedule_entry, Z_OBJ_P(ZEND_THIS), "startTime",
						 strlen("startTime"), time_zv);
	zend_update_property(bacnet_ce_schedule_entry, Z_OBJ_P(ZEND_THIS), "value", strlen("value"),
						 value_zv);
}

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_schedule_entry_get_start_time, 0, 0, Bacnet\\Time, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ScheduleEntry, getStartTime) {
	ZEND_PARSE_PARAMETERS_NONE();
	zval rv;
	zval *prop = zend_read_property(bacnet_ce_schedule_entry, Z_OBJ_P(ZEND_THIS), "startTime",
									strlen("startTime"), 0, &rv);
	ZVAL_COPY(return_value, prop);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_schedule_entry_get_value, 0, 0, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_ScheduleEntry, getValue) {
	ZEND_PARSE_PARAMETERS_NONE();
	zval rv;
	zval *prop = zend_read_property(bacnet_ce_schedule_entry, Z_OBJ_P(ZEND_THIS), "value",
									strlen("value"), 0, &rv);
	ZVAL_COPY(return_value, prop);
}

static const zend_function_entry bacnet_schedule_entry_methods[] = {
	PHP_ME(Bacnet_ScheduleEntry, __construct, arginfo_schedule_entry_construct, ZEND_ACC_PUBLIC)
		PHP_ME(Bacnet_ScheduleEntry, getStartTime, arginfo_schedule_entry_get_start_time,
			   ZEND_ACC_PUBLIC) PHP_ME(Bacnet_ScheduleEntry, getValue,
									   arginfo_schedule_entry_get_value, ZEND_ACC_PUBLIC)
			PHP_FE_END};

/* ────────────────────────────────────────────────────────────────────── */
/*  Bacnet\WeeklySchedule — 7-day schedule (array of ScheduleEntry/day)   */
/* ────────────────────────────────────────────────────────────────────── */

ZEND_BEGIN_ARG_INFO_EX(arginfo_weekly_schedule_construct, 0, 0, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, monday, IS_ARRAY, 0, "[]")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, tuesday, IS_ARRAY, 0, "[]")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, wednesday, IS_ARRAY, 0, "[]")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, thursday, IS_ARRAY, 0, "[]")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, friday, IS_ARRAY, 0, "[]")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, saturday, IS_ARRAY, 0, "[]")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, sunday, IS_ARRAY, 0, "[]")
ZEND_END_ARG_INFO()

static const char *s_day_names[7] = {"monday", "tuesday",  "wednesday", "thursday",
									 "friday", "saturday", "sunday"};

PHP_METHOD(Bacnet_WeeklySchedule, __construct) {
	zval *days[7];
	zval empty;
	array_init(&empty);
	for (int i = 0; i < 7; i++)
		days[i] = &empty;

	ZEND_PARSE_PARAMETERS_START(0, 7)
	Z_PARAM_OPTIONAL
	Z_PARAM_ARRAY(days[0])
	Z_PARAM_ARRAY(days[1])
	Z_PARAM_ARRAY(days[2])
	Z_PARAM_ARRAY(days[3])
	Z_PARAM_ARRAY(days[4])
	Z_PARAM_ARRAY(days[5])
	Z_PARAM_ARRAY(days[6])
	ZEND_PARSE_PARAMETERS_END();

	for (int i = 0; i < 7; i++) {
		zend_update_property(bacnet_ce_weekly_schedule, Z_OBJ_P(ZEND_THIS), s_day_names[i],
							 strlen(s_day_names[i]), days[i]);
	}
	zval_ptr_dtor(&empty);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_weekly_schedule_get_day, 0, 1, IS_ARRAY, 0)
ZEND_ARG_TYPE_INFO(0, weekday, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_WeeklySchedule, getDay) {
	zend_long weekday;
	ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_LONG(weekday) ZEND_PARSE_PARAMETERS_END();

	if (weekday < 1 || weekday > 7) {
		zend_throw_exception_ex(bacnet_ce_exception, 0,
								"weekday must be 1 (Mon) to 7 (Sun), got %ld", (long)weekday);
		RETURN_THROWS();
	}
	const char *name = s_day_names[weekday - 1];
	zval rv;
	zval *prop = zend_read_property(bacnet_ce_weekly_schedule, Z_OBJ_P(ZEND_THIS), name,
									strlen(name), 0, &rv);
	ZVAL_COPY(return_value, prop);
}

static const zend_function_entry bacnet_weekly_schedule_methods[] = {
	PHP_ME(Bacnet_WeeklySchedule, __construct, arginfo_weekly_schedule_construct, ZEND_ACC_PUBLIC)
		PHP_ME(Bacnet_WeeklySchedule, getDay, arginfo_weekly_schedule_get_day, ZEND_ACC_PUBLIC)
			PHP_FE_END};

/* ────────────────────────────────────────────────────────────────────── */
/*  Bacnet\TrendLogRecord — one entry from a TrendLog LOG_BUFFER          */
/* ────────────────────────────────────────────────────────────────────── */

ZEND_BEGIN_ARG_INFO_EX(arginfo_trend_log_record_construct, 0, 0, 3)
ZEND_ARG_INFO(0, timestamp)
ZEND_ARG_INFO(0, value)
ZEND_ARG_TYPE_INFO(0, statusFlags, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_TrendLogRecord, __construct) {
	zval *ts_zv, *value_zv;
	zend_long status_flags = 0;
	ZEND_PARSE_PARAMETERS_START(3, 3)
	Z_PARAM_ZVAL(ts_zv)
	Z_PARAM_ZVAL(value_zv)
	Z_PARAM_LONG(status_flags)
	ZEND_PARSE_PARAMETERS_END();

	zend_update_property(bacnet_ce_trend_log_record, Z_OBJ_P(ZEND_THIS), "timestamp",
						 strlen("timestamp"), ts_zv);
	zend_update_property(bacnet_ce_trend_log_record, Z_OBJ_P(ZEND_THIS), "value", strlen("value"),
						 value_zv);
	zend_update_property_long(bacnet_ce_trend_log_record, Z_OBJ_P(ZEND_THIS), "statusFlags",
							  strlen("statusFlags"), status_flags);
}

static const zend_function_entry bacnet_trend_log_record_methods[] = {
	PHP_ME(Bacnet_TrendLogRecord, __construct, arginfo_trend_log_record_construct, ZEND_ACC_PUBLIC)
		PHP_FE_END};

/* ────────────────────────────────────────────────────────────────────── */
/*  Bacnet\Server — server / device mode with PHP callbacks              */
/* ────────────────────────────────────────────────────────────────────── */

static zend_object *php_bacnet_server_create_object(zend_class_entry *ce) {
	php_bacnet_server_obj *obj =
		(php_bacnet_server_obj *)zend_object_alloc(sizeof(php_bacnet_server_obj), ce);
	obj->client = NULL;
	obj->device_id = 0;
	obj->vendor_id = 0;
	obj->auto_iam = true;
	obj->read_handler_set = false;
	obj->write_handler_set = false;
	obj->local_objects = NULL;
	obj->security = NULL;
	obj->object_name = NULL;
	obj->vendor_name = NULL;
	obj->model_name = NULL;
	obj->description = NULL;
	obj->firmware_revision = NULL;
	obj->application_software_version = NULL;
	ZVAL_UNDEF(&obj->read_handler_zv);
	ZVAL_UNDEF(&obj->write_handler_zv);
	zend_object_std_init(&obj->std, ce);
	object_properties_init(&obj->std, ce);
	obj->std.handlers = &php_bacnet_server_handlers;
	return &obj->std;
}

static void php_bacnet_server_free_object(zend_object *object) {
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
	php_bacnet_security_destroy(obj->security);
	obj->security = NULL;
	if (obj->object_name) {
		zend_string_release(obj->object_name);
	}
	if (obj->vendor_name) {
		zend_string_release(obj->vendor_name);
	}
	if (obj->model_name) {
		zend_string_release(obj->model_name);
	}
	if (obj->description) {
		zend_string_release(obj->description);
	}
	if (obj->firmware_revision) {
		zend_string_release(obj->firmware_revision);
	}
	if (obj->application_software_version) {
		zend_string_release(obj->application_software_version);
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
ZEND_ARG_TYPE_INFO(0, deviceId, IS_LONG, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, bindInterface, IS_STRING, 0, "\"0.0.0.0\"")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, port, IS_LONG, 0, "47808")
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Server, __construct) {
	zend_long device_id = 0;
	zend_string *iface_str = NULL;
	zend_long port = PHP_BACNET_DEFAULT_PORT;

	ZEND_PARSE_PARAMETERS_START(1, 3)
	Z_PARAM_LONG(device_id)
	Z_PARAM_OPTIONAL
	Z_PARAM_STR(iface_str)
	Z_PARAM_LONG(port)
	ZEND_PARSE_PARAMETERS_END();

	if (device_id < 0 || device_id > BACNET_MAX_INSTANCE) {
		zend_throw_exception_ex(bacnet_ce_exception, 0, "Invalid BACnet device ID %ld",
								(long)device_id);
		RETURN_THROWS();
	}
	if (BACNET_G(client_initialized)) {
		zend_throw_error(NULL,
						 "Only one BACnet socket (Client or Server) may exist per PHP process");
		RETURN_THROWS();
	}

	php_bacnet_server_obj *srv = Z_BACNET_SERVER_P(ZEND_THIS);

	srv->security = php_bacnet_security_create();
	if (!srv->security) {
		RETURN_THROWS();
	}

	/* Allocate local_objects HashTable */
	srv->local_objects = (HashTable *)emalloc(sizeof(HashTable));
	zend_hash_init(srv->local_objects, 16, NULL, NULL, 0);

	const char *iface =
		(iface_str && ZSTR_LEN(iface_str) > 0 && strcmp(ZSTR_VAL(iface_str), "0.0.0.0") != 0)
			? ZSTR_VAL(iface_str)
			: NULL;

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
	srv->object_name = strpprintf(0, "php-bacnet Device %u", srv->device_id);
	srv->vendor_name = zend_string_init("Unassigned", strlen("Unassigned"), 0);
	srv->model_name = zend_string_init("php-bacnet Server", strlen("php-bacnet Server"), 0);
	srv->description = zend_string_init("php-bacnet server", strlen("php-bacnet server"), 0);
	srv->firmware_revision = zend_string_init(PHP_BACNET_VERSION, strlen(PHP_BACNET_VERSION), 0);
	srv->application_software_version =
		zend_string_init(PHP_BACNET_VERSION, strlen(PHP_BACNET_VERSION), 0);
	srv->auto_iam = true;
	if (bacnet_ce_mixed && instanceof_function(Z_OBJCE_P(ZEND_THIS), bacnet_ce_mixed)) {
		php_bacnet_client_enable_cache(srv->client);
		srv->client->unsolicited_handler = php_bacnet_mixed_queue_unsolicited;
		srv->client->unsolicited_context = srv;
		srv->client->ignore_device_id = true;
		srv->client->local_device_id = srv->device_id;
	}
	BACNET_G(client_initialized) = 1;
}

/* addLocalObject(ObjectIdentifier $oid): void */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_server_add_local_object, 0, 1, IS_VOID, 0)
ZEND_ARG_OBJ_INFO(0, oid, Bacnet\\ObjectIdentifier, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Server, addLocalObject) {
	zval *oid_zv;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_OBJECT_OF_CLASS(oid_zv, bacnet_ce_object_identifier)
	ZEND_PARSE_PARAMETERS_END();

	php_bacnet_server_obj *srv = Z_BACNET_SERVER_P(ZEND_THIS);
	if (!srv->local_objects)
		return;

	php_bacnet_object_identifier_obj *oid = Z_BACNET_OID_P(oid_zv);
	if (oid->object_type == OBJECT_DEVICE && oid->instance == srv->device_id)
		return;
	zend_ulong key = ((zend_ulong)oid->object_type << 22) | (zend_ulong)oid->instance;
	zval tval;
	ZVAL_TRUE(&tval);
	zend_hash_index_update(srv->local_objects, key, &tval);
}

/* removeLocalObject(ObjectIdentifier $oid): void */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_server_remove_local_object, 0, 1, IS_VOID, 0)
ZEND_ARG_OBJ_INFO(0, oid, Bacnet\\ObjectIdentifier, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Server, removeLocalObject) {
	zval *oid_zv;
	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_OBJECT_OF_CLASS(oid_zv, bacnet_ce_object_identifier)
	ZEND_PARSE_PARAMETERS_END();

	php_bacnet_server_obj *srv = Z_BACNET_SERVER_P(ZEND_THIS);
	if (!srv->local_objects)
		return;

	php_bacnet_object_identifier_obj *oid = Z_BACNET_OID_P(oid_zv);
	zend_ulong key = ((zend_ulong)oid->object_type << 22) | (zend_ulong)oid->instance;
	zend_hash_index_del(srv->local_objects, key);
}

/* onReadProperty(callable $handler): void */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_server_on_read_property, 0, 1, IS_VOID, 0)
ZEND_ARG_TYPE_INFO(0, handler, IS_CALLABLE, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Server, onReadProperty) {
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
		zend_throw_exception_ex(bacnet_ce_exception, 0, "onReadProperty: not callable: %s",
								err_str ? err_str : "?");
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

PHP_METHOD(Bacnet_Server, onWriteProperty) {
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
		zend_throw_exception_ex(bacnet_ce_exception, 0, "onWriteProperty: not callable: %s",
								err_str ? err_str : "?");
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

PHP_METHOD(Bacnet_Server, setAutoIAm) {
	bool enabled;
	ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_BOOL(enabled) ZEND_PARSE_PARAMETERS_END();
	Z_BACNET_SERVER_P(ZEND_THIS)->auto_iam = enabled;
}

static bool php_bacnet_server_send_iam(php_bacnet_server_obj *srv);

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_server_set_device_info, 0, 1, IS_VOID, 0)
ZEND_ARG_TYPE_INFO(0, info, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Server, setDeviceInfo) {
	HashTable *info;
	zval *value;
	zend_string *key;
	static const char *const allowed[] = {"vendorId",
										  "vendorName",
										  "modelName",
										  "objectName",
										  "description",
										  "firmwareRevision",
										  "applicationSoftwareVersion"};

	ZEND_PARSE_PARAMETERS_START(1, 1)
	Z_PARAM_ARRAY_HT(info)
	ZEND_PARSE_PARAMETERS_END();

	ZEND_HASH_FOREACH_STR_KEY_VAL(info, key, value) {
		bool known = false;
		if (key) {
			for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
				if (zend_string_equals_cstr(key, allowed[i], strlen(allowed[i]))) {
					known = true;
					break;
				}
			}
		}
		if (!known) {
			zend_value_error("setDeviceInfo(): unknown key '%s'", key ? ZSTR_VAL(key) : "numeric");
			RETURN_THROWS();
		}
	}
	ZEND_HASH_FOREACH_END();

	zval *vendor_id_zv = zend_hash_str_find(info, "vendorId", strlen("vendorId"));
	zval *vendor_name_zv = zend_hash_str_find(info, "vendorName", strlen("vendorName"));
	zval *model_name_zv = zend_hash_str_find(info, "modelName", strlen("modelName"));
	if (!vendor_id_zv || Z_TYPE_P(vendor_id_zv) != IS_LONG || !vendor_name_zv ||
		Z_TYPE_P(vendor_name_zv) != IS_STRING || !model_name_zv ||
		Z_TYPE_P(model_name_zv) != IS_STRING) {
		zend_value_error("setDeviceInfo(): vendorId, vendorName and modelName are required");
		RETURN_THROWS();
	}
	if (Z_LVAL_P(vendor_id_zv) < 1 || Z_LVAL_P(vendor_id_zv) > UINT16_MAX) {
		zend_value_error(
			"setDeviceInfo(): vendorId must be a registered BACnet vendor ID (1..65535)");
		RETURN_THROWS();
	}
	if (ZSTR_LEN(Z_STR_P(vendor_name_zv)) == 0 || ZSTR_LEN(Z_STR_P(model_name_zv)) == 0) {
		zend_value_error("setDeviceInfo(): vendorName and modelName must not be empty");
		RETURN_THROWS();
	}

	const char *const optional[] = {"objectName", "description", "firmwareRevision",
									"applicationSoftwareVersion"};
	for (size_t i = 0; i < sizeof(optional) / sizeof(optional[0]); i++) {
		value = zend_hash_str_find(info, optional[i], strlen(optional[i]));
		if (value && (Z_TYPE_P(value) != IS_STRING || Z_STRLEN_P(value) == 0)) {
			zend_value_error("setDeviceInfo(): %s must be a non-empty string", optional[i]);
			RETURN_THROWS();
		}
	}

	php_bacnet_server_obj *srv = Z_BACNET_SERVER_P(ZEND_THIS);
	srv->vendor_id = (uint16_t)Z_LVAL_P(vendor_id_zv);
#define PHP_BACNET_SERVER_SET_INFO_STRING(member, field)                                           \
	do {                                                                                           \
		value = zend_hash_str_find(info, field, strlen(field));                                    \
		if (value) {                                                                               \
			if (srv->member) {                                                                     \
				zend_string_release(srv->member);                                                  \
			}                                                                                      \
			srv->member = zend_string_copy(Z_STR_P(value));                                        \
		}                                                                                          \
	} while (0)
	PHP_BACNET_SERVER_SET_INFO_STRING(vendor_name, "vendorName");
	PHP_BACNET_SERVER_SET_INFO_STRING(model_name, "modelName");
	PHP_BACNET_SERVER_SET_INFO_STRING(object_name, "objectName");
	PHP_BACNET_SERVER_SET_INFO_STRING(description, "description");
	PHP_BACNET_SERVER_SET_INFO_STRING(firmware_revision, "firmwareRevision");
	PHP_BACNET_SERVER_SET_INFO_STRING(application_software_version, "applicationSoftwareVersion");
#undef PHP_BACNET_SERVER_SET_INFO_STRING
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_server_announce, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Server, announce) {
	ZEND_PARSE_PARAMETERS_NONE();
	if (!php_bacnet_server_send_iam(Z_BACNET_SERVER_P(ZEND_THIS))) {
		zend_throw_exception(bacnet_ce_exception, "Unable to send BACnet I-Am", 0);
		RETURN_THROWS();
	}
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_server_set_security_options, 0, 1, IS_VOID,
										0)
ZEND_ARG_TYPE_INFO(0, options, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Server, setSecurityOptions) {
	HashTable *options;
	ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_ARRAY_HT(options) ZEND_PARSE_PARAMETERS_END();
	php_bacnet_server_obj *srv = Z_BACNET_SERVER_P(ZEND_THIS);
	if (!srv->security) {
		zend_throw_exception(bacnet_ce_exception, "Server not initialized", 0);
		RETURN_THROWS();
	}
	if (!php_bacnet_security_configure(srv->security, options)) {
		RETURN_THROWS();
	}
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_server_get_security_options, 0, 0, IS_ARRAY,
										0)
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Server, getSecurityOptions) {
	ZEND_PARSE_PARAMETERS_NONE();
	php_bacnet_server_obj *srv = Z_BACNET_SERVER_P(ZEND_THIS);
	if (!srv->security) {
		zend_throw_exception(bacnet_ce_exception, "Server not initialized", 0);
		RETURN_THROWS();
	}
	php_bacnet_security_options_to_array(srv->security, return_value);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_server_get_security_stats, 0, 0, IS_ARRAY, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, includeSources, _IS_BOOL, 0, "false")
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, reset, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

PHP_METHOD(Bacnet_Server, getSecurityStats) {
	bool include_sources = false, reset = false;
	ZEND_PARSE_PARAMETERS_START(0, 2)
	Z_PARAM_OPTIONAL Z_PARAM_BOOL(include_sources) Z_PARAM_BOOL(reset) ZEND_PARSE_PARAMETERS_END();
	php_bacnet_server_obj *srv = Z_BACNET_SERVER_P(ZEND_THIS);
	if (!srv->security) {
		zend_throw_exception(bacnet_ce_exception, "Server not initialized", 0);
		RETURN_THROWS();
	}
	php_bacnet_security_stats_to_array(srv->security, include_sources, reset, return_value);
}

/* ── poll() — process one pending PDU (non-blocking if timeoutMs=0) ──── */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_bacnet_server_poll, 0, 0, IS_VOID, 0)
ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeoutMs, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

/*
 * BACnet/IP transports an already encoded NPDU.  bip_send_pdu() only adds
 * the BVLC header, therefore handing it an APDU directly produces packets
 * that conforming BACnet clients reject.
 */
static bool php_bacnet_server_send_apdu(BACNET_ADDRESS *dest, const uint8_t *apdu,
										uint16_t apdu_len) {
	uint8_t pdu[MAX_APDU + MAX_NPDU];
	BACNET_NPDU_DATA npdu_data;
	int npdu_len;

	if (!dest || !apdu) {
		return false;
	}
	php_bacnet_transport_lock();

	npdu_encode_npdu_data(&npdu_data, false, MESSAGE_PRIORITY_NORMAL);
	npdu_len = npdu_encode_pdu(pdu, dest, NULL, &npdu_data);
	if (npdu_len < 0 || (size_t)npdu_len + apdu_len > sizeof(pdu)) {
		php_bacnet_transport_unlock();
		return false;
	}

	memcpy(pdu + npdu_len, apdu, apdu_len);
	if (dest->mac_len == 0) {
		BACNET_IP_ADDRESS broadcast;
		uint8_t mpdu[BIP_MPDU_MAX];
		bip_get_broadcast_addr(&broadcast);
		broadcast.port = PHP_BACNET_DEFAULT_PORT;
		int mpdu_len = bvlc_encode_original_broadcast(mpdu, sizeof(mpdu), pdu,
													  (uint16_t)(npdu_len + apdu_len));
		bool sent = mpdu_len > 0 && bip_send_mpdu(&broadcast, mpdu, (uint16_t)mpdu_len) > 0;
		php_bacnet_transport_unlock();
		return sent;
	}
	bool sent = bip_send_pdu(dest, &npdu_data, pdu, (uint16_t)(npdu_len + apdu_len)) > 0;
	php_bacnet_transport_unlock();
	return sent;
}

static bool php_bacnet_server_send_iam(php_bacnet_server_obj *srv) {
	uint8_t iam_apdu[MAX_APDU];
	BACNET_ADDRESS broadcast;
	int iam_len = iam_encode_apdu(iam_apdu, srv->device_id, 480, SEGMENTATION_NONE, srv->vendor_id);

	if (iam_len <= 0) {
		return false;
	}
	memset(&broadcast, 0, sizeof(broadcast));
	/* B/IP subnet broadcast: no routed/global NPDU address. */
	broadcast.net = 0;
	return php_bacnet_server_send_apdu(&broadcast, iam_apdu, (uint16_t)iam_len);
}

static bool php_bacnet_server_is_own_device(php_bacnet_server_obj *srv, zend_ulong key) {
	zend_ulong device_key = ((zend_ulong)OBJECT_DEVICE << 22) | srv->device_id;
	return key == device_key;
}

static uint32_t php_bacnet_server_object_list_count(php_bacnet_server_obj *srv) {
	zend_ulong key;
	zval *value;
	uint32_t count = 1;

	if (!srv->local_objects) {
		return count;
	}
	ZEND_HASH_FOREACH_NUM_KEY_VAL(srv->local_objects, key, value) {
		(void)value;
		if (!php_bacnet_server_is_own_device(srv, key)) {
			count++;
		}
	}
	ZEND_HASH_FOREACH_END();
	return count;
}

static bool php_bacnet_server_object_list_item(php_bacnet_server_obj *srv, uint32_t index,
											   BACNET_OBJECT_ID *object_id) {
	zend_ulong key;
	zval *value;
	uint32_t current = 1;

	if (index == 1) {
		object_id->type = OBJECT_DEVICE;
		object_id->instance = srv->device_id;
		return true;
	}
	if (!srv->local_objects) {
		return false;
	}
	ZEND_HASH_FOREACH_NUM_KEY_VAL(srv->local_objects, key, value) {
		(void)value;
		if (php_bacnet_server_is_own_device(srv, key)) {
			continue;
		}
		current++;
		if (current == index) {
			object_id->type = (BACNET_OBJECT_TYPE)(key >> 22);
			object_id->instance = (uint32_t)(key & BACNET_MAX_INSTANCE);
			return true;
		}
	}
	ZEND_HASH_FOREACH_END();
	return false;
}

static int php_bacnet_server_encode_object_list(php_bacnet_server_obj *srv,
												BACNET_ARRAY_INDEX array_index, uint8_t *buffer,
												size_t buffer_size) {
	BACNET_APPLICATION_DATA_VALUE value;
	BACNET_OBJECT_ID object_id;
	uint32_t count = php_bacnet_server_object_list_count(srv);
	int total = 0;

	memset(&value, 0, sizeof(value));
	if (array_index == 0) {
		value.tag = BACNET_APPLICATION_TAG_UNSIGNED_INT;
		value.type.Unsigned_Int = count;
		return bacapp_encode_application_data(buffer, &value);
	}
	if (array_index != BACNET_ARRAY_ALL) {
		if (!php_bacnet_server_object_list_item(srv, array_index, &object_id)) {
			return -1;
		}
		value.tag = BACNET_APPLICATION_TAG_OBJECT_ID;
		value.type.Object_Id = object_id;
		return bacapp_encode_application_data(buffer, &value);
	}
	for (uint32_t index = 1; index <= count; index++) {
		if (!php_bacnet_server_object_list_item(srv, index, &object_id)) {
			return -1;
		}
		value.tag = BACNET_APPLICATION_TAG_OBJECT_ID;
		value.type.Object_Id = object_id;
		int encoded = bacapp_encode_application_data(buffer + total, &value);
		if (encoded <= 0 || (size_t)(total + encoded) > buffer_size) {
			return -1;
		}
		total += encoded;
	}
	return total;
}

/* Standardwerte für das DEVICE-Objekt; ein PHP-Read-Hook darf sie überschreiben. */
static bool php_bacnet_server_default_device_property(php_bacnet_server_obj *srv,
													  BACNET_OBJECT_TYPE type, uint32_t instance,
													  BACNET_PROPERTY_ID property, zval *result) {
	if (type != OBJECT_DEVICE || instance != srv->device_id)
		return false;
	switch (property) {
	case PROP_OBJECT_IDENTIFIER:
		php_bacnet_oid_from_c(OBJECT_DEVICE, srv->device_id, result);
		return true;
	case PROP_OBJECT_TYPE:
		ZVAL_LONG(result, OBJECT_DEVICE);
		return true;
	case PROP_OBJECT_NAME:
		ZVAL_STR_COPY(result, srv->object_name);
		return true;
	case PROP_DESCRIPTION:
		ZVAL_STR_COPY(result, srv->description);
		return true;
	case PROP_APDU_SEGMENT_TIMEOUT:
		ZVAL_LONG(result, 2000);
		return true;
	case PROP_APDU_TIMEOUT:
		ZVAL_LONG(result, 3000);
		return true;
	case PROP_APPLICATION_SOFTWARE_VERSION:
		ZVAL_STR_COPY(result, srv->application_software_version);
		return true;
	case PROP_FIRMWARE_REVISION:
		ZVAL_STR_COPY(result, srv->firmware_revision);
		return true;
	case PROP_MAX_APDU_LENGTH_ACCEPTED:
		ZVAL_LONG(result, 480);
		return true;
	case PROP_NUMBER_OF_APDU_RETRIES:
		ZVAL_LONG(result, 3);
		return true;
	case PROP_MODEL_NAME:
		ZVAL_STR_COPY(result, srv->model_name);
		return true;
	case PROP_PROTOCOL_VERSION:
		ZVAL_LONG(result, BACNET_PROTOCOL_VERSION);
		return true;
	case PROP_PROTOCOL_REVISION:
		ZVAL_LONG(result, BACNET_PROTOCOL_REVISION);
		return true;
	case PROP_PROTOCOL_SERVICES_SUPPORTED: {
		BACNET_BIT_STRING services;
		bitstring_init(&services);
		bitstring_set_bit(&services, SERVICE_CONFIRMED_READ_PROPERTY, true);
		bitstring_set_bit(&services, SERVICE_CONFIRMED_READ_PROP_MULTIPLE, true);
		bitstring_set_bit(&services, SERVICE_CONFIRMED_WRITE_PROPERTY, true);
		php_bacnet_bitstring_new(&services, result);
		return true;
	}
	case PROP_PROTOCOL_OBJECT_TYPES_SUPPORTED: {
		BACNET_BIT_STRING object_types;
		zend_ulong key;
		zval *value;
		bitstring_init(&object_types);
		bitstring_set_bit(&object_types, OBJECT_DEVICE, true);
		if (srv->local_objects) {
			ZEND_HASH_FOREACH_NUM_KEY_VAL(srv->local_objects, key, value) {
				(void)value;
				bitstring_set_bit(&object_types, (uint8_t)(key >> 22), true);
			}
			ZEND_HASH_FOREACH_END();
		}
		php_bacnet_bitstring_new(&object_types, result);
		return true;
	}
	case PROP_SYSTEM_STATUS:
		ZVAL_LONG(result, 0);
		return true;
	case PROP_VENDOR_IDENTIFIER:
		ZVAL_LONG(result, srv->vendor_id);
		return true;
	case PROP_SEGMENTATION_SUPPORTED:
		ZVAL_LONG(result, SEGMENTATION_NONE);
		return true;
	case PROP_MAX_SEGMENTS_ACCEPTED:
		ZVAL_LONG(result, 0);
		return true;
	case PROP_VENDOR_NAME:
		ZVAL_STR_COPY(result, srv->vendor_name);
		return true;
	default:
		return false;
	}
}

static bool php_bacnet_server_is_managed_device_property(php_bacnet_server_obj *srv,
														 BACNET_OBJECT_TYPE type, uint32_t instance,
														 BACNET_PROPERTY_ID property) {
	return type == OBJECT_DEVICE && instance == srv->device_id &&
		   (property == PROP_OBJECT_LIST || property == PROP_PROTOCOL_OBJECT_TYPES_SUPPORTED);
}

/* Resolve a server property once for both ReadProperty and RPM.  A NULL from
 * PHP deliberately means "use the built-in DEVICE value"; it never becomes a
 * BACnet NULL response for an unsupported property. */
static bool php_bacnet_server_resolve_property(php_bacnet_server_obj *srv, BACNET_OBJECT_TYPE type,
											   uint32_t instance, BACNET_PROPERTY_ID property,
											   BACNET_ARRAY_INDEX array_index, zval *result,
											   BACNET_ERROR_CLASS *error_class,
											   BACNET_ERROR_CODE *error_code) {
	zend_ulong key = ((zend_ulong)type << 22) | (zend_ulong)instance;
	bool is_device = type == OBJECT_DEVICE && instance == srv->device_id;

	*error_class = ERROR_CLASS_PROPERTY;
	*error_code = ERROR_CODE_UNKNOWN_PROPERTY;
	ZVAL_NULL(result);

	if (!is_device && (!srv->local_objects || !zend_hash_index_exists(srv->local_objects, key))) {
		*error_class = ERROR_CLASS_OBJECT;
		*error_code = ERROR_CODE_UNKNOWN_OBJECT;
		return false;
	}

	if (srv->read_handler_set &&
		!php_bacnet_server_is_managed_device_property(srv, type, instance, property)) {
		zval oid_zv, prop_zv, prop_int_zv, aidx_zv, args[3];
		php_bacnet_oid_from_c(type, instance, &oid_zv);
		ZVAL_LONG(&prop_int_zv, (zend_long)property);
		ZVAL_UNDEF(&prop_zv);
		zend_call_method_with_1_params(NULL, bacnet_ce_property_enum, NULL, "tryfrom", &prop_zv,
									   &prop_int_zv);
		if (EG(exception)) {
			zval_ptr_dtor(&oid_zv);
			return false;
		}
		if (Z_TYPE(prop_zv) != IS_OBJECT) {
			ZVAL_COPY_VALUE(&prop_zv, &prop_int_zv);
		}
		if (array_index == BACNET_ARRAY_ALL)
			ZVAL_NULL(&aidx_zv);
		else
			ZVAL_LONG(&aidx_zv, (zend_long)array_index);
		ZVAL_COPY_VALUE(&args[0], &oid_zv);
		ZVAL_COPY_VALUE(&args[1], &prop_zv);
		ZVAL_COPY_VALUE(&args[2], &aidx_zv);

		zend_fcall_info fci;
		memset(&fci, 0, sizeof(fci));
		fci.size = sizeof(fci);
		fci.retval = result;
		fci.param_count = 3;
		fci.params = args;
		int call_rc = zend_call_function(&fci, &srv->read_fcc);
		zval_ptr_dtor(&oid_zv);
		zval_ptr_dtor(&prop_zv);
		if (call_rc == FAILURE || EG(exception))
			return false;
	}

	if (Z_TYPE_P(result) == IS_NULL &&
		php_bacnet_server_default_device_property(srv, type, instance, property, result)) {
		return true;
	}
	if (Z_TYPE_P(result) == IS_NULL) {
		*error_code = ERROR_CODE_UNKNOWN_PROPERTY;
		return false;
	}
	return true;
}

static const BACNET_PROPERTY_ID php_bacnet_server_device_all_properties[] = {
	PROP_OBJECT_IDENTIFIER,
	PROP_OBJECT_NAME,
	PROP_OBJECT_TYPE,
	PROP_DESCRIPTION,
	PROP_SYSTEM_STATUS,
	PROP_VENDOR_NAME,
	PROP_VENDOR_IDENTIFIER,
	PROP_MODEL_NAME,
	PROP_FIRMWARE_REVISION,
	PROP_APPLICATION_SOFTWARE_VERSION,
	PROP_PROTOCOL_VERSION,
	PROP_PROTOCOL_REVISION,
	PROP_SEGMENTATION_SUPPORTED,
	PROP_PROTOCOL_SERVICES_SUPPORTED,
	PROP_PROTOCOL_OBJECT_TYPES_SUPPORTED,
	PROP_OBJECT_LIST,
	PROP_APDU_TIMEOUT,
	PROP_NUMBER_OF_APDU_RETRIES,
	PROP_MAX_APDU_LENGTH_ACCEPTED,
	PROP_APDU_SEGMENT_TIMEOUT,
	PROP_MAX_SEGMENTS_ACCEPTED};
static const BACNET_PROPERTY_ID php_bacnet_server_object_all_properties[] = {
	PROP_OBJECT_IDENTIFIER, PROP_OBJECT_NAME, PROP_OBJECT_TYPE, PROP_DESCRIPTION,
	PROP_PRESENT_VALUE};

PHP_METHOD(Bacnet_Server, poll) {
	zend_long timeout_ms_arg = 0;
	ZEND_PARSE_PARAMETERS_START(0, 1)
	Z_PARAM_OPTIONAL
	Z_PARAM_LONG(timeout_ms_arg)
	ZEND_PARSE_PARAMETERS_END();

	if (timeout_ms_arg < 0)
		timeout_ms_arg = 0;

	php_bacnet_server_obj *srv = Z_BACNET_SERVER_P(ZEND_THIS);
	if (!srv->client || !srv->client->initialized) {
		zend_throw_exception(bacnet_ce_exception, "Server not initialized", 0);
		RETURN_THROWS();
	}

	uint8_t pdu[MAX_APDU + MAX_NPDU];
	BACNET_ADDRESS src;
	memset(&src, 0, sizeof(src));

	uint16_t pdu_len = 0;
	bool from_queue = php_bacnet_client_pop_pdu(srv->client, &src, pdu, &pdu_len);
	if (!from_queue) {
		php_bacnet_transport_lock();
		pdu_len = bip_receive(&src, pdu, (uint16_t)sizeof(pdu), (unsigned)timeout_ms_arg);
		php_bacnet_transport_unlock();
	}
	if (pdu_len == 0)
		return;

	if (!from_queue) {
		php_bacnet_packet_kind kind;
		if (!php_bacnet_security_accept(srv->security, &src, pdu, pdu_len, &kind))
			return;
	}

	BACNET_ADDRESS npdu_dest, npdu_src_addr;
	BACNET_NPDU_DATA npdu_hdr;
	int npdu_len = bacnet_npdu_decode(pdu, pdu_len, &npdu_dest, &npdu_src_addr, &npdu_hdr);
	if (npdu_len < 0 || npdu_hdr.network_layer_message) {
		php_bacnet_security_malformed(srv->security, &src);
		return;
	}

	uint8_t *apdu = pdu + npdu_len;
	uint16_t apdu_len = pdu_len - (uint16_t)npdu_len;
	if (apdu_len < 2) {
		php_bacnet_security_malformed(srv->security, &src);
		return;
	}

	uint8_t pdu_type = apdu[0] & 0xF0;
	if (pdu_type == PDU_TYPE_UNCONFIRMED_SERVICE_REQUEST &&
		apdu[1] == SERVICE_UNCONFIRMED_COV_NOTIFICATION) {
		(void)php_bacnet_cov_dispatch(srv->client, &src, apdu, apdu_len, false);
		goto poll_done;
	}
	if (pdu_type == PDU_TYPE_CONFIRMED_SERVICE_REQUEST && apdu_len >= 5 &&
		apdu[3] == SERVICE_CONFIRMED_COV_NOTIFICATION) {
		(void)php_bacnet_cov_dispatch(srv->client, &src, apdu, apdu_len, true);
		uint8_t ack[3];
		int ack_len = encode_simple_ack(ack, apdu[2], SERVICE_CONFIRMED_COV_NOTIFICATION);
		if (ack_len > 0)
			php_bacnet_server_send_apdu(&src, ack, (uint16_t)ack_len);
		goto poll_done;
	}

	/* ── Macro: send BACnet Error PDU back to src ─────────────────────── */
#define BACNET_SEND_ERROR(iid, svc, ec, code)                                                      \
	do {                                                                                           \
		uint8_t _ea[32];                                                                           \
		int _el = bacerror_encode_apdu(_ea, (iid), (svc), (ec), (code));                           \
		if (_el > 0) {                                                                             \
			php_bacnet_server_send_apdu(&src, _ea, (uint16_t)_el);                                 \
		}                                                                                          \
	} while (0)

	/* ── Who-Is → I-Am ─────────────────────────────────────────────────── */
	if (pdu_type == PDU_TYPE_UNCONFIRMED_SERVICE_REQUEST) {
		if (apdu[1] == SERVICE_UNCONFIRMED_WHO_IS && srv->auto_iam) {
			int32_t low = -1, high = -1;
			if (apdu_len > 2) {
				if (whois_decode_service_request(apdu + 2, apdu_len - 2, &low, &high) < 0) {
					php_bacnet_security_malformed(srv->security, &src);
					goto poll_done;
				}
			}
			uint32_t did = srv->device_id;
			bool in_range =
				((low < 0) || ((int32_t)did >= low)) && ((high < 0) || ((int32_t)did <= high));
			if (in_range)
				php_bacnet_server_send_iam(srv);
		}
		goto poll_done;
	}

	/* ── Confirmed request ──────────────────────────────────────────────── */
	if (pdu_type == PDU_TYPE_CONFIRMED_SERVICE_REQUEST) {
		if (apdu_len < 4)
			goto poll_done;
		uint8_t invoke_id = apdu[2];
		uint8_t service = apdu[3];

		/* ── ReadProperty ─────────────────────────────────────────────── */
		if (service == SERVICE_CONFIRMED_READ_PROPERTY) {
			BACNET_READ_PROPERTY_DATA rpdata;
			memset(&rpdata, 0, sizeof(rpdata));
			if (rp_decode_service_request(apdu + 4, apdu_len - 4, &rpdata) < 0) {
				php_bacnet_security_malformed(srv->security, &src);
				goto poll_done;
			}

			zval retval;
			BACNET_ERROR_CLASS error_class;
			BACNET_ERROR_CODE error_code;
			uint8_t app_buf[MAX_APDU];
			int app_len = 0;
			bool managed_object_list =
				php_bacnet_server_is_managed_device_property(
					srv, rpdata.object_type, rpdata.object_instance, rpdata.object_property) &&
				rpdata.object_property == PROP_OBJECT_LIST;
			if (managed_object_list) {
				app_len = php_bacnet_server_encode_object_list(srv, rpdata.array_index, app_buf,
															   sizeof(app_buf));
				if (app_len < 0) {
					error_class = ERROR_CLASS_PROPERTY;
					error_code = ERROR_CODE_INVALID_ARRAY_INDEX;
				}
			} else if (!php_bacnet_server_resolve_property(
						   srv, rpdata.object_type, rpdata.object_instance, rpdata.object_property,
						   rpdata.array_index, &retval, &error_class, &error_code)) {
				app_len = -1;
			} else {
				BACNET_APPLICATION_DATA_VALUE appval;
				if (!zval_to_bacapp_value(&retval, &appval) ||
					(app_len = bacapp_encode_application_data(app_buf, &appval)) <= 0) {
					app_len = -1;
					error_class = ERROR_CLASS_PROPERTY;
					error_code = ERROR_CODE_DATATYPE_NOT_SUPPORTED;
				}
				zval_ptr_dtor(&retval);
			}
			if (app_len < 0) {
				if (!EG(exception)) {
					BACNET_SEND_ERROR(invoke_id, SERVICE_CONFIRMED_READ_PROPERTY, error_class,
									  error_code);
				}
				goto poll_done;
			}

			rpdata.application_data = app_buf;
			rpdata.application_data_len = app_len;

			uint8_t ack_apdu[MAX_APDU];
			int ack_len = rp_ack_encode_apdu(ack_apdu, invoke_id, &rpdata);
			if (ack_len > 0) {
				php_bacnet_server_send_apdu(&src, ack_apdu, (uint16_t)ack_len);
			}

			/* ── ReadPropertyMultiple ────────────────────────────────────── */
		} else if (service == SERVICE_CONFIRMED_READ_PROP_MULTIPLE) {
			const uint8_t *request = apdu + 4;
			unsigned request_len = apdu_len - 4;
			unsigned offset = 0;
			uint8_t ack_apdu[MAX_APDU];
			int ack_len = rpm_ack_encode_apdu_init(ack_apdu, invoke_id);
			bool aborted = false;

			while (offset < request_len && !aborted) {
				BACNET_RPM_DATA rpmdata;
				memset(&rpmdata, 0, sizeof(rpmdata));
				int len = rpm_decode_object_id(request + offset, request_len - offset, &rpmdata);
				if (len <= 0) {
					php_bacnet_security_malformed(srv->security, &src);
					goto poll_done;
				}
				offset += (unsigned)len;
				/* rpm_decode_object_property() decodes only the property
				 * reference.  Keep the enclosing object reference because
				 * the per-property structure is reset below. */
				BACNET_OBJECT_TYPE request_object_type = rpmdata.object_type;
				uint32_t request_object_instance = rpmdata.object_instance;
				len = rpm_ack_encode_apdu_object_begin(ack_apdu + ack_len, &rpmdata);
				if (ack_len + len > MAX_APDU) {
					aborted = true;
					break;
				}
				ack_len += len;
				bool object_closed = false;

				while (offset < request_len) {
					int end_len = rpm_decode_object_end(request + offset, request_len - offset);
					if (end_len > 0) {
						offset += (unsigned)end_len;
						len = rpm_ack_encode_apdu_object_end(ack_apdu + ack_len);
						if (ack_len + len > MAX_APDU)
							aborted = true;
						else
							ack_len += len;
						object_closed = true;
						break;
					}
					memset(&rpmdata, 0, sizeof(rpmdata));
					len = rpm_decode_object_property(request + offset, request_len - offset,
													 &rpmdata);
					if (len <= 0) {
						php_bacnet_security_malformed(srv->security, &src);
						goto poll_done;
					}
					offset += (unsigned)len;

					rpmdata.object_type = request_object_type;
					rpmdata.object_instance = request_object_instance;

					const BACNET_PROPERTY_ID *properties = &rpmdata.object_property;
					size_t property_count = 1;
					if (rpmdata.object_property == PROP_ALL) {
						if (rpmdata.object_type == OBJECT_DEVICE &&
							rpmdata.object_instance == srv->device_id) {
							properties = php_bacnet_server_device_all_properties;
							property_count = sizeof(php_bacnet_server_device_all_properties) /
											 sizeof(php_bacnet_server_device_all_properties[0]);
						} else {
							properties = php_bacnet_server_object_all_properties;
							property_count = sizeof(php_bacnet_server_object_all_properties) /
											 sizeof(php_bacnet_server_object_all_properties[0]);
						}
					}
					for (size_t i = 0; i < property_count; i++) {
						BACNET_ERROR_CLASS error_class;
						BACNET_ERROR_CODE error_code;
						zval retval;
						uint8_t app_buf[MAX_APDU];
						int app_len = 0;
						bool managed_object_list =
							php_bacnet_server_is_managed_device_property(
								srv, rpmdata.object_type, rpmdata.object_instance, properties[i]) &&
							properties[i] == PROP_OBJECT_LIST;
						bool have_value = false;
						if (managed_object_list) {
							app_len = php_bacnet_server_encode_object_list(
								srv, rpmdata.array_index, app_buf, sizeof(app_buf));
							have_value = app_len >= 0;
							if (!have_value) {
								error_class = ERROR_CLASS_PROPERTY;
								error_code = ERROR_CODE_INVALID_ARRAY_INDEX;
							}
						} else {
							have_value = php_bacnet_server_resolve_property(
								srv, rpmdata.object_type, rpmdata.object_instance, properties[i],
								rpmdata.array_index, &retval, &error_class, &error_code);
						}
						if (have_value && !managed_object_list) {
							BACNET_APPLICATION_DATA_VALUE appval;
							if (!zval_to_bacapp_value(&retval, &appval) ||
								(app_len = bacapp_encode_application_data(app_buf, &appval)) <= 0) {
								have_value = false;
								error_class = ERROR_CLASS_PROPERTY;
								error_code = ERROR_CODE_DATATYPE_NOT_SUPPORTED;
							}
							zval_ptr_dtor(&retval);
						}
						len =
							rpm_ack_encode_apdu_object_property(NULL, properties[i],
																rpmdata.array_index) +
							(have_value
								 ? rpm_ack_encode_apdu_object_property_value(NULL, app_buf, app_len)
								 : rpm_ack_encode_apdu_object_property_error(NULL, error_class,
																			 error_code));
						if (ack_len + len > MAX_APDU) {
							aborted = true;
							break;
						}
						ack_len += rpm_ack_encode_apdu_object_property(
							ack_apdu + ack_len, properties[i], rpmdata.array_index);
						ack_len += have_value ? rpm_ack_encode_apdu_object_property_value(
													ack_apdu + ack_len, app_buf, app_len)
											  : rpm_ack_encode_apdu_object_property_error(
													ack_apdu + ack_len, error_class, error_code);
					}
					if (aborted)
						break;
				}
				if (!aborted && !object_closed) {
					php_bacnet_security_malformed(srv->security, &src);
					goto poll_done;
				}
			}
			if (aborted) {
				uint8_t abort_apdu[3];
				int abort_len = abort_encode_apdu(abort_apdu, invoke_id,
												  ABORT_REASON_SEGMENTATION_NOT_SUPPORTED, true);
				php_bacnet_server_send_apdu(&src, abort_apdu, (uint16_t)abort_len);
			} else if (ack_len > 3) {
				php_bacnet_server_send_apdu(&src, ack_apdu, (uint16_t)ack_len);
			}

			/* ── WriteProperty ────────────────────────────────────────────── */
		} else if (service == SERVICE_CONFIRMED_WRITE_PROPERTY) {
			BACNET_WRITE_PROPERTY_DATA wpdata;
			memset(&wpdata, 0, sizeof(wpdata));
			if (wp_decode_service_request(apdu + 4, apdu_len - 4, &wpdata) < 0) {
				php_bacnet_security_malformed(srv->security, &src);
				goto poll_done;
			}

			if (php_bacnet_security_is_duplicate_write(srv->security, &src, apdu, apdu_len)) {
				uint8_t ack_apdu[3] = {PDU_TYPE_SIMPLE_ACK, invoke_id,
									   SERVICE_CONFIRMED_WRITE_PROPERTY};
				php_bacnet_server_send_apdu(&src, ack_apdu, 3);
				goto poll_done;
			}

			zend_ulong obj_key =
				((zend_ulong)wpdata.object_type << 22) | (zend_ulong)wpdata.object_instance;
			if (!srv->local_objects || !zend_hash_index_exists(srv->local_objects, obj_key) ||
				!srv->write_handler_set) {
				BACNET_SEND_ERROR(invoke_id, SERVICE_CONFIRMED_WRITE_PROPERTY, ERROR_CLASS_OBJECT,
								  ERROR_CODE_UNKNOWN_OBJECT);
				goto poll_done;
			}

			/* Decode application_data → PHP value */
			zval value_zv;
			ZVAL_NULL(&value_zv);
			if (wpdata.application_data_len > 0) {
				bacapp_values_to_zval(wpdata.application_data,
									  (unsigned)wpdata.application_data_len, &value_zv);
			}

			/* Property enum via tryFrom */
			zval prop_int_zv, prop_zv;
			ZVAL_LONG(&prop_int_zv, (zend_long)wpdata.object_property);
			ZVAL_UNDEF(&prop_zv);
			zend_call_method_with_1_params(NULL, bacnet_ce_property_enum, NULL, "tryfrom", &prop_zv,
										   &prop_int_zv);
			if (EG(exception)) {
				zval_ptr_dtor(&value_zv);
				goto poll_done;
			}
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
			fci.size = sizeof(fci);
			fci.retval = &retval;
			fci.param_count = 4;
			fci.params = args;

			int call_rc = zend_call_function(&fci, &srv->write_fcc);
			zval_ptr_dtor(&oid_zv);
			zval_ptr_dtor(&prop_zv);
			zval_ptr_dtor(&value_zv);
			zval_ptr_dtor(&retval);

			if (call_rc == FAILURE || EG(exception))
				goto poll_done;

			php_bacnet_security_record_write(srv->security, &src, apdu, apdu_len);

			/* Simple-ACK */
			uint8_t ack_apdu[3];
			ack_apdu[0] = PDU_TYPE_SIMPLE_ACK;
			ack_apdu[1] = invoke_id;
			ack_apdu[2] = SERVICE_CONFIRMED_WRITE_PROPERTY;
			php_bacnet_server_send_apdu(&src, ack_apdu, 3);
		} else {
			uint8_t reject_apdu[3];
			int reject_len =
				reject_encode_apdu(reject_apdu, invoke_id, REJECT_REASON_UNRECOGNIZED_SERVICE);
			if (reject_len > 0)
				php_bacnet_server_send_apdu(&src, reject_apdu, (uint16_t)reject_len);
		}
	}

poll_done:
#undef BACNET_SEND_ERROR
	return;
}

static const zend_function_entry bacnet_server_methods[] = {
	PHP_ME(Bacnet_Server, __construct, arginfo_bacnet_server_construct, ZEND_ACC_PUBLIC) PHP_ME(
		Bacnet_Server, addLocalObject, arginfo_bacnet_server_add_local_object, ZEND_ACC_PUBLIC)
		PHP_ME(Bacnet_Server, removeLocalObject, arginfo_bacnet_server_remove_local_object,
			   ZEND_ACC_PUBLIC) PHP_ME(Bacnet_Server, onReadProperty,
									   arginfo_bacnet_server_on_read_property, ZEND_ACC_PUBLIC)
			PHP_ME(Bacnet_Server, onWriteProperty, arginfo_bacnet_server_on_write_property,
				   ZEND_ACC_PUBLIC) PHP_ME(Bacnet_Server, setAutoIAm,
										   arginfo_bacnet_server_set_auto_iam, ZEND_ACC_PUBLIC)
				PHP_ME(Bacnet_Server, setDeviceInfo, arginfo_bacnet_server_set_device_info,
					   ZEND_ACC_PUBLIC)
					PHP_ME(Bacnet_Server, announce, arginfo_bacnet_server_announce, ZEND_ACC_PUBLIC)
						PHP_ME(Bacnet_Server, setSecurityOptions,
							   arginfo_bacnet_server_set_security_options, ZEND_ACC_PUBLIC)
							PHP_ME(Bacnet_Server, getSecurityOptions,
								   arginfo_bacnet_server_get_security_options, ZEND_ACC_PUBLIC)
								PHP_ME(Bacnet_Server, getSecurityStats,
									   arginfo_bacnet_server_get_security_stats, ZEND_ACC_PUBLIC)
									PHP_ME(Bacnet_Server, poll, arginfo_bacnet_server_poll,
										   ZEND_ACC_PUBLIC) PHP_FE_END};

/* ────────────────────────────────────────────────────────────────────── */
/*  Helper: register one int-backed enum case                            */
/* ────────────────────────────────────────────────────────────────────── */

static void php_bacnet_enum_add_long(zend_class_entry *ce, const char *name, zend_long val) {
	zval v;
	ZVAL_LONG(&v, val);
	zend_enum_add_case_cstr(ce, name, &v);
}

/* ────────────────────────────────────────────────────────────────────── */
/*  Registration                                                          */
/* ────────────────────────────────────────────────────────────────────── */

void php_bacnet_register_classes(void) {
	zend_class_entry ce;

	/* ── Exceptions ─────────────────────────────────────────────────── */

	INIT_CLASS_ENTRY(ce, "Bacnet\\Exception", NULL);
	bacnet_ce_exception = zend_register_internal_class_ex(&ce, zend_ce_exception);

	INIT_CLASS_ENTRY(ce, "Bacnet\\TimeoutException", NULL);
	bacnet_ce_timeout_exception = zend_register_internal_class_ex(&ce, bacnet_ce_exception);

	INIT_CLASS_ENTRY(ce, "Bacnet\\DeviceException", NULL);
	bacnet_ce_device_exception = zend_register_internal_class_ex(&ce, bacnet_ce_exception);
	zend_declare_property_long(bacnet_ce_device_exception, "errorClass", strlen("errorClass"), 0,
							   ZEND_ACC_PUBLIC);
	zend_declare_property_long(bacnet_ce_device_exception, "errorCode", strlen("errorCode"), 0,
							   ZEND_ACC_PUBLIC);

	/* ── enum Bacnet\ObjectType: int ────────────────────────────────── */

	bacnet_ce_object_type_enum = zend_register_internal_enum("Bacnet\\ObjectType", IS_LONG, NULL);

	php_bacnet_enum_add_long(bacnet_ce_object_type_enum, "ANALOG_INPUT", 0);
	php_bacnet_enum_add_long(bacnet_ce_object_type_enum, "ANALOG_OUTPUT", 1);
	php_bacnet_enum_add_long(bacnet_ce_object_type_enum, "ANALOG_VALUE", 2);
	php_bacnet_enum_add_long(bacnet_ce_object_type_enum, "BINARY_INPUT", 3);
	php_bacnet_enum_add_long(bacnet_ce_object_type_enum, "BINARY_OUTPUT", 4);
	php_bacnet_enum_add_long(bacnet_ce_object_type_enum, "BINARY_VALUE", 5);
	php_bacnet_enum_add_long(bacnet_ce_object_type_enum, "DEVICE", 8);
	php_bacnet_enum_add_long(bacnet_ce_object_type_enum, "EVENT_ENROLLMENT", 9);
	php_bacnet_enum_add_long(bacnet_ce_object_type_enum, "MULTI_STATE_INPUT", 13);
	php_bacnet_enum_add_long(bacnet_ce_object_type_enum, "MULTI_STATE_OUTPUT", 14);
	php_bacnet_enum_add_long(bacnet_ce_object_type_enum, "NOTIFICATION_CLASS", 15);
	php_bacnet_enum_add_long(bacnet_ce_object_type_enum, "SCHEDULE", 17);
	php_bacnet_enum_add_long(bacnet_ce_object_type_enum, "MULTI_STATE_VALUE", 19);
	php_bacnet_enum_add_long(bacnet_ce_object_type_enum, "TREND_LOG", 20);

	/* ── enum Bacnet\Property: int ──────────────────────────────────── */

	bacnet_ce_property_enum = zend_register_internal_enum("Bacnet\\Property", IS_LONG, NULL);

	php_bacnet_enum_add_long(bacnet_ce_property_enum, "OBJECT_IDENTIFIER", 75);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "APDU_SEGMENT_TIMEOUT", 10);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "APDU_TIMEOUT", 11);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "APPLICATION_SOFTWARE_VERSION", 12);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "OBJECT_NAME", 77);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "OBJECT_TYPE", 79);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "DESCRIPTION", 28);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "DEVICE_ADDRESS_BINDING", 30);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "FIRMWARE_REVISION", 44);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "OBJECT_LIST", 76);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "LOCAL_DATE", 56);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "LOCAL_TIME", 57);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "MAX_APDU_LENGTH_ACCEPTED", 62);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "MODEL_NAME", 70);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "NUMBER_OF_APDU_RETRIES", 73);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "PRESENT_VALUE", 85);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "STATUS_FLAGS", 111);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "EVENT_STATE", 36);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "OUT_OF_SERVICE", 81);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "UNITS", 117);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "PRIORITY_ARRAY", 87);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "RELINQUISH_DEFAULT", 104);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "NUMBER_OF_STATES", 74);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "STATE_TEXT", 110);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "NOTIFICATION_CLASS", 17);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "ACK_REQUIRED", 1);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "NOTIFY_TYPE", 72);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "EVENT_TYPE", 37);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "EVENT_PARAMETERS", 83);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "OBJECT_PROPERTY_REFERENCE", 78);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "RECIPIENT_LIST", 102);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "WEEKLY_SCHEDULE", 123);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "EXCEPTION_SCHEDULE", 38);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "SCHEDULE_DEFAULT", 174);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "EFFECTIVE_PERIOD", 32);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "LOG_BUFFER", 131);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "LOG_DEVICE_OBJECT_PROPERTY", 132);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "RECORD_COUNT", 141);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "TOTAL_RECORD_COUNT", 145);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "RELIABILITY", 103);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "PROTOCOL_OBJECT_TYPES_SUPPORTED", 96);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "PROTOCOL_SERVICES_SUPPORTED", 97);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "PROTOCOL_VERSION", 98);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "SYSTEM_STATUS", 112);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "SEGMENTATION_SUPPORTED", 107);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "VENDOR_IDENTIFIER", 120);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "VENDOR_NAME", 121);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "PROTOCOL_REVISION", 139);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "DATABASE_REVISION", 155);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "ACTIVE_COV_SUBSCRIPTIONS", 152);
	php_bacnet_enum_add_long(bacnet_ce_property_enum, "MAX_SEGMENTS_ACCEPTED", 167);

	INIT_CLASS_ENTRY(ce, "Bacnet\\CacheBackendInterface", bacnet_cache_backend_methods);
	bacnet_ce_cache_backend = zend_register_internal_interface(&ce);

	/* ── Bacnet\Client ───────────────────────────────────────────────── */

	memcpy(&php_bacnet_client_handlers, zend_get_std_object_handlers(),
		   sizeof(zend_object_handlers));
	php_bacnet_client_handlers.offset = XtOffsetOf(php_bacnet_client_obj, std);
	php_bacnet_client_handlers.free_obj = php_bacnet_client_free_object;
	php_bacnet_client_handlers.clone_obj = NULL;

	INIT_CLASS_ENTRY(ce, "Bacnet\\Client", bacnet_client_methods);
	bacnet_ce_client = zend_register_internal_class(&ce);
	bacnet_ce_client->create_object = php_bacnet_client_create_object;

	/* ── Bacnet\Device ───────────────────────────────────────────────── */

	memcpy(&php_bacnet_device_handlers, zend_get_std_object_handlers(),
		   sizeof(zend_object_handlers));
	php_bacnet_device_handlers.offset = XtOffsetOf(php_bacnet_device_obj, std);
	php_bacnet_device_handlers.free_obj = php_bacnet_device_free_object;
	php_bacnet_device_handlers.clone_obj = NULL;

	INIT_CLASS_ENTRY(ce, "Bacnet\\Device", bacnet_device_methods);
	bacnet_ce_device = zend_register_internal_class(&ce);
	bacnet_ce_device->create_object = php_bacnet_device_create_object;

	/* ── Bacnet\ObjectIdentifier ─────────────────────────────────────── */

	memcpy(&php_bacnet_oid_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_bacnet_oid_handlers.offset = XtOffsetOf(php_bacnet_object_identifier_obj, std);
	php_bacnet_oid_handlers.free_obj = php_bacnet_oid_free_object;
	php_bacnet_oid_handlers.clone_obj = NULL;

	INIT_CLASS_ENTRY(ce, "Bacnet\\ObjectIdentifier", bacnet_oid_methods);
	bacnet_ce_object_identifier = zend_register_internal_class(&ce);
	bacnet_ce_object_identifier->create_object = php_bacnet_oid_create_object;

	zend_declare_property_null(bacnet_ce_object_identifier, "type", strlen("type"),
							   ZEND_ACC_PUBLIC);
	zend_declare_property_long(bacnet_ce_object_identifier, "instance", strlen("instance"), 0,
							   ZEND_ACC_PUBLIC);

	/* ── Bacnet\BitString ────────────────────────────────────────────── */

	memcpy(&php_bacnet_bitstring_handlers, zend_get_std_object_handlers(),
		   sizeof(zend_object_handlers));
	php_bacnet_bitstring_handlers.offset = XtOffsetOf(php_bacnet_bitstring_obj, std);
	php_bacnet_bitstring_handlers.free_obj = php_bacnet_bitstring_free_object;
	php_bacnet_bitstring_handlers.clone_obj = NULL;

	INIT_CLASS_ENTRY(ce, "Bacnet\\BitString", bacnet_bitstring_methods);
	bacnet_ce_bit_string = zend_register_internal_class(&ce);
	bacnet_ce_bit_string->create_object = php_bacnet_bitstring_create_object;

	zend_declare_property_long(bacnet_ce_bit_string, "length", strlen("length"), 0,
							   ZEND_ACC_PUBLIC);

	/* ── Bacnet\Date ─────────────────────────────────────────────────── */

	memcpy(&php_bacnet_date_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_bacnet_date_handlers.offset = XtOffsetOf(php_bacnet_date_obj, std);
	php_bacnet_date_handlers.free_obj = php_bacnet_date_free_object;
	php_bacnet_date_handlers.clone_obj = NULL;

	INIT_CLASS_ENTRY(ce, "Bacnet\\Date", bacnet_date_methods);
	bacnet_ce_date = zend_register_internal_class(&ce);
	bacnet_ce_date->create_object = php_bacnet_date_create_object;

	zend_declare_property_long(bacnet_ce_date, "year", strlen("year"), 0, ZEND_ACC_PUBLIC);
	zend_declare_property_long(bacnet_ce_date, "month", strlen("month"), 0, ZEND_ACC_PUBLIC);
	zend_declare_property_long(bacnet_ce_date, "day", strlen("day"), 0, ZEND_ACC_PUBLIC);
	zend_declare_property_long(bacnet_ce_date, "weekday", strlen("weekday"), 0, ZEND_ACC_PUBLIC);

	/* ── Bacnet\Time ─────────────────────────────────────────────────── */

	memcpy(&php_bacnet_time_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	php_bacnet_time_handlers.offset = XtOffsetOf(php_bacnet_time_obj, std);
	php_bacnet_time_handlers.free_obj = php_bacnet_time_free_object;
	php_bacnet_time_handlers.clone_obj = NULL;

	INIT_CLASS_ENTRY(ce, "Bacnet\\Time", bacnet_time_methods);
	bacnet_ce_time = zend_register_internal_class(&ce);
	bacnet_ce_time->create_object = php_bacnet_time_create_object;

	zend_declare_property_long(bacnet_ce_time, "hour", strlen("hour"), 0, ZEND_ACC_PUBLIC);
	zend_declare_property_long(bacnet_ce_time, "minute", strlen("minute"), 0, ZEND_ACC_PUBLIC);
	zend_declare_property_long(bacnet_ce_time, "second", strlen("second"), 0, ZEND_ACC_PUBLIC);
	zend_declare_property_long(bacnet_ce_time, "hundredths", strlen("hundredths"), 0,
							   ZEND_ACC_PUBLIC);

	/* ── Bacnet\ObjectRef ────────────────────────────────────────────── */

	memcpy(&php_bacnet_objectref_handlers, zend_get_std_object_handlers(),
		   sizeof(zend_object_handlers));
	php_bacnet_objectref_handlers.offset = XtOffsetOf(php_bacnet_objectref_obj, std);
	php_bacnet_objectref_handlers.free_obj = php_bacnet_objectref_free_object;
	php_bacnet_objectref_handlers.clone_obj = NULL;

	INIT_CLASS_ENTRY(ce, "Bacnet\\ObjectRef", bacnet_objectref_methods);
	bacnet_ce_object_ref = zend_register_internal_class(&ce);
	bacnet_ce_object_ref->create_object = php_bacnet_objectref_create_object;

	/* ── Bacnet\Value ────────────────────────────────────────────────── */

	memcpy(&php_bacnet_value_handlers, zend_get_std_object_handlers(),
		   sizeof(zend_object_handlers));
	php_bacnet_value_handlers.offset = XtOffsetOf(php_bacnet_value_obj, std);
	php_bacnet_value_handlers.free_obj = php_bacnet_value_free_object;
	php_bacnet_value_handlers.clone_obj = NULL;

	INIT_CLASS_ENTRY(ce, "Bacnet\\Value", bacnet_value_methods);
	bacnet_ce_value = zend_register_internal_class(&ce);
	bacnet_ce_value->create_object = php_bacnet_value_create_object;

	/* ── Bacnet\Server ──────────────────────────────────────────────── */

	memcpy(&php_bacnet_server_handlers, zend_get_std_object_handlers(),
		   sizeof(zend_object_handlers));
	php_bacnet_server_handlers.offset = XtOffsetOf(php_bacnet_server_obj, std);
	php_bacnet_server_handlers.free_obj = php_bacnet_server_free_object;
	php_bacnet_server_handlers.clone_obj = NULL;

	INIT_CLASS_ENTRY(ce, "Bacnet\\Server", bacnet_server_methods);
	bacnet_ce_server = zend_register_internal_class(&ce);
	bacnet_ce_server->create_object = php_bacnet_server_create_object;

	/* ── Bacnet\MixedServer ───────────────────────────────────────── */

	INIT_CLASS_ENTRY(ce, "Bacnet\\MixedServer", bacnet_mixed_methods);
	bacnet_ce_mixed = zend_register_internal_class_ex(&ce, bacnet_ce_server);
	bacnet_ce_mixed->create_object = php_bacnet_server_create_object;

	/* ── Bacnet\ScheduleEntry ─────────────────────────────────────── */

	INIT_CLASS_ENTRY(ce, "Bacnet\\ScheduleEntry", bacnet_schedule_entry_methods);
	bacnet_ce_schedule_entry = zend_register_internal_class(&ce);
	zend_declare_property_null(bacnet_ce_schedule_entry, "startTime", strlen("startTime"),
							   ZEND_ACC_PUBLIC);
	zend_declare_property_null(bacnet_ce_schedule_entry, "value", strlen("value"), ZEND_ACC_PUBLIC);

	/* ── Bacnet\WeeklySchedule ───────────────────────────────────── */

	INIT_CLASS_ENTRY(ce, "Bacnet\\WeeklySchedule", bacnet_weekly_schedule_methods);
	bacnet_ce_weekly_schedule = zend_register_internal_class(&ce);
	for (int i = 0; i < 7; i++) {
		zend_declare_property_null(bacnet_ce_weekly_schedule, s_day_names[i],
								   strlen(s_day_names[i]), ZEND_ACC_PUBLIC);
	}

	/* ── Bacnet\TrendLogRecord ───────────────────────────────────── */

	INIT_CLASS_ENTRY(ce, "Bacnet\\TrendLogRecord", bacnet_trend_log_record_methods);
	bacnet_ce_trend_log_record = zend_register_internal_class(&ce);
	zend_declare_property_null(bacnet_ce_trend_log_record, "timestamp", strlen("timestamp"),
							   ZEND_ACC_PUBLIC);
	zend_declare_property_null(bacnet_ce_trend_log_record, "value", strlen("value"),
							   ZEND_ACC_PUBLIC);
	zend_declare_property_long(bacnet_ce_trend_log_record, "statusFlags", strlen("statusFlags"), 0,
							   ZEND_ACC_PUBLIC);
}
