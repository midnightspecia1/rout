#include "ipc_token.h"

#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#include <shlobj.h>

namespace {

constexpr size_t kTokenBytes = 32;

std::wstring TokenDirectory() {
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        dir = std::wstring(base) + L"\\rout";
    }
    CoTaskMemFree(base);
    return dir;
}

std::string RandomHex() {
    unsigned char bytes[kTokenBytes];
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        return {};
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    for (unsigned char b : bytes) {
        hex.push_back(kHex[b >> 4]);
        hex.push_back(kHex[b & 0xF]);
    }
    SecureZeroMemory(bytes, sizeof(bytes));
    return hex;
}

}  // namespace

std::string CreateIpcToken() {
    const std::wstring dir = TokenDirectory();
    if (dir.empty()) return {};
    CreateDirectoryW(dir.c_str(), nullptr);  // Fine if it exists.
    const std::wstring path = dir + L"\\ipc-token";

    const std::string token = RandomHex();
    if (token.empty()) return {};

    // Protected DACL (no inherited ACEs): full access for the file's
    // owner (OW, the creating user) and SYSTEM, nobody else.
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;OW)(A;;FA;;;SY)", SDDL_REVISION_1,
                                                              &sd, nullptr)) {
        return {};
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};

    // A security descriptor only applies when a file is created, so a
    // leftover file (maybe with a looser DACL) is removed first.
    DeleteFileW(path.c_str());
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    LocalFree(sd);
    if (file == INVALID_HANDLE_VALUE) return {};

    DWORD written = 0;
    const bool ok = WriteFile(file, token.data(), static_cast<DWORD>(token.size()), &written, nullptr) &&
                    written == token.size();
    CloseHandle(file);
    return ok ? token : std::string();
}

bool TokenEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}
