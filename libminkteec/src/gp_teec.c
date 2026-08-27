// Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause

/* Direct libqcomtee backend.
 *
 * Implements the nine entry points declared in gp_teec.h on top of libqcomtee,
 * without going through libminkadaptor or the idlc generated stubs. The
 * counterpart for the default build is mink_teec.c; exactly one of the two is
 * compiled, selected by MINKTEEC_DIRECT_QCOMTEE.
 *
 * Everything this file knows about QTEE's wire format comes from gp_teec.h.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gp_teec.h"

/* The public TEEC_Context carries nothing but the three handles this backend
 * fills in, so the assert catches a field being added to tee_client_api.h
 * without gp_teec.c learning about it.
 */
_Static_assert(sizeof(TEEC_Context) == 3 * sizeof(teec_obj_t),
	       "TEEC_Context layout drift");

/**
 * @brief Report whether handing an object to QTEE consumes a reference.
 *
 * QTEE takes ownership of the reference for the object classes it has to keep
 * alive on its own: callback objects, which it may invoke back at any time,
 * and memory objects, which it maps. For objects that live in QTEE already
 * (QCOMTEE_OBJECT_TYPE_TEE) or for the root, passing the handle transfers
 * nothing. A NULL slot is answered with 0, since qcomtee_object_typeof()
 * maps QCOMTEE_OBJECT_NULL to QCOMTEE_OBJECT_TYPE_NULL.
 *
 * @param o The object about to be placed in an input slot.
 * @return Non-zero if a reference has to be added on behalf of QTEE.
 */
static inline int oi_transfers_ref(struct qcomtee_object *o)
{
	qcomtee_object_type_t t = qcomtee_object_typeof(o);

	return t == QCOMTEE_OBJECT_TYPE_CB || t == QCOMTEE_OBJECT_TYPE_MEMORY;
}

/**
 * @brief Place a borrowed object in an input slot.
 *
 * The caller keeps its own reference; the one added here belongs to QTEE and
 * is consumed by a successful qcomtee_object_invoke(). Objects the caller
 * means to hand over outright must not go through this helper: fill the slot
 * directly and release on failure instead.
 *
 * @param p The slot to fill.
 * @param o The object to place in it, possibly TEEC_OBJ_NULL.
 */
static inline void oi_fill(struct qcomtee_param *p, struct qcomtee_object *o)
{
	p->attr = QCOMTEE_OBJREF_INPUT;
	p->object = o;

	if (oi_transfers_ref(o))
		qcomtee_object_refs_inc(o);
}

/**
 * @brief Give back the references oi_fill() added.
 *
 * Only ever correct after a transport level failure, that is when
 * qcomtee_object_invoke() itself returned non-zero and the request never
 * reached QTEE. Once QTEE has seen the request it owns those references even
 * if the method result is an error, so calling this on a non-zero result
 * double frees.
 *
 * @param p The slot array.
 * @param first Index of the first input object slot to unwind.
 * @param n Number of slots to unwind.
 */
static void oi_rollback(struct qcomtee_param *p, int first, int n)
{
	for (int i = first; i < first + n; i++)
		if (oi_transfers_ref(p[i].object))
			qcomtee_object_refs_dec(p[i].object);
}

