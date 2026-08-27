// Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef __GP_PARAMS_H_
#define __GP_PARAMS_H_

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "tee_client_api.h"

/* Internal interface shared by both backends.
 *
 * The GP parameter translation layer (gp_params.c) turns the TEEC_Operation
 * payload handed in by the public API into the flat buffer/object triplets
 * that the QTEE side expects. It is backend agnostic: it only ever touches
 * object handles through the TEEC_OBJ_* helpers below, so the very same
 * translation code serves the libminkadaptor backend (mink_teec.c) and the
 * direct libqcomtee backend (gp_teec.c).
 *
 * The nine entry points the public API calls down into are NOT declared here.
 * Each backend declares them in its own private header - mink_teec.h and
 * gp_teec.h - so that neither has to see the contract of the other.
 */

#define MSGV printf
#define MSGD printf
#define MSGE printf

#define CANCEL_CODE_MASK             0x7FFFFFFF
#define MINK_TEEC_TIMEOUT_INFINITE   0xFFFFFFFF

#define TEEC_SHM_MAX_HEAP_SZ         0x1000

#define TEE_PARAM_TYPE_MEMREF_INPUT  5
#define TEE_PARAM_TYPE_MEMREF_OUTPUT 6
#define TEE_PARAM_TYPE_MEMREF_INOUT  7

/* Extended parameter types understood by QTEE.
 *
 * These mirror the constants declared in idl/IGPSession.idl. The direct
 * libqcomtee backend marshals the wire format by hand and therefore pulls in
 * no generated IDL header, so the values are restated here. The GP_ prefix
 * keeps them clear of the TEE_EX_PARAM_TYPE_* names emitted by idlc, which
 * the libminkadaptor backend still sees through IGPSession.h.
 */
#define GP_EX_PARAM_TYPE_NONE            0
#define GP_EX_PARAM_TYPE_MEMREF_NULL     1
#define GP_EX_PARAM_TYPE_MEMREF_DUP      2
#define GP_EX_PARAM_TYPE_MEMREF_FORCE_RW 3

#define DEFINING_INDEX_NA ((size_t)0xFFFFFFFF)

#define TRUE  1
#define FALSE 0

enum TEEC_MEMORY_TYPE {
	TEEC_MEMORY_FREE = 0,
	TEEC_MEMORY_ALLOCATED,
	TEEC_MEMORY_REGISTERED
};

/* Object handle helpers.
 *
 * teec_obj_t is picked by tee_client_api.h according to
 * MINKTEEC_DIRECT_QCOMTEE: a libqcomtee object pointer for the direct
 * backend, a MINK Object passed by value otherwise. Everything below the
 * public API manipulates handles exclusively through these four helpers, so
 * that no translation code has to know which of the two it is looking at.
 */
#if MINKTEEC_DIRECT_QCOMTEE

#include "qcomtee_object.h"

#define TEEC_OBJ_NULL       QCOMTEE_OBJECT_NULL
#define TEEC_OBJ_IS_NULL(o) ((o) == QCOMTEE_OBJECT_NULL)
#define TEEC_OBJ_RETAIN(o)  qcomtee_object_refs_inc(o)
#define TEEC_OBJ_RELEASE(o)                                                    \
	do {                                                                   \
		if (o)                                                         \
			qcomtee_object_refs_dec(o);                            \
		(o) = QCOMTEE_OBJECT_NULL;                                     \
	} while (0)

#else

#include "object.h"

#define TEEC_OBJ_NULL       Object_NULL
#define TEEC_OBJ_IS_NULL(o) Object_isNull(o)
#define TEEC_OBJ_RETAIN(o)  Object_retain(o)
#define TEEC_OBJ_RELEASE(o) Object_ASSIGN_NULL(o)

#endif // MINKTEEC_DIRECT_QCOMTEE

/**
 * @brief GP_OutBuffer.
 *
 * The GP Parameter representing an output buffer.
 */
