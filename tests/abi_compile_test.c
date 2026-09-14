/* \file abi_compile_test.c
 * \brief Verifies that the public ABI headers compile as C11.
 */
#include <mt5bridge/abi.h>
#include <mt5bridge/data.h>
#include <mt5bridge/trade.h>

int main(void) {
    Mt5Tick tick = {0};
    Mt5TicksRequest request = {0};
    Mt5SubscriptionHandle handle = {0};
    Mt5AccountInfo account = {0};
    Mt5SymbolRequest symbol = {0};
    Mt5SymbolCapabilities capabilities = {0};
    Mt5OrderCheckRequest order_check = {0};
    Mt5OrderCheckResult result = {0};
    (void)tick;
    (void)request;
    (void)handle;
    (void)account;
    (void)symbol;
    (void)capabilities;
    (void)order_check;
    (void)result;
    return MT5BRIDGE_ABI_VERSION == 8 && MT5BRIDGE_TRADE_API_VERSION == 1 ? 0 : 1;
}
