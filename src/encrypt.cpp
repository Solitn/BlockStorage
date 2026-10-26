#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "encrypt.h"
#include "types.h"
#include "json.h"

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

namespace {

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// 免密仓库用的固定 PBKDF2 口令（32 字节）
const std::uint8_t kSlowKey[32] = {
    0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE,
    0x13, 0x37, 0xC0, 0xDE, 0x42, 0x42, 0x42, 0x42,
    0xFE, 0xED, 0xFA, 0xCE, 0x8B, 0xAD, 0xF0, 0x0D,
    0xDE, 0xAD, 0xC0, 0xDE, 0xBE, 0xEF, 0x42, 0x42
};

bool gcm_crypt(const U8list& in, const U8list& key, const std::uint8_t* nonce,
               const std::uint8_t* tag, U8list& out, bool encrypt) {
    out.clear();
    const U8list k = Sha256(key);
    if (k.size() != 32) return false;

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0))) {
        return false;
    }
    NTSTATUS st = BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                                    reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                                    sizeof(BCRYPT_CHAIN_MODE_GCM), 0);

    BCRYPT_KEY_HANDLE kh = nullptr;
    if (NT_SUCCESS(st)) {
        st = BCryptGenerateSymmetricKey(alg, &kh, nullptr, 0, const_cast<PUCHAR>(k.data()),
                                        static_cast<ULONG>(k.size()), 0);
    }

    out.assign(in.size(), 0);
    ULONG done = 0;
    if (NT_SUCCESS(st)) {
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = const_cast<PUCHAR>(nonce);
        info.cbNonce = 12;
        info.pbTag = const_cast<PUCHAR>(tag);
        info.cbTag = 16;

        PUCHAR src = in.empty() ? nullptr : const_cast<PUCHAR>(in.data());
        PUCHAR dst = out.empty() ? nullptr : out.data();
        if (encrypt) {
            st = BCryptEncrypt(kh, src, static_cast<ULONG>(in.size()), &info, nullptr, 0,
                               dst, static_cast<ULONG>(out.size()), &done, 0);
        } else {
            st = BCryptDecrypt(kh, src, static_cast<ULONG>(in.size()), &info, nullptr, 0,
                               dst, static_cast<ULONG>(out.size()), &done, 0);
        }
    }

    if (kh) BCryptDestroyKey(kh);
    BCryptCloseAlgorithmProvider(alg, 0);

    if (!NT_SUCCESS(st) || done != in.size()) {
        out.clear();
        return false;
    }
    return true;
}

}  // namespace

U8list To32(const U8list& in) {
    U8list out(in.begin(), in.begin() + std::min<size_t>(in.size(), 32));
    out.resize(32, 0);
    return out;
}

std::string Base64Encode(const std::vector<uint8_t>& data) {
    if (data.empty()) return {};

    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    std::size_t i = 0;
    while (i + 3 <= data.size()) {
        const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                                static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(kB64[(v >> 18) & 0x3F]);
        out.push_back(kB64[(v >> 12) & 0x3F]);
        out.push_back(kB64[(v >> 6) & 0x3F]);
        out.push_back(kB64[v & 0x3F]);
        i += 3;
    }
    if (i + 1 == data.size()) {
        const std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
        out.push_back(kB64[(v >> 18) & 0x3F]);
        out.push_back(kB64[(v >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == data.size()) {
        const std::uint32_t v = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out.push_back(kB64[(v >> 18) & 0x3F]);
        out.push_back(kB64[(v >> 12) & 0x3F]);
        out.push_back(kB64[(v >> 6) & 0x3F]);
        out.push_back('=');
    }
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

    auto value_of = [](char c) -> int {
        const char* p = std::strchr(kB64, c);
        return p ? static_cast<int>(p - kB64) : -1;
    };

    for (std::size_t i = 0; i < clean.size(); i += 4) {
        const bool last = (i + 4 == clean.size());
        const char c0 = clean[i], c1 = clean[i + 1], c2 = clean[i + 2], c3 = clean[i + 3];

        const int v0 = value_of(c0);
        const int v1 = value_of(c1);
        if (v0 < 0 || v1 < 0) return false;

        const bool pad2 = (c2 == '=');
        const bool pad1 = (c3 == '=');
        if (!last && (pad1 || pad2)) return false;
        if (pad2 && !pad1) return false;

        int v2 = 0;
        int v3 = 0;
        if (!pad1) {
            v3 = value_of(c3);
            if (v3 < 0) return false;
        }
        if (!pad2) {
            v2 = value_of(c2);
            if (v2 < 0) return false;
        }

        const std::uint32_t v = (static_cast<std::uint32_t>(v0) << 18) |
                                (static_cast<std::uint32_t>(v1) << 12) |
                                (static_cast<std::uint32_t>(v2) << 6) |
                                static_cast<std::uint32_t>(v3);
        out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
        if (!pad2) out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        if (!pad1) out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    }
    return true;
}

U8list Sha256(const U8list& data) {
    U8list out(32);
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return {};
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    NTSTATUS st = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    if (NT_SUCCESS(st)) {
        PUCHAR p = data.empty() ? nullptr : const_cast<PUCHAR>(data.data());
        st = BCryptHashData(hash, p, static_cast<ULONG>(data.size()), 0);
    }
    if (NT_SUCCESS(st)) st = BCryptFinishHash(hash, out.data(), 32, 0);

    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!NT_SUCCESS(st)) return {};
    return out;
}

U8list SlowHash(const U8list& data) {
    U8list out(32);
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                                BCRYPT_ALG_HANDLE_HMAC_FLAG))) {
        return {};
    }
    NTSTATUS st = BCryptDeriveKeyPBKDF2(
        alg,
        const_cast<PUCHAR>(kSlowKey), static_cast<ULONG>(sizeof(kSlowKey)),
        const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()),
        600000, out.data(), static_cast<ULONG>(out.size()), 0);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!NT_SUCCESS(st)) return {};
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
    if (n == 0) return out;
    if (!NT_SUCCESS(BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(n),
                                    BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        return {};
    }
    return out;
}

// AES-256-GCM 封装：nonce(12) ‖ ciphertext ‖ tag(16)
U8list GcmSeal(const U8list& plain, const U8list& key) {
    std::uint8_t nonce[12];
    if (!NT_SUCCESS(BCryptGenRandom(nullptr, nonce, 12, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        return {};
    }

    U8list cipher;
    std::uint8_t tag[16] = {0};
    if (!gcm_crypt(plain, key, nonce, tag, cipher, true)) return {};

    U8list out;
    out.reserve(12 + cipher.size() + 16);
    out.insert(out.end(), nonce, nonce + 12);
    out.insert(out.end(), cipher.begin(), cipher.end());
    out.insert(out.end(), tag, tag + 16);
    return out;
}

bool GcmOpen(const U8list& sealed, const U8list& key, U8list& plain) {
    if (sealed.size() < 12 + 16) return false;

    const std::uint8_t* nonce = sealed.data();
    const std::size_t ct_len = sealed.size() - 12 - 16;
    const std::uint8_t* ct = sealed.data() + 12;
    const std::uint8_t* tag = sealed.data() + 12 + ct_len;

    U8list in(ct, ct + ct_len);
    return gcm_crypt(in, key, nonce, tag, plain, false);
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
