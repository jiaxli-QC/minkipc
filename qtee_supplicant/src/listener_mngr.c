// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include <dlfcn.h>
#include <fcntl.h>
#include <stdbool.h>
#include <unistd.h>

#include "listener_mngr.h"

#define MAX_LISTENERS 8

/*
 * Bounded retry around the listener's init() callback. The QTEE-side memory
 * registration path (MinkCom_getMemoryObject -> qcomtee_memory_object_alloc ->
 * kernel qcom_scm SHM Bridge create) can return transient -EINVAL on early
 * boot while QTEE/SCM finishes coming up. A short, bounded retry lets these
 * settle without forcing systemd into a fast-restart loop.
 */
#define LISTENER_INIT_MAX_RETRIES 3
#define LISTENER_INIT_RETRY_DELAY_MS 200
static struct listener_svc listeners[] = {
#ifdef TIME_LISTENER
	{
		.service_name = "Time service",
		.is_registered = false,
		.file_name = "libtimeservice.so.1",
		.lib_handle = NULL,
		.critical = false,
	},
#endif
#ifdef TA_AUTOLOAD_LISTENER
	{
		.service_name = "TA Autoload service",
		.is_registered = false,
		.file_name = "libtaautoload.so.1",
		.lib_handle = NULL,
		.critical = false,
	},
#endif
#ifdef FS_LISTENER
	{
		.service_name = "FS service",
		.is_registered = false,
		.file_name = "libfsservice.so.1",
		.lib_handle = NULL,
		.critical = false,
	},
#endif
#ifdef GPFS_LISTENER
	{
		.service_name = "GPFS service",
		.is_registered = false,
		.file_name = "libgpfsservice.so.1",
		.lib_handle = NULL,
		.critical = false,
	},
#endif
#ifdef RPMB_LISTENER
	{
		.service_name = "rpmb service",
		.is_registered = false,
		.file_name = "librpmbservice.so.1",
		.lib_handle = NULL,
		.critical = false,
	},
#endif
};

/**
 * @brief De-initialize a listener service.
 *
 * De-initialize a listener service by invoking the de-init callback defined by
 * the listener.
 */
static void deinit_listener_svc(size_t i)
{
	listener_deinit deinit_func;

	deinit_func = (listener_deinit)dlsym(listeners[i].lib_handle,
					     "deinit");
	if (deinit_func == NULL) {
		MSGE("dlsym(deinit) not found in lib %s: %s\n",
		listeners[i].file_name, dlerror());
		return;
	}

	(*deinit_func)();
}

/**
 * @brief Stop listener services.
 *
 * Stops all listener services waiting for a listener request from QTEE.
 *
 * @param call_deinit  When true, invoke each registered listener's deinit()
 *                     callback before closing its library handle (normal
 *                     teardown). When false, skip the deinit() callbacks and
 *                     only close the library handles.
 *
 * The deinit() callbacks issue remote Object_release calls over the
 * MINK/smcinvoke transport. On the failed-startup rollback path the transport
 * may already be in a broken/unusable state (the very fault that aborted
 * startup, e.g. a missing RPMB device node or a down QTEE-MINK channel).
 * Releasing over a dead transport dereferences a stale invoke/context pointer
 * and crashes the process with SIGSEGV. Since main() returns immediately after
 * rollback and the process exits, QTEE reclaims this client's listener
 * registrations on process death, so skipping deinit() on that path is safe.
 */
static void stop_listeners_services(bool call_deinit)
{
	size_t idx = 0;
	size_t n_listeners = sizeof(listeners)/sizeof(struct listener_svc);

	MSGD("Total listener services to be stopped = %zu\n", n_listeners);

	for (idx = 0; idx < n_listeners; idx++) {
		/* Resource cleanup for registered listeners */
		if(listeners[idx].is_registered) {
			if (call_deinit) {
				deinit_listener_svc(idx);
			}

			listeners[idx].is_registered = false;
		}

		/* Close lib_handle for all listeners */
		if(listeners[idx].lib_handle != NULL) {
			dlclose(listeners[idx].lib_handle);
			listeners[idx].lib_handle = NULL;
		}
	}
}

/**
 * @brief Initialize a listener service.
 *
 * Initialize a listener service by invoking the init callback defined by
 * the listener. A bounded retry is wrapped around init() so that transient
 * QTEE/SCM SHM Bridge failures during early boot (e.g. kernel logs
 * "qcom_scm SHM Bridge failed: ret -22") do not immediately tear down the
 * supplicant.
 *
 * Cleanup contract: on ANY failure path inside this function (dlopen,
 * dlsym, or init() exhausted retries) the lib_handle is closed and reset
 * to NULL here. Callers MUST NOT additionally dlclose() lib_handle on the
 * failure return; doing so would double-free. On success, lib_handle is
 * left open for use by stop_listeners_services()/deinit_listener_svc().
 *
 * The retry loop relies on init() being idempotent on its own error path —
 * see the contract documented on struct listener_svc in listener_mngr.h.
 */
