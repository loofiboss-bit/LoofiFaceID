// SPDX-License-Identifier: GPL-3.0-or-later

#define _GNU_SOURCE

#include "crypto_bridge.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/videodev2.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(KFACEAUTH_HAS_TPM2) && KFACEAUTH_HAS_TPM2
#include <tss2/tss2_esys.h>
#include <tss2/tss2_mu.h>
#endif

#if defined(KFACEAUTH_HAS_KEYUTILS) && KFACEAUTH_HAS_KEYUTILS
#include <keyutils.h>
#endif

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

int kfaceauth_socket_peer_cred(int socket_fd, uint32_t *uid, uint32_t *gid, int32_t *pid)
{
    if (socket_fd < 0 || uid == NULL || gid == NULL || pid == NULL)
        return KFACEAUTH_CRYPTO_INVALID_ARGUMENT;

    struct ucred cred;
    socklen_t len = (socklen_t)sizeof(cred);
    memset(&cred, 0, sizeof(cred));
    if (getsockopt(socket_fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || len < (socklen_t)sizeof(cred))
        return KFACEAUTH_CRYPTO_PROVIDER_FAILURE;

    *uid = (uint32_t)cred.uid;
    *gid = (uint32_t)cred.gid;
    *pid = (int32_t)cred.pid;
    return KFACEAUTH_CRYPTO_OK;
}

int kfaceauth_drop_privileges(const char *username, const char *groupname)
{
    if (username == NULL || groupname == NULL)
        return KFACEAUTH_CRYPTO_INVALID_ARGUMENT;

    if (geteuid() != 0)
    {
        return KFACEAUTH_CRYPTO_OK;
    }

    struct passwd *pw = getpwnam(username);
    if (pw == NULL)
        return KFACEAUTH_CRYPTO_PROVIDER_FAILURE;

    struct group *gr = getgrnam(groupname);
    if (gr == NULL)
        return KFACEAUTH_CRYPTO_PROVIDER_FAILURE;

    if (initgroups(username, gr->gr_gid) != 0)
        return KFACEAUTH_CRYPTO_PROVIDER_FAILURE;

    if (setgid(gr->gr_gid) != 0)
        return KFACEAUTH_CRYPTO_PROVIDER_FAILURE;

    if (setuid(pw->pw_uid) != 0)
        return KFACEAUTH_CRYPTO_PROVIDER_FAILURE;

    if (geteuid() == 0 || getegid() == 0)
        return KFACEAUTH_CRYPTO_PROVIDER_FAILURE;

    return KFACEAUTH_CRYPTO_OK;
}

#define DEFAULT_KEYS_DIR "/etc/kfaceauth/keys"

static int resolve_keys_dir(char *buffer, size_t capacity, const char *custom)
{
    if (buffer == NULL || capacity == 0)
        return -1;
    const char *dir = custom;
    if (dir == NULL || dir[0] == '\0')
    {
        dir = getenv("KFACEAUTH_KEYS_DIR");
        if (dir == NULL || dir[0] == '\0')
            dir = DEFAULT_KEYS_DIR;
    }
    size_t len = strlen(dir);
    if (len >= capacity)
        return -1;
    memcpy(buffer, dir, len + 1);
    return 0;
}

static int read_key_file(const char *path, uint8_t *key_out, size_t key_len)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return -1;

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & 0777) != 0600 || st.st_size != (off_t)key_len ||
        st.st_nlink != 1)
    {
        close(fd);
        return -1;
    }

    size_t total_read = 0;
    while (total_read < key_len)
    {
        ssize_t n = read(fd, key_out + total_read, key_len - total_read);
        if (n <= 0)
        {
            close(fd);
            OPENSSL_cleanse(key_out, key_len);
            return -1;
        }
        total_read += (size_t)n;
    }
    close(fd);
    return 0;
}

static int ensure_dir_exists(const char *dir)
{
    struct stat st;
    if (stat(dir, &st) == 0)
    {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    char tmp[512];
    size_t len = strlen(dir);
    if (len >= sizeof(tmp))
        return -1;
    memcpy(tmp, dir, len + 1);
    for (char *p = tmp + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            if (stat(tmp, &st) != 0)
            {
                mkdir(tmp, 0700);
            }
            *p = '/';
        }
    }
    return (mkdir(tmp, 0700) == 0 || errno == EEXIST) ? 0 : -1;
}

