#include <stdint.h>
#include "hookapi.h"
#define SVAR(x) &x, sizeof(x)
#define SBUF(x) (x), sizeof(x)
#define NOPE(x)\
{\
    return rollback((x), sizeof(x), __LINE__);\
}

#define ttNFTOKEN_BURN 26
#define ttIMPORT 97
#define ttURITOKEN_MINT 45

int64_t hook(uint32_t r)
{
    _g(1,1);

    uint8_t hook_acc[20];
    hook_account(SBUF(hook_acc));

    uint8_t otxn_acc[20];
    otxn_field(SBUF(otxn_acc), sfAccount);

    // outgoing
    if (BUFFER_EQUAL_20(hook_acc, otxn_acc))
        accept(SBUF("NFTImport: Passing outgoing txn."), __LINE__);

    if (otxn_type() != ttIMPORT)
        accept(SBUF("NFTImport: Passing non ttIMPORT txn."), otxn_type());

    int64_t retval = xpop_slot(1,2);

    if (retval <= 0)
        NOPE("Failed to slot xpop");

#define tx_slot 1
#define meta_slot 2

    trace_num("Slotted xpop", 12, retval);
    uint8_t dump1[2048];
    uint8_t dump2[2048];
    int64_t len1 = slot(dump1, sizeof(dump1), tx_slot);
    int64_t len2 = slot(dump2, sizeof(dump2), meta_slot);

    trace("tx", 2, dump1, len1, 1);
    trace("meta", 4, dump2, len2, 1);


    if (slot_subfield(meta_slot, sfTransactionResult, 3) != 3)
        NOPE("Failed to slot transaction result");

    uint8_t tr;
    if (slot(SVAR(tr), 3) != 1)
        NOPE("Failed to dump transaction result");

    trace_num(SBUF("Inner Transaction Result:"), tr);
    if (tr != 0)
        NOPE("Inner Transaction Result not tesSUCCESS (0).");

    // execution to here means tesSUCCESS on inner

    if (slot_subfield(tx_slot, sfTransactionType, 4) != 4)
        NOPE("Could not slot transaction type");

    uint8_t tt_buf[2];
    if (slot(SBUF(tt_buf), 4) != 2)
        NOPE("Could not dump transaction type");

    uint16_t tt = UINT16_FROM_BUF(tt_buf);

    if (tt != ttNFTOKEN_BURN)
        NOPE("Only NFTokenBurn is accepted");

    // go track down the URI of the token (this is a huge pain, has to be done through metadata)
    //
#define nodes 5
    if (slot_subfield(meta_slot, sfAffectedNodes, nodes) != nodes)
       NOPE("Could not slot sfAffectedNodes");

    uint8_t dump4[1024];
    trace(SBUF("slot nodes"), dump4, slot(SBUF(dump4), nodes), 1);


    int64_t count = slot_count(nodes);
    if (count > 5) count = 5;

    int64_t found;

    for (int i = 0; GUARD(5), i < count; ++i)
    {
        if (slot_subarray(nodes, i, 6) != 6)
            break;

        if (slot_subfield(6, sfLedgerEntryType, 7) != 7)
            NOPE("Could not slot LedgerEntryType");

        uint8_t buf[2];
        slot(SVAR(buf), 7);

        if (UINT16_FROM_BUF(buf) == 0x0050U)
        {
            found = 1;
            break;
        }
    }

    if (!found)
        NOPE("Could not find NFTokenPage in xpop metadata");

    if (slot_subfield(6, sfPreviousFields, 6) != 6 && slot_subfield(6, sfFinalFields, 6) != 6)
        NOPE("Could not slot sfPreviousFields");

    if (slot_subfield(6, sfNFTokens, 6) != 6)
        NOPE("Could not slot sfNFTokens");

    count = slot_count(6);
    if (count > 32) count = 32;

    trace_num(SBUF("Modified node count:"), count);
    uint8_t burned_tid[32];
    if (slot_subfield(tx_slot, sfNFTokenID, 7) != 7 || slot(SBUF(burned_tid), 7) != 32)
        NOPE("Xpop txn did not contain valid nftokenid");

    uint8_t uri[256];
    int64_t urilen;

    found = 0;
    for (int i = 0; GUARD(32), i < count; ++i)
    {
        if (slot_subarray(6, i, 7) != 7)
            break;

        {
            uint8_t dump4[1024];
            int64_t len = slot(SBUF(dump4), 7);
            trace(SBUF("dump4"), dump4, len, 1);
        }

        uint8_t tid[32];
        int64_t r;
        if ((r=slot_subfield(7, sfNFTokenID, 8)) != 8)
        {
            trace_num(SBUF("r"), r);
            break;
        }

        slot(SBUF(tid), 8);

        if (slot_subfield(7, sfURI, 8) != 8)
            continue;

        urilen = slot(SBUF(uri), 8);

        trace(SBUF("uri: "), uri, urilen, 1);
        trace(SBUF("tid1"), tid, 32, 1);
        trace(SBUF("tid2"), burned_tid, 32, 1);
        found = BUFFER_EQUAL_32(tid, burned_tid);
        if (found)
        {
            trace(SBUF("found"), 0,0,0);
            found = 1;
            break;
        }
    }

    if (!found)
        NOPE("Could not find the NFTokenID in the metadata");

    trace(SBUF("URI from xpop: "), uri, urilen, 1);

    etxn_reserve(1);
    uint8_t txn_buf[1024];
    int64_t txn_len;
    {
        uint8_t* buf_out = txn_buf;
        uint32_t cls = (uint32_t)ledger_seq();
        _01_02_ENCODE_TT                   (buf_out, ttURITOKEN_MINT                );
        _02_02_ENCODE_FLAGS                (buf_out, tfCANONICAL                    );
        _02_04_ENCODE_SEQUENCE             (buf_out, 0                              );
        _02_26_ENCODE_FLS                  (buf_out, cls + 1                        );
        _02_27_ENCODE_LLS                  (buf_out, cls + 5                        );
        _06_01_ENCODE_DROPS_AMOUNT         (buf_out, 0                              );
        uint8_t* fee_ptr = buf_out;
        _06_08_ENCODE_DROPS_FEE            (buf_out, 0                              );
        _07_03_ENCODE_SIGNING_PUBKEY_NULL  (buf_out                                 );

        // URI
        *buf_out++ = 0x75U;
        for (int i = 0; GUARD(32), i < 32; ++i)
            *(((uint64_t*)buf_out) + i) = *(((uint64_t*)uri) + i);
        buf_out += urilen;

        _08_01_ENCODE_ACCOUNT_SRC          (buf_out, hook_acc                       );
        _08_03_ENCODE_ACCOUNT_DST          (buf_out, otxn_acc                       );
        int64_t edlen = etxn_details((uint32_t)buf_out, 512);
        trace_num(SBUF("edlen"), edlen);
        buf_out += edlen;
        txn_len = buf_out - txn_buf;
        int64_t fee = etxn_fee_base(txn_buf, txn_len);
        _06_08_ENCODE_DROPS_FEE            (fee_ptr, fee                            );

    }

    trace(SBUF("emit txn"), txn_buf, txn_len, 1);

    uint8_t etxid[32];
    if (emit(SBUF(etxid), txn_buf, txn_len) < 0)
        NOPE("Emission failed");

    trace(SBUF("Emission success"), etxid, 32, 1);

	return accept(0UL, 0UL, __LINE__);
}