static int init_listener_svc(size_t i)
{
	listener_init init_func;
	int ret = -1;
	int attempt = 0;

	listeners[i].lib_handle = dlopen(listeners[i].file_name,
					 RTLD_NOW);
	if (listeners[i].lib_handle == NULL) {
		MSGE("dlopen(%s, RLTD_NOW) failed: %s\n",
		     listeners[i].file_name, dlerror());
		return -1;
	}

	init_func = (listener_init)dlsym(listeners[i].lib_handle,
					 "init");
	if (init_func == NULL) {
		MSGE("dlsym(init) not found in lib %s: %s\n",
		     listeners[i].file_name, dlerror());
		dlclose(listeners[i].lib_handle);
		listeners[i].lib_handle = NULL;
		return -1;
	}

	for (attempt = 0; attempt < LISTENER_INIT_MAX_RETRIES; attempt++) {
		ret = (*init_func)();
		if (ret == 0)
			break;

		MSGE("Init for %s failed (attempt %d/%d): %d\n",
		     listeners[i].service_name,
		     attempt + 1, LISTENER_INIT_MAX_RETRIES, ret);

		if (attempt + 1 < LISTENER_INIT_MAX_RETRIES)
			usleep(LISTENER_INIT_RETRY_DELAY_MS * 1000);
	}

	if (ret < 0) {
		MSGE("Init for %s failed after %d attempts: %d\n",
		     listeners[i].service_name,
		     LISTENER_INIT_MAX_RETRIES, ret);
		dlclose(listeners[i].lib_handle);
		listeners[i].lib_handle = NULL;
		return -1;
	}

	/*
	 * is_registered is owned by listener_mngr.c (see header contract).
	 * Set it here so stop_listeners_services() can use it as the gate for
	 * calling deinit() on this listener during normal teardown.
	 */
	listeners[i].is_registered = true;

	return ret;
}

int start_listener_services(void)
{
	int ret = 0;
	size_t idx = 0;
	size_t n_listeners = sizeof(listeners)/sizeof(struct listener_svc);
	size_t n_ok = 0;
	size_t n_failed = 0;
	size_t n_critical_failed = 0;

	MSGD("Total listener services to start = %zu\n", n_listeners);

	if (n_listeners == 0) {
		/*
		 * No listener compiled in. This is a legal build configuration
		 * (e.g. supplicant-framework-only bring-up / minimal images).
		 * Preserve the legacy behaviour of returning success so that
		 * systemd does not enter a fast-restart loop on such builds,
		 * but make the situation obvious in the log so an operator
		 * does not mistake it for a runtime fault.
		 */
		MSGE("WARN: no listener services compiled in; "
		     "qtee_supplicant has nothing to register with QTEE\n");
		return 0;
	}

	for (idx = 0; idx < n_listeners; idx++) {

		ret = init_listener_svc(idx);
		if (ret) {
			MSGE("init_listener_svc(%s) failed: 0x%x; "
			     "skipping this listener and continuing "
			     "(critical=%d)\n",
			     listeners[idx].service_name, ret,
			     (int)listeners[idx].critical);
			/*
			 * init_listener_svc() owns the lib_handle cleanup on
			 * its failure path (see its cleanup contract). Do not
			 * dlclose() here — that would double-free.
			 */
			listeners[idx].is_registered = false;
			n_failed++;
			if (listeners[idx].critical)
				n_critical_failed++;
			continue;
		}
		n_ok++;
	}

	MSGD("Listener startup summary: %zu ok, %zu failed "
	     "(of %zu; %zu critical failures)\n",
	     n_ok, n_failed, n_listeners, n_critical_failed);

	if (n_critical_failed > 0) {
		/*
		 * A listener marked critical=true could not be registered.
		 * Surface this as a hard startup failure so the fault is
		 * visible at boot (in syslog, via systemd unit state) rather
		 * than being deferred to TA invoke time, where the user-
		 * visible symptom is just "RPMB-backed TA failed at runtime".
		 */
		MSGE("ERROR: %zu critical listener service(s) failed; "
		     "aborting supplicant startup\n", n_critical_failed);
		stop_listeners_services(false);
		return -1;
	}

	if (n_ok == 0) {
		/*
		 * No critical listener was configured, but every non-critical
		 * listener still failed. The supplicant has nothing to offer
		 * to QTEE. Tear down any half-opened state (no deinit() — see
		 * stop_listeners_services() for rationale) and let main()
		 * exit so systemd retries with back-off.
		 */
		MSGE("ERROR: all listener services failed to start\n");
		stop_listeners_services(false);
		return -1;
	}

	if (n_failed > 0) {
		/*
		 * Some non-critical listeners failed but at least one is up.
		 * Log a clearly-prefixed WARN so this partial-registration
		 * state is greppable in syslog; downstream TAs depending on
		 * the missing listener(s) will see invoke-time errors.
		 */
		MSGE("WARN: %zu non-critical listener(s) failed to register; "
		     "TAs depending on them will fail at invoke time\n",
		     n_failed);
	}

	/*
	 * At least one listener is up and no critical listener failed.
	 * Continue running so QTEE can use the listeners that did register.
	 */
	return 0;
}
