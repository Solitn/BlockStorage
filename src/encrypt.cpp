#include "encrypt.h"
#include <cstring>

namespace encrypt {

static constexpr size_t SALT_SIZE    = 16;
static constexpr size_t NONCE_SIZE   = 12;
static constexpr size_t TAG_SIZE     = 16;
static constexpr size_t KEY_SIZE     = 32;
static constexpr uint32_t ITERATIONS = 100000;

// ---------- 内部：PBKDF2 派生密钥 ----------
static bool derive_key(const std::string& password,
                        const uint8_t* salt, size_t saltLen,
                        uint8_t* out, size_t outLen) {
    BCRYPT_ALG_HANDLE hPrf = NULL;
    if (BCryptOpenAlgorithmProvider(&hPrf, BCRYPT_SHA256_ALGORITHM,
                                     NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0)
        return false;

    NTSTATUS s = BCryptDeriveKeyPBKDF2(
        hPrf,
        (PUCHAR)password.data(), (ULONG)password.size(),
        (PUCHAR)salt, (ULONG)saltLen,
        ITERATIONS,
        out, (ULONG)outLen, 0);

    BCryptCloseAlgorithmProvider(hPrf, 0);
    return s == 0;
}

// ---------- 内部：随机字节 ----------
static bool random_bytes(uint8_t* out, size_t len) {
    return BCryptGenRandom(NULL, out, (ULONG)len,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

// ---------- 内部：打开 AES-GCM ----------
static bool open_gcm(BCRYPT_ALG_HANDLE& hAlg,
                     BCRYPT_KEY_HANDLE& hKey,
                     const uint8_t* key, size_t keyLen) {
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, NULL, 0) != 0)
        return false;

    if (BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                          (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                          sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }

    if (BCryptGenerateSymmetricKey(hAlg, &hKey, NULL, 0,
                                    (PUCHAR)key, (ULONG)keyLen, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }
    return true;
}

// ============================================================
// 1. 加密字符串
// 输出布局：salt(16) || nonce(12) || tag(16) || 密文
// ============================================================
std::vector<uint8_t> encrypt_string(const std::string& plain,
                                     const std::string& password) {
    uint8_t salt[SALT_SIZE];
    uint8_t nonce[NONCE_SIZE];
    if (!random_bytes(salt, SALT_SIZE))   return {};
    if (!random_bytes(nonce, NONCE_SIZE)) return {};

    uint8_t key[KEY_SIZE];
    if (!derive_key(password, salt, SALT_SIZE, key, KEY_SIZE)) return {};

    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_KEY_HANDLE hKey = NULL;
    if (!open_gcm(hAlg, hKey, key, KEY_SIZE)) {
        SecureZeroMemory(key, KEY_SIZE);
        return {};
    }

    std::vector<uint8_t> out(SALT_SIZE + NONCE_SIZE + TAG_SIZE + plain.size());
    std::memcpy(out.data(),              salt,  SALT_SIZE);
    std::memcpy(out.data() + SALT_SIZE,  nonce, NONCE_SIZE);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce;
    info.cbNonce = NONCE_SIZE;
    info.pbTag   = out.data() + SALT_SIZE + NONCE_SIZE;
    info.cbTag   = TAG_SIZE;

    ULONG written = 0;
    NTSTATUS s = BCryptEncrypt(
        hKey,
        (PUCHAR)plain.data(), (ULONG)plain.size(),
        &info, NULL, 0,
        out.data() + SALT_SIZE + NONCE_SIZE + TAG_SIZE,
        (ULONG)plain.size(), &written, 0);

    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    SecureZeroMemory(key, KEY_SIZE);

    if (s != 0 || written != plain.size()) return {};
    return out;
}

// ============================================================
// 2. SHA256
// ============================================================
std::vector<uint8_t> sha256(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out(32);
    BCRYPT_ALG_HANDLE  hAlg  = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0)
        return {};
    if (BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    if (BCryptHashData(hHash, (PUCHAR)data.data(), (ULONG)data.size(), 0) != 0) {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    if (BCryptFinishHash(hHash, out.data(), 32, 0) != 0) {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return out;
}

// ============================================================
// 内部：AES-CTR（加解密对称，长度不变）
// ============================================================
static bool aes_ctr(const std::vector<uint8_t>& input,
                     std::vector<uint8_t>* output,
                     const uint8_t* key, size_t keyLen,
                     const uint8_t iv[16]) {
    if (!output) return false;

    BCRYPT_ALG_HANDLE  hAlg = NULL;
    BCRYPT_KEY_HANDLE  hKey = NULL;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, NULL, 0) != 0)
        return false;

    if (BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                          (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                          sizeof(BCRYPT_CHAIN_MODE_ECB), 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }

    if (BCryptGenerateSymmetricKey(hAlg, &hKey, NULL, 0,
                                    (PUCHAR)key, (ULONG)keyLen, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }

    output->resize(input.size());

    uint8_t counter[16];
    std::memcpy(counter, iv, 16);
    uint8_t keystream[16];

    for (size_t i = 0; i < input.size(); ++i) {
        if (i % 16 == 0) {
            ULONG written = 0;
            BCryptEncrypt(hKey, counter, 16, NULL, NULL, 0,
                          keystream, 16, &written, 0);
            // 计数器自增（大端）
            for (int j = 15; j >= 0; --j) {
                if (++counter[j] != 0) break;
            }
        }
        (*output)[i] = input[i] ^ keystream[i % 16];
    }

    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return true;
}

// ============================================================
// 3. 文件加密（长度不变）
// ============================================================
bool encrypt_file(const std::vector<uint8_t>& input,
                  std::vector<uint8_t>* output,
                  const std::string& password) {
    if (!output) return false;

    static const char* FIXED_SALT = "encrypt-file-salt-v1";

    uint8_t derived[48];
    if (!derive_key(password,
                    (const uint8_t*)FIXED_SALT, strlen(FIXED_SALT),
                    derived, 48))
        return false;

    bool ok = aes_ctr(input, output, derived, 32, derived + 32);
    SecureZeroMemory(derived, sizeof(derived));
    return ok;
}

// ============================================================
// 4. 文件解密（长度不变）
// ============================================================
bool decrypt_file(const std::vector<uint8_t>& input,
                  std::vector<uint8_t>* output,
                  const std::string& password) {
    // CTR 加解密对称
    return encrypt_file(input, output, password);
}

} // namespace encrypt