// Compile against the shipped SDK without hand-declaring host imports.
#include "../hookapi.h"

_Static_assert(ENTROPY_ALLOW_ANY_STRONG_VETO == 1, "ANY flag value");
_Static_assert(
    ENTROPY_ALLOW_SAME_ACCOUNT_STRONG_VETO == 2,
    "SAME_ACCOUNT flag value");
_Static_assert(LATER_STRONG_HOOK == -49, "composition refusal code");

int64_t
hook(uint32_t reserved)
{
    _g(1, 1);
    int64_t status = entropy_cr_status();
    if (status < 0)
        return rollback(0, 0, status);

    uint8_t bytes[32];
    for (uint32_t flags = 0; GUARD(4), flags < 4; ++flags)
    {
        int64_t draw = entropy_cr_dice(1, 3, flags);
        if (draw != 0)
            return rollback(0, 0, draw);
        int64_t count = entropy_cr_random(SBUF(bytes), 3, flags);
        if (count != sizeof(bytes))
            return rollback(0, 0, count);
    }
    return accept(0, 0, ENTROPY_TIER(status));
}
