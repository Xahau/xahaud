#include "hookapi.h"
//#define REWARD_DELAY 2600000LL
#define REWARD_DELAY 5LL
#define REWARD_MULTIPLIER_XFL 6038156834009797973ULL
//0.00333333333f

#define L1SEATS 20U
#define MAXUNL 128U
#define SVAR(x) &(x), sizeof(x)

#define ASSERT(x)\
    if (!(x))\
        rollback(SBUF("Reward: Assertion failure."),__LINE__);

#define DEBUG 1

uint8_t txn_mint[928] =
{
/* size,upto */
/*   3,  0 */   0x12U, 0x00U, 0x60U,                                                           /* tt = GenesisMint */
/*   5,  3 */   0x22U, 0x80U, 0x00U, 0x00U, 0x00U,                                          /* flags = tfCanonical */
/*   5,  8 */   0x24U, 0x00U, 0x00U, 0x00U, 0x00U,                                                 /* sequence = 0 */
/*   6, 13 */   0x20U, 0x1AU, 0x00U, 0x00U, 0x00U, 0x00U,                                      /* first ledger seq */
/*   6, 19 */   0x20U, 0x1BU, 0x00U, 0x00U, 0x00U, 0x00U,                                       /* last ledger seq */
/*   9, 25 */   0x68U, 0x40U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,                         /* fee      */
/*  35, 34 */   0x73U, 0x21U, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,       /* pubkey   */
/*  22, 69 */   0x81U, 0x14U, 0xB5U,0xF7U,0x62U,0x79U,0x8AU,0x53U,0xD5U,0x43U,0xA0U,0x14U,
                              0xCAU,0xF8U,0xB2U,0x97U,0xCFU,0xF8U,0xF2U,0xF9U,0x37U,0xE8U,             /* src acc  */

/* 116, 91 */   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,    /* emit detail */
                0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,

/* 207, ... */  0xF0U, 0x60U,                                                                      /* gen mints arr */
/* 34 bytes per entries + 1 tail byte
    E060
    61
    4111111111111111                                // amount
    8114
    1234567891234567891234567891234567891234        // account
    E1
    ... repeat
    F1                                              // tail byte
 *
 * */
// 210 bytes + 34 bytes per entry * number of entries + any alignment padding desired
};

uint8_t template[40] = {
/*  0,  2 */    0xE0U, 0x60U,          // obj start
/*  2,  1 */    0x61U,                 // amount header
/*  3,  8 */    0,0,0,0,0,0,0,0,       // amount payload
/* 11,  2 */    0x83U, 0x14U,          // account header
/* 13, 20 */    0,0,0,0,0,0,0,0,0,0,
                0,0,0,0,0,0,0,0,0,0,   // account payload
/* 33,  1 */    0xE1U                  // obj end
};


#define TEMPLATE_DROPS(drops_tmp)\
{\
    uint8_t* b = template + 3U;\
    *b++ = 0b01000000 + (( drops_tmp >> 56 ) & 0b00111111 );\
    *b++ = (drops_tmp >> 48) & 0xFFU;\
    *b++ = (drops_tmp >> 40) & 0xFFU;\
    *b++ = (drops_tmp >> 32) & 0xFFU;\
    *b++ = (drops_tmp >> 24) & 0xFFU;\
    *b++ = (drops_tmp >> 16) & 0xFFU;\
    *b++ = (drops_tmp >>  8) & 0xFFU;\
    *b++ = (drops_tmp >>  0) & 0xFFU;\
}

#define BE_DROPS(drops)\
{\
    uint64_t drops_tmp = drops;\
    uint8_t* b = (uint8_t*)&drops;\
    *b++ = 0b01000000 + (( drops_tmp >> 56 ) & 0b00111111 );\
    *b++ = (drops_tmp >> 48) & 0xFFU;\
    *b++ = (drops_tmp >> 40) & 0xFFU;\
    *b++ = (drops_tmp >> 32) & 0xFFU;\
    *b++ = (drops_tmp >> 24) & 0xFFU;\
    *b++ = (drops_tmp >> 16) & 0xFFU;\
    *b++ = (drops_tmp >>  8) & 0xFFU;\
    *b++ = (drops_tmp >>  0) & 0xFFU;\
}

uint8_t member_count_key[2] = {'M', 'C'};


uint8_t unlreport_keylet[32] = 
{
    0x61U,0xE3U,0x2EU,0x7AU,0x24U,0xA2U,0x38U,0xF1U,0xC6U,0x19U,
    0xD5U,0xF9U,0xDDU,0xCCU,0x41U,0xA9U,0x4BU,0x33U,0xB6U,0x6CU,
    0x01U,0x63U,0xF7U,0xEFU,0xCCU,0x8AU,0x19U,0xC9U,0xFDU,0x6FU,
    0x28U,0xDCU
};

