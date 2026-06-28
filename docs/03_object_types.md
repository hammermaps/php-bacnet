# 3. BACNet Standard-Objekt-Typen

Basierend auf [BACNet Stack Referenz](https://github.com/bacnet-stack)

Implementierung der **wichtigsten Standard-Objekt-Typen** für Anlagenkommunikation.

## Übersicht

| Typ ID | Name | Beschreibung | Beispiel |
|--------|------|-------------|----------|
| 0 | Analog Input | Digitalisierter Messwert | Temperatur, Druck |
| 1 | Analog Output | Setpunkt / Steuergröße | Ventil-Position (0-100%) |
| 2 | Analog Value | Nicht-I/O Wert | Berechnung, Konstante |
| 3 | Binary Input | Digitaler Eingang | Schalter, Türsensor |
| 4 | Binary Output | Digitaler Ausgang | Pumpe On/Off |
| 5 | Binary Value | Nicht-I/O Binärwert | Status-Flag |
| 13 | Multi-state Input | Mehrwert-Eingang | 3-Punkt-Schalter |
| 14 | Multi-state Output | Mehrwert-Ausgang | Mode Select (0=Off, 1=Heat, 2=Cool) |
| 19 | Device | BACNet Device | Anlage selbst |

## Object Identifier (OID)

```c
// Struktur
typedef struct {
    uint16_t type;        // Object Type (0-19, etc.)
    uint32_t instance;    // Instance Number (0-4194303)
} BACNet_Object_Identifier;

// Encoding: 4 Bytes
// Bits 31-22: Type (10 bits)
// Bits 21-0:  Instance (22 bits)

// Beispiele:
// Analog Input #0:    type=0, instance=0
// Binary Output #42:  type=4, instance=42
// Device #1234567:    type=19, instance=1234567
```

## Standard Properties

Alle Objekte haben folgende **Kern-Properties**:

```c
typedef enum {
    PROP_OBJECT_IDENTIFIER    = 75,   // Read-only, OID
    PROP_OBJECT_NAME          = 77,   // Read-Write, String
    PROP_OBJECT_TYPE          = 79,   // Read-only, Enumeration
    PROP_DESCRIPTION          = 28,   // Read-Write, String
    PROP_PRESENT_VALUE        = 85,   // meist Read-Write
    PROP_STATUS_FLAGS         = 111,  // Read-only, Bitstring
    PROP_EVENT_STATE          = 36,   // Read-only, Enumeration
    PROP_OUT_OF_SERVICE       = 81,   // Read-Write, Boolean
    PROP_UNITS                = 117,  // Read-only, Enumeration (nur Analog)
} BACNET_PROPERTY_ID;
```

## Object Type Details

### 1. Analog Input (Type 0)

Messwert aus der Anlage.

```
Properties:
├─ present_value (float, Read-Only)      // Aktueller Messwert
├─ units (enum, Read-Only)               // Physikalische Einheit (°C, Pa, etc.)
├─ object_name (string)                  // Name: "Außentemperatur"
├─ description (string)                  // Beschreibung
└─ status_flags (bitstring, Read-Only)   // Quality: online/offline, etc.

Beispiel Werte (units):
- 0 = Meter
- 73 = Degrees Celsius
- 78 = Pascal
- 94 = Percent Relative Humidity
```

**C Struktur:**
```c
typedef struct {
    BACNet_Object_Identifier oid;
    float present_value;
    uint16_t units;
    char object_name[64];
    char description[128];
} BACNet_Analog_Input;
```

### 2. Analog Output (Type 1)

Steuergröße zur Regelung.

```
Properties:
├─ present_value (float, Read-Write)     // Sollwert (z.B. Ventil 0-100%)
├─ units (enum, Read-Only)               // Einheit
├─ object_name (string)
├─ description (string)
├─ relinquish_default (float)            // Fallback-Wert
└─ status_flags (bitstring)

Typische Werte:
- 0 bis 100% für Proportional-Ventile
- 0-10V für analoge Signale
```

**C Struktur:**
```c
typedef struct {
    BACNet_Object_Identifier oid;
    float present_value;
    float relinquish_default;
    uint16_t units;
    char object_name[64];
} BACNet_Analog_Output;
```

### 3. Binary Input (Type 3)

Digitaler Eingang (An/Aus).

```
Properties:
├─ present_value (enum: INACTIVE=0 / ACTIVE=1, Read-Only)
├─ object_name (string)
├─ description (string)
├─ status_flags (bitstring)
└─ inactive_text (string)       // Label für 0
    active_text (string)        // Label für 1

Beispiele:
- Türkontakt: ACTIVE=offen, INACTIVE=geschlossen
- Leck-Melder: ACTIVE=Leck erkannt, INACTIVE=OK
- Bewegungsmelder: ACTIVE=Bewegung, INACTIVE=Keine Bewegung
```

**C Struktur:**
```c
typedef struct {
    BACNet_Object_Identifier oid;
    uint8_t present_value;  // 0=INACTIVE, 1=ACTIVE
    char object_name[64];
    char inactive_text[32];
    char active_text[32];
} BACNet_Binary_Input;
```

### 4. Binary Output (Type 4)

Digitaler Ausgang (Steuerschalter).

```
Properties:
├─ present_value (enum: INACTIVE=0 / ACTIVE=1, Read-Write)
├─ object_name (string)
├─ description (string)
├─ relinquish_default (enum)        // Fallback-Zustand
├─ inactive_text (string)
├─ active_text (string)
└─ status_flags (bitstring)

Beispiele:
- Pumpe: ACTIVE=Ein, INACTIVE=Aus
- Heizung: ACTIVE=Heizen, INACTIVE=Aus
- Alarm: ACTIVE=Alarm aktiv, INACTIVE=Kein Alarm
```

**C Struktur:**
```c
typedef struct {
    BACNet_Object_Identifier oid;
    uint8_t present_value;  // Schreib-/Lesbar
    uint8_t relinquish_default;
    char object_name[64];
} BACNet_Binary_Output;
```

### 5. Multi-state Input (Type 13)

Mehrwert-Sensor (mehr als 2 Zustände).

```
Properties:
├─ present_value (uint32, Read-Only)     // 1, 2, 3, ...
├─ number_of_states (uint16, Read-Only)  // z.B. 4 Zustände
├─ state_text_0 (string)                 // Labels für Zustände
├─ state_text_1 (string)
├─ state_text_2 (string)
├─ state_text_3 (string)
└─ description (string)

Beispiele (number_of_states=4):
- Betriebsmodus: 1=Aus, 2=Standby, 3=Aktiv, 4=Fehler
- Fahrstuhl: 1=EG, 2=1.OG, 3=2.OG, 4=3.OG
```

**C Struktur:**
```c
typedef struct {
    BACNet_Object_Identifier oid;
    uint32_t present_value;
    uint16_t number_of_states;
    char **state_text;  // Array of strings
    char object_name[64];
} BACNet_Multi_state_Input;
```

### 6. Multi-state Output (Type 14)

Mehrwert-Stellglied (Modus-Selektor).

```
Properties:
├─ present_value (uint32, Read-Write)
├─ number_of_states (uint16, Read-Only)
├─ state_text_* (string)
├─ relinquish_default (uint32)
└─ description (string)

Beispiele:
- Lüftermodus: 1=Aus, 2=Niedrig, 3=Mittel, 4=Hoch
- Betriebsmodus: 1=Heizen, 2=Kühlen, 3=Automatik
```

**C Struktur:**
```c
typedef struct {
    BACNet_Object_Identifier oid;
    uint32_t present_value;  // Schreib-/Lesbar
    uint16_t number_of_states;
    char **state_text;
    uint32_t relinquish_default;
} BACNet_Multi_state_Output;
```

### 7. Device (Type 19)

Die Anlage selbst.

```
Properties:
├─ object_identifier (OID, Read-Only)
├─ object_name (string)
├─ model_name (string)                   // z.B. "Honeywell VAV-123"
├─ firmware_revision (string)            // z.B. "2.4.1"
├─ application_software_version (string)
├─ protocol_version (uint32)
├─ protocol_revision (uint32)
├─ max_apdu_length_accepted (uint16)
├─ segmentation_supported (enum)         // NONE, TRANSMIT, RECEIVE, BOTH
├─ apdu_timeout (uint32, ms)
└─ number_of_apdu_retries (uint8)

Beispiel Device:
- Device #1234567
- Name: "Heizzentrale Block A"
- Modell: "Siemens S7-1200"
- Max APDU: 1024 Bytes
```

**C Struktur:**
```c
typedef struct {
    BACNet_Object_Identifier oid;
    char object_name[64];
    char model_name[64];
    char firmware_revision[32];
    uint32_t max_apdu_length;
    uint16_t segmentation_supported;
} BACNet_Device;
```

## Property Access Patterns

### Read-Only Properties
```
Verwendung: Sensoren lesen, Status abrufen
Beispiele:
- Analog Input: present_value (Messwert)
- Binary Input: present_value (Schalter-Status)
- Device: firmware_revision
- Alle: object_type, object_identifier

Fehlerbehandlung:
- ABORT_REASON_SEGMENTATION_NOT_SUPPORTED
- ABORT_REASON_UNSUPPORTED_TAG_NUMBER
- REJECT_REASON_UNDEFINED_ENUMERATION
```

### Read-Write Properties
```
Verwendung: Setpoints schreiben, Modi ändern
Beispiele:
- Analog Output: present_value (Ventil stellen)
- Binary Output: present_value (Schalter aktivieren)
- Multi-state Output: present_value (Modus ändern)
- Alle: object_name, description

Priorität-Array (optional):
- 1-8: Verschiedene Prioritäten (nicht implementiert zunächst)
```

## Property Write Priority

BACNet unterstützt Prioritäts-Stacking für Schreibvorgänge:

```
Priorität  Typischer Zweck
1          Manual Life-Safety Override
2          Life Safety Default
3          Application Life-Safety
4          High
5          High
6          High
7          High
8          Normal / Default
9          Operator Override
10         Program
11         Unoccupied Program
12         System Default
13         ---
14         ---
15         Unoccupied Command
16         (Lowest)
```

**Initialemplementierung:** Nur Priorität 8 (Normal).

## Encoding im BACNet Protokoll

### BACNet Tag Format

```
Byte 0:    [Class (1 bit)] [Tag (4 bits)] [Length (3 bits)]
Bytes 1-N: Encoded Value

Beispiel - Real (float):
Tag: 0x4A = Class=0, Tag=4 (Real), Length=4
Value: 4 Bytes IEEE 754 Float
```

### Object Identifier Encoding

```
2 Bytes:
Bits 15-14: Reserved (0)
Bits 13-6:  Object Type (8 bits)
Bits 5-0:   Instance High (6 bits)

2 Bytes:
Bits 15-0:  Instance Low (16 bits)

Total: Type (0-255) + Instance (0-262143)
```

## Implementation Checklist

- [ ] Alle 7 Object Types definieren (C Structs)
- [ ] Property ID Mapping (ID → Name)
- [ ] Encoding/Decoding für jeden Type
- [ ] Unit Mappings (°C, Pa, %, etc.)
- [ ] Status Flags Interpretation
- [ ] Read-Only vs Read-Write Validierung
- [ ] Error Codes für ungültige Operations
- [ ] Tests mit echten BACNet-Geräten
