// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "listener_mngr.h"

/*
 * If listener registration fails because of a transient external
 * dependency (kernel qcom_scm SHM Bridge / QTEE 5.2.0 handshake
 * returning -EINVAL), exit "slowly enough" that systemd's
 * StartLimitBurst budget does not get burned through within seconds.
 * Combined with RestartSec=5 and StartLimitIntervalSec=120 in the
 * service unit, this widens the recovery window to roughly 2 minutes
 * of automatic retries before systemd gives up.
 */
#define EXIT_BACKOFF_SECS 3

int main() {

	MSGD("qtee_supplicant: process entry PPID = %d\n", getppid());

	if(0 != start_listener_services()) {
		MSGE("ERROR: listeners registration failed\n");
		MSGE("ERROR: this is almost always caused by a failure in the "
		     "kernel qcom_scm SHM Bridge / QTEE handshake (look for "
		     "'SHM Bridge failed: ret -22' in dmesg). qtee_supplicant "
		     "itself only opens /dev/tee0 and registers listeners; if "
		     "the underlying SCM SMC call fails this process cannot "
		     "succeed. Backing off %d s before exit so systemd does "
		     "not hit StartLimitBurst.\n", EXIT_BACKOFF_SECS);
		sleep(EXIT_BACKOFF_SECS);
		return -1;
	}

	MSGE("QTEE_SUPPLICANT RUNNING\n");
	pause();
	MSGD("qtee_supplicant: Process exiting!!!\n");
	return -1;
}
