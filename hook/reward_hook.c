#include "hookapi.h"

#define ASSERT(x)\
    if (!(x))\
        rollback(0,0,__LINE__);

int64_t hook(uint32_t r)
{
    etxn_reserve(1);
    _g(1,1);

    // get the account id
    uint8_t account_field[20];
    ASSERT(otxn_field(SBUF(account_field), sfAccount) == 20);

    uint8_t hook_accid[20];
    hook_account(SBUF(hook_accid));


    int equal = 0; BUFFER_EQUAL(equal, hook_accid, account_field, 20);
    if (equal)
        accept(0,0,0);


    // get the account root keylet
    uint8_t kl[34];
    ASSERT(util_keylet(SBUF(kl), KEYLET_ACCOUNT, SBUF(account_field), 0,0,0,0) == 34);

    // slot the account root
    ASSERT(slot_set(SBUF(kl), 1) == 1);

    
    int64_t accum_slot = slot_subfield(1, sfRewardAccumulator, 2);

    // this is a first time claim reward has run and will setup these fields
    if (accum_slot == DOESNT_EXIST)
        accept(0,0,0);

    // this is an actual claim reward
    ASSERT(accum_slot == 2);
    ASSERT(slot_subfield(1, sfRewardLgrFirst, 3) == 3);
    ASSERT(slot_subfield(1, sfRewardLgrLast, 4) == 4);
    ASSERT(slot_subfield(1, sfBalance, 5) == 5);


    int64_t accumulator = slot(0,0,2);
    int64_t first = slot(0,0,3);
    int64_t last = slot(0,0,4);
    int64_t bal = slot(0,0,5);

    TRACEVAR(accumulator);
    TRACEVAR(first);
    TRACEVAR(last);

    ASSERT(accumulator > 0 && first > 0 && last > 0);

    // we need to add the final block ourselves

    int64_t cur = ledger_seq();

    int64_t elapsed = cur - first;

    ASSERT(elapsed > 0);

    int64_t elapsed_since_last = ledger_seq() - last;

    bal &= ~0xE000000000000000ULL;
    bal /= 1000000;

    TRACEVAR(bal);

    TRACEVAR(accumulator);

    if (bal > 0 && elapsed_since_last > 0)
        accumulator += bal * elapsed_since_last;
    
    TRACEVAR(accumulator);

    int64_t reward = accumulator / elapsed;
    
    TRACEVAR(reward);

    int64_t reward_drops = reward * 1000000ULL;

    ASSERT(reward_drops > reward);

    TRACEVAR(reward_drops);

    uint8_t tx[PREPARE_PAYMENT_SIMPLE_SIZE];
    PREPARE_PAYMENT_SIMPLE(tx, reward_drops, account_field, 0, 0);

    // emit the transaction
    uint8_t emithash[32];
    int64_t emit_result = emit(SBUF(emithash), SBUF(tx));
    TRACEVAR(emit_result);

    accept(0,0,0);
}