uint8_t msg_buf[] = "You must wait 0000000 seconds";
int64_t hook(uint32_t r)
{
    etxn_reserve(1);
    _g(1,1);

    // only process ttCLAIM_REWARD
    if (otxn_type() != 98)
        accept(SBUF("Reward: Passing non-claim txn"), __LINE__);

    uint8_t otxn_acc[20];
    uint8_t hook_acc[20];

    // get the account id
    otxn_field(SBUF(otxn_acc), sfAccount);

    // write the hook account into the txn template
    hook_account(SBUF(hook_acc));

    if (BUFFER_EQUAL_20(hook_acc, otxn_acc))
        accept(SBUF("Reward: Passing outgoing txn"), __LINE__);

    // get the account root keylet
    uint8_t kl[34];
    util_keylet(SBUF(kl), KEYLET_ACCOUNT, SBUF(otxn_acc), 0,0,0,0);

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


    uint64_t reward_drops = float_int(xfl_reward, 6, 1);

    uint64_t l1_drops = reward_drops / L1SEATS;


    otxn_slot(1);
    slot_subfield(1, sfFee, 2);
    int64_t xfl_fee = slot_float(2);

    // user gets back the fee they spent running the hook
    if (xfl_fee > 0)
        reward_drops += float_int(xfl_fee, 6, 1);


    TEMPLATE_DROPS(reward_drops);

    uint8_t* upto = txn_mint + 209U;
    uint8_t* end = upto + (34U * (L1SEATS + 1));

    // first account is always the rewardee
    {
        uint64_t* d = (uint64_t*)upto;
        uint64_t* s = (uint64_t*)template;
        *d++ = *s++;
        *d++ = *s++;
        *(d+2) = *(s+2);
        otxn_field(upto + 13, 20, sfAccount);
    }
    upto += 34U;

    // now iterate all possible seats in all possible tables
    TEMPLATE_DROPS(l1_drops);


    // there are two conditions for L1 governance members to receive rewards:
    // 1. they must be an L1 member
    // 2. they must be an active validator

    // load the UNLReport, this will let us know who has been validating and who hasn't 

    uint64_t can_reward[L1SEATS];


    uint8_t av_array[(34 * MAXUNL) + 3];
    if (slot_set(SBUF(unlreport_keylet), 1) == 1 &&
            slot_subfield(1, sfActiveValidators, 1) == 1 &&
                slot(SBUF(av_array), 1) > 0)
    {
        // at least some validators have been validating so those get a reward if they are on the governance table
        // we are going to assume the UNL never exceeds 64
        uint8_t seat = 0;
        uint8_t* av_upto = av_array + 15 /* offset to the first account */;
        uint64_t av_size = slot_count(1);
        if (av_size > MAXUNL) av_size = MAXUNL;
        for (uint64_t i = 0; GUARD(MAXUNL), i < av_size; ++i)
        {
            if (state(SVAR(seat), av_upto, 20) != 1 || seat > L1SEATS)
                continue;
            can_reward[seat] = 1;
            av_upto += 34U;
        }

        // iterate the seats at the table and add reward entries for the active validators
        for (uint8_t l1_seat = 0; GUARD(L1SEATS), upto < end && l1_seat < L1SEATS;)
        {
            if (!can_reward[l1_seat])
                continue;

            // copy template 1 into next GenesisMints array position
            uint64_t* d = (uint64_t*)upto;
            uint64_t* s = (uint64_t*)template;
            *d++ = *s++;
            *d++ = *s++;
            *(d+2) = *(s+2);

            if (state(upto + 13, 20, &l1_seat, 1) == 20)
                upto += 34;
        }

    }
    *upto++ = 0xF1U;


    // populate other txn fields
    etxn_details(txn_mint + 91, 116);
    int64_t fee = etxn_fee_base(txn_mint, upto - txn_mint);
    BE_DROPS(fee);

    *((uint64_t*)(txn_mint + 26)) = fee;

    int64_t seq = ledger_seq() + 1;
    txn_mint[15] = (seq >> 24U) & 0xFFU;
    txn_mint[16] = (seq >> 16U) & 0xFFU;
    txn_mint[17] = (seq >>  8U) & 0xFFU;
    txn_mint[18] = seq & 0xFFU;

    seq += 4;
    txn_mint[21] = (seq >> 24U) & 0xFFU;
    txn_mint[22] = (seq >> 16U) & 0xFFU;
    txn_mint[23] = (seq >>  8U) & 0xFFU;
    txn_mint[24] = seq & 0xFFU;


    trace(SBUF("emit:"), txn_mint, upto-txn_mint, 1);

    uint8_t emithash[32];
    int64_t emit_result = emit(SBUF(emithash), txn_mint, upto - txn_mint);

    if (DEBUG)
        TRACEVAR(emit_result);

    if (emit_result < 0)
        rollback(SBUF("Reward: Emit loopback failed."), __LINE__);


    accept(SBUF("Reward: Emitted reward txn successfully."), __LINE__);
}
