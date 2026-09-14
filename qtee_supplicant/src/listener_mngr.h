// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef __LISTENER_MNGR_H
#define __LISTENER_MNGR_H

#include <stdint.h>
#include <stdio.h>
#include <syslog.h>

#define MSGV(fmt, ...) syslog(LOG_INFO, fmt, ##__VA_ARGS__)
#define MSGD(fmt, ...) syslog(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define MSGE(fmt, ...) syslog(LOG_ERR, fmt, ##__VA_ARGS__)

typedef int (*listener_init)(void);
typedef void (*listener_deinit)(void);

/**
 * @brief Listener service.
 *
 * Represents a listener service to be initialized and started by the QTEE
 * supplicant. Each listener service offers a specific REE service to QTEE,
 * e.g. time service.
 */
struct listener_svc {
	char *service_name; /**< Name of the listener service. */
	int is_registered; /**< Listener registration status. */
	char *file_name; /**< File name of the listener service. */
	void *lib_handle; /**< LibHandle for the listener. */
};

/**
 * @brief Start listener services.
 *
 * Starts listener services which wait for a listener request from QTEE.
 */
int start_listener_services(void);
void stop_listeners_services(void);

#endif // __LISTENER_MNGR_H
