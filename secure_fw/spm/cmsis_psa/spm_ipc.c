/*
 * Copyright (c) 2018-2022, Arm Limited. All rights reserved.
 * Copyright (c) 2021, Cypress Semiconductor Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include <inttypes.h>
#include <stdbool.h>
#include "aapcs_local.h"
#include "bitops.h"
#include "config_impl.h"
#include "critical_section.h"
#include "current.h"
#include "fih.h"
#include "psa/client.h"
#include "psa/service.h"
#include "thread.h"
#include "internal_errors.h"
#include "tfm_spm_hal.h"
#include "tfm_api.h"
#include "tfm_arch.h"
#include "tfm_secure_api.h"
#include "tfm_memory_utils.h"
#include "tfm_hal_defs.h"
#include "tfm_hal_interrupt.h"
#include "tfm_hal_isolation.h"
#include "spm_ipc.h"
#include "tfm_peripherals_def.h"
#include "tfm_core_utils.h"
#include "tfm_nspm.h"
#include "tfm_rpc.h"
#include "tfm_core_trustzone.h"
#include "lists.h"
#include "tfm_pools.h"
#include "region.h"
#include "psa_manifest/pid.h"
#include "ffm/backend.h"
#include "load/partition_defs.h"
#include "load/service_defs.h"
#include "load/asset_defs.h"
#include "load/spm_load_api.h"
#include "tfm_nspm.h"

#if !(defined CONFIG_TFM_CONN_HANDLE_MAX_NUM) || (CONFIG_TFM_CONN_HANDLE_MAX_NUM == 0)
#error "CONFIG_TFM_CONN_HANDLE_MAX_NUM must be defined and not zero."
#endif

/* Partition and service runtime data list head/runtime data table */
static struct service_head_t services_listhead;
struct service_t *stateless_services_ref_tbl[STATIC_HANDLE_NUM_LIMIT];

/* Pools */
TFM_POOL_DECLARE(conn_handle_pool, sizeof(struct conn_handle_t),
                 CONFIG_TFM_CONN_HANDLE_MAX_NUM);

extern uint32_t scheduler_lock;

/*********************** Connection handle conversion APIs *******************/

#define CONVERSION_FACTOR_BITOFFSET    3
#define CONVERSION_FACTOR_VALUE        (1 << CONVERSION_FACTOR_BITOFFSET)
/* Set 32 as the maximum */
#define CONVERSION_FACTOR_VALUE_MAX    0x20

#if CONVERSION_FACTOR_VALUE > CONVERSION_FACTOR_VALUE_MAX
#error "CONVERSION FACTOR OUT OF RANGE"
#endif

static uint32_t loop_index;

/*
 * A handle instance psa_handle_t allocated inside SPM is actually a memory
 * address among the handle pool. Return this handle to the client directly
 * exposes information of secure memory address. In this case, converting the
 * handle into another value does not represent the memory address to avoid
 * exposing secure memory directly to clients.
 *
 * This function converts the handle instance into another value by scaling the
 * handle in pool offset, the converted value is named as a user handle.
 *
 * The formula:
 *  user_handle = (handle_instance - POOL_START) * CONVERSION_FACTOR_VALUE +
 *                CLIENT_HANDLE_VALUE_MIN + loop_index
 * where:
 *  CONVERSION_FACTOR_VALUE = 1 << CONVERSION_FACTOR_BITOFFSET, and should not
 *  exceed CONVERSION_FACTOR_VALUE_MAX.
 *
 *  handle_instance in RANGE[POOL_START, POOL_END]
 *  user_handle     in RANGE[CLIENT_HANDLE_VALUE_MIN, 0x3FFFFFFF]
 *  loop_index      in RANGE[0, CONVERSION_FACTOR_VALUE - 1]
 *
 *  note:
 *  loop_index is used to promise same handle instance is converted into
 *  different user handles in short time.
 */
psa_handle_t tfm_spm_to_user_handle(struct conn_handle_t *handle_instance)
{
    psa_handle_t user_handle;

    loop_index = (loop_index + 1) % CONVERSION_FACTOR_VALUE;
    user_handle = (psa_handle_t)((((uintptr_t)handle_instance -
                  (uintptr_t)conn_handle_pool) << CONVERSION_FACTOR_BITOFFSET) +
                  CLIENT_HANDLE_VALUE_MIN + loop_index);

    return user_handle;
}

