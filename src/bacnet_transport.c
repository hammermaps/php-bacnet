#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include <string.h>

#ifdef PHP_WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "bacnet/datalink/bip.h"

#include "../php_bacnet.h"
#include "bacnet_transport.h"

typedef struct {
#ifdef PHP_WIN32
	CRITICAL_SECTION mutex;
#else
	pthread_mutex_t mutex;
#endif
	bool mutex_initialized;
	uint32_t references;
	uint16_t port;
	char *interface_name;
	bool active;
} php_bacnet_transport_state;

static php_bacnet_transport_state php_bacnet_transport = {0};

bool php_bacnet_transport_startup(void) {
	if (php_bacnet_transport.mutex_initialized)
		return true;
#ifdef PHP_WIN32
	InitializeCriticalSection(&php_bacnet_transport.mutex);
	php_bacnet_transport.mutex_initialized = true;
	return true;
#else
	if (pthread_mutex_init(&php_bacnet_transport.mutex, NULL) != 0)
		return false;
	php_bacnet_transport.mutex_initialized = true;
	return true;
#endif
}

void php_bacnet_transport_shutdown(void) {
	if (!php_bacnet_transport.mutex_initialized)
		return;
	php_bacnet_transport_lock();
	if (php_bacnet_transport.active)
		bip_cleanup();
	php_bacnet_transport.active = false;
	php_bacnet_transport.references = 0;
	if (php_bacnet_transport.interface_name) {
		pefree(php_bacnet_transport.interface_name, 1);
		php_bacnet_transport.interface_name = NULL;
	}
	php_bacnet_transport_unlock();
#ifdef PHP_WIN32
	DeleteCriticalSection(&php_bacnet_transport.mutex);
#else
	pthread_mutex_destroy(&php_bacnet_transport.mutex);
#endif
	php_bacnet_transport.mutex_initialized = false;
}

void php_bacnet_transport_lock(void) {
	if (!php_bacnet_transport.mutex_initialized)
		return;
#ifdef PHP_WIN32
	EnterCriticalSection(&php_bacnet_transport.mutex);
#else
	pthread_mutex_lock(&php_bacnet_transport.mutex);
#endif
}

void php_bacnet_transport_unlock(void) {
	if (!php_bacnet_transport.mutex_initialized)
		return;
#ifdef PHP_WIN32
	LeaveCriticalSection(&php_bacnet_transport.mutex);
#else
	pthread_mutex_unlock(&php_bacnet_transport.mutex);
#endif
}

bool php_bacnet_transport_acquire(const char *iface, uint16_t port, int *socket_fd,
								  char **error_message) {
	const char *requested_interface = iface ? iface : "";
	bool success = false;
	php_bacnet_transport_lock();
	if (php_bacnet_transport.active) {
		if (php_bacnet_transport.port != port ||
			strcmp(php_bacnet_transport.interface_name, requested_interface)) {
			if (error_message)
				*error_message =
					estrdup("BACnet transport is already bound to another interface or port");
			goto done;
		}
		php_bacnet_transport.references++;
		*socket_fd = bip_get_socket();
		success = true;
		goto done;
	}
	bip_set_port(port);
	char *bip_iface = *requested_interface ? estrdup(requested_interface) : NULL;
	if (!bip_init(bip_iface)) {
		efree(bip_iface);
		if (error_message)
			*error_message = estrdup("bip_init failed");
		goto done;
	}
	efree(bip_iface);
	php_bacnet_transport.interface_name = pestrdup(requested_interface, 1);
	php_bacnet_transport.port = port;
	php_bacnet_transport.references = 1;
	php_bacnet_transport.active = true;
	*socket_fd = bip_get_socket();
	success = true;
done:
	php_bacnet_transport_unlock();
	return success;
}

void php_bacnet_transport_release(void) {
	php_bacnet_transport_lock();
	if (php_bacnet_transport.references && --php_bacnet_transport.references == 0 &&
		php_bacnet_transport.active) {
		bip_cleanup();
		php_bacnet_transport.active = false;
		if (php_bacnet_transport.interface_name) {
			pefree(php_bacnet_transport.interface_name, 1);
			php_bacnet_transport.interface_name = NULL;
		}
	}
	php_bacnet_transport_unlock();
}
