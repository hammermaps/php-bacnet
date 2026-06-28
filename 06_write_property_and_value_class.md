# Step 6: WriteProperty & Value-Klasse

**Ziel:** Schreibende Zugriffe mit korrekter BACnet-Typisierung. Einführung von `Bacnet\Value` für explizite Tags.

## Aktionen

1.  **`Bacnet\Value`-Klasse definieren:**
    ```php
    namespace Bacnet;

    class Value {
        public static function boolean(bool $value): self;
        public static function unsignedInt(int $value): self;
        public static function signedInt(int $value): self;
        public static function real(float $value): self;
        public static function enumerated(int $value): self;
        public static function characterString(string $value): self;
        public static function bitString(BitString $value): self;
        public static function date(Date $value): self;
        public static function time(Time $value): self;
        public static function objectIdentifier(ObjectIdentifier  $ value): self;
    }
    ```
    - Intern speichert `php_bacnet_value_obj` eine `BACNET_APPLICATION_DATA_VALUE` und den `uint8_t tag`.
    - Factory-Methoden setzen beides und returnen ` $ this` (neues Objekt).

2.  **Write-Property Request bauen:**
    - Nutze `wp_encode_apdu()` aus `bacnet/wp.h`.
    - Fülle `BACNET_WRITE_PROPERTY_DATA`:
      -
