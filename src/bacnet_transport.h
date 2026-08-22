#ifndef PHP_BACNET_TRANSPORT_H
#define PHP_BACNET_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "php.h"

/* The bundled bacnet-stack keeps B/IP state process-global. This manager
 * serializes access and keeps its socket alive until the final PHP object is
 * released. It intentionally contains no PHP zvals or request-owned state. */
bool php_bacnet_transport_startup(void);
void php_bacnet_transport_shutdown(void);
bool php_bacnet_transport_acquire(const char *iface, uint16_t port, int *socket_fd,
								  char **error_message);
void php_bacnet_transport_release(void);
void php_bacnet_transport_lock(void);
void php_bacnet_transport_unlock(void);

#endif