/**
 * @brief Marshal and issue an openSession or invokeCommand request.
 *
 * The two methods differ only in which slots exist, so both are expressed as
 * one call: the slot indices are passed in, and the three that only
 * openSession has (@p bi_uuid, @p oi_waiter, @p oo_session) may be -1 to say
 * "no such slot". @p bi_scalars, @p bo_scalars, @p bi_param0, @p bo_param0
 * and @p oi_param0 exist in both methods and must be valid; they are not
 * checked, and a negative value writes below the slot array.
 *
 * On return the memref sizes reported by QTEE have already been written back
 * through GP_Parameter::out_buf::len_out, which is why gp_params_INIT() has to
 * have run over @p call_param.
 *
 * @param target The object to invoke: the app client for openSession, the
 *               session object for invokeCommand.
 * @param method_id GP_OP_OPEN_SESSION or GP_OP_INVOKE_COMMAND.
 * @param n Number of slots the method uses, at most GP_WIRE_MAX_SLOTS.
 * @param bi_uuid Slot of the destination UUID, or -1.
 * @param uuid The destination UUID.
 * @param uuid_len Size of @p uuid.
 * @param bi_scalars Slot of the packed input scalars.
 * @param in_scalars The packed input scalars, struct os_in or struct ic_in.
 * @param in_len Size of @p in_scalars.
 * @param bi_param0 Slot of the first input memref buffer.
 * @param bo_scalars Slot of the packed output scalars.
 * @param out Receives the packed output scalars.
 * @param bo_param0 Slot of the first output memref buffer.
 * @param oi_waiter Slot of the cancellation waiter, or -1.
 * @param waiter_obj The cancellation waiter, borrowed.
 * @param oi_param0 Slot of the first memref memory object.
 * @param call_param The translated parameters.
 * @param oo_session Slot of the session object, or -1.
 * @param session_obj_out Receives the session object, owned by the caller.
 * @param result Receives the result QTEE returned for the method itself.
 * @return 0 if the request reached QTEE and @p result is meaningful.
 *         -1 if it did not, in which case @p result is untouched.
 */
static int gp_wire_invoke(teec_obj_t target, uint32_t method_id, int n,
			  int bi_uuid, const void *uuid, size_t uuid_len,
			  int bi_scalars, const void *in_scalars, size_t in_len,
			  int bi_param0, int bo_scalars,
			  struct gp_out_scalars *out, int bo_param0,
			  int oi_waiter, teec_obj_t waiter_obj, int oi_param0,
			  const GP_Parameter *call_param, int oo_session,
			  teec_obj_t *session_obj_out, qcomtee_result_t *result)
{
	struct qcomtee_param p[GP_WIRE_MAX_SLOTS] = { 0 };

	if (bi_uuid >= 0)
		UBUF_IN(p[bi_uuid], uuid, uuid_len);
	UBUF_IN(p[bi_scalars], in_scalars, in_len);
	UBUF_OUT(p[bo_scalars], out, sizeof(*out));
	if (oi_waiter >= 0)
		oi_fill(&p[oi_waiter], waiter_obj);
	if (oo_session >= 0)
		OBJ_OUT(p[oo_session]);

	for (int i = 0; i < MAX_NUM_PARAMS; i++) {
		UBUF_IN(p[bi_param0 + i], call_param[i].in_buf.buf,
			call_param[i].in_buf.len);
		UBUF_OUT(p[bo_param0 + i], call_param[i].out_buf.buf,
			 call_param[i].out_buf.len);
		oi_fill(&p[oi_param0 + i], call_param[i].mem_obj);
	}

	if (qcomtee_object_invoke(target, method_id, p, n, result)) {
		/* The waiter, when present, sits directly ahead of the memory
		 * object slots, so one sweep covers every slot oi_fill()
		 * touched.
		 */
		int oi_first = (oi_waiter >= 0) ? oi_waiter : oi_param0;

		oi_rollback(p, oi_first,
			    oi_param0 + MAX_NUM_PARAMS - oi_first);

		return -1;
	}

	if (*result)
		return 0;

	/* Plain assignment: QTEE hands over its reference, so retaining here
	 * would leak. Note that this happens even when the GP level
	 * out->ret_value is an error, because the method itself succeeded;
	 * disposing of the object in that case is up to the caller.
	 */
	if (oo_session >= 0)
		*session_obj_out = p[oo_session].object;

	/* QTEE reports the size it actually produced in the packed scalars.
	 * A zero there means it left the buffer alone, in which case the size
	 * the slot still carries is what the caller asked for.
	 */
	for (int i = 0; i < MAX_NUM_PARAMS; i++) {
		if (!call_param[i].out_buf.len_out)
			continue;

		*call_param[i].out_buf.len_out =
			out->memref_sz[i] ? out->memref_sz[i]
					  : p[bo_param0 + i].ubuf.size;
	}

	return 0;
}
