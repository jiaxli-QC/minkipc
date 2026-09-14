// Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef __MINK_TEEC_H_
#define __MINK_TEEC_H_

#include <stdio.h>

#include "tee_client_api.h"
#include "IGPSession.h"
#include <syslog.h>

#define MSGV(fmt, ...) syslog(LOG_INFO, fmt, ##__VA_ARGS__)
#define MSGD(fmt, ...) syslog(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define MSGE(fmt, ...) syslog(LOG_ERR, fmt, ##__VA_ARGS__)

#define CANCEL_CODE_MASK             0x7FFFFFFF
#define MINK_TEEC_TIMEOUT_INFINITE   0xFFFFFFFF

#define TEEC_SHM_MAX_HEAP_SZ         0x1000

#define TEE_PARAM_TYPE_MEMREF_INPUT  5
#define TEE_PARAM_TYPE_MEMREF_OUTPUT 6
#define TEE_PARAM_TYPE_MEMREF_INOUT  7

#define DEFINING_INDEX_NA ((size_t)0xFFFFFFFF)

#define TRUE  1
#define FALSE 0

enum TEEC_MEMORY_TYPE {
	TEEC_MEMORY_FREE = 0,
	TEEC_MEMORY_ALLOCATED,
	TEEC_MEMORY_REGISTERED
};

/**
 * @brief MINK_OutBuffer.
 *
 * The MINK Parameter representing an output buffer.
 */
typedef struct {
	void *buf;
	size_t len;
	size_t *len_out;
} MINK_OutBuffer;

/**
 * @brief MINK_InBuffer.
 *
 * The MINK Parameter representing an input buffer.
 */
typedef struct {
	void *buf;
	size_t len;
	size_t sh_obj_index;
} MINK_InBuffer;

typedef IGPSession_MemoryObjectParameters MemoryObjectParams;

/**
 * @brief MINK_Parameter.
 *
 * The MINK Parameter to be passed to a MINK API
 */
typedef struct {
	MINK_InBuffer in_buf;
	MINK_OutBuffer out_buf;
	Object mem_obj;
	MemoryObjectParams mem_obj_params;
} MINK_Parameter;

/**
 * @brief Initializes a new TEE Context over MINK IPC, forming a connection
 * between the Client Application and QTEE.
 *
 * @param ctx The TEE context to be initialized.
 * @return TEEC_SUCCESS if the initialization was successful.
 *	   TEEC_ERROR_* otherwise.
 */
TEEC_Result initialize_context(TEEC_Context *ctx);

/**
 * @brief Finalizes an initialized TEE Context, closing the connection
 * between the Client Application and QTEE.
 *
 * @param ctx The TEE context to be finalized.
 */
void finalize_context(TEEC_Context *ctx);

/**
 * @brief Opens a new Session between the Client Application and the specified
 * Trusted Application in QTEE.
 *
 * @param ctx The initialized TEE context over which to establish a session.
 * @param session The session to be established with QTEE.
 * @param destination The UUID of the destination Trusted Application.
 * @param connection_method The method of connection to use.
 * @param connection_data Any necessary data required to support the connection
 *                        method chosen.
 * @param op The optional operation payload for this request.
 * @param ret_origin The origin of the returned value from QTEE.
 * @return TEEC_SUCCESS If the session was initialized successfully.
 *	   TEEC_ERROR_* otherwise.
 */
TEEC_Result open_session(TEEC_Context *ctx, TEEC_Session *session,
			 const TEEC_UUID *destination, uint32_t conn_method,
			 const void *connection_data, TEEC_Operation *op,
			 uint32_t *ret_origin);

/**
 * @brief Closes an established Session between the Client Application and a
 * Trusted Application in QTEE.
 *
 * @param session The session to be closed with QTEE.
 */
void close_session(TEEC_Session *session);

/**
 * @brief Invoke a command over an established Session to a Trusted Application
 * in QTEE.
 *
 * @param session The session over which to invoke the command.
 * @param command_id Identifier for the command to invoke.
 * @param op The optional operation payload for this request.
 * @param ret_origin The origin of the returned value from QTEE.
 * @return TEEC_SUCCESS If the command was invoked successfully.
 *	   TEEC_ERROR_* otherwise.
 */
TEEC_Result invoke_command(TEEC_Session *session, uint32_t command_id,
			   TEEC_Operation *op, uint32_t *ret_origin);

/**
 * @brief Register a shared memory with QTEE.
 *
 * @param ctx The initialized TEE context over which to register a shared
 *            memory.
 * @param shm The Shared Memory to be registered with QTEE.
 * @param convert Boolean indicating whether this shared memory was converted
 *                from a temporary memory.
 * @return TEEC_SUCCESS If the memory was registered successfully.
 *	   TEEC_ERROR_* otherwise.
 */
TEEC_Result register_shared_memory(TEEC_Context *ctx, TEEC_SharedMemory *shm,
				   uint8_t convert);

/**
 * @brief Allocate a memory shared with QTEE.
 *
 * @param ctx The initialized TEE context over which to allocate a shared
 *            memory.
 * @param shm The Shared Memory to be allocated.
 * @return TEEC_SUCCESS If the memory was allocated successfully.
 *	   TEEC_ERROR_* otherwise.
 */
TEEC_Result allocate_shared_memory(TEEC_Context *ctx, TEEC_SharedMemory *shm);

/**
 * @brief Release a memory shared with QTEE.
 *
 * @param shm The Shared Memory to be released.
 */
void release_shared_memory(TEEC_SharedMemory *shm);

/**
 * @brief Requests cancellation of a pending open Session operation or a
 * Command invocation operation.
 *
 * @param op The operation to be cancelled.
 */
void request_cancellation(TEEC_Operation *op);

#endif // __MINK_TEEC_H_
