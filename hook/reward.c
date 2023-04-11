#include "hookapi.h"
//#define REWARD_DELAY 2600000LL
#define REWARD_DELAY 60LL
#define REWARD_MULTIPLIER_XFL 6038156834009797973ULL
//0.00333333333f

#define ASSERT(x)\
    if (!(x))\
        rollback(SBUF("Reward: Assertion failure."),__LINE__);

#define DEBUG 1

// this txn template is sent back to this account to distribute a duplicate reward to governance members
// this distribution is handled by the governance reward hook
uint8_t txn_loopback[235] =
{
/* size,upto */
/*   3,  0 */   0x12U, 0x00U, 0x63U,                                                                /* tt = Invoke */
/*   5,  3 */   0x22U, 0x80U, 0x00U, 0x00U, 0x00U,                                          /* flags = tfCanonical */
/*   5,  8 */   0x24U, 0x00U, 0x00U, 0x00U, 0x00U,                                                 /* sequence = 0 */
/*   6, 13 */   0x20U, 0x1AU, 0x00U, 0x00U, 0x00U, 0x00U,                                      /* first ledger seq */
/*   6, 19 */   0x20U, 0x1BU, 0x00U, 0x00U, 0x00U, 0x00U,                                       /* last ledger seq */
/*   9, 25 */   0x68U, 0x40U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,                         /* fee      */
/*  35, 34 */   0x73U, 0x21U, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,       /* pubkey   */
/*  22, 69 */   0x81U, 0x14U, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,                                 /* src acc  */

/* 116, 91 */   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,    /* emit detail */
                0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,

/*  28,207 */   0xF0U,0x13U,0xE0U,0x17U,0x70U,0x18U,0x06U,0x72U,                                    /* hook params */
                0x65U,0x77U,0x61U,0x72U,0x64U,0x70U,0x19U,0x08U,                                    /* reward key  */
    /* 223 */   0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,                                    /* value/drops */
                0xE1U,0xF1U,0xE1U,0xF1                                                              /* end params  */
/*     235 */
};


// this txn template is used to pay out rewards
uint8_t txn_payment[238] =
{
/* size,upto */
/*   3,  0 */   0x12U, 0x00U, 0x00U,                                                               /* tt = Payment */
/*   5,  3*/    0x22U, 0x80U, 0x00U, 0x00U, 0x00U,                                          /* flags = tfCanonical */
/*   5,  8 */   0x24U, 0x00U, 0x00U, 0x00U, 0x00U,                                                 /* sequence = 0 */
/*   6, 13 */   0x20U, 0x1AU, 0x00U, 0x00U, 0x00U, 0x00U,                                      /* first ledger seq */
/*   6, 19 */   0x20U, 0x1BU, 0x00U, 0x00U, 0x00U, 0x00U,                                       /* last ledger seq */
/*   9, 25 */   0x61U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U, 0x99U,      /* amount field 9 bytes */
/*   9, 34 */   0x68U, 0x40U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,                         /* fee      */
/*  35, 43 */   0x73U, 0x21U, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,       /* pubkey   */
/*  22, 78 */   0x81U, 0x14U, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,                                 /* src acc  */
/*  22,100 */   0x83U, 0x14U, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,                                 /* dst acc  */
/* 116,122 */   /* emit details */
/*   0,238 */
};

#define HOOK_ACCOUNT    (txn_loopback +   71U)
#define HOOK_ACCOUNT_2  (txn_payment  +   80U)
#define ACC_FIELD       (txn_payment  +  102U)
#define AMOUNT_OUT      (txn_payment  +   26U)
#define AMOUNT_OUT_2    (txn_loopback +  223U)