/*
 * This function converts a user handle into a corresponded handle instance.
 * The converted value is validated before returning, an invalid handle instance
 * is returned as NULL.
 *
 * The formula:
 *  handle_instance = ((user_handle - CLIENT_HANDLE_VALUE_MIN) /
 *                    CONVERSION_FACTOR_VALUE) + POOL_START
 * where:
 *  CONVERSION_FACTOR_VALUE = 1 << CONVERSION_FACTOR_BITOFFSET, and should not
 *  exceed CONVERSION_FACTOR_VALUE_MAX.
 *
 *  handle_instance in RANGE[POOL_START, POOL_END]
 *  user_handle     in RANGE[CLIENT_HANDLE_VALUE_MIN, 0x3FFFFFFF]
 *  loop_index      in RANGE[0, CONVERSION_FACTOR_VALUE - 1]
 */
struct conn_handle_t *tfm_spm_to_handle_instance(psa_handle_t user_handle)
{
    struct conn_handle_t *handle_instance;

    if (user_handle == PSA_NULL_HANDLE) {
        return NULL;
    }

    handle_instance = (struct conn_handle_t *)((((uintptr_t)user_handle -
                      CLIENT_HANDLE_VALUE_MIN) >> CONVERSION_FACTOR_BITOFFSET) +
                      (uintptr_t)conn_handle_pool);

    return handle_instance;
}

/* Service handle management functions */
struct conn_handle_t *tfm_spm_create_conn_handle(struct service_t *service,
                                                 int32_t client_id)
{
    struct conn_handle_t *p_handle;

    TFM_CORE_ASSERT(service);

    /* Get buffer for handle list structure from handle pool */
    p_handle = (struct conn_handle_t *)tfm_pool_alloc(conn_handle_pool);
    if (!p_handle) {
        return NULL;
    }

    spm_memset(p_handle, 0, sizeof(*p_handle));

    p_handle->service = service;
    p_handle->status = TFM_HANDLE_STATUS_IDLE;
    p_handle->client_id = client_id;

    return p_handle;
}

int32_t tfm_spm_validate_conn_handle(const struct conn_handle_t *conn_handle,
                                     int32_t client_id)
{
    /* Check the handle address is validated */
    if (is_valid_chunk_data_in_pool(conn_handle_pool,
                                    (uint8_t *)conn_handle) != true) {
        return SPM_ERROR_GENERIC;
    }

    /* Check the handle caller is correct */
    if (conn_handle->client_id != client_id) {
        return SPM_ERROR_GENERIC;
    }

    return SPM_SUCCESS;
}

int32_t tfm_spm_free_conn_handle(struct service_t *service,
                                 struct conn_handle_t *conn_handle)
{
    struct critical_section_t cs_assert = CRITICAL_SECTION_STATIC_INIT;

    TFM_CORE_ASSERT(service);
    TFM_CORE_ASSERT(conn_handle != NULL);

    /* Clear magic as the handler is not used anymore */
    conn_handle->magic = 0;

    CRITICAL_SECTION_ENTER(cs_assert);

    /* Back handle buffer to pool */
    tfm_pool_free(conn_handle_pool, conn_handle);
    CRITICAL_SECTION_LEAVE(cs_assert);

    return SPM_SUCCESS;
}

/* Partition management functions */

/* This API is only used in IPC backend. */
#if CONFIG_TFM_SPM_BACKEND_IPC == 1
struct conn_handle_t *spm_get_handle_by_signal(struct partition_t *p_ptn,
                                               psa_signal_t signal)
{
    struct conn_handle_t *p_msg_iter;
    struct conn_handle_t **pr_msg_iter, **last_found_msg_holder = NULL;
    struct critical_section_t cs_assert = CRITICAL_SECTION_STATIC_INIT;
    uint32_t nr_found_msgs = 0;

    CRITICAL_SECTION_ENTER(cs_assert);

    /* Return the last found message which applies a FIFO mechanism. */
    UNI_LIST_FOREACH_NODE_PNODE(pr_msg_iter, p_msg_iter, p_ptn, p_handles) {
        if (p_msg_iter->service->p_ldinf->signal == signal) {
            last_found_msg_holder = pr_msg_iter;
            nr_found_msgs++;
        }
    }

    if (last_found_msg_holder) {
        p_msg_iter = *last_found_msg_holder;
        UNI_LIST_REMOVE_NODE_BY_PNODE(last_found_msg_holder, p_handles);

        if (nr_found_msgs == 1) {
            p_ptn->signals_asserted &= ~signal;
        }
    }

    CRITICAL_SECTION_LEAVE(cs_assert);

    return p_msg_iter;
}
#endif

struct service_t *tfm_spm_get_service_by_sid(uint32_t sid)
{
    struct service_t *p_prev, *p_curr;

