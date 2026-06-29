# Step 4: OOP-Modell & Zend-Klassen registrieren

**Ziel:** Alle PHP-Klassen, Enums und Exceptions in `MINIT` registrieren. Jedes Objekt mit internem Zustand erhält korrekte `zend_object_handlers`.

## Aktionen

1.  **Klassen registrieren (in `bacnet_classes.c` -> `php_bacnet_register_classes()`):**
    
    ```c
    /* Normale Klassen */
    zend_class_entry *bacnet_ce_client = zend_register_internal_class(...)
    zend_class_entry *bacnet_ce_device = zend_register_internal_class(...)
    zend_class_entry *bacnet_ce_object_ref = zend_register_internal_class(...)
    zend_class_entry *bacnet_ce_object_identifier = zend_register_internal_class(...)
    zend_class_entry *bacnet_ce_bit_string = zend_register_internal_class(...)
    zend_class_entry *bacnet_ce_date = zend_register_internal_class(...)
    zend_class_entry *bacnet_ce_time = zend_register_internal_class(...)
    zend_class_entry *bacnet_ce_value = zend_register_internal_class(...)
    
    /* Enums */
    zend_class_entry *bacnet_ce_object_type_enum = zend_register_internal_enum(
        "Bacnet\\ObjectType", IS_LONG, bacnet_object_type_enum_cases
    );
    // Analog für Bacnet\\Property
    
    /* Exceptions */
    zend_class_entry bacnet_ce_exception = zend_register_internal_class_ex(
        "Bacnet\\Exception", zend_ce_exception
    );
    bacnet_ce_timeout_exception = zend_register_internal_class_ex(
        "Bacnet\\TimeoutException", bacnet_ce_exception
    );
    bacnet_ce_device_exception = zend_register_internal_class_ex(
        "Bacnet\\DeviceException", bacnet_ce_exception
    );
    ```
    
    > `DeviceException` erhält zusätzliche Properties `errorClass` und `errorCode` (über `zend_declare_property_long`).

2.  **Enum-Cases definieren:**

    `Bacnet\ObjectType`:
    - `ANALOG_INPUT = 0`
    - `ANALOG_OUTPUT = 1`
    - `ANALOG_VALUE = 2`
    - `BINARY_INPUT = 3`
    - `BINARY_OUTPUT = 4`
    - `BINARY_VALUE = 5`
    - `DEVICE = 8`
    - `EVENT_ENROLLMENT = 9`
    - `MULTI_STATE_INPUT = 13`
    - `MULTI_STATE_OUTPUT = 14`
    - `NOTIFICATION_CLASS = 15`
    - `SCHEDULE = 17`
    - `MULTI_STATE_VALUE = 19`
    - `TREND_LOG = 20`

    `Bacnet\Property` (Auswahl der wichtigsten):
    - `OBJECT_IDENTIFIER = 75`
    - `OBJECT_NAME = 77`
    - `OBJECT_TYPE = 79`
    - `DESCRIPTION = 28`
    - `PRESENT_VALUE = 85`
    - `STATUS_FLAGS = 111`
    - `EVENT_STATE = 36`
    - `OUT_OF_SERVICE = 81`
    - `UNITS = 117`
    - `NUMBER_OF_STATES = 74`
    - `STATE_TEXT = 110`
    - `NOTIFICATION_CLASS = 17`

3.  **Zend Object Handlers für jeden internen State-Typ:**

    - `php_bacnet_client_obj`:
      ```c
      typedef struct {
          php_bacnet_client *client;
          zend_object std;
      } php_bacnet_client_obj;
      ```
    - `php_bacnet_device_obj`:
      ```c
      typedef struct {
          BACNET_ADDRESS address;
          uint32_t device_id;
          zval client_zval; // ZVAL_COPY des Parent-Clients
          zend_object std;
      } php_bacnet_device_obj;
      ```
    - `php_bacnet_object_ref_obj`:
      ```c
      typedef struct {
          BACNET_OBJECT_TYPE object_type;
          uint32_t instance;
          zval device_zval; // ZVAL_COPY des Parent-Devices
          zend_object std;
      } php_bacnet_object_ref_obj;
      ```
    - `php_bacnet_object_identifier_obj`, `php_bacnet_bit_string_obj`, `php_bacnet_date_obj`, `php_bacnet_time_obj`, `php_bacnet_value_obj`:
      Definiere analog mit ihren C-Entsprechungen.

4.  **Handler-Implementierungen:**
    - Für jede Klasse: `create_object` und `free_obj`.
    - `free_obj` muss:
      - `php_bacnet_client_destroy` aufrufen (bei Client)
      - `zval_ptr_dtor(&client_zval)` / `zval_ptr_dtor(&device_zval)` aufrufen (Parent-Referenzen freigeben)
      - `zend_object_std_dtor(&obj->std)` aufrufen.

5.  **Methoden-Stubs mit `ZEND_BEGIN_ARG_INFO_EX`:**
    - Alle Methoden mit korrekten Typ-Hinweisen (aus PHP 8.4) als Stubs deklarieren. Body zunächst `RETURN_NULL()` oder `zend_throw_error(NULL, "Not implemented")`.

## Akzeptanzkriterien

- [ ] `php -d extension=modules/bacnet.so -r "var_dump(enum_exists('Bacnet\\ObjectType')); "` -> `bool(true)`.
- [ ] `new Bacnet\Client()`, `new Bacnet\ObjectIdentifier(Bacnet\ObjectType::ANALOG_VALUE, 1)` erzeugt Objekte ohne Crash.
- [ ] `Bacnet\DeviceException::class` ist abrufbar und hat Properties `errorClass`, `errorCode`.
- [ ] `Bacnet\ObjectType::SCHEDULE->value` ist `17`.
