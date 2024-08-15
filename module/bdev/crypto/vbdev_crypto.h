/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef SPDK_VBDEV_CRYPTO_H
#define SPDK_VBDEV_CRYPTO_H

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"

#include "spdk/bdev.h"

#define AESNI_MB "crypto_aesni_mb"
#define QAT "crypto_qat"
#define OPENSSL "crypto_openssl"

/* Supported ciphers */
#define AES_CBC "AES_CBC" /* QAT, AESNI_MB and OPENSSL*/
#define AES_XTS "AES_XTS" /* QAT only */
#define AES_CTR "AES_CTR" /* OPENSSL only */
#define CRYPTO_RSA "RSA" /* OPENSSL only */

#define RSA_NED "NED" /* OPENSSL only */
#define RSA_CRT "CRT" /* OPENSSL only */

typedef void (*spdk_delete_crypto_complete)(void *cb_arg, int bdeverrno);

/**
 * set crypto driver.
 *
 * \param driver_name Driver name that need to be initialized.
 * \return 0 on success, other on failure.
 */
int bdev_crypto_set_driver(const char *driver_name);

/**
 * Create new crypto bdev.
 *
 * \param bdev_name Name of the bdev on which the crypto vbdev will be created.
 * \param vbdev_name Name of the new crypto vbdev.
 * \param crypto_pmd Name of the polled mode driver to use for this vbdev.
 * \param key The key to use for this vbdev.
 * \param cipher The cipher to use for this vbdev.
 * \param keys The 2nd key to use for AES_XTS cipher.
 * \return 0 on success, other on failure.
 */
int create_crypto_disk(const char *bdev_name, const char *vbdev_name,
		       const char *crypto_pmd, const char *key,
		       const char *cipher, const char *key2);

int vbdev_cryptodev_set_engine(const char *engine_name);

/**
 * Delete crypto bdev.
 *
 * \param bdev Pointer to crypto bdev.
 * \param cb_fn Function to call after deletion.
 * \param cb_arg Argument to pass to cb_fn.
 */
void delete_crypto_disk(struct spdk_bdev *bdev, spdk_delete_crypto_complete cb_fn,
			void *cb_arg);

#endif /* SPDK_VBDEV_CRYPTO_H */
