// This hook just tests GenesisMint transactor, it is not for production use

#include "hookapi.h"
#define ASSERT(x)\
    if (!(x))\
        rollback(SBUF("MintTest: Assertion failure."),__LINE__);

#define DEBUG 1

uint8_t txn_mint[60000] =
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


int64_t hook(uint32_t r)
{
    etxn_reserve(1);
    _g(1,1);

    hook_account(txn_mint + 71, 20);

    otxn_slot(1);
    ASSERT(slot_subfield(1, sfBlob, 2) == 2);

    
    int64_t bytes = slot(txn_mint + 207, 60000 - 207, 2);

    ASSERT(bytes > 0);

    bytes += 207;

    // nop out any vl encoding
    uint8_t x = txn_mint[207];
    uint8_t y = txn_mint[208];
    uint8_t z = txn_mint[209];

    txn_mint[207] = 0x99U;
    if (x > 192U)
    {
        txn_mint[208] = 0x99U;
        if (y > 240)
            txn_mint[209] = 0x99U;
    }


    trace(SBUF("Txn:"), txn_mint, bytes, 1);

    ASSERT(etxn_details(txn_mint + 91, 116) > 0);

    int64_t fee = etxn_fee_base(txn_mint, bytes);
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


    trace(SBUF("emit:"), txn_mint, bytes, 1);

    uint8_t emithash[32];
    int64_t emit_result = emit(SBUF(emithash), txn_mint, bytes);

    if (DEBUG)
        TRACEVAR(emit_result);

    if (emit_result < 0)
        rollback(SBUF("MintTest: Emit failed."), __LINE__);


    accept(SBUF("MintTest: Emitted txn successfully."), __LINE__);
}