    UNI_LIST_FOREACH_NODE_PREV(p_prev, p_curr, &services_listhead, next) {
        if (p_curr->p_ldinf->sid == sid) {
            UNI_LIST_MOVE_AFTER(&services_listhead, p_prev, p_curr, next);
            return p_curr;
        }
    }

    return NULL;
}

/**
 * \brief                   Get the partition context by partition ID.
 *
 * \param[in] partition_id  Partition identity
 *
 * \retval NULL             Failed
 * \retval "Not NULL"       Target partition context pointer,
 *                          \ref partition_t structures
 */
struct partition_t *tfm_spm_get_partition_by_id(int32_t partition_id)
{
    struct partition_t *p_part;

    UNI_LIST_FOREACH(p_part, PARTITION_LIST_ADDR, next) {
        if (p_part->p_ldinf->pid == partition_id) {
            return p_part;
        }
    }

    return NULL;
}

int32_t tfm_spm_check_client_version(struct service_t *service,
                                     uint32_t version)
{
    TFM_CORE_ASSERT(service);

    switch (SERVICE_GET_VERSION_POLICY(service->p_ldinf->flags)) {
    case SERVICE_VERSION_POLICY_RELAXED:
        if (version > service->p_ldinf->version) {
            return SPM_ERROR_VERSION;
        }
        break;
    case SERVICE_VERSION_POLICY_STRICT:
        if (version != service->p_ldinf->version) {
            return SPM_ERROR_VERSION;
        }
        break;
    default:
        return SPM_ERROR_VERSION;
    }
    return SPM_SUCCESS;
}

int32_t tfm_spm_check_authorization(uint32_t sid,
                                    struct service_t *service,
                                    bool ns_caller)
{
    struct partition_t *partition = NULL;
    uint32_t *dep;
    int32_t i;

    TFM_CORE_ASSERT(service);

    if (ns_caller) {
        if (!SERVICE_IS_NS_ACCESSIBLE(service->p_ldinf->flags)) {
            return SPM_ERROR_GENERIC;
        }
    } else {
        partition = GET_CURRENT_COMPONENT();
        if (!partition) {
            tfm_core_panic();
        }

        dep = (uint32_t *)LOAD_INFO_DEPS(partition->p_ldinf);
        for (i = 0; i < partition->p_ldinf->ndeps; i++) {
            if (dep[i] == sid) {
                break;
            }
        }

        if (i == partition->p_ldinf->ndeps) {
            return SPM_ERROR_GENERIC;
        }
    }
    return SPM_SUCCESS;
}

/* Message functions */

struct conn_handle_t *spm_get_handle_by_user_handle(psa_handle_t msg_handle)
{
    /*
     * The message handler passed by the caller is considered invalid in the
     * following cases:
     *   1. Not a valid message handle. (The address of a message is not the
     *      address of a possible handle from the pool
     *   2. Handle not belongs to the caller partition (The handle is either
     *      unused, or owned by anither partition)
     * Check the conditions above
     */
    int32_t partition_id;
    struct conn_handle_t *p_conn_handle =
                                    tfm_spm_to_handle_instance(msg_handle);

    if (is_valid_chunk_data_in_pool(
        conn_handle_pool, (uint8_t *)p_conn_handle) != true) {
        return NULL;
    }

    /*
     * Check that the magic number is correct. This proves that the message
     * structure contains an active message.
     */
    if (p_conn_handle->magic != TFM_MSG_MAGIC) {
        return NULL;
    }

    /* Check that the running partition owns the message */
    partition_id = tfm_spm_partition_get_running_partition_id();
    if (partition_id != p_conn_handle->service->partition->p_ldinf->pid) {
        return NULL;
    }

    return p_conn_handle;
}

