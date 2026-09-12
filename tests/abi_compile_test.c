/* \file abi_compile_test.c
 * \brief Verifies that the public ABI headers compile as C11.
 */
#include <mt5bridge/abi.h>
#include <mt5bridge/data.h>

int main(void) {
    Mt5Tick tick = {0};
    Mt5TicksRequest request = {0};
    Mt5SubscriptionHandle handle = {0};
    (void)tick;
    (void)request;
    (void)handle;
    return MT5BRIDGE_ABI_VERSION >= 7 ? 0 : 1;
}
