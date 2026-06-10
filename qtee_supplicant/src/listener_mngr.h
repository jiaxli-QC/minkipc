// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef __LISTENER_MNGR_H
#define __LISTENER_MNGR_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define MSGV printf
#define MSGD printf
#define MSGE printf

typedef int (*listener_init)(void);
typedef void (*listener_deinit)(void);

/**
 * @brief Listener service.
 *
 * Represents a listener service to be initialized and started by the QTEE
 * supplicant. Each listener service offers a specific REE service to QTEE,
 * e.g. time service.
 *
 * Contract for listener init() implementations
 * --------------------------------------------
 *  - init() MUST be idempotent on its own error path. start_listener_services()
 *    retries init() up to LISTENER_INIT_MAX_RETRIES times when it returns a
 *    non-zero value (to ride out transient QTEE/SCM SHM Bridge -EINVAL on
 *    early boot). Each err path inside the listener (cbo / mo / register_obj
 *    allocation paths) MUST ASSIGN_NULL the affected handles so a subsequent
 *    retry starts from a clean slate.
 *  - init() MUST NOT leak non-Object_* state across a failed call: any global
 *    counters, threads, or fds opened before the failure must be torn down
 *    inside init() before it returns the error. The outer retry loop only
 *    walks Object_* state.
 *  - The kernel-side qcom_scm SHM Bridge create path is outside our control;
 *    a partial registration leaving kernel-side dirt is possible. The retry
 *    bound is intentionally small (3) to limit amplification.
 *
 * is_registered ownership
 * -----------------------
 *  - Owned by listener_mngr.c. init_listener_svc() sets it to true on
 *    success; the failure path in start_listener_services() sets it to false.
 *    Listener init()/deinit() implementations MUST NOT touch this field.
 *    stop_listeners_services() uses it as the gate for calling deinit().
 */
struct listener_svc {
	char *service_name; /**< Name of the listener service. */
	bool is_registered; /**< Listener registration status (owned by listener_mngr). */
	char *file_name; /**< File name of the listener service. */
	void *lib_handle; /**< LibHandle for the listener. */
	bool critical; /**< If true, a failure of this listener aborts startup. */
};

/**
 * @brief Start listener services.
 *
 * Starts listener services which wait for a listener request from QTEE.
 */
int start_listener_services(void);

#endif // __LISTENER_MNGR_H