void spm_fill_message(struct conn_handle_t *hdl,
                      struct service_t *service,
                      psa_handle_t handle,
                      int32_t type, int32_t client_id,
                      psa_invec *invec, size_t in_len,
                      psa_outvec *outvec, size_t out_len,
                      psa_outvec *caller_outvec)
{
    uint32_t i;

    TFM_CORE_ASSERT(hdl);
    TFM_CORE_ASSERT(service);
    TFM_CORE_ASSERT(!(invec == NULL && in_len != 0));
    TFM_CORE_ASSERT(!(outvec == NULL && out_len != 0));
    TFM_CORE_ASSERT(in_len <= PSA_MAX_IOVEC);
    TFM_CORE_ASSERT(out_len <= PSA_MAX_IOVEC);
    TFM_CORE_ASSERT(in_len + out_len <= PSA_MAX_IOVEC);

    /* Clear message buffer before using it */
    spm_memset(&hdl->msg, 0, sizeof(psa_msg_t));

    THRD_SYNC_INIT(&hdl->ack_evnt);
    hdl->magic = TFM_MSG_MAGIC;
    hdl->service = service;
    hdl->p_client = GET_CURRENT_COMPONENT();
    hdl->caller_outvec = caller_outvec;
    hdl->msg.client_id = client_id;

    /* Copy contents */
    hdl->msg.type = type;

    for (i = 0; i < in_len; i++) {
        hdl->msg.in_size[i] = invec[i].len;
        hdl->invec[i].base = invec[i].base;
    }

    for (i = 0; i < out_len; i++) {
        hdl->msg.out_size[i] = outvec[i].len;
        hdl->outvec[i].base = outvec[i].base;
        /* Out len is used to record the wrote number, set 0 here again */
        hdl->outvec[i].len = 0;
    }

    /* Use the user connect handle as the message handle */
    hdl->msg.handle = handle;
    hdl->msg.rhandle = hdl->rhandle;

    /* Set the private data of NSPE client caller in multi-core topology */
    if (TFM_CLIENT_ID_IS_NS(client_id)) {
        tfm_rpc_set_caller_data(hdl, client_id);
    }
}

int32_t tfm_spm_partition_get_running_partition_id(void)
{
    struct partition_t *partition;

    partition = GET_CURRENT_COMPONENT();
    if (partition && partition->p_ldinf) {
        return partition->p_ldinf->pid;
    } else {
        return INVALID_PARTITION_ID;
    }
}

int32_t tfm_memory_check(const void *buffer, size_t len, bool ns_caller,
                         enum tfm_memory_access_e access,
                         uint32_t privileged)
{
    enum tfm_hal_status_t err;
    uint32_t attr = 0;

    /* If len is zero, this indicates an empty buffer and base is ignored */
    if (len == 0) {
        return SPM_SUCCESS;
    }

    if (!buffer) {
        return SPM_ERROR_BAD_PARAMETERS;
    }

    if ((uintptr_t)buffer > (UINTPTR_MAX - len)) {
        return SPM_ERROR_MEMORY_CHECK;
    }

    if (access == TFM_MEMORY_ACCESS_RW) {
        attr |= (TFM_HAL_ACCESS_READABLE | TFM_HAL_ACCESS_WRITABLE);
    } else {
        attr |= TFM_HAL_ACCESS_READABLE;
    }

    if (privileged == TFM_PARTITION_UNPRIVILEGED_MODE) {
        attr |= TFM_HAL_ACCESS_UNPRIVILEGED;
    } else {
        attr &= ~TFM_HAL_ACCESS_UNPRIVILEGED;
    }

    if (ns_caller) {
        attr |= TFM_HAL_ACCESS_NS;
    }

    err = tfm_hal_memory_has_access((uintptr_t)buffer, len, attr);

    if (err == TFM_HAL_SUCCESS) {
        return SPM_SUCCESS;
    }

    return SPM_ERROR_MEMORY_CHECK;
}

bool tfm_spm_is_ns_caller(void)
{
    struct partition_t *partition = GET_CURRENT_COMPONENT();

    if (!partition) {
        tfm_core_panic();
    }

    return (partition->p_ldinf->pid == TFM_SP_NON_SECURE_ID);
}

int32_t tfm_spm_get_client_id(bool ns_caller)
{
    int32_t client_id;

    if (ns_caller) {
        client_id = tfm_nspm_get_current_client_id();
    } else {
        client_id = tfm_spm_partition_get_running_partition_id();
    }

    if (ns_caller != (client_id < 0)) {
        /* NS client ID must be negative and Secure ID must >= 0 */
        tfm_core_panic();
    }

    return client_id;
}

