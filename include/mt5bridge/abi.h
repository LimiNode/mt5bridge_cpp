#pragma once

/// \file abi.h
/// \brief Declares the stable control-plane C ABI exported by mt5_bridge.dll.

#include <stdint.h>
#include <wchar.h>

/// \def MT5BRIDGE_EXPORT
/// \brief Applies the platform-specific DLL import or export annotation.
#if defined(_WIN32)
#  if defined(MT5BRIDGE_BUILD_DLL) || defined(MT5BRIDGE_BUILD)
#    define MT5BRIDGE_EXPORT __declspec(dllexport)
#  else
#    define MT5BRIDGE_EXPORT __declspec(dllimport)
#  endif
#else
#  define MT5BRIDGE_EXPORT
#endif

/// \def MT5BRIDGE_ABI_VERSION
/// \brief Identifies the exact public ABI layout expected by the client.
#define MT5BRIDGE_ABI_VERSION 2u

/// \def MT5BRIDGE_API
/// \brief Backward-compatible alias for MT5BRIDGE_EXPORT.
#define MT5BRIDGE_API MT5BRIDGE_EXPORT

#ifdef __cplusplus
extern "C" {
#endif

/// \brief Returns the ABI version implemented by the loaded runtime.
/// \return MT5BRIDGE_ABI_VERSION for a compatible runtime.
MT5BRIDGE_EXPORT uint32_t mt5bridge_abi_version(void);

/// \brief Initializes CPython and establishes the MetaTrader 5 connection.
/// \param python_home Optional Python home directory, or NULL to use the default runtime.
/// \return Zero on success; non-zero on failure with details in mt5bridge_last_error().
/// \note Repeated calls after successful initialization are harmless.
MT5BRIDGE_EXPORT int mt5bridge_initialize(const wchar_t *python_home);

/// \brief Closes the MetaTrader 5 connection and releases runtime resources.
/// \note The function is idempotent and must complete before unloading the DLL.
MT5BRIDGE_EXPORT void mt5bridge_shutdown(void);

/// \brief Executes a control-plane request encoded as UTF-8 JSON.
/// \param request_json Null-terminated UTF-8 JSON object describing the operation.
/// \param[out] response_json Receives a DLL-owned null-terminated UTF-8 JSON response.
/// \return Zero on success; non-zero on failure with details in mt5bridge_last_error().
/// \note Release a successful non-null response with mt5bridge_free().
MT5BRIDGE_EXPORT int mt5bridge_eval_json(const char *request_json,
                                         char **response_json);

/// \brief Releases a JSON response allocated by mt5_bridge.dll.
/// \param response_json Pointer returned through mt5bridge_eval_json(); NULL is permitted.
MT5BRIDGE_EXPORT void mt5bridge_free(char *response_json);

/// \brief Returns diagnostic text for the last bridge failure on the calling thread.
/// \return A borrowed UTF-8 string, or NULL when no error is recorded.
/// \note The pointer remains valid until the next bridge call on the same thread.
MT5BRIDGE_EXPORT const char *mt5bridge_last_error(void);

#ifdef __cplusplus
}
#endif
