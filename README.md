# php-bacnet — BACnet Extension for PHP 8.4+

[![CI](https://img.shields.io/badge/CI-placeholder-brightgreen)](#)
[![PHP Version](https://img.shields.io/badge/PHP-8.4%2B-blue)](#)
[![License](https://img.shields.io/badge/License-BSD--3-blue)](#)

**php-bacnet** is a C extension for PHP 8.4 and 8.5 that provides native BACnet/IP communication. It wraps the [bacnet-stack](https://github.com/bacnet-stack/bacnet-stack) C library and exposes a full-featured, type-safe OOP API under the `Bacnet\` namespace.

## Overview

BACnet is a widely used building automation protocol (ASHRAE/ASHOK standard). This extension enables PHP applications to act as BACnet clients or servers on the network:

- **Client mode** — discover devices via Who-Is, read and write properties of any BACnet object
- **Server mode** — expose a BACnet device via PHP callbacks, responding to Who-Is, Read-Property, Write-Property and other confirmed requests from the network

Complex BACnet application types (`BitString`, `Date`, `Time`, `ObjectIdentifier`) are preserved as dedicated PHP classes, ensuring that the standard BACnet data semantics are never lost or ambiguously coerced into PHP's native scalar types.

## Supported Object Types (MVP)

| Object Type                              | Object ID | Notes                              |
|-------------------------------------------|-----------|-------------------------------------|
| Device                                   | 8         | Root device information             |
| Analog Input                             | 0         | Sensor read values                 |
| Analog Output                            | 1         | Actuator set-points                |
| Analog Value                             | 2         | Numeric set-points / read-back     |
| Binary Input                             | 3         | Digital sensor states              |
| Binary Output                            | 4         | Digital actuator states            |
| Binary Value                             | 5         | Switch / on-off states             |
| Multi-State Input                        | 13        | Multi-state sensor input           |
| Multi-State Output                       | 14        | Multi-state actuator output        |
| Multi-State Value                        | 19        | Multi-state set-points             |
| Event Enrollment                         | 9         | Alarm/event management             |
| Notification Class                       | 15        | Event notification configuration   |
| Schedule                                 | 17        | Time-based scheduling              |
| Trend Log                                | 20        | Historical data logging             |

## Key Features

- **OOP API** under the `Bacnet\` namespace — clean, testable PHP code
- **Type-safe read/write** via the `Bacnet\Value` class (explicit BACnet Application Tag control)
- **Complex BACnet types** — `BitString`, `Date`, `Time`, `ObjectIdentifier` preserved end-to-end
- **Device discovery** via Who-Is / I-Am broadcast
- **BACnet Server mode** — expose any BACnet object via PHP callbacks registered by the application
- **Convenience APIs** — `Schedule` and `TrendLog` helper methods for common building automation use cases
- **PHP 8.4+ and 8.5 compatible** — targets the NTS build (thread-safety not required)
- **Embedded bacnet-stack** — fast updates when upstream bug fixes or new features land

## Requirements

- PHP 8.4 or 8.5 (NTS, dev headers)
- Linux (GCC/Autotools)
- `build-essential`, `cmake`
- `bacnet-stack` (loaded as a Git submodule at `deps/bacnet-stack`)

## Quick Start

```php
<?php

use Bacnet\Client;
use Bacnet\ObjectType;
use Bacnet\PropertyId;
use Bacnet\Value;
use Bacnet\ObjectIdentifier;

// --- Device Discovery ---

$client = new Client('bacnet.ip');

$devices = $client->whoIs(
    lowLimit:  0,
    highLimit: 1000,
    timeoutMs: 3000,
);

echo "Found ", count($devices), " device(s)\n";

foreach ($devices as $device) {
    echo "Device #", $device->objectIdentifier, " at ",
         $device->address, " — ", $device->objectName, "\n";
}

// --- Read an Analog Value ---

$client = new Client('bacnet.ip', port: 47808);

$av = $client->readProperty(
    deviceIdentifier:  new ObjectIdentifier(ObjectType::Device, 200),
    objectIdentifier:   new ObjectIdentifier(ObjectType::AnalogValue, 1),
    propertyIdentifier: PropertyId::PresentValue,
);

echo "AV-1 present-value: ", $av->float(), " ", $av->units(), "\n";

// --- Write a Binary Value ---

$client->writeProperty(
    deviceIdentifier:  new ObjectIdentifier(ObjectType::Device, 200),
    objectIdentifier:   new ObjectIdentifier(ObjectType::BinaryValue, 3),
    propertyIdentifier: PropertyId::PresentValue,
    value:              Value::binaryActive(),   // Application-Tag = binary-active
    priority:           8,
);

echo "BV-3 switched to ACTIVE\n";
```

## Documentation / Development Plan

The project is built step-by-step. Each step is documented for a coding agent:

- [Step 1 — Project Setup & Build System](./docs/01_project_setup_and_build_system.md)
- [Step 2 — Extension Skeleton & Lifecycle](./docs/02_extension_skeleton_and_lifecycle.md)
- [Step 3 — C-Wrapper & Stack Integration](./docs/03_c_wrapper_and_stack_integration.md)
- [Step 4 — OOP Model & Zend Classes](./docs/04_oop_model_and_zend_classes.md)
- [Step 5 — ReadProperty & Type Mapping](./docs/05_read_property_and_type_mapping.md)
- [Step 6 — WriteProperty & Value Class](./docs/06_write_property_and_value_class.md)
- [Step 7 — Who-Is & Device Discovery](./docs/07_who_is_and_device_discovery.md)
- [Step 8 — Server Mode & Event Loop](./docs/08_server_mode_and_event_loop.md)
- [Step 9 — Advanced Object Types & Convenience APIs](./docs/09_advanced_object_types_and_convenience_apis.md)
- [Step 10 — Testing, QA & Release Prep](./docs/10_testing_qa_and_release_prep.md)

## License

BSD-3-Clause — see [LICENSE](./LICENSE). This license is compatible with the [bacnet-stack BSD-3 license](https://github.com/bacnet-stack/bacnet-stack/blob/master/LICENSE).

## Acknowledgments

- **[bacnet-stack](https://github.com/bacnet-stack/bacnet-stack)** — embedded BACnet protocol stack
- **[PHP Internals Book](https://www.phpinternalsbook.com/php7/extensions_design.html)** — foundational guide for PHP extension development