static int write_key_file(const char *dir, const char *final_path, const uint8_t *key, size_t key_len)
{
    char tmp_path[512];
    uint8_t rand_suffix[8];
    if (kfaceauth_crypto_random(rand_suffix, sizeof(rand_suffix)) != KFACEAUTH_CRYPTO_OK)
        return -1;

    int written_len = snprintf(tmp_path, sizeof(tmp_path), "%s/.key_%02x%02x%02x%02x.tmp", dir, rand_suffix[0],
                               rand_suffix[1], rand_suffix[2], rand_suffix[3]);
    if (written_len < 0 || (size_t)written_len >= sizeof(tmp_path))
        return -1;

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;

    size_t total_written = 0;
    while (total_written < key_len)
    {
        ssize_t n = write(fd, key + total_written, key_len - total_written);
        if (n <= 0)
        {
            close(fd);
            unlink(tmp_path);
            return -1;
        }
        total_written += (size_t)n;
    }

    if (fchmod(fd, 0600) != 0 || fsync(fd) != 0)
    {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    close(fd);

    if (rename(tmp_path, final_path) != 0)
    {
        unlink(tmp_path);
        return -1;
    }

    int dir_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd >= 0)
    {
        fsync(dir_fd);
        close(dir_fd);
    }
    return 0;
}

#if defined(KFACEAUTH_HAS_TPM2) && KFACEAUTH_HAS_TPM2
static int try_tpm2_unseal(const char *tpm_path, uint8_t *key_out, size_t key_len)
{
    (void)key_out;
    if (access(tpm_path, R_OK) != 0)
        return -1;

    ESYS_CONTEXT *ctx = NULL;
    if (Esys_Initialize(&ctx, NULL, NULL) != TSS2_RC_SUCCESS)
        return -1;

    FILE *fp = fopen(tpm_path, "rb");
    if (fp == NULL)
    {
        Esys_Finalize(&ctx);
        return -1;
    }
    uint8_t buffer[1024];
    size_t len = fread(buffer, 1, sizeof(buffer), fp);
    fclose(fp);

    if (len < key_len)
    {
        Esys_Finalize(&ctx);
        return -1;
    }

    Esys_Finalize(&ctx);
    return -1;
}
#endif

int kfaceauth_master_key_for_uid(uint32_t uid, uint8_t *key_out, size_t key_len, const char *custom_keys_dir)
{
    if (key_out == NULL || key_len != KeyBytes)
        return KFACEAUTH_CRYPTO_INVALID_ARGUMENT;

    char dir[384];
    if (resolve_keys_dir(dir, sizeof(dir), custom_keys_dir) != 0)
        return KFACEAUTH_CRYPTO_INVALID_ARGUMENT;

    ensure_dir_exists(dir);

    char key_path[512];
    int path_len = snprintf(key_path, sizeof(key_path), "%s/%u.key", dir, uid);
    if (path_len < 0 || (size_t)path_len >= sizeof(key_path))
        return KFACEAUTH_CRYPTO_INVALID_ARGUMENT;

#if defined(KFACEAUTH_HAS_TPM2) && KFACEAUTH_HAS_TPM2
    char tpm_path[512];
    int tpm_len = snprintf(tpm_path, sizeof(tpm_path), "%s/%u.tpm", dir, uid);
    if (tpm_len >= 0 && (size_t)tpm_len < sizeof(tpm_path))
    {
        if (try_tpm2_unseal(tpm_path, key_out, key_len) == 0)
            return KFACEAUTH_CRYPTO_OK;
    }
#endif

    if (read_key_file(key_path, key_out, key_len) == 0)
    {
#if defined(KFACEAUTH_HAS_KEYUTILS) && KFACEAUTH_HAS_KEYUTILS
        char desc[64];
        snprintf(desc, sizeof(desc), "kfaceauth:%u", uid);
        add_key("user", desc, key_out, key_len, KEY_SPEC_USER_KEYRING);
#endif
        return KFACEAUTH_CRYPTO_OK;
    }

    if (kfaceauth_crypto_random(key_out, key_len) != KFACEAUTH_CRYPTO_OK)
        return KFACEAUTH_CRYPTO_PROVIDER_FAILURE;

    if (write_key_file(dir, key_path, key_out, key_len) != 0)
    {
        OPENSSL_cleanse(key_out, key_len);
        return KFACEAUTH_CRYPTO_PROVIDER_FAILURE;
    }

#if defined(KFACEAUTH_HAS_KEYUTILS) && KFACEAUTH_HAS_KEYUTILS
    char desc[64];
    snprintf(desc, sizeof(desc), "kfaceauth:%u", uid);
    add_key("user", desc, key_out, key_len, KEY_SPEC_USER_KEYRING);
#endif

    return KFACEAUTH_CRYPTO_OK;
}

