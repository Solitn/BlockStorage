#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include "encrypt.h"
#include "types.h"
#include "json.h"

U8list To32(const U8list& in) {
    U8list out(in.begin(), in.begin() + std::min<size_t>(in.size(), 32));
    out.resize(32, 0);
    return out;
}

std::string Base64Encode(const std::vector<uint8_t>& data) {
    if (data.empty()) return {};

    // 4 * ceil(n/3) + 1（结尾 '\0'）
    size_t outLen = 4 * ((data.size() + 2) / 3) + 1;
    std::string out(outLen, '\0');

    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]),
                            data.data(),
                            static_cast<int>(data.size()));
    if (n < 0) return {};

    out.resize(static_cast<size_t>(n));
    return out;
}

bool Base64Decode(const std::string& text, U8list& out) {
    out.clear();
    std::string clean;
    clean.reserve(text.size());
    for (char c : text) {
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        clean.push_back(c);
    }
    if (clean.empty()) return true;
    if (clean.size() % 4 != 0) return false;

    out.resize(clean.size() / 4 * 3);
    const int n = EVP_DecodeBlock(out.data(),
                                  reinterpret_cast<const unsigned char*>(clean.data()),
                                  static_cast<int>(clean.size()));
    if (n < 0) {
        out.clear();
        return false;
    }
    std::size_t pad = 0;
    if (clean.size() >= 1 && clean[clean.size() - 1] == '=') ++pad;
    if (clean.size() >= 2 && clean[clean.size() - 2] == '=') ++pad;
    out.resize(static_cast<std::size_t>(n) - pad);
    return true;
}

U8list Sha256(const U8list& data) {    U8list out(32);
    unsigned int len = 0;
    EVP_Digest(data.data(), data.size(), out.data(), &len, EVP_sha256(), nullptr);
    return out;
}

U8list SlowHash(const U8list& data) {
    static const uint8_t KEY[32] = {
        0xDE,0xAD,0xBE,0xEF, 0xCA,0xFE,0xBA,0xBE,
        0x13,0x37,0xC0,0xDE, 0x42,0x42,0x42,0x42,
        0xFE,0xED,0xFA,0xCE, 0x8B,0xAD,0xF0,0x0D,
        0xDE,0xAD,0xC0,0xDE, 0xBE,0xEF,0x42,0x42
    };

    U8list out(32);
    if (PKCS5_PBKDF2_HMAC(
            reinterpret_cast<const char*>(KEY), sizeof(KEY),
            data.data(), (int)data.size(),
            600000,                 // 迭代次数，越大越慢
            EVP_sha256(),
            32, out.data()) != 1) {
        return {};
    }
    return out;
}

U8list DeriveKey(const U8list& password, const U8list& salt) {
    U8list joined = password;
    joined.insert(joined.end(), salt.begin(), salt.end());
    return SlowHash(joined);
}

U8list SubKey(const U8list& master, uint8_t domain, uint64_t id) {
    U8list buf = master;
    buf.push_back(domain);
    for (int i = 7; i >= 0; --i) {
        buf.push_back(static_cast<uint8_t>((id >> (8 * i)) & 0xFF));
    }
    return Sha256(buf);
}

U8list RandomBytes(size_t n) {
    U8list out(n);
    if (RAND_bytes(out.data(), static_cast<int>(n)) != 1) {
        return {};
    }
    return out;
}

// AES-256-GCM 封装：nonce(12) ‖ ciphertext ‖ tag(16)
U8list GcmSeal(const U8list& plain, const U8list& key) {
    U8list k = Sha256(key);

    uint8_t nonce[12];
    if (RAND_bytes(nonce, 12) != 1) return {};

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};

    U8list cipher(plain.size() + 16);
    int out_len = 0;
    int final_len = 0;
    uint8_t tag[16];
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
              EVP_EncryptInit_ex(ctx, nullptr, nullptr, k.data(), nonce) == 1 &&
              EVP_EncryptUpdate(ctx, cipher.data(), &out_len, plain.data(),
                                static_cast<int>(plain.size())) == 1 &&
              EVP_EncryptFinal_ex(ctx, cipher.data() + out_len, &final_len) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return {};

    out_len += final_len;
    cipher.resize(static_cast<size_t>(out_len));

    U8list out;
    out.reserve(12 + cipher.size() + 16);
    out.insert(out.end(), nonce, nonce + 12);
    out.insert(out.end(), cipher.begin(), cipher.end());
    out.insert(out.end(), tag, tag + 16);
    return out;
}

bool GcmOpen(const U8list& sealed, const U8list& key, U8list& plain) {
    if (sealed.size() < 12 + 16) return false;

    const uint8_t* nonce = sealed.data();
    const uint8_t* ct = sealed.data() + 12;
    size_t ct_len = sealed.size() - 12 - 16;
    const uint8_t* tag = sealed.data() + 12 + ct_len;

    U8list k = Sha256(key);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    plain.assign(ct_len, 0);
    int out_len = 0;
    int final_len = 0;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
              EVP_DecryptInit_ex(ctx, nullptr, nullptr, k.data(), nonce) == 1 &&
              EVP_DecryptUpdate(ctx, plain.data(), &out_len, ct,
                                static_cast<int>(ct_len)) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, const_cast<uint8_t*>(tag)) == 1 &&
              EVP_DecryptFinal_ex(ctx, plain.data() + out_len, &final_len) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        plain.clear();
        return false;
    }

    out_len += final_len;
    plain.resize(static_cast<size_t>(out_len));
    return true;
}

U8list encrypt_json(const nlohmann::json& j, const std::vector<uint8_t>& key) {
    const std::string text = j.dump();
    return GcmSeal(U8list(text.begin(), text.end()), key);
}

nlohmann::json decrypt_json(const std::vector<uint8_t>& data, const std::vector<uint8_t>& key) {
    U8list plain;
    if (!GcmOpen(data, key, plain)) return nullptr;
    std::string text(plain.begin(), plain.end());
    return nlohmann::json::parse(text, nullptr, false);
}
