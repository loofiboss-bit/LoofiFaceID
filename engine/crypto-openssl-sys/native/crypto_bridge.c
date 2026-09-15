// SPDX-License-Identifier: GPL-3.0-or-later

#include "crypto_bridge.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <limits.h>
#include <string.h>
#include <unistd.h>

enum
{
    KeyBytes = 32,
    NonceBytes = 12,
    TagBytes = 16,
};

static int valid_buffer(const uint8_t *buffer, size_t size)
{
    return size == 0 || buffer != NULL;
}

static int fits_provider_int(size_t size)
{
    return size <= (size_t)INT_MAX;
}

int kfaceauth_crypto_random(uint8_t *output, size_t output_size)
{
    if (output == NULL || output_size == 0 || !fits_provider_int(output_size))
        return KFACEAUTH_CRYPTO_INVALID_ARGUMENT;
    if (RAND_bytes(output, (int)output_size) != 1)
    {
        OPENSSL_cleanse(output, output_size);
        return KFACEAUTH_CRYPTO_PROVIDER_FAILURE;
    }
    return KFACEAUTH_CRYPTO_OK;
}

int kfaceauth_crypto_aes256gcm_encrypt(const uint8_t *key, size_t key_size, const uint8_t *nonce, size_t nonce_size,
                                       const uint8_t *associated_data, size_t associated_data_size,
                                       const uint8_t *plaintext, size_t plaintext_size, uint8_t *ciphertext,
                                       size_t ciphertext_capacity, size_t *ciphertext_size, uint8_t *tag,
                                       size_t tag_size)
{
    if (key == NULL || key_size != KeyBytes || nonce == NULL || nonce_size != NonceBytes ||
        !valid_buffer(associated_data, associated_data_size) || !valid_buffer(plaintext, plaintext_size) ||
        ciphertext == NULL || ciphertext_capacity != plaintext_size || ciphertext_size == NULL || tag == NULL ||
        tag_size != TagBytes || !fits_provider_int(associated_data_size) || !fits_provider_int(plaintext_size))
        return KFACEAUTH_CRYPTO_INVALID_ARGUMENT;

    *ciphertext_size = 0;
    OPENSSL_cleanse(ciphertext, ciphertext_capacity);
    OPENSSL_cleanse(tag, tag_size);
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    if (context == NULL)
        return KFACEAUTH_CRYPTO_PROVIDER_FAILURE;

    int output_size = 0;
    int final_size = 0;
    int status = KFACEAUTH_CRYPTO_PROVIDER_FAILURE;
    if (EVP_EncryptInit_ex2(context, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1 ||
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, (int)nonce_size, NULL) != 1 ||
        EVP_EncryptInit_ex2(context, NULL, key, nonce, NULL) != 1)
        goto cleanup;
    if (associated_data_size > 0 &&
        EVP_EncryptUpdate(context, NULL, &output_size, associated_data, (int)associated_data_size) != 1)
        goto cleanup;
    output_size = 0;
    if (plaintext_size > 0 && EVP_EncryptUpdate(context, ciphertext, &output_size, plaintext, (int)plaintext_size) != 1)
        goto cleanup;
    if (EVP_EncryptFinal_ex(context, ciphertext + (size_t)output_size, &final_size) != 1)
        goto cleanup;
    if ((size_t)output_size + (size_t)final_size != plaintext_size ||
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, (int)tag_size, tag) != 1)
        goto cleanup;
    *ciphertext_size = plaintext_size;
    status = KFACEAUTH_CRYPTO_OK;

cleanup:
    EVP_CIPHER_CTX_free(context);
    if (status != KFACEAUTH_CRYPTO_OK)
    {
        OPENSSL_cleanse(ciphertext, ciphertext_capacity);
        OPENSSL_cleanse(tag, tag_size);
        *ciphertext_size = 0;
    }
    return status;
}

int kfaceauth_crypto_aes256gcm_decrypt(const uint8_t *key, size_t key_size, const uint8_t *nonce, size_t nonce_size,
                                       const uint8_t *associated_data, size_t associated_data_size,
                                       const uint8_t *ciphertext, size_t ciphertext_size, const uint8_t *tag,
                                       size_t tag_size, uint8_t *plaintext, size_t plaintext_capacity,
                                       size_t *plaintext_size)
{
    if (key == NULL || key_size != KeyBytes || nonce == NULL || nonce_size != NonceBytes ||
        !valid_buffer(associated_data, associated_data_size) || !valid_buffer(ciphertext, ciphertext_size) ||
        tag == NULL || tag_size != TagBytes || plaintext == NULL || plaintext_capacity != ciphertext_size ||
        plaintext_size == NULL || !fits_provider_int(associated_data_size) || !fits_provider_int(ciphertext_size))
        return KFACEAUTH_CRYPTO_INVALID_ARGUMENT;

    *plaintext_size = 0;
    OPENSSL_cleanse(plaintext, plaintext_capacity);
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    if (context == NULL)
        return KFACEAUTH_CRYPTO_PROVIDER_FAILURE;

    int output_size = 0;
    int final_size = 0;
    int status = KFACEAUTH_CRYPTO_PROVIDER_FAILURE;
    if (EVP_DecryptInit_ex2(context, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1 ||
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, (int)nonce_size, NULL) != 1 ||
        EVP_DecryptInit_ex2(context, NULL, key, nonce, NULL) != 1)
        goto cleanup;
    if (associated_data_size > 0 &&
        EVP_DecryptUpdate(context, NULL, &output_size, associated_data, (int)associated_data_size) != 1)
        goto cleanup;
    output_size = 0;
    if (ciphertext_size > 0 &&
        EVP_DecryptUpdate(context, plaintext, &output_size, ciphertext, (int)ciphertext_size) != 1)
        goto cleanup;
    if (EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, (int)tag_size, (void *)tag) != 1)
        goto cleanup;
    if (EVP_DecryptFinal_ex(context, plaintext + (size_t)output_size, &final_size) != 1)
    {
        status = KFACEAUTH_CRYPTO_AUTHENTICATION_FAILURE;
        goto cleanup;
    }
    if ((size_t)output_size + (size_t)final_size != ciphertext_size)
        goto cleanup;
    *plaintext_size = ciphertext_size;
    status = KFACEAUTH_CRYPTO_OK;

cleanup:
    EVP_CIPHER_CTX_free(context);
    if (status != KFACEAUTH_CRYPTO_OK)
    {
        OPENSSL_cleanse(plaintext, plaintext_capacity);
        *plaintext_size = 0;
    }
    return status;
}

int kfaceauth_crypto_sha256(const uint8_t *input, size_t input_size, uint8_t *output, size_t output_size)
{
    if (output == NULL || output_size != 32 || (!valid_buffer(input, input_size)) || !fits_provider_int(input_size))
        return KFACEAUTH_CRYPTO_INVALID_ARGUMENT;

    size_t digest_size = 0;
    if (EVP_Q_digest(NULL, "SHA256", NULL, input, input_size, output, &digest_size) != 1 || digest_size != 32)
    {
        OPENSSL_cleanse(output, output_size);
        return KFACEAUTH_CRYPTO_PROVIDER_FAILURE;
    }
    return KFACEAUTH_CRYPTO_OK;
}

uint32_t kfaceauth_current_uid(void)
{
    return (uint32_t)getuid();
}
