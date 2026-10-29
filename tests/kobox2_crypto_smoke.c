/* SPDX-License-Identifier: MIT */
/* A real Linux/musl ELF: no native caps, test RNG, crypto stubs or OSS edits. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

static volatile sig_atomic_t user_signals, timer_signals;
extern int crypto_signal_fp_probe(int pid, int tid, int signo);
extern void crypto_signal_clobber_fp(void);

static _Noreturn void fail(const char *stage, unsigned line)
{
    fprintf(stderr, "CRYPTO_SMOKE=FAIL stage=%s line=%u errno=%d\n", stage, line, errno);
    ERR_print_errors_fp(stderr);
    exit(1);
}
#define CHECK(stage, test) do { if (!(test)) fail(stage, __LINE__); } while (0)

static void expect_hex(const char *stage, const unsigned char *bytes,
    size_t length, const char *hex)
{
    static const char digits[] = "0123456789abcdef";
    CHECK(stage, strlen(hex) == 2 * length);
    for (size_t i = 0; i < length; ++i)
        CHECK(stage, digits[bytes[i] >> 4] == hex[2 * i] &&
            digits[bytes[i] & 15] == hex[2 * i + 1]);
}

static void read_random(unsigned char *bytes, size_t size)
{
    size_t used = 0;
    while (used < size) {
        ssize_t count = getrandom(bytes + used, size - used, GRND_NONBLOCK);
        if (count < 0 && errno == EINTR) continue;
        /* EAGAIN is a failure: the runner must seed Linux before launch. */
        CHECK("getrandom", count > 0 && (size_t)count <= size - used);
        used += (size_t)count;
    }
}

static void entropy(void)
{
    unsigned char first[64], second[64], zero[64] = {0};
    read_random(first, sizeof(first));
    read_random(second, sizeof(second));
    /* Only catch obvious stuck output. These checks cannot establish entropy
     * quality/provenance: the host's RDSEED injection is a separate boundary. */
    CHECK("getrandom", CRYPTO_memcmp(first, second, sizeof(first)) != 0 &&
        CRYPTO_memcmp(first, zero, sizeof(first)) != 0 &&
        CRYPTO_memcmp(second, zero, sizeof(second)) != 0);
    CHECK("openssl-rand", RAND_status() == 1 && RAND_priv_bytes(first, sizeof(first)) == 1);
    OPENSSL_cleanse(first, sizeof(first));
    OPENSSL_cleanse(second, sizeof(second));
}

static void aes(void)
{
    /* FIPS 197 AES-128/256 examples, with padding explicitly disabled. */
    const unsigned char plain[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
        0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
    unsigned char key[32], out[48], recovered[32];
    for (unsigned i = 0; i < sizeof(key); ++i) key[i] = (unsigned char)i;
    for (unsigned bits = 128; bits <= 256; bits += 128) {
        const EVP_CIPHER *cipher = bits == 128 ? EVP_aes_128_ecb() : EVP_aes_256_ecb();
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        int count = 0, tail = 0;
        CHECK("aes", ctx && EVP_EncryptInit_ex(ctx, cipher, NULL, key, NULL) == 1 &&
            EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
            EVP_EncryptUpdate(ctx, out, &count, plain, sizeof(plain)) == 1 &&
            EVP_EncryptFinal_ex(ctx, out + count, &tail) == 1 && count + tail == 16);
        expect_hex("aes", out, 16, bits == 128 ?
            "69c4e0d86a7b0430d8cdb78070b4c55a" : "8ea2b7ca516745bfeafc49904b496089");
        CHECK("aes-decrypt", EVP_DecryptInit_ex(ctx, cipher, NULL, key, NULL) == 1 &&
            EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
            EVP_DecryptUpdate(ctx, recovered, &count, out, 16) == 1 &&
            EVP_DecryptFinal_ex(ctx, recovered + count, &tail) == 1 && count + tail == 16 &&
            CRYPTO_memcmp(recovered, plain, 16) == 0);
        EVP_CIPHER_CTX_free(ctx);
    }
    /* NIST GCM zero-key, zero-IV, one zero-block vector (also upstream EVP tests). */
    const unsigned char zero[16] = {0}, iv[12] = {0};
    unsigned char tag[16];
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int count = 0, tail = 0;
    CHECK("gcm", ctx && EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, zero, iv) == 1 &&
        EVP_EncryptUpdate(ctx, out, &count, zero, 16) == 1 &&
        EVP_EncryptFinal_ex(ctx, out + count, &tail) == 1 && count + tail == 16 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1);
    expect_hex("gcm", out, 16, "0388dace60b6a392f328c2b971b2fe78");
    expect_hex("gcm-tag", tag, 16, "ab6e47d42cec13bdf53a67b21257bddf");
    for (unsigned corrupt = 0; corrupt < 2; ++corrupt) {
        tag[0] ^= corrupt;
        CHECK("gcm-decrypt", EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, zero, iv) == 1 &&
            EVP_DecryptUpdate(ctx, recovered, &count, out, 16) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag) == 1);
        int result = EVP_DecryptFinal_ex(ctx, recovered + count, &tail);
        CHECK("gcm-auth", corrupt ? result == 0 :
            (result == 1 && count + tail == 16 && CRYPTO_memcmp(recovered, zero, 16) == 0));
    }
    EVP_CIPHER_CTX_free(ctx);
}

