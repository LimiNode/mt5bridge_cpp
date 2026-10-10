#pragma once

/// \file file_store_support.hpp
/// \brief Shares private Windows file-store I/O and binary codec primitives.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mt5bridge::file_store_support {

constexpr std::size_t kMaxStringBytes = 1U << 20;
constexpr std::size_t kMaxPayloadBytes = 16U << 20;

/// \brief Computes the stable FNV-1a digest used in store filenames.
/// \param value String to hash.
/// \return Deterministic 64-bit digest.
std::uint64_t fnv1a(const std::string &value);

/// \brief Formats a 64-bit value as fixed-width lowercase hexadecimal.
/// \param value Value to format.
/// \return Sixteen-character hexadecimal text.
std::string hex_u64(std::uint64_t value);

/// \brief Converts ASCII filename text to a Windows path component.
/// \param value ASCII text.
/// \return Widened path text.
std::wstring widen_ascii(const std::string &value);

/// \brief Computes the stable FNV-1a digest used in record envelopes.
/// \param bytes Bytes to hash.
/// \return Deterministic 64-bit digest.
std::uint64_t checksum(const std::vector<std::uint8_t> &bytes);

/// \brief Appends a little-endian 32-bit integer.
/// \param bytes Destination buffer.
/// \param value Integer to append.
void append_u32(std::vector<std::uint8_t> &bytes, std::uint32_t value);

/// \brief Appends a little-endian 64-bit integer.
/// \param bytes Destination buffer.
/// \param value Integer to append.
void append_u64(std::vector<std::uint8_t> &bytes, std::uint64_t value);

/// \brief Reads a little-endian 32-bit integer.
/// \param bytes Source buffer.
/// \param offset In/out byte offset.
/// \param[out] value Decoded integer.
/// \return True when the value was fully decoded.
bool read_u32(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
              std::uint32_t &value);

/// \brief Reads a little-endian 64-bit integer.
/// \param bytes Source buffer.
/// \param offset In/out byte offset.
/// \param[out] value Decoded integer.
/// \return True when the value was fully decoded.
bool read_u64(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
              std::uint64_t &value);

/// \brief Appends a bounded UTF-8/byte string with a 32-bit length prefix.
/// \param bytes Destination buffer.
/// \param value String to append.
/// \param max_bytes Maximum accepted payload length.
/// \return True when the value fits the store contract.
bool append_string(std::vector<std::uint8_t> &bytes, const std::string &value,
                   std::size_t max_bytes = kMaxStringBytes);

/// \brief Appends a bounded opaque payload with a 32-bit length prefix.
/// \param bytes Destination buffer.
/// \param value Payload to append.
/// \param max_bytes Maximum accepted payload length.
/// \return True when the value fits the store contract.
bool append_payload(std::vector<std::uint8_t> &bytes,
                    const std::vector<std::uint8_t> &value,
                    std::size_t max_bytes = kMaxPayloadBytes);

/// \brief Reads a bounded length-prefixed string.
/// \param bytes Source buffer.
/// \param offset In/out byte offset.
/// \param[out] value Decoded string.
/// \param max_bytes Maximum accepted payload length.
/// \return True when the value was fully decoded.
bool read_string(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
                 std::string &value, std::size_t max_bytes = kMaxStringBytes);

/// \brief Reads a bounded length-prefixed opaque payload.
/// \param bytes Source buffer.
/// \param offset In/out byte offset.
/// \param[out] value Decoded payload.
/// \param max_bytes Maximum accepted payload length.
/// \return True when the value was fully decoded.
bool read_payload(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
                  std::vector<std::uint8_t> &value,
                  std::size_t max_bytes = kMaxPayloadBytes);

#if defined(_WIN32)

/// \brief Converts a Win32 error code into a store diagnostic.
/// \param operation Operation that failed.
/// \param error Win32 error code.
/// \return Stable diagnostic text.
std::string win32_error(const char *operation, unsigned long error);

/// \brief Converts the current Win32 error into a store diagnostic.
/// \param operation Operation that failed.
/// \return Stable diagnostic text.
std::string win32_error(const char *operation);

/// \class FileLock
/// \brief Holds one exclusive Windows byte-range lock for a store directory.
class FileLock final {
public:
    /// \brief Opens and exclusively locks a lock file.
    /// \param path Lock-file path.
    explicit FileLock(const std::filesystem::path &path);
    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;
    ~FileLock();

    /// \brief Tests whether the lock was acquired.
    /// \return True when the handle is valid.
    bool acquired() const;

    /// \brief Returns the lock failure diagnostic.
    /// \return Diagnostic text, possibly empty.
    const std::string &error() const;

private:
    void *handle_ = nullptr;
    std::string error_;
};

/// \enum ReadStatus
/// \brief Classifies a private file read for durable-store callers.
enum class ReadStatus { absent, valid, malformed, io_error };

/// \brief Reads a bounded file into memory.
/// \param path File to read.
/// \param max_bytes Maximum accepted file size.
/// \param[out] bytes File contents.
/// \param[out] error Diagnostic text on malformed/I/O failure.
/// \param size_query_failure Status to return when the file size cannot be queried.
/// \return Classified read result.
ReadStatus read_bytes_file(const std::filesystem::path &path, std::size_t max_bytes,
                           std::vector<std::uint8_t> &bytes, std::string &error,
                           ReadStatus size_query_failure = ReadStatus::io_error);

/// \brief Writes and flushes a file, deleting it after a failed write.
/// \param path Destination path.
/// \param bytes Exact bytes to publish to the temporary path.
/// \param[out] error Diagnostic text on failure.
/// \return True when the bytes were flushed successfully.
bool write_file(const std::filesystem::path &path,
                const std::vector<std::uint8_t> &bytes, std::string &error);

#endif

} // namespace mt5bridge::file_store_support
