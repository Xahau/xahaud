#include "hookapi.h"
//#define REWARD_DELAY 2600000LL
#define REWARD_DELAY 60LL
#define REWARD_MULTIPLIER_XFL 6038156834009797973ULL
//0.00333333333f


#define ASSERT(x)\
    if (!(x))\
        rollback(0,0,__LINE__);

int64_t hook(uint32_t r)
{
    etxn_reserve(1);
    _g(1,1);

    uint8_t ttbuf[16];
    int64_t br = otxn_field(SBUF(ttbuf), sfTransactionType);
    uint32_t txntype = ((uint32_t)(ttbuf[0]) << 16U) + ((uint32_t)(ttbuf[1]));

    if (txntype != 98)
        accept(0,0,0);

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
    ASSERT(slot_subfield(1, sfRewardTime, 6) == 6);

    int64_t time = slot(0,0,6);

    int64_t time_elapsed = ledger_last_time() - time;
    if (time_elapsed < REWARD_DELAY)
    {
        uint8_t msg_buf[] = 
        { 
            'Y','o','u',' ','m','u','s','t',    // 8
            ' ','w','a','i','t',' ',            //+6 = 14 
            '0','0','0','0','0','0','0',
            ' ','s','e','c','o','n','d','s' };

        //2 600 000
        time_elapsed = REWARD_DELAY - time_elapsed;
        msg_buf[14] += (time_elapsed / 1000000) % 10;
        msg_buf[15] += (time_elapsed /  100000) % 10;
        msg_buf[16] += (time_elapsed /   10000) % 10;
        msg_buf[17] += (time_elapsed /    1000) % 10;
        msg_buf[18] += (time_elapsed /     100) % 10;
        msg_buf[19] += (time_elapsed /      10) % 10;
        msg_buf[20] += (time_elapsed          ) % 10;

        rollback(SBUF(msg_buf), time_elapsed);
    }

    int64_t accumulator = slot(0,0,2);
    int64_t first = slot(0,0,3);
    int64_t last = slot(0,0,4);
    int64_t bal = slot(0,0,5);


    TRACEVAR(accumulator);
    TRACEVAR(first);
    TRACEVAR(last);

    ASSERT(/*accumulator > 0 &&*/ first > 0 && last > 0);

    // we need to add the final block ourselves

    int64_t cur = ledger_seq();

    int64_t elapsed = cur - first;

    ASSERT(elapsed > 0);

    int64_t elapsed_since_last = ledger_seq() - last;

    bal &= ~0xE000000000000000ULL;
    bal /= 1000000LL;

    TRACEVAR(bal);

    TRACEVAR(accumulator);

    if (bal > 0 && elapsed_since_last > 0)
        accumulator += bal * elapsed_since_last;
    
    TRACEVAR(accumulator);

    uint8_t key[32];
    key[0] = 0xFFU;
    // mr = monthly reward rate
    int64_t xfl_mr = 
        state(0,0, SBUF(key));

    if (xfl_mr <= 0 || // invalid xfl
        float_compare(xfl_mr, 0, COMPARE_LESS) ||  // negative xfl
        float_compare(xfl_mr, float_one(), COMPARE_GREATER)) // greater than 100%
        xfl_mr = REWARD_MULTIPLIER_XFL;
    {
        ASSERT(xfl_mr > 0);
    }

    int64_t xfl_accum = float_set(0, accumulator);
    ASSERT(xfl_accum > 0);

    int64_t xfl_elapsed = float_set(0, elapsed);
    ASSERT(xfl_elapsed > 0);

    int64_t xfl_reward = float_divide(xfl_accum, xfl_elapsed);
    xfl_reward = float_multiply(xfl_mr, xfl_reward);

    TRACEVAR(xfl_reward);

    int64_t reward_drops = float_int(xfl_reward, 6, 0);

    TRACEVAR(reward_drops);

    uint8_t tx[PREPARE_PAYMENT_SIMPLE_SIZE];
    PREPARE_PAYMENT_SIMPLE(tx, reward_drops, account_field, 0, 0);

    // emit the transaction
    uint8_t emithash[32];
    int64_t emit_result = emit(SBUF(emithash), SBUF(tx));
    TRACEVAR(emit_result);

    // RH TODO:
    // on callback pay out each of the filled gov seat

    accept(0,0,0);
}
