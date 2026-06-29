#include "php.h"
#include "../php_bacnet.h"
#include "bacnet_classes.h"

/* Class entry globals */
zend_class_entry *bacnet_ce_client           = NULL;
zend_class_entry *bacnet_ce_device           = NULL;
zend_class_entry *bacnet_ce_object_ref       = NULL;
zend_class_entry *bacnet_ce_object_identifier = NULL;
zend_class_entry *bacnet_ce_bit_string       = NULL;
zend_class_entry *bacnet_ce_date             = NULL;
zend_class_entry *bacnet_ce_time             = NULL;
zend_class_entry *bacnet_ce_value            = NULL;
zend_class_entry *bacnet_ce_object_type_enum = NULL;
zend_class_entry *bacnet_ce_property_enum    = NULL;
zend_class_entry *bacnet_ce_exception        = NULL;
zend_class_entry *bacnet_ce_timeout_exception = NULL;
zend_class_entry *bacnet_ce_device_exception = NULL;

/* Stub — full implementation in Phase 3 */
void php_bacnet_register_classes(void)
{
    /* Phase 3: register all Zend classes, enums, exceptions */
}