uint32_t tfm_spm_init(void)
{
    struct partition_t *partition;
    const struct partition_load_info_t *p_pldi;
    uint32_t service_setting = 0;

#ifdef TFM_FIH_PROFILE_ON
    fih_int fih_rc = FIH_FAILURE;
#endif

    tfm_pool_init(conn_handle_pool,
                  POOL_BUFFER_SIZE(conn_handle_pool),
                  sizeof(struct conn_handle_t),
                  CONFIG_TFM_CONN_HANDLE_MAX_NUM);

    UNI_LISI_INIT_NODE(PARTITION_LIST_ADDR, next);
    UNI_LISI_INIT_NODE(&services_listhead, next);

    /* Init the nonsecure context. */
    tfm_nspm_ctx_init();

    while (1) {
        partition = load_a_partition_assuredly(PARTITION_LIST_ADDR);
        if (partition == NO_MORE_PARTITION) {
            break;
        }

        p_pldi = partition->p_ldinf;

        if (p_pldi->nservices) {
            service_setting = load_services_assuredly(
                                partition,
                                &services_listhead,
                                stateless_services_ref_tbl,
                                sizeof(stateless_services_ref_tbl));
        }

        if (p_pldi->nirqs) {
            load_irqs_assuredly(partition);
        }

        /* Bind the partition with platform. */
#if TFM_FIH_PROFILE_ON
        FIH_CALL(tfm_hal_bind_boundaries, fih_rc, partition->p_ldinf,
                 &partition->p_boundaries);
        if (fih_not_eq(fih_rc, fih_int_encode(TFM_HAL_SUCCESS))) {
            tfm_core_panic();
        }
#else /* TFM_FIH_PROFILE_ON */
        if (tfm_hal_bind_boundaries(partition->p_ldinf,
                                    &partition->p_boundaries)
                != TFM_HAL_SUCCESS) {
            tfm_core_panic();
        }
#endif /* TFM_FIH_PROFILE_ON */

        backend_instance.comp_init_assuredly(partition, service_setting);
    }

    return backend_instance.system_run();
}

uint64_t do_schedule(void)
{
    AAPCS_DUAL_U32_T ctx_ctrls;
    struct partition_t *p_part_curr, *p_part_next;
    struct context_ctrl_t *p_curr_ctx;
    struct thread_t *pth_next = thrd_next();
    struct critical_section_t cs = CRITICAL_SECTION_STATIC_INIT;

    p_curr_ctx = (struct context_ctrl_t *)(CURRENT_THREAD->p_context_ctrl);

    AAPCS_DUAL_U32_SET(ctx_ctrls, (uint32_t)p_curr_ctx, (uint32_t)p_curr_ctx);

    p_part_curr = GET_CURRENT_COMPONENT();
    p_part_next = GET_THRD_OWNER(pth_next);

    if (scheduler_lock != SCHEDULER_LOCKED && pth_next != NULL &&
        p_part_curr != p_part_next) {
        /* Check if there is enough room on stack to save more context */
        if ((p_curr_ctx->sp_limit +
                sizeof(struct tfm_additional_context_t)) > __get_PSP()) {
            tfm_core_panic();
        }

        CRITICAL_SECTION_ENTER(cs);
        /*
         * If required, let the platform update boundary based on its
         * implementation. Change privilege, MPU or other configurations.
         */
        if (p_part_curr->p_boundaries != p_part_next->p_boundaries) {
            if (tfm_hal_update_boundaries(p_part_next->p_ldinf,
                                          p_part_next->p_boundaries)
                                                        != TFM_HAL_SUCCESS) {
                tfm_core_panic();
            }
        }
        ARCH_FLUSH_FP_CONTEXT();

        AAPCS_DUAL_U32_SET_A1(ctx_ctrls, (uint32_t)pth_next->p_context_ctrl);

        CURRENT_THREAD = pth_next;
        CRITICAL_SECTION_LEAVE(cs);
    }

    return AAPCS_DUAL_U32_AS_U64(ctx_ctrls);
}

void update_caller_outvec_len(struct conn_handle_t *hdl)
{
    uint32_t i;

    /*
     * FixeMe: abstract these part into dedicated functions to avoid
     * accessing thread context in psa layer
     */
    /*
     * If it is a NS request via RPC, the owner of this message is not set.
     * Or if it is a SFN message, it does not have owner thread state either.
     */
    if ((!is_tfm_rpc_msg(hdl)) && (hdl->sfn_magic != TFM_MSG_MAGIC_SFN)) {
        TFM_CORE_ASSERT(hdl->ack_evnt.owner->state == THRD_STATE_BLOCK);
    }

    for (i = 0; i < PSA_MAX_IOVEC; i++) {
        if (hdl->msg.out_size[i] == 0) {
            continue;
        }

        TFM_CORE_ASSERT(hdl->caller_outvec[i].base == hdl->outvec[i].base);

        hdl->caller_outvec[i].len = hdl->outvec[i].len;
    }
}

void spm_assert_signal(void *p_pt, psa_signal_t signal)
{
    struct critical_section_t cs_assert = CRITICAL_SECTION_STATIC_INIT;
    struct partition_t *partition = (struct partition_t *)p_pt;

    if (!partition) {
        tfm_core_panic();
    }

    CRITICAL_SECTION_ENTER(cs_assert);

    partition->signals_asserted |= signal;

    if (partition->signals_waiting & signal) {
        thrd_wake_up(&partition->waitobj,
                     partition->signals_asserted & partition->signals_waiting);
        partition->signals_waiting &= ~signal;
    }

    CRITICAL_SECTION_LEAVE(cs_assert);
}
