/// \file file_store_support.cpp
/// \brief Implements private Windows file-store I/O and binary codec primitives.

#include "file_store_support.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mt5bridge::file_store_support {

std::uint64_t fnv1a(const std::string &value) {
    std::uint64_t result = 1469598103934665603ULL;
    for (const unsigned char byte : value) {
        result ^= byte;
        result *= 1099511628211ULL;
    }
    return result;
}

std::string hex_u64(std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << value;
    return stream.str();
}

std::wstring widen_ascii(const std::string &value) {
    return std::wstring(value.begin(), value.end());
}

std::uint64_t checksum(const std::vector<std::uint8_t> &bytes) {
    std::uint64_t result = 1469598103934665603ULL;
    for (const std::uint8_t byte : bytes) {
        result ^= byte;
        result *= 1099511628211ULL;
    }
    return result;
}

void append_u32(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void append_u64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
    for (unsigned shift = 0; shift != 64; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

bool read_u32(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
              std::uint32_t &value) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t))
        return false;
    value = 0;
    for (unsigned shift = 0; shift != 32; shift += 8)
        value |= static_cast<std::uint32_t>(bytes[offset++]) << shift;
    return true;
}

bool read_u64(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
              std::uint64_t &value) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint64_t))
        return false;
    value = 0;
    for (unsigned shift = 0; shift != 64; shift += 8)
        value |= static_cast<std::uint64_t>(bytes[offset++]) << shift;
    return true;
}

bool append_string(std::vector<std::uint8_t> &bytes, const std::string &value,
                   std::size_t max_bytes) {
    if (value.size() > max_bytes ||
        value.size() > (std::numeric_limits<std::uint32_t>::max)())
        return false;
    append_u32(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
    return true;
}

bool append_payload(std::vector<std::uint8_t> &bytes,
                    const std::vector<std::uint8_t> &value, std::size_t max_bytes) {
    if (value.size() > max_bytes ||
        value.size() > (std::numeric_limits<std::uint32_t>::max)())
        return false;
    append_u32(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
    return true;
}

bool read_string(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
                 std::string &value, std::size_t max_bytes) {
    std::uint32_t size = 0;
    if (!read_u32(bytes, offset, size) || size > max_bytes || offset > bytes.size() ||
        bytes.size() - offset < size)
        return false;
    value.assign(reinterpret_cast<const char *>(bytes.data() + offset), size);
    offset += size;
    return true;
}

bool read_payload(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
                  std::vector<std::uint8_t> &value, std::size_t max_bytes) {
    std::uint32_t size = 0;
    if (!read_u32(bytes, offset, size) || size > max_bytes || offset > bytes.size() ||
        bytes.size() - offset < size)
        return false;
    value.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                 bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
    offset += size;
    return true;
}

#if defined(_WIN32)

std::string win32_error(const char *operation, unsigned long error) {
    return std::string(operation) + " failed (Win32 error " + std::to_string(error) + ")";
}

std::string win32_error(const char *operation) {
    return win32_error(operation, GetLastError());
}

FileLock::FileLock(const std::filesystem::path &path) {
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    handle_ = handle;
    if (handle == INVALID_HANDLE_VALUE) {
        handle_ = nullptr;
        error_ = win32_error("CreateFileW(lock)", GetLastError());
        return;
    }
    OVERLAPPED overlapped{};
    if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD,
                    &overlapped)) {
        error_ = win32_error("LockFileEx", GetLastError());
        CloseHandle(handle);
        handle_ = nullptr;
    }
}

FileLock::~FileLock() {
    const HANDLE handle = static_cast<HANDLE>(handle_);
    if (handle) {
        OVERLAPPED overlapped{};
        UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &overlapped);
        CloseHandle(handle);
    }
}

bool FileLock::acquired() const { return handle_ != nullptr; }

const std::string &FileLock::error() const { return error_; }

ReadStatus read_bytes_file(const std::filesystem::path &path, std::size_t max_bytes,
                           std::vector<std::uint8_t> &bytes, std::string &error,
                           ReadStatus size_query_failure) {
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            return ReadStatus::absent;
        error = win32_error("CreateFileW(record)", code);
        return ReadStatus::io_error;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size)) {
        error = size_query_failure == ReadStatus::malformed
                    ? "durable record has an invalid size"
                    : win32_error("GetFileSizeEx", GetLastError());
        CloseHandle(handle);
        return size_query_failure;
    }
    if (size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > max_bytes) {
        error = "durable record exceeds storage limits";
        CloseHandle(handle);
        return ReadStatus::malformed;
    }
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD requested = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - offset, (std::numeric_limits<DWORD>::max)()));
        DWORD read = 0;
        if (!ReadFile(handle, bytes.data() + offset, requested, &read, nullptr) || read == 0) {
            error = win32_error("ReadFile", GetLastError());
            CloseHandle(handle);
            return ReadStatus::io_error;
        }
        offset += read;
    }
    CloseHandle(handle);
    return ReadStatus::valid;
}

bool write_file(const std::filesystem::path &path,
                const std::vector<std::uint8_t> &bytes, std::string &error) {
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = win32_error("CreateFileW(temp)", GetLastError());
        return false;
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD requested = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - offset, (std::numeric_limits<DWORD>::max)()));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data() + offset, requested, &written, nullptr) ||
            written == 0) {
            error = win32_error("WriteFile", GetLastError());
            CloseHandle(handle);
            DeleteFileW(path.c_str());
            return false;
        }
        offset += written;
    }
    if (!FlushFileBuffers(handle)) {
        error = win32_error("FlushFileBuffers", GetLastError());
        CloseHandle(handle);
        DeleteFileW(path.c_str());
        return false;
    }
    CloseHandle(handle);
    return true;
}

#endif

} // namespace mt5bridge::file_store_support
