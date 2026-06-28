# Step 5: ReadProperty & Datentyp-Mapping

**Ziel:** `Device::readProperty()` und `ObjectRef::readProperty()` vollständig implementieren. Rückgabe primitiver PHP-Typen oder komplexer BACnet-Klassen (`BitString`, `Date`, `Time`, `ObjectIdentifier`).

## Aktionen

1.  **Read-Property Request bauen:**
    - Nutze `rp_encode_apdu()` aus `bacnet/rp.h`.
    - Parameter: Buffer, Buffer-Size, `invoke_id`, `BACNET_READ_PROPERTY_DATA` (object_type, object_instance, property, array_index).
    - Array-Index: 0 = nicht gesetzt (BACNET_ARRAY_ALL). Wenn PHP-Parameter `?int $arrayIndex = null` gesetzt, verwenden.

2.  **Senden & Empfangen:**
    - Nutze `php_bacnet_send_and_wait()` aus Step 3.
    - `expected_invoke_id` muss mit dem gesendeten übereinstimmen (aus `BACNET_G(next_invoke_id)` vor dem Senden holen und post-incrementieren).

3.  **Response decodieren:**
    - Bei `PDU_TYPE_COMPLEX_ACK`:
      - `rp_ack_decode_service_request()` -> `BACNET_READ_PROPERTY_DATA`.
      - `bacapp_decode_application_data()` iteriert über `application_data`.
    - Bei `PDU_TYPE_ERROR`:
      - `error_decode_service_request()` -> `error_class`, `error_code`.
      - Werfe `Bacnet\DeviceException` mit diesen Werten.
    - Bei `PDU_TYPE_REJECT` oder `PDU_TYPE_ABORT`:
      - Werfe ebenfalls `Bacnet\DeviceException` mit sinnvoller Nachricht.

4.  **Mapping `bacapp_value_to_zval`:**
    Implementiere in `src/bacnet_helpers.c`:
    ```c
    void bacapp_value_to_zval(BACNET_APPLICATION_DATA_VALUE *val, zval *zv)
    ```
    | BACnet Tag | PHP Rückgabe |
    |------------|--------------|
    | BOOLEAN | `IS_TRUE` / `IS_FALSE` |
    | UNSIGNED_INT | `IS_LONG` |
    | SIGNED_INT | `IS_LONG` |
    | REAL | `IS_DOUBLE` |
    | ENUMERATED | `IS_LONG` |
    | CHARACTER_STRING | `IS_STRING` (aus `characterstring_value()`) |
    | OCTET_STRING | `IS_STRING` (binär) |
    | BIT_STRING | `new Bacnet\BitString(array_of_bools)` |
    | DATE | `new Bacnet\Date(y,m,d,wday)` (0xFF = wildcard) |
    | TIME | `new Bacnet\Time(h,min,s,hs)` |
    | OBJECT_IDENTIFIER | `new Bacnet\ObjectIdentifier(ObjectType::X, instance)` |
    
    Für Sequenzen (z. B. `Object_List`): Sammle in `zend_array` (IS_ARRAY).

5.  **`BitString`-Klasse:**
    - Constructor akzeptiert `array<int, bool>`.
    - Properties: readonly `length`, Methoden `getBit(int $index): bool`, `toArray(): array`.
    - Intern speichert `BACNET_BIT_STRING` oder einfach ein `char *bits` + `uint8_t bits_used`.

6.  **`Date` / `Time`-Klassen:**
    - `Date::__construct(int $year, int $month, int $day, int $weekday)` (0xFF für wildcard erlaubt).
    - `Time::__construct(int $hour, int $minute, int $second, int $hundredths)`.
    - Readonly Properties für alle Felder.

7.  **`ObjectIdentifier`-Klasse:**
    - `__construct(ObjectType $type, int $instance)`.
    - Readonly `type` und `instance`.

8.  **PHP-Methoden:**
    - `ZEND_METHOD(Bacnet_Device, readProperty)`:
      `readProperty(ObjectType $objectType, int $instance, Property $property, ?int $arrayIndex = null): mixed`
    - `ZEND_METHOD(Bacnet_ObjectRef, readProperty)`:
      `readProperty(Property $property, ?int $arrayIndex = null): mixed` (nutzt gespeicherte ObjectType/Instance).

## Akzeptanzkriterien

- [ ] Lesen von `AnalogValue.PRESENT_VALUE` returned `float` (REAL).
- [ ] Lesen von `Device.OBJECT_LIST` returned ein `array` aus `Bacnet\ObjectIdentifier`-Objekten.
- [ ] Lesen von `BinaryValue.PRESENT_VALUE` returned `int` (ENUMERATED).
- [ ] Bei Timeout nach 3000ms wird `Bacnet\TimeoutException` geworfen.
- [ ] Bei BACnet-Error (z. B. `unknown-object`) wird `Bacnet\DeviceException` mit korrekter `errorClass`/`errorCode` geworfen.
