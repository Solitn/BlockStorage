#include "encrypt.h"
#include <cstring>
#include <string>
#include <vector>
namespace encrypt {
    static const char* CHARS =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    // ============================================================
    // 编码
    // ============================================================
    std::string encode(const std::vector<uint8_t>& data) {
        std::string out;
        out.reserve(((data.size() + 2) / 3) * 4);

        size_t i = 0;
        while (i + 2 < data.size()) {
            uint32_t n = (data[i] << 16) | (data[i+1] << 8) | data[i+2];
            out.push_back(CHARS[(n >> 18) & 0x3F]);
            out.push_back(CHARS[(n >> 12) & 0x3F]);
            out.push_back(CHARS[(n >>  6) & 0x3F]);
            out.push_back(CHARS[(n      ) & 0x3F]);
            i += 3;
        }

        if (i + 1 == data.size()) {
            uint32_t n = data[i] << 16;
            out.push_back(CHARS[(n >> 18) & 0x3F]);
            out.push_back(CHARS[(n >> 12) & 0x3F]);
            out.push_back('=');
            out.push_back('=');
        }
        else if (i + 2 == data.size()) {
            uint32_t n = (data[i] << 16) | (data[i+1] << 8);
            out.push_back(CHARS[(n >> 18) & 0x3F]);
            out.push_back(CHARS[(n >> 12) & 0x3F]);
            out.push_back(CHARS[(n >>  6) & 0x3F]);
            out.push_back('=');
        }

        return out;
    }

    std::string encode(const std::string& text) {
        return encode(std::vector<uint8_t>(text.begin(), text.end()));
    }

    // ============================================================
    // 解码
    // ============================================================
    static int decode_char(char c) {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    }

    std::vector<uint8_t> decode(const std::string& text) {
        std::vector<uint8_t> out;
        out.reserve((text.size() / 4) * 3);

        int buf = 0;
        int bits = 0;

        for (char c : text) {
            if (c == '=' || c == '\n' || c == '\r' || c == ' ')
                continue;

            int v = decode_char(c);
            if (v < 0) continue;

            buf = (buf << 6) | v;
            bits += 6;

            if (bits >= 8) {
                bits -= 8;
                out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
            }
        }

        return out;
    }
    // ---------- 内部：SHA256 归一化密钥到 32 字节 ----------
    static bool normalize_key(const std::vector<uint8_t>& key,
                              uint8_t out[32]) {
        BCRYPT_ALG_HANDLE  hAlg  = NULL;
        BCRYPT_HASH_HANDLE hHash = NULL;

        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM,
                                        NULL, 0) != 0)
            return false;

        if (BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0) != 0) {
            BCryptCloseAlgorithmProvider(hAlg, 0);
            return false;
        }

        if (BCryptHashData(hHash, (PUCHAR)key.data(),
                           (ULONG)key.size(), 0) != 0) {
            BCryptDestroyHash(hHash);
            BCryptCloseAlgorithmProvider(hAlg, 0);
            return false;
        }

        if (BCryptFinishHash(hHash, out, 32, 0) != 0) {
            BCryptDestroyHash(hHash);
            BCryptCloseAlgorithmProvider(hAlg, 0);
            return false;
        }

        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return true;
    }

    // ---------- 内部：AES-256-CTR（加解密对称） ----------
    static bool aes_ctr(const std::vector<uint8_t>& input,
                        std::vector<uint8_t>* output,
                        const uint8_t key[32]) {
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
                                    (PUCHAR)key, 32, 0) != 0) {
            BCryptCloseAlgorithmProvider(hAlg, 0);
            return false;
        }

        output->resize(input.size());

        uint8_t counter[16] = {0};
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
    uint8_t random_byte() {
        uint8_t b = 0;
        BCryptGenRandom(NULL, &b, 1, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        return b;
    }
    // ============================================================
    // 1. 加密字符串
    // 输入：明文 + 密钥，输出纯密文，长度 = 明文长度
    // ============================================================
    std::vector<uint8_t> encrypt_string(const U8list& plain,
                                        const U8list& key) {
        uint8_t k[32];
        if (!normalize_key(key, k)) return {};

        std::vector<uint8_t> in(plain.begin(), plain.end());
        std::vector<uint8_t> out;
        if (!aes_ctr(in, &out, k)) {
            SecureZeroMemory(k, 32);
            return {};
        }
        SecureZeroMemory(k, 32);
        return out;
    }

    // ============================================================
    // 2. 解密字符串
    // ============================================================
    std::string decrypt_string(const std::vector<uint8_t>& cipher,
                            const std::vector<uint8_t>& key) {
        uint8_t k[32];
        if (!normalize_key(key, k)) return "";

        std::vector<uint8_t> out;
        if (!aes_ctr(cipher, &out, k)) {
            SecureZeroMemory(k, 32);
            return "";
        }
        SecureZeroMemory(k, 32);
        return std::string(out.begin(), out.end());
    }

    // ============================================================
    // 3. SHA256
    // ============================================================
    std::vector<uint8_t> sha256(const std::vector<uint8_t>& data) {
        std::vector<uint8_t> out(32);
        BCRYPT_ALG_HANDLE  hAlg  = NULL;
        BCRYPT_HASH_HANDLE hHash = NULL;

        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM,
                                        NULL, 0) != 0)
            return {};

        if (BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0) != 0) {
            BCryptCloseAlgorithmProvider(hAlg, 0);
            return {};
        }

        if (BCryptHashData(hHash, (PUCHAR)data.data(),
                        (ULONG)data.size(), 0) != 0) {
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
    // 4. 文件加密（长度不变）
    // ============================================================
    bool encrypt_file(const std::vector<uint8_t>& input,
                    std::vector<uint8_t>* output,
                    const std::vector<uint8_t>& key) {
        uint8_t k[32];
        if (!normalize_key(key, k)) return false;

        bool ok = aes_ctr(input, output, k);
        SecureZeroMemory(k, 32);
        return ok;
    }

    // ============================================================
    // 5. 文件解密（长度不变）
    // ============================================================
    bool decrypt_file(const std::vector<uint8_t>& input,
                    std::vector<uint8_t>* output,
                    const std::vector<uint8_t>& key) {
        return encrypt_file(input, output, key);
    }

} // namespace encrypt