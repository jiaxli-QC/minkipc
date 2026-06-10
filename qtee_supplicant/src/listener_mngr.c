// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include <dlfcn.h>
#include <fcntl.h>
#include <stdbool.h>
#include <unistd.h>

#include "listener_mngr.h"

#define MAX_LISTENERS 8
static struct listener_svc listeners[] = {
#ifdef TIME_LISTENER
	{
		.service_name = "Time service",
		.is_registered = false,
		.file_name = "libtimeservice.so.1",
		.lib_handle = NULL,
	},
#endif
#ifdef TA_AUTOLOAD_LISTENER
	{
		.service_name = "TA Autoload service",
		.is_registered = false,
		.file_name = "libtaautoload.so.1",
		.lib_handle = NULL,
	},
#endif
#ifdef FS_LISTENER
	{
		.service_name = "FS service",
		.is_registered = false,
		.file_name = "libfsservice.so.1",
		.lib_handle = NULL,
	},
#endif
#ifdef GPFS_LISTENER
	{
		.service_name = "GPFS service",
		.is_registered = false,
		.file_name = "libgpfsservice.so.1",
		.lib_handle = NULL,
	},
#endif
#ifdef RPMB_LISTENER
	{
		.service_name = "rpmb service",
		.is_registered = false,
		.file_name = "librpmbservice.so.1",
		.lib_handle = NULL,
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
 * NOTE: We intentionally do NOT dlclose() the listener libraries here.
 *
 * Each listener's init() calls MinkCom_getRootEnvObject(), which in turn
 * calls supplicant_start() and spawns DEFAULT_CBOBJ_THREAD_CNT
 * supplicant_worker threads (see libminkadaptor/src/supplicant.c). Those
 * worker threads loop in qcomtee_object_process_one() and dispatch into
 * the listener's CListenerCBO_invoke / smci_dispatch, both of which are
 * statically linked into the listener .so. The worker threads are bound
 * to the lifetime of the per-listener supplicant root object and are
 * only torn down asynchronously through the supplicant_release()
 * pthread_kill(SIGUSR1) + pthread_join() callback wired into root_init.
 *
 * The listener's deinit() only does Object_ASSIGN_NULL on its local
 * register_obj/cbo/mo handles; it does not (and cannot, with the current
 * libminkadaptor API) synchronously join the per-listener supplicant
 * worker threads or guarantee that QTEE has dropped its references to
 * the CListenerCBO callback object.
 *
 * If we dlclose() the listener .so while a supplicant_worker is still
 * scheduled to dispatch into it, the worker jumps to munmap'd text and
 * the process takes a SIGSEGV. This is precisely what the log showed
 * during the rpmb-init-failure rollback path: after init_listener_svc
 * for rpmb returned -1, the cleanup loop dlclose()'d the previously
 * registered listener libs while their workers were still live.
 *
 * Leaving the libraries mapped for the remaining lifetime of the
 * process is safe: qtee_supplicant either exits via main() returning
 * (kernel reaps all threads atomically) or runs forever via pause(); in
 * neither case is dlclose() necessary for correctness.
 */
static void stop_listeners_services(void)
{
	size_t idx = 0;
	size_t n_listeners = sizeof(listeners)/sizeof(struct listener_svc);

	MSGD("Total listener services to be stopped = %zu\n", n_listeners);

	for (idx = 0; idx < n_listeners; idx++) {
		/* Resource cleanup for registered listeners.
		 *
		 * deinit() only releases mink-side handles via
		 * Object_ASSIGN_NULL; the supplicant worker threads spawned
		 * by this listener's init() remain live until the underlying
		 * qcomtee root reference drops to 0. That is OK as long as
		 * we do NOT unload the listener .so under their feet (see
		 * function-level comment above).
		 */
		if(listeners[idx].is_registered) {
			deinit_listener_svc(idx);

			listeners[idx].is_registered = false;
		}

		/* Deliberately leak the dlopen handle to keep the listener
		 * .so mapped. Clearing lib_handle here only marks our local
		 * bookkeeping so we do not attempt to use it again.
		 */
		listeners[idx].lib_handle = NULL;
	}
}

/**
 * @brief Initialize a listener service.
 *
 * Initialize a listener service by invoking the init callback defined by
 * the listener.
 */
static int init_listener_svc(size_t i)
{
	listener_init init_func;
	int ret = 0;

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
		/* init() never ran, so no supplicant_worker threads were
		 * spawned for this listener. It is therefore safe (and
		 * symmetric) to dlclose here.
		 */
		dlclose(listeners[i].lib_handle);
		listeners[i].lib_handle = NULL;
		return -1;
	}

	ret = (*init_func)();
	if (ret < 0) {
		/* The listener's init() may have partially run before
		 * returning -1 and may have spawned supplicant_worker threads
		 * via MinkCom_getRootEnvObject(). Do NOT dlclose this .so
		 * here -- the running workers would dispatch into munmap'd
		 * text and SIGSEGV. The lib_handle is intentionally retained
		 * (and will be retained for the rest of the process lifetime
		 * by stop_listeners_services()).
		 *
		 * NOTE: every listener init() in this tree (atime.c,
		 * fs_main.c, gpfs_main.c, rpmb_service.c, ta_autoload) goes
		 * through MinkCom_getRootEnvObject() -> supplicant_start() ->
		 * qcomtee_object_root_init(/dev/tee0) and then
		 * MinkCom_getMemoryObject() -> qcomtee_memory_object_alloc()
		 * -> TEE_IOC_SHM_ALLOC -> kernel qcom_scm_shm_bridge_create.
		 * If the kernel keeps logging
		 *   "qcom_scm firmware:scm: SHM Bridge failed: ret -22"
		 * then this init() will reliably fail at the
		 * getMemoryObject step regardless of which listener is first
		 * in the static listeners[] table. The fix lives in the
		 * HLOS qcom_scm <-> QTEE SHM Bridge SMC contract, NOT in
		 * this user-space process. Hint that to whoever reads the
		 * journal so they do not chase ghosts in qtee_supplicant.
		 */
		MSGE("Init for %s failed: %d\n", listeners[i].service_name,
		     ret);
		MSGE("Hint: this typically indicates /dev/tee0 is up but the "
		     "kernel qcom_scm SHM Bridge / QTEE handshake is broken. "
		     "Check 'dmesg | grep -E \"SHM Bridge|qcomtee|qcom_scm\"' "
		     "for SMC return values before suspecting this listener.\n");
		return -1;
	}

	listeners[i].is_registered = true;

	return ret;
}

int start_listener_services(void)
{
	int ret = 0;
	size_t idx = 0;
	size_t n_listeners = sizeof(listeners)/sizeof(struct listener_svc);

	MSGD("Total listener services to start = %zu\n", n_listeners);

	for (idx = 0; idx < n_listeners; idx++) {

		ret = init_listener_svc(idx);
		if (ret) {
			MSGE("init_listener_svc failed: 0x%x\n", ret);
			goto fail;
		}
	}

	return ret;

fail:
	stop_listeners_services();
	return ret;

}