static void hashes_and_kdf(void)
{
    unsigned char out[64], key[20];
    unsigned int digest_size = 0;
    size_t size = 0;
    CHECK("sha256", EVP_Digest("abc", 3, out, &digest_size, EVP_sha256(), NULL) == 1 && digest_size == 32);
    expect_hex("sha256", out, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    /* RFC 4231 case 1. */
    memset(key, 0x0b, sizeof(key));
    CHECK("hmac", EVP_Q_mac(NULL, "HMAC", NULL, "SHA256", NULL, key, sizeof(key),
        (const unsigned char *)"Hi There", 8, out, sizeof(out), &size) && size == 32);
    expect_hex("hmac", out, size, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    /* NIST SP 800-38B, AES-CMAC empty-message example. */
    const unsigned char cmac_key[16] = {0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
        0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
    CHECK("cmac", EVP_Q_mac(NULL, "CMAC", NULL, "AES-128-CBC", NULL, cmac_key, 16,
        (const unsigned char *)"", 0, out, sizeof(out), &size) && size == 16);
    expect_hex("cmac", out, size, "bb1d6929e95937287fa37d129b756746");
    /* Upstream OpenSSL's PBKDF2-HMAC-SHA256 password/salt vector. */
    CHECK("pbkdf2-sha256", PKCS5_PBKDF2_HMAC("password", 8,
        (const unsigned char *)"salt", 4, 1, EVP_sha256(), 32, out) == 1);
    expect_hex("pbkdf2-sha256", out, 32, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    /* WPA2's actual passphrase-to-PSK iteration count and SHA-1 PRF. */
    CHECK("pbkdf2-wpa", PKCS5_PBKDF2_HMAC("password", 8,
        (const unsigned char *)"IEEE", 4, 4096, EVP_sha1(), 32, out) == 1);
    expect_hex("pbkdf2-wpa", out, 32, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e");
}

static void sign_verify(EVP_PKEY *key)
{
    unsigned char signature[128];
    size_t size = sizeof(signature);
    const unsigned char message[] = "PachaOS hosted OpenSSL P-256 signal smoke";
    unsigned char changed[sizeof(message)];
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    CHECK("p256-sign", ctx && EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, key) == 1 &&
        EVP_DigestSign(ctx, signature, &size, message, sizeof(message) - 1) == 1);
    CHECK("p256-verify", EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, key) == 1 &&
        EVP_DigestVerify(ctx, signature, size, message, sizeof(message) - 1) == 1);
    memcpy(changed, message, sizeof(message));
    changed[0] ^= 1;
    CHECK("p256-reject", EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, key) == 1 &&
        EVP_DigestVerify(ctx, signature, size, changed, sizeof(changed) - 1) == 0);
    EVP_MD_CTX_free(ctx);
    ERR_clear_error();
}

static void signal_handler(int signo)
{
    if (signo == SIGUSR1) ++user_signals;
    if (signo == SIGALRM) ++timer_signals;
    /* No OpenSSL/libc calls in this handler. The assembly deliberately
     * dirties x87/MXCSR/XMM, to require real sigreturn FP restoration. */
    crypto_signal_clobber_fp();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("CRYPTO_OPENSSL=%s\n", OpenSSL_version(OPENSSL_VERSION));
    CHECK("init", OPENSSL_init_crypto(OPENSSL_INIT_NO_LOAD_CONFIG, NULL) == 1);
    entropy();
    puts("CRYPTO_GETRANDOM=PASS");
    aes();
    hashes_and_kdf();
    puts("CRYPTO_KNOWN_VECTORS=PASS");
    EVP_PKEY *key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "prime256v1");
    CHECK("p256-keygen", key);
    sign_verify(key);
    puts("CRYPTO_P256=PASS");

    struct sigaction action = {.sa_handler = signal_handler, .sa_flags = SA_RESTART};
    CHECK("sigaction", sigemptyset(&action.sa_mask) == 0 &&
        sigaction(SIGUSR1, &action, NULL) == 0 && sigaction(SIGALRM, &action, NULL) == 0);
    CHECK("signal-fp", crypto_signal_fp_probe(getpid(), (int)syscall(SYS_gettid), SIGUSR1) == 0 &&
        user_signals == 1);
    puts("CRYPTO_SIGNAL_FP=PASS");
    struct itimerval timer = {.it_interval = {.tv_usec = 1000},
        .it_value = {.tv_usec = 1000}}, off = {0};
    CHECK("timer", setitimer(ITIMER_REAL, &timer, NULL) == 0);
    /* Finite real crypto work, not a wait/spin for signal delivery. Both
     * synchronous signal restoration and timed preemption must pass. */
    for (unsigned round = 0; round < 2; ++round) {
        aes();
        hashes_and_kdf();
        sign_verify(key);
    }
    CHECK("timer-stop", setitimer(ITIMER_REAL, &off, NULL) == 0);
    CHECK("timer-delivery", timer_signals > 0);
    printf("CRYPTO_SIGNAL_WORK=PASS signals=%d\n", (int)timer_signals);
    EVP_PKEY_free(key);
    puts("CRYPTO_SMOKE=PASS");
    return 0;
}