int kfaceauth_seal_master_key(uint32_t uid, const uint8_t *key_in, size_t key_len, const char *custom_keys_dir)
{
    if (key_in == NULL || key_len != KeyBytes)
        return KFACEAUTH_CRYPTO_INVALID_ARGUMENT;

    char dir[384];
    if (resolve_keys_dir(dir, sizeof(dir), custom_keys_dir) != 0)
        return KFACEAUTH_CRYPTO_INVALID_ARGUMENT;

    ensure_dir_exists(dir);

    char key_path[512];
    int path_len = snprintf(key_path, sizeof(key_path), "%s/%u.key", dir, uid);
    if (path_len < 0 || (size_t)path_len >= sizeof(key_path))
        return KFACEAUTH_CRYPTO_INVALID_ARGUMENT;

    if (write_key_file(dir, key_path, key_in, key_len) != 0)
        return KFACEAUTH_CRYPTO_PROVIDER_FAILURE;

#if defined(KFACEAUTH_HAS_KEYUTILS) && KFACEAUTH_HAS_KEYUTILS
    char desc[64];
    snprintf(desc, sizeof(desc), "kfaceauth:%u", uid);
    add_key("user", desc, key_in, key_len, KEY_SPEC_USER_KEYRING);
#endif

    return KFACEAUTH_CRYPTO_OK;
}

int kfaceauth_systemd_listen_fds(void)
{
    const char *listen_pid_str = getenv("LISTEN_PID");
    const char *listen_fds_str = getenv("LISTEN_FDS");
    if (listen_pid_str == NULL || listen_fds_str == NULL)
        return 0;

    pid_t pid = (pid_t)atoi(listen_pid_str);
    if (pid != getpid())
        return 0;

    int fds = atoi(listen_fds_str);
    if (fds < 1)
        return 0;

    unsetenv("LISTEN_PID");
    unsetenv("LISTEN_FDS");
    return fds;
}

int kfaceauth_set_socket_permissions(const char *path, uint32_t mode, const char *groupname)
{
    if (path == NULL)
        return KFACEAUTH_CRYPTO_INVALID_ARGUMENT;

    if (chmod(path, (mode_t)mode) != 0)
        return KFACEAUTH_CRYPTO_PROVIDER_FAILURE;

    if (groupname != NULL && groupname[0] != '\0')
    {
        struct group *gr = getgrnam(groupname);
        if (gr != NULL)
        {
            if (chown(path, (uid_t)-1, gr->gr_gid) != 0)
            {
                // Non-fatal if unprivileged
            }
        }
    }
    return KFACEAUTH_CRYPTO_OK;
}

int kfaceauth_v4l2_capture(const char *device_path, uint32_t timeout_ms, uint8_t *buffer, size_t buffer_size,
                           uint32_t *width_out, uint32_t *height_out, uint32_t *format_out)
{
    (void)timeout_ms;
    (void)buffer;
    (void)buffer_size;
    (void)width_out;
    (void)height_out;
    (void)format_out;

    const char *path = (device_path != NULL && device_path[0] != '\0') ? device_path : "/dev/video0";
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return KFACEAUTH_CRYPTO_PROVIDER_FAILURE;

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0)
    {
        close(fd);
        return KFACEAUTH_CRYPTO_PROVIDER_FAILURE;
    }

    close(fd);
    return KFACEAUTH_CRYPTO_PROVIDER_FAILURE;
}
