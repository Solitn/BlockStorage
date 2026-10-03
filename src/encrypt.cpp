#include "encrypt.h"
#include <cstring>

namespace encrypt {

    static constexpr size_t NONCE_SIZE = 12;
    static constexpr size_t TAG_SIZE   = 16;
    static constexpr size_t KEY_SIZE   = 32;

    // ---------- 内部：SHA256 ----------
    static bool sha256_raw(const uint8_t* data, size_t len, uint8_t out[32]) {
        BCRYPT_ALG_HANDLE  hAlg  = NULL;
        BCRYPT_HASH_HANDLE hHash = NULL;

        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM,
                                        NULL, 0) != 0)
            return false;

        if (BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0) != 0) {
            BCryptCloseAlgorithmProvider(hAlg, 0);
            return false;
        }

        BCryptHashData(hHash, (PUCHAR)data, (ULONG)len, 0);
        BCryptFinishHash(hHash, out, 32, 0);

        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return true;
    }

    // ---------- 内部：HMAC-SHA256 ----------
    static bool hmac_raw(const uint8_t* key, size_t keyLen,
                         const uint8_t* data, size_t dataLen,
                         uint8_t out[32]) {
        BCRYPT_ALG_HANDLE  hAlg  = NULL;
        BCRYPT_HASH_HANDLE hHash = NULL;

        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM,
                                        NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0)
            return false;

        if (BCryptCreateHash(hAlg, &hHash, NULL, 0,
                             (PUCHAR)key, (ULONG)keyLen, 0) != 0) {
            BCryptCloseAlgorithmProvider(hAlg, 0);
            return false;
        }

        BCryptHashData(hHash, (PUCHAR)data, (ULONG)dataLen, 0);
        BCryptFinishHash(hHash, out, 32, 0);

        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return true;
    }

    // ---------- 内部：密钥归一化 ----------
    static bool normalize_key(const std::vector<uint8_t>& key, uint8_t out[32]) {
        return sha256_raw(key.data(), key.size(), out);
    }

    // ---------- 内部：AES-CTR ----------
    static bool aes_ctr(const std::vector<uint8_t>& input,
                        std::vector<uint8_t>* output,
                        const uint8_t key[KEY_SIZE],
                        const uint8_t iv[16]) {
        if (!output) return false;

        BCRYPT_ALG_HANDLE hAlg = NULL;
        BCRYPT_KEY_HANDLE hKey = NULL;

        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM,
                                        NULL, 0) != 0)
            return false;

        if (BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                              (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                              sizeof(BCRYPT_CHAIN_MODE_ECB), 0) != 0) {
            BCryptCloseAlgorithmProvider(hAlg, 0);
            return false;
        }

        if (BCryptGenerateSymmetricKey(hAlg, &hKey, NULL, 0,
                                       (PUCHAR)key, KEY_SIZE, 0) != 0) {
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

    // ---------- 内部：派生流加密密钥和 IV ----------
    static bool derive_stream_keys(const std::vector<uint8_t>& key,
                                    uint8_t out_key[KEY_SIZE],
                                    uint8_t out_iv[16]) {
        static const uint8_t TAG_KEY[8] = {'s','t','r','k','e','y','0','1'};
        static const uint8_t TAG_IV [8] = {'s','t','r','i','v','0','1','0'};

        std::vector<uint8_t> in1(TAG_KEY, TAG_KEY + 8);
        in1.insert(in1.end(), key.begin(), key.end());

        std::vector<uint8_t> in2(TAG_IV, TAG_IV + 8);
        in2.insert(in2.end(), key.begin(), key.end());

        uint8_t h1[32], h2[32];
        if (!sha256_raw(in1.data(), in1.size(), h1)) return false;
        if (!sha256_raw(in2.data(), in2.size(), h2)) return false;

        std::memcpy(out_key, h1, KEY_SIZE);
        std::memcpy(out_iv,  h2, 16);
        return true;
    }

    // ============================================================
    // 1. 字符串加密（确定性）
    // ============================================================
    std::vector<uint8_t> encrypt_string(const std::vector<uint8_t>& plain,
                                        const std::vector<uint8_t>& key) {
        uint8_t k[KEY_SIZE];
        if (!normalize_key(key, k)) return {};

        uint8_t mac[32];
        if (!hmac_raw(k, KEY_SIZE, plain.data(), plain.size(), mac)) {
            SecureZeroMemory(k, KEY_SIZE);
            return {};
        }

        uint8_t nonce[NONCE_SIZE];
        std::memcpy(nonce, mac, NONCE_SIZE);

        BCRYPT_ALG_HANDLE hAlg = NULL;
        BCRYPT_KEY_HANDLE hKey = NULL;

        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM,
                                        NULL, 0) != 0) {
            SecureZeroMemory(k, KEY_SIZE);
            return {};
        }

        if (BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                              (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                              sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0) {
            BCryptCloseAlgorithmProvider(hAlg, 0);
            SecureZeroMemory(k, KEY_SIZE);
            return {};
        }

        if (BCryptGenerateSymmetricKey(hAlg, &hKey, NULL, 0,
                                       k, KEY_SIZE, 0) != 0) {
            BCryptCloseAlgorithmProvider(hAlg, 0);
            SecureZeroMemory(k, KEY_SIZE);
            return {};
        }

        std::vector<uint8_t> out(NONCE_SIZE + plain.size() + TAG_SIZE);
        std::memcpy(out.data(), nonce, NONCE_SIZE);

        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = nonce;
        info.cbNonce = NONCE_SIZE;
        info.pbTag   = out.data() + NONCE_SIZE + plain.size();
        info.cbTag   = TAG_SIZE;

        ULONG written = 0;
        NTSTATUS s = BCryptEncrypt(
            hKey,
            (PUCHAR)plain.data(), (ULONG)plain.size(),
            &info, NULL, 0,
            out.data() + NONCE_SIZE, (ULONG)plain.size(), &written, 0);

        BCryptDestroyKey(hKey);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        SecureZeroMemory(k, KEY_SIZE);

        if (s != 0 || written != plain.size()) return {};
        return out;
    }

    // ============================================================
    // 2. SHA256
    // ============================================================
    std::vector<uint8_t> sha256(const std::vector<uint8_t>& data) {
        std::vector<uint8_t> out(32);
        if (!sha256_raw(data.data(), data.size(), out.data()))
            return {};
        return out;
    }

    // ============================================================
    // 3. 文件流加密（长度不变）
    // ============================================================
    bool encrypt_stream(const std::vector<uint8_t>& input,
                        std::vector<uint8_t>* output,
                        const std::vector<uint8_t>& key) {
        uint8_t k[KEY_SIZE];
        uint8_t iv[16];
        if (!derive_stream_keys(key, k, iv)) return false;

        bool ok = aes_ctr(input, output, k, iv);
        SecureZeroMemory(k, KEY_SIZE);
        SecureZeroMemory(iv, 16);
        return ok;
    }

    // ============================================================
    // 4. 文件流解密（长度不变）
    // ============================================================
    bool decrypt_stream(const std::vector<uint8_t>& input,
                        std::vector<uint8_t>* output,
                        const std::vector<uint8_t>& key) {
        uint8_t k[KEY_SIZE];
        uint8_t iv[16];
        if (!derive_stream_keys(key, k, iv)) return false;

        bool ok = aes_ctr(input, output, k, iv);
        SecureZeroMemory(k, KEY_SIZE);
        SecureZeroMemory(iv, 16);
        return ok;
    }

    // ============================================================
    // 5. 随机一个字节
    // ============================================================
    uint8_t random_byte() {
        uint8_t b = 0;
        BCryptGenRandom(NULL, &b, 1, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        return b;
    }

}