uint8_t msg_buf[] = "You must wait 0000000 seconds";
int64_t hook(uint32_t r)
{
    etxn_reserve(1);
    _g(1,1);

    // only process ttCLAIM_REWARD
    if (otxn_type() != 98)
        accept(SBUF("Reward: Passing non-claim txn"), __LINE__);

    // get the account id
    otxn_field(ACC_FIELD, 20, sfAccount);

    // write the hook account into the txn template
    hook_account(HOOK_ACCOUNT, 20);

    // there are two txn templates so also write into the second template
    hook_account(HOOK_ACCOUNT_2, 20);

    if (BUFFER_EQUAL_20(HOOK_ACCOUNT, ACC_FIELD))
        accept(SBUF("Reward: Passing outgoing txn"), __LINE__);

    // get the account root keylet
    uint8_t kl[34];
    util_keylet(SBUF(kl), KEYLET_ACCOUNT, ACC_FIELD, 20, 0,0,0,0);

    // slot the account root, this can't fail
    slot_set(SBUF(kl), 1);

    // this is a first time claim reward has run and will setup these fields
    if (slot_subfield(1, sfRewardAccumulator, 2) != 2)
        accept(SBUF("Reward: Passing reward setup txn"), __LINE__);

    // this is an actual claim reward
    slot_subfield(1, sfRewardLgrFirst, 3);
    slot_subfield(1, sfRewardLgrLast, 4);
    slot_subfield(1, sfBalance, 5);
    slot_subfield(1, sfRewardTime, 6);

    int64_t time = slot(0,0,6);

    int64_t time_elapsed = ledger_last_time() - time;
    if (time_elapsed < REWARD_DELAY)
    {
        //2 600 000
        time_elapsed = REWARD_DELAY - time_elapsed;
        msg_buf[14] += (time_elapsed / 1000000) % 10;
        msg_buf[15] += (time_elapsed /  100000) % 10;
        msg_buf[16] += (time_elapsed /   10000) % 10;
        msg_buf[17] += (time_elapsed /    1000) % 10;
        msg_buf[18] += (time_elapsed /     100) % 10;
        msg_buf[19] += (time_elapsed /      10) % 10;
        msg_buf[20] += (time_elapsed          ) % 10;

        rollback(SBUF(msg_buf), __LINE__);
    }

    int64_t accumulator = slot(0,0,2);
    int64_t first = slot(0,0,3);
    int64_t last = slot(0,0,4);
    int64_t bal = slot(0,0,5);

    if (DEBUG)
    {
        TRACEVAR(accumulator);
        TRACEVAR(first);
        TRACEVAR(last);
    }

    ASSERT(/*accumulator > 0 &&*/ first > 0 && last > 0);

    // we need to add the final block ourselves

    int64_t cur = ledger_seq();

    int64_t elapsed = cur - first;

    ASSERT(elapsed > 0);

    int64_t elapsed_since_last = ledger_seq() - last;

    bal &= ~0xE000000000000000ULL;
    bal /= 1000000LL;

    if (DEBUG)
    {
        TRACEVAR(bal);
        TRACEVAR(accumulator);
    }

    if (bal > 0 && elapsed_since_last > 0)
        accumulator += bal * elapsed_since_last;

    if (DEBUG)
        TRACEVAR(accumulator);

    // reward hook shares the same namespace as governance hook, so we can request the RR key directly
    uint8_t key[2] = {'R', 'R'};

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

    if (DEBUG)
        TRACEVAR(xfl_reward);


    // write drops to paymen txn
    {
        uint64_t drops = float_int(xfl_reward, 6, 1);                                                                     
        uint8_t* b = AMOUNT_OUT;                                                                                   
        *b++ = 0b01000000 + (( drops >> 56 ) & 0b00111111 );                                                           
        *b++ = (drops >> 48) & 0xFFU;                                                                                  
        *b++ = (drops >> 40) & 0xFFU;                                                                                  
        *b++ = (drops >> 32) & 0xFFU;                                                                                  
        *b++ = (drops >> 24) & 0xFFU;                                                                                  
        *b++ = (drops >> 16) & 0xFFU;                                                                                  
        *b++ = (drops >>  8) & 0xFFU;                                                                                  
        *b++ = (drops >>  0) & 0xFFU;

        // copy to other template
        *((uint64_t*)AMOUNT_OUT_2) = *((uint64_t*)AMOUNT_OUT);
    }

    // emit the payment transaction
    uint8_t emithash[32];
    int64_t emit_result = emit(SBUF(emithash), SBUF(txn_payment));

    if (DEBUG)
        TRACEVAR(emit_result);

    if (emit_result < 0)
        rollback(SBUF("Reward: Emit reward failed."), __LINE__);

    // emit the loopback txn
    emit_result = emit(SBUF(emithash), SBUF(txn_loopback));
    
    if (DEBUG)
        TRACEVAR(emit_result);
    
    if (emit_result < 0)
        rollback(SBUF("Reward: Emit loopback failed."), __LINE__);


    accept(SBUF("Reward: Emitted reward txn successfully."), __LINE__);
}
