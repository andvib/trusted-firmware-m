/*
 * Copyright (c) 2021, Arm Limited. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */

#include "cc3xx_internal_chacha20.h"

#include "cc_pal_types.h"
#include "cc_pal_mem.h"
#include "cc_pal_abort.h"

/** \file cc3xx_internal_chacha20.c
 *
 * This file contains the implementation of the internal functions to
 * perform symmetric encryption and decryption using the Chacha20
 * algorithm
 *
 */

/** \defgroup internal_chacha20 Internal Chacha20 functions
 *
 *  Internal functions used by the driver to perform Chacha20 cipher encryption
 *  and decryption
 *
 *  @{
 */
void cc3xx_chacha20_init(ChachaContext_t *ctx)
{
    if (NULL == ctx) {
        CC_PalAbort("ctx cannot be NULL");
        return;
    }

    ctx->inputDataAddrType  = DLLI_ADDR;
    ctx->outputDataAddrType = DLLI_ADDR;
}

void cc3xx_chacha20_free(ChachaContext_t *ctx)
{
    if (NULL == ctx) {
        CC_PAL_LOG_ERR("ctx cannot be NULL\n");
        return;
    }

    CC_PalMemSet(ctx, 0, sizeof(ChachaContext_t));
}

psa_status_t cc3xx_chacha20_setkey(
        ChachaContext_t *ctx,
        const uint8_t key[CHACHA_256_BIT_KEY_SIZE])
{
    if (NULL == ctx) {
        CC_PAL_LOG_ERR("ctx cannot be NULL\n");
        return PSA_ERROR_INVALID_ARGUMENT;
    }

    if (NULL == key) {
        CC_PAL_LOG_ERR("key cannot be NULL\n");
        return PSA_ERROR_INVALID_ARGUMENT;
    }

    CC_PalMemCopy(ctx->keyBuf, key, CHACHA_256_BIT_KEY_SIZE);

    return PSA_SUCCESS;
}

psa_status_t cc3xx_chacha20_starts(
        ChachaContext_t *ctx,
        const uint8_t nonce[CHACHA_IV_96_SIZE_BYTES],
        uint32_t counter)
{
    if (NULL == ctx) {
        CC_PAL_LOG_ERR("ctx cannot be NULL\n");
        return PSA_ERROR_INVALID_ARGUMENT;
    }

    if (nonce == NULL) {
        CC_PAL_LOG_ERR("nonce cannot be NULL\n");
        return PSA_ERROR_INVALID_ARGUMENT;
    }

    ctx->nonceSize = NONCE_SIZE_96;

    CC_PalMemCopy(ctx->nonceBuf, nonce, CHACHA_IV_96_SIZE_BYTES);

    ctx->blockCounterLsb = counter;
    ctx->blockCounterMsb = 0;

    return PSA_SUCCESS;
}

psa_status_t cc3xx_chacha20_update(
        ChachaContext_t *ctx,
        size_t size,
        const uint8_t *input,
        unsigned char *output)
{
    drvError_t drvRc;
    CCBuffInfo_t inBuffInfo;
    CCBuffInfo_t outBuffInfo;

    if (0 == size) {
        return PSA_SUCCESS;
    }

    if (NULL == ctx || NULL == input || NULL == output) {
        CC_PAL_LOG_ERR("Null pointer exception\n");
        return PSA_ERROR_INVALID_ARGUMENT;
    }

    drvRc = SetDataBuffersInfo( input, size, &inBuffInfo,
                               output, size, &outBuffInfo);
    if (drvRc != 0) {
        CC_PAL_LOG_ERR("Bad i/o buffers\n");
        return PSA_ERROR_INVALID_ARGUMENT;
    }

    drvRc = ProcessChacha(ctx, &inBuffInfo, &outBuffInfo, size);
    if (drvRc != 0) {
        CC_PAL_LOG_ERR("cc3xx_chacha20_update failed: %d\n", drvRc);
        return PSA_ERROR_INVALID_ARGUMENT;
    }

    return PSA_SUCCESS;
}
/** @} */ // end of internal_chacha20
