#include "hookapi.h"

int64_t hook(uint32_t reserved) {
    TRACESTR("tsh.c: Start.");

    switch (reserved)
    {
    case 0:
        TRACESTR("tsh.c: Strong. Execute BEFORE transaction is applied to ledger");
        break;
    case 1:
        TRACESTR("tsh.c: Weak. Execute AFTER transaction is applied to ledger");
        break;
    case 2:
        TRACESTR("tsh.c: Weak Again. Execute AFTER transaction is applied to ledger");
        break;
    default:
        break;
    }

    TRACESTR("tsh.c: End.");
    uint8_t r_buf[4];
    UINT32_TO_BUF(r_buf, reserved);
    accept(SBUF(r_buf), __LINE__);
    _g(1,1);
    // unreachable
    return 0;
}