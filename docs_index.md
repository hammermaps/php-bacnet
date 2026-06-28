# Documentation Index — php-bacnet

| Step | File                                     | Description                                                    |
|------|------------------------------------------|----------------------------------------------------------------|
| 1    | `01_project_setup_and_build_system.md`   | Repository init, bacnet-stack submodule, config.m4, first build |
| 2    | `02_extension_skeleton_and_lifecycle.md` | Extension entry point, MINIT/MSHUTDOWN, request init/reset      |
| 3    | `03_c_wrapper_and_stack_integration.md`  | BACnet-stack integration, DataLink UDP, socket lifecycle        |
| 4    | `04_oop_model_and_zend_classes.md`       | PHP classes, constants, ObjectIdentifier, BitString, Date, Time |
| 5    | `05_read_property_and_type_mapping.md`    | readProperty(), BACnet→PHP type mapping, Error handling         |
| 6    | `06_write_property_and_value_class.md`    | writeProperty(), Value class, Application Tag dispatch          |
| 7    | `07_who_is_and_device_discovery.md`       | Who-Is broadcast, I-Am handler, Device[], dedup, timeout        |
| 8    | `08_server_mode_and_event_loop.md`        | BACnet Server, callback registry, WhoIs/ReadProp handlers       |
| 9    | `09_advanced_object_types_and_convenience_apis.md` | Event Enrollment, NotificationClass, Schedule, TrendLog helpers |
| 10   | `10_testing_qa_and_release_prep.md`       | Unit tests, PHP test scripts, CI pipeline, PECL release prep    |
