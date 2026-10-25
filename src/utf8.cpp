#include "utf8.h"

#ifdef _WIN32
#include <windows.h>
#endif

std::filesystem::path PathFromUtf8(const std::string& utf8) {
    return std::filesystem::path(Utf8ToWide(utf8));
}

std::string PathToUtf8(const std::filesystem::path& p) {
    return WideToUtf8(p.wstring());
}

#ifdef _WIN32

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                         nullptr, 0);
    if (need <= 0) return {};
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), need);
    return out;
}

bool IsValidUtf8(const std::string& s) {
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t extra = 0;
        if (c < 0x80) {
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else return false;
        if (i + extra >= n) return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

std::string NormalizeInputEncoding(const std::string& raw) {
    if (raw.empty() || IsValidUtf8(raw)) return raw;

    // 不是合法 UTF-8：按系统 ANSI 代码页解码后转 UTF-8（兼容 GBK 管道输入）
    const int need = MultiByteToWideChar(CP_ACP, 0, raw.data(), static_cast<int>(raw.size()),
                                         nullptr, 0);
    if (need <= 0) return raw;
    std::wstring wide(static_cast<std::size_t>(need), L'\0');
    MultiByteToWideChar(CP_ACP, 0, raw.data(), static_cast<int>(raw.size()), wide.data(), need);
    return WideToUtf8(wide);
}

#endif
