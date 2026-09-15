// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    enum KFaceAuthCryptoStatus
    {
        KFACEAUTH_CRYPTO_OK = 0,
        KFACEAUTH_CRYPTO_INVALID_ARGUMENT = 1,
        KFACEAUTH_CRYPTO_PROVIDER_FAILURE = 2,
        KFACEAUTH_CRYPTO_AUTHENTICATION_FAILURE = 3,
    };

    int kfaceauth_crypto_random(uint8_t *output, size_t output_size);

    int kfaceauth_crypto_aes256gcm_encrypt(const uint8_t *key, size_t key_size, const uint8_t *nonce, size_t nonce_size,
                                           const uint8_t *associated_data, size_t associated_data_size,
                                           const uint8_t *plaintext, size_t plaintext_size, uint8_t *ciphertext,
                                           size_t ciphertext_capacity, size_t *ciphertext_size, uint8_t *tag,
                                           size_t tag_size);

    int kfaceauth_crypto_aes256gcm_decrypt(const uint8_t *key, size_t key_size, const uint8_t *nonce, size_t nonce_size,
                                           const uint8_t *associated_data, size_t associated_data_size,
                                           const uint8_t *ciphertext, size_t ciphertext_size, const uint8_t *tag,
                                           size_t tag_size, uint8_t *plaintext, size_t plaintext_capacity,
                                           size_t *plaintext_size);

    int kfaceauth_crypto_sha256(const uint8_t *input, size_t input_size, uint8_t *output, size_t output_size);

    uint32_t kfaceauth_current_uid(void);

#ifdef __cplusplus
}
#endif