typedef struct {
	void *buf;
	size_t len;
	size_t *len_out;
} GP_OutBuffer;

/**
 * @brief GP_InBuffer.
 *
 * The GP Parameter representing an input buffer.
 */
typedef struct {
	void *buf;
	size_t len;
	size_t sh_obj_index;
} GP_InBuffer;

/**
 * @brief gp_mem_params.
 *
 * Additional info associated to a memory object, as expected by QTEE. This is
 * a hand-written mirror of struct IGPSession_MemoryObjectParameters declared
 * in idl/IGPSession.idl; the field order is part of the wire format.
 */
typedef struct {
	uint64_t size;
	uint64_t offset;
	uint64_t sharedObjIndex;
} gp_mem_params;

_Static_assert(sizeof(gp_mem_params) == 24,
	       "gp_mem_params must match IGPSession_MemoryObjectParameters");

/**
 * @brief GP_Parameter.
 *
 * A single TEEC parameter after translation, ready to be marshalled.
 */
typedef struct {
	GP_InBuffer in_buf;
	GP_OutBuffer out_buf;
	teec_obj_t mem_obj;
	gp_mem_params mem_obj_params;
} GP_Parameter;

/**
 * @brief Query the address and size of the memory backing a memory object.
 *
 * Provided by the active backend: over libminkadaptor it forwards to
 * MinkCom_getMemoryObjectInfo, over libqcomtee it queries the memory object
 * directly.
 *
 * @param mem_obj The memory object to query.
 * @param addr Receives the address of the mapped memory.
 * @param size Receives the size of the mapped memory.
 * @return 0 on success, -1 on failure.
 */
int gp_obj_mem_info(teec_obj_t mem_obj, void **addr, size_t *size);

/**
 * @brief Initialize the GP parameters.
 *
 * @param gp_params The GP parameters to be passed to the QTEE side.
 */
void gp_params_INIT(GP_Parameter *gp_params);

/**
 * @brief Convert TEEC_* types to TEE_* types for Memory Reference parameters.
 *
 * @param op The operation payload for the request being sent.
 * @param tee_pType The parameter type encoding for the operation payload.
 */
void tee_types_from_teec_types(TEEC_Operation *op, uint32_t *tee_pType);

/**
 * @brief Convert TEEC_* parameters to GP parameters.
 *
 * @param param_types The parameter type encoding for the operation payload.
 * @param params The list of TEEC_* parameters in operation payload.
 * @param gp_params The GP parameters to be passed to the QTEE side.
 * @param tee_exParamTypes The extended parameters type encoding in this
 *                         request for use by QTEE.
 */
void gp_params_from_teec_params(uint32_t param_types, TEEC_Parameter *params,
				GP_Parameter *gp_params,
				uint32_t *tee_exParamTypes);

/**
 * @brief Convert MEMREF_TEMP_* parameters to MEMREF_PARTIAL_* parameters.
 *
 * @param ctx The initialized TEE context.
 * @param param_types The parameter type encoding for the list of parameters.
 * @param params The list of parameters.
 * @return TEEC_SUCCESS on success.
 *         TEEC_ERROR_* on failure.
 */
TEEC_Result memref_temp_to_partial_params(TEEC_Context *ctx,
					  uint32_t *param_types,
					  TEEC_Parameter *params);

/**
 * @brief Convert MEMREF_PARTIAL_* parameters to MEMREF_TEMP_* parameters.
 *
 * @param param_types The parameter type encoding for the list of parameters.
 * @param params The list of parameters.
 */
void memref_temp_from_partial_params(uint32_t *param_types,
				     TEEC_Parameter *params);

/**
 * @brief Update the contents of Shared Memory with it's associated Memory
 *        object.
 *
 * @param param_types The parameter type encoding for the list of parameters.
 * @param params The list of parameters.
 */
void update_shm_memref_from_mem_obj(uint32_t param_types,
				    TEEC_Parameter *params);

#endif // __GP_PARAMS_H_
