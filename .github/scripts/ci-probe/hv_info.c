// [TEST PR] Print hypervisor identification and KVM paravirt feature bits.
#include <cpuid.h>
#include <stdio.h>
#include <string.h>

int
main(void)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    char sig[13] = {0};
    __cpuid(0x40000000, a, b, c, d);
    memcpy(sig, &b, 4);
    memcpy(sig + 4, &c, 4);
    memcpy(sig + 8, &d, 4);
    printf("hv_signature=%s hv_max_leaf=0x%x\n", sig, a);
    if (strcmp(sig, "KVMKVMKVM") == 0 && a >= 0x40000001)
    {
        __cpuid(0x40000001, a, b, c, d);
        printf(
            "kvm_features=0x%x clocksource2=%u async_pf=%u steal_time=%u "
            "pv_eoi=%u pv_unhalt=%u pv_tlb_flush=%u pv_sched_yield=%u "
            "hints_realtime=%u\n",
            a,
            (a >> 3) & 1,
            (a >> 4) & 1,
            (a >> 5) & 1,
            (a >> 6) & 1,
            (a >> 7) & 1,
            (a >> 9) & 1,
            (a >> 13) & 1,
            d & 1);
    }
    return 0;
}
