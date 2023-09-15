#include "hookapi.h"
#define ASSERT(x)\
    if (!(x))\
        rollback(SBUF("Govern: Assertion failed."),__LINE__);

#define SEAT_COUNT 20

#define HOOK_MAX 10 // maximum number of hooks on an account

/**
 * Xahau Governance Hook
 *
 *  The governance model is a 20 seat round table.
 *  Each seat may be filled or empty (provided there is at least one filled seat.)
 *  The primary governance table sits on the genesis account. This is also called the L1 table.
 *  Seats at this table are called L1s or L1 members.
 *  At L1, for votes relating to table membership 80% of the filled seats must vote in favour.
 *  At L1, for votes relating to everything else 100% of the filled seats must vote in favour.
 *  One or more L1 seats may contain an account that has an L2 governance hook installed on it and is blackholed.
 *  This is referred to as an L2 table. The seats at the table are called L2s or L2 members.
 *  There may be multiple L2 tables.
 *
 *  Hook Parameters:
 *
 *      // both table types uses these parameters V
 *
 *      Parameter Name: {'I', 'M', 'C'}
 *      Parameter Value: Initial Member Count <1 byte>
 *
 *      Parameter Name:  {'I', 'S', '\0'}
 *      Parameter Value: Initial seat #0's member's 20 byte Account ID.
 *
 *      // only L1 table uses these parameters V
 *
 *      Parameter Name: {'I', 'R', 'R'}
 *      Parameter Value: Initial Reward Rate <8 byte XFL fraction between 0 and 1, LE>
 *
 *      Parameter Name: {'I', 'R', 'D'}
 *      Parameter Value: Initial Reward Delay <8 byte LE int seconds between rewards>
 *      ...
 *  
 *  Topics:
 *      'H[0-9]'    - Hook Hash in positions 0-9 <32 byte hash> on genesis
 *      'RR'        - reward rate  <le xfl 8 bytes>
 *      'RD'        - reward delay <le xfl 8 bytes>
 *      'S[0-19]'   - who is a governance member occupying that seat <20 byte accid>
 *
 *  Hook State:
 *      State Key: {0..0, 'M', 'C'}
 *      State Data: Current member count <1 byte>
 *
 *      State Key: {0..0, 'R', 'R'}
 *      State Data: Current reward rate <8 byte LE XFL> (L1 table only)
 *
 *      State Key: {0..0, 'R', 'D'}
 *      State Data: Current reward delay <8 byte LE int> (L1 table only)
 *
 *      State Key: {0..0, '\0 + seat id'}
 *      State Data: 20 byte account ID for the member who occupies this seat. If absent unoccupied.
 *
 *      State Key: {0..0, <20 byte account id>}
 *      State Data: Seat number this member occupies <1 byte>
 *
 *      State Key: {'V', 'H|R|S' <topic type>, '\0 + topic id', <layer>,  0..0, <member accid>}
 *      State Data: A vote by a member for a topic and topic data
 *
 *      State Key: {'C', 'H|R|S' <topic type>, '\0 + topic id', <layer>, 0*, <front truncated topic data>}
 *      State Data: The number of members who have voted for that topic data and topic combination <1 byte>
 *
 *  Hook Invocation:
 *      ttINVOKE:
 *          First time:
 *              Behaviour: Setup hook, setup initial accounts, end (accept).
 *
 *          Subsequent:
 *              Behaviour: Vote on a topic, if the votes meet the topic vote threshold, action the topic.
 *  
 *              Parameter Name: {'L'}
 *              Parameter Value: Which layer the vote is inteded for (ONLY L2 TABLES USE THIS PARAMETER)
 *                  { '1' a vote cast by an L2 member about an L1 topic }, or
 *                  { '2' a vote cast by an L2 member about an L2 topic }
 *
 *
 *              Parameter Name: {'T'}
 *              Parameter Value: The topic to vote on <2 bytes>
 *                  { 'S|H' (seat, hook), '\0 + topic id' }, or
 *                  { 'R' (reward), 'R|D' (rate, delay) }
 *
 *              Parameter Name: {'V'}
 *              Parameter Value: The data to vote for this topic (accid, hook hash, reward rate/delay)
 **/

#define SVAR(x) &x, sizeof(x)

#define DONE(x)\
    accept(SBUF(x),__LINE__);

#define NOPE(x)\
    rollback(SBUF(x), __LINE__);

#define DEBUG 1

// genesis account id
uint8_t genesis[20] =
    {0xB5U,0xF7U,0x62U,0x79U,0x8AU,0x53U,0xD5U,0x43U,0xA0U,0x14U,
     0xCAU,0xF8U,0xB2U,0x97U,0xCFU,0xF8U,0xF2U,0xF9U,0x37U,0xE8U};

uint8_t zero32[32];

int64_t hook(uint32_t r)
{
    _g(1,1);

    etxn_reserve(1);

    // in debug mode the test case can supply a line number that the hook will then print here.
    {
        uint8_t ln[2];
        if (otxn_param(SBUF(ln), "D", 1) == 2)
        {
            uint16_t lineno = (((uint16_t)ln[0]) << 8U) + ln[1];
            trace_num(SBUF("DBGLN"), lineno);
        }
    }

    int64_t tt = otxn_type();

    if (tt != 99)  // ttINVOKE only
        DONE("Governance: Passing non-Invoke txn. HookOn should be changed to avoid this.");

    // get the account id
    uint8_t account_field[32];
    otxn_field(account_field + 12, 20, sfAccount);

    uint8_t hook_accid[32];
    hook_account(hook_accid + 12, 20);

    // outgoing txns to other hooks allowed
    if (BUFFER_EQUAL_20(hook_accid + 12, account_field + 12))
    {
        uint8_t dest_acc[20];
        if (otxn_field(SBUF(dest_acc), sfDestination) == 20 && !BUFFER_EQUAL_20(hook_accid + 12, dest_acc))
            DONE("Goverance: Passing outgoing txn.");
    }
    
    int64_t is_L1_table = BUFFER_EQUAL_20(hook_accid + 12, genesis);

    if (is_L1_table)
        trace(SBUF("Governance: Starting governance logic on L1 table."), 0,0,0);
    else
        trace(SBUF("Governance: Starting governance logic on L2 table."), 0,0,0);

    int64_t member_count = state(0,0, "MC", 2);
   
    // initial execution, setup hook
    if (member_count == DOESNT_EXIST)
    {
        // gather hook parameters

        uint8_t imc;
        uint64_t irr, ird;
        if (hook_param(SVAR(imc), "IMC", 3) < 0)
            NOPE("Governance: Initial Member Count Parameter missing (IMC).");
        TRACEVAR(imc);
        
        // set member count
        ASSERT(0 < state_set(SVAR(imc), "MC", 2));

        member_count = imc;
        TRACEVAR(member_count);

        
        if (imc == 0)
            NOPE("Governance: Initial Member Count must be > 0.");

        if (imc > SEAT_COUNT)
            NOPE("Governance: Initial Member Count must be <= Seat Count (20).");

        if (is_L1_table)
        {
            if (hook_param(SVAR(irr), "IRR", 3) < 0)
                NOPE("Governance: Initial Reward Rate Parameter missing (IRR).");
    
            if (hook_param(SVAR(ird), "IRD", 3) < 0)
                NOPE("Governance: Initial Reward Delay Parameter miss (IRD).");
        
            if (ird == 0)
                NOPE("Governance: Initial Reward Delay must be > 0.");
        
            // set reward rate
            ASSERT(0 < state_set(SVAR(irr), "RR", 2));

            // set reward delay
            ASSERT(0 < state_set(SVAR(ird), "RD", 2));
        }

        for (uint8_t i = 0; GUARD(SEAT_COUNT), i < member_count; ++i)
        {
            uint8_t member_acc[20];
            uint8_t member_pkey[3] = {'I', 'S', i};
            if (hook_param(SBUF(member_acc), member_pkey, 3) != 20)
                NOPE("Governance: One or more initial member account ID's is missing");

                                                            // 0... X where X is member id started from 1
                                                            // maps to the member's account ID
            trace(SBUF("Member:"), SBUF(member_acc), 1);
            // reverse key 
            ASSERT(state_set(SBUF(member_acc), SVAR(i)) == 20);
        
                                                            // 0, 0... ACCOUNT ID maps to member_id (as above)
            // forward key
            ASSERT(state_set(SVAR(i), SBUF(member_acc)) == 1);
        }

        DONE("Governance: Setup completed successfully.");
    }
    
    if (DEBUG)
        TRACEVAR(member_count);
    
    
    // otherwise a normal execution (not initial)
    // first let's check if the invoking party is a member
    
    int64_t member_id = state(0,0,account_field + 12, 20);
    if (member_id < 0)
        NOPE("Governance: You are not currently a governance member at this table.");

    // the only thing a member can do is vote for a topic
    // so lets process their vote
  
    // { 'S|H|R', '\0 + topicid' }
    uint8_t topic[2];
    int64_t result = otxn_param(SBUF(topic), "T", 1);
    uint8_t t = topic[0];   // topic type
    uint8_t n = topic[1];   // number (seats) (or R/D for reward rate/delay)

    if (result != 2 || (
                t != 'S' &&      // topic type: seat 
                t != 'H' &&      // topic type: hook 
                t != 'R'))       // topic type: reward 
        NOPE("Governance: Valid TOPIC must be specified as otxn parameter.");

   
    if (t == 'S' && n > (SEAT_COUNT - 1))
        NOPE("Governance: Valid seat topics are 0 through 19.");

    if (t == 'H' && n > HOOK_MAX)
        NOPE("Governance: Valid hook topics are 0 through 9.");

    if (t == 'R' && n != 'R' && n != 'D')
        NOPE("Governance: Valid reward topics are R (rate) and D (delay).");
   
    // is their vote for the L2 table or the L1 table?
    uint8_t l = 1;
    if (!is_L1_table)
    {
        result = otxn_param(SVAR(l), "L", 1);
        if (result != 1)
            NOPE("Governance: Missing L parameter. Which layer are you voting for?");
    
        if (l != '1' && l != '2')
            NOPE("Governance: Layer parameter must be '1' or '2'.");

        l -= '0'; // now it will be 1/2 rather than '1'/'2'.
    }

    if (l == 2 && t == 'R')
        NOPE("Governance: L2s cannot vote on RR/RD at L2, did you mean to set L=1?");


    // RH TODO: validate RR/RD xfl > 0
    uint8_t topic_data[56 /* there's a 24 byte pad on the back for later logic */];
    uint8_t topic_size = 
        t == 'H' ? 32 :      // hook topics are a 32 byte hook hash
        t == 'S' ? 20 :      // account topics are a 20 byte account ID
                    8;       // reward topics are an 8 byte le xfl
    
    uint8_t padding = 32 - topic_size;

    result = otxn_param(topic_data + padding, topic_size, "V", 1);
    if (result != topic_size)
        NOPE("Governance: Missing or incorrect size of VOTE data for TOPIC type.");

    // set this flag if the topic data is all zeros
    int topic_data_zero = 
        (*((uint64_t*)(topic_data +  0)) == 0) &&
        (*((uint64_t*)(topic_data +  8)) == 0) &&
        (*((uint64_t*)(topic_data + 16)) == 0) &&
        (*((uint64_t*)(topic_data + 24)) == 0);

    trace(SBUF("topic_data_raw:"), topic_data, 56, 1);
    trace_num(SBUF("topic_padding:"), padding);
    trace_num(SBUF("topic_size:"), topic_size);
    trace(SBUF("topic_data:"), topic_data + padding, topic_size, 1);

    // reuse account_field to create vote key
    account_field[0] = 'V';
    account_field[1] = t;
    account_field[2] = n;
    account_field[3] = l;

    // get their previous vote if any on this topic
    uint8_t previous_topic_data[32];
    int64_t previous_topic_size =
        state(previous_topic_data + padding, topic_size, SBUF(account_field));

    // check if the vote they're making has already been cast before,
    // if it is identical to their existing vote for this topic then just end with tesSUCCESS
    trace(SBUF("previous_topic_data"), previous_topic_data, 32, 1);
    trace(SBUF("topic_data"), topic_data, 32, 1);
    trace_num(SBUF("previous_topic_size"), previous_topic_size);
    trace_num(SBUF("topic_size"), topic_size);
    if (previous_topic_size == topic_size && BUFFER_EQUAL_32(previous_topic_data, topic_data))
        DONE("Governance: Your vote is already cast this way for this topic.");

    // execution to here means the vote is different
    // we might have to decrement the old voting if they voted previously
    // and we will have to increment the new voting

    // write vote to their voting key
    ASSERT(state_set(topic_data + padding, topic_size, SBUF(account_field)) == topic_size);
    
    uint8_t previous_votes = 0;
    // decrement old vote counter for this option
    if (previous_topic_size > 0)
    {
        uint8_t votes = 0;
        // override the first two bytes to turn it into a vote count key
        previous_topic_data[0] = 'C';
        previous_topic_data[1] = t;
        previous_topic_data[2] = n;
        previous_topic_data[3] = l;

        ASSERT(state(&votes, 1, SBUF(previous_topic_data)) == 1);
        ASSERT(votes > 0);
        previous_votes = votes;
        votes--;
        // delete the state entry if votes hit zero
        ASSERT(state_set(votes == 0 ? 0 : &votes, votes == 0 ? 0 : 1, SBUF(previous_topic_data)) >= 0);
    }
    
    // increment new counter 
    uint8_t votes = 0;
    {
        // we're going to clobber the topic data to turn it into a vote count key
        // so store the first bytes 
        uint64_t saved_data = *((uint64_t*)topic_data);
        topic_data[0] = 'C';
        topic_data[1] = t;
        topic_data[2] = n;
        topic_data[3] = l;

        state(&votes, 1, topic_data, 32);
        votes++;
        ASSERT(0 < state_set(&votes, 1, topic_data, 32));

        // restore the saved bytes
        *((uint64_t*)topic_data) = saved_data;
    }
   


    if (DEBUG)
    {
        TRACEVAR(topic_data_zero);
        TRACEVAR(votes);
        TRACEVAR(member_count);
        trace(SBUF("topic"), topic, 2, 1);
    }
   
    // this flag is used to determine if a L2 table should send a "nulling" vote to remove its existing vote
    // from the L1 table it sits at. 
    int64_t lost_majority = 0;

    int64_t q80 = member_count * 0.8;
    int64_t q51 = member_count * 0.51;

    if (q80 < 2)
        q80 = 2;
    
    if (q51 < 2)
        q51 = 2;

    if (l == 1)
    {
       if (votes <
            (t == 'S'
                ? q80                      // L1s have 80% threshold for membership/seat voting
                : member_count))            // L1s have 100% threshold for all other voting
        DONE("Governance: L1 vote record. Not yet enough votes to action.");
    }
    else    // l == 2
    {
        lost_majority = previous_votes >= q51 && votes < q51;
        if (lost_majority)
            trace(SBUF("Governance: L2 vote recorded. Majority lost, undoing L1 vote."),0,0,0);
        else if (votes < q51)
            DONE("Governance: L2 vote recorded. Not yet enough votes to action L1 vote..");
    }

    

    // action vote
    if (DEBUG)
        TRACESTR("Actioning votes");

    if (l == 2)
    {
    

        uint8_t txn_out[1024];
        uint8_t* buf_out = txn_out;
        uint32_t cls = (uint32_t)ledger_seq();
        _01_02_ENCODE_TT                   (buf_out, ttINVOKE);
        _02_02_ENCODE_FLAGS                (buf_out, tfCANONICAL);
        _02_04_ENCODE_SEQUENCE             (buf_out, 0);
        _02_26_ENCODE_FLS                  (buf_out, cls + 1);
        _02_27_ENCODE_LLS                  (buf_out, cls + 5);
        uint8_t* fee_ptr = buf_out;
        _06_08_ENCODE_DROPS_FEE            (buf_out, 0);
        _07_03_ENCODE_SIGNING_PUBKEY_NULL  (buf_out);
        _08_01_ENCODE_ACCOUNT_SRC          (buf_out, hook_accid + 12);
        _08_03_ENCODE_ACCOUNT_DST          (buf_out, genesis);
        int64_t edlen = etxn_details((uint32_t)buf_out, 512);
        buf_out += edlen;

        /** Parameters:
         * F013E017 70180154
         * 701902 
         * <00> <two byte topic code>
         * E1E0177018015670
         * 19
         * <topic len>
         * <topic data>
         * E1F1
        */

        // note wasm is LE and Txns are BE so these large constants are endian flipped
        // to undo the flipping on insertion into memory
        *((uint64_t*)buf_out) = 0x5401187017E013F0ULL; // parameters array, first parameter preamble
        buf_out += 8;
        *((uint32_t*)buf_out) = 0x021970UL;
        buf_out += 3;
        *buf_out++ = (t << 4) + n;  // topic
        *((uint64_t*)buf_out) = 0x705601187017E0E1ULL;
        buf_out += 8;
        *buf_out++ = 0x19U;
        // topic data len
        *buf_out++ = topic_size;
        if (lost_majority)
        {
            // do nothing, the array already has zeros here
        }
        else
        {
            uint64_t* d = (uint64_t*)buf_out;
            uint64_t* s = (uint64_t*)(topic_data + padding);
            *d++ = *s++;
            *d++ = *s++;
            *d++ = *s++;
            *d++ = *s++;
        }
        buf_out += topic_size;
        // topicdata
        *((uint16_t*)buf_out) = 0xF1E1U;

        int64_t txn_len = buf_out - txn_out;

        // populate fee
        int64_t fee = etxn_fee_base(txn_out, txn_len);
        _06_08_ENCODE_DROPS_FEE            (fee_ptr, fee                            );

        trace(SBUF("Governance: Emitting invoke to L1"), txn_out, txn_len, 1);


        uint8_t emit_hash[32];
        int64_t emit_result = emit(SBUF(emit_hash), txn_out, txn_len);

        trace_num(SBUF("Governance: Emit result"), emit_result);

        if (emit_result == 32)
            accept(SBUF("Governance: Successfully emitted L1 vote."), __LINE__);

        NOPE("Governance: L1 vote emission failed.");
    }
    
    switch(t)
    {
        case 'R':
        {
            // reward topics
            int64_t result = state_set(topic_data + padding, topic_size, SBUF(topic));
            TRACEVAR(result);
            ASSERT(0 < result);
            if (n == 'R')
                DONE("Governance: Reward rate change actioned!");
            
            DONE("Governance: Reward delay change actioned!");
        }
        
        case 'H':
        {
            // hook topics

            // first get the hook ledget object
            uint8_t keylet[34];
            util_keylet(SBUF(keylet), KEYLET_HOOK, hook_accid + 12, 20, 0,0,0,0);
            slot_set(SBUF(keylet), 5);

            // now get the hooks array
            slot_subfield(5, sfHooks, 6);

            // now check the entry
            if (slot_subarray(6, n, 7) == 7)
            {
                // it exists
                // check if its identical
                uint8_t existing_hook[32];
                if (slot_subfield(7, sfHookHash, 8) == 8)
                {
                    ASSERT(slot(SBUF(existing_hook), 8) == 32);

                    // if it is then do nothing
                    if (BUFFER_EQUAL_32(existing_hook, topic_data))
                        DONE("Goverance: Target hook is already the same as actioned hook.");
                }
            }

            // generate the hook definition keylet
            if (!topic_data_zero)
            {
                util_keylet(SBUF(keylet), KEYLET_HOOK_DEFINITION, topic_data, 32, 0,0,0,0);

                // check if the ledger contains such a hook definition
                if (slot_set(SBUF(keylet), 9) != 9)
                    NOPE("Goverance: Hook Hash doesn't exist on ledger while actioning hook.");
            }

            // it does so now we can do the emit

            uint8_t* hookhash = 
                topic_data_zero 
                ? ((uint8_t*)0xFFFFFFFFU) // if the topic data is all zero then it's a delete operation
                : topic_data;             // otherwise it's an install operation


            uint8_t* h[10];
            h[n] = hookhash;

            uint8_t emit_buf[1024];
            uint32_t emit_size = 0;
            PREPARE_HOOKSET(emit_buf, sizeof(emit_buf), h, emit_size);

            trace(SBUF("EmittedTxn"), emit_buf, emit_size, 1);

            uint8_t emithash[32];
            int64_t emit_result = emit(SBUF(emithash), emit_buf, emit_size);

            if (DEBUG)
                TRACEVAR(emit_result);

            if (emit_result != 32)
                NOPE("Governance: Emit failed during hook actioning.");

            trace(SBUF("EmittedTxnHash"), emithash, 32, 1);
            DONE("Governance: Hook actioned.");
        }

        case 'S':
        {
            // add / change member
            uint8_t previous_member[32];
            int previous_present = (state(previous_member + 12, 20, &n, 1) == 20);
            if (previous_present)
            {
                trace(SBUF("Previous present==:"), previous_member, 32, 1);
            }


            if (BUFFER_EQUAL_20((previous_member + 12), (topic_data + 12)))
                DONE("Governance: Actioning seat change, but seat already contains the new member.");

            int64_t existing_member = state(0,0, topic_data + 12, 20);

            int existing_member_moving = existing_member >= 0;
            if (existing_member_moving)
                trace(SBUF("Governance: Moving existing member to new seat."), 0,0,0);


            uint8_t op = ((!previous_present) << 2U) +
                    (topic_data_zero << 1U) + existing_member_moving;

            ASSERT(op != 0b011U && op != 0b111U && op < 8);

            // logic table:
            // E/!E - seat is empty/filled
            // Z/!Z - topic data is zero non zero (zero = member deletion)
            // M/!M - topic is an existing member who is moving
            //
            // E|Z|M
            // -+-+-
            // 0 0 0    - seat is full, vote is for a member, an existing member is not moving              MC
            // 0 0 1    - seat is full, vote is for a member, an existing member is moving                  MC--
            // 0 1 0    - seat is full, vote is for deletion, an existing member is not moving              MC--
            // 0 1 1    - seat is full, vote is for deletion, an existing member is moving (impossible)
            // 1 0 0    - seat is empty, vote is for a member, member is not an existing member             MC++
            // 1 0 1    - seat is empty, vote is for a member, member is an existing member                 MC
            // 1 1 0    - seat is empty, vote is for deletion, not an existing member moving                MC
            // 1 1 1    - seat is empty, vote is for deletion, an existing member moving (impossible)

            TRACEVAR(op);
            trace_num(SBUF("E"), !previous_present);
            trace_num(SBUF("Z"), topic_data_zero);
            trace_num(SBUF("M"), existing_member_moving);

            // adjust member count
            {
                if (op == 0b001U || op == 0b010U)
                    member_count--;
                else if (op == 0b100U)
                    member_count++;

                TRACEVAR(previous_present);
                TRACEVAR(topic_data_zero); 
                TRACEVAR(member_count);

                ASSERT(member_count > 1);   // just bail out if the second last member is being removed

                uint8_t mc = member_count;
                ASSERT(state_set(&mc, 1, "MC", 2) == 1);
            }

            // if an existing member is moving we need to delete them before re-adding them
            if (existing_member_moving)
            {

                // delete the old member
                // reverse key 
                uint8_t m = (uint8_t)existing_member;
                ASSERT(state_set(0,0, &m, 1) == 0);
                
                // forward key
                ASSERT(state_set(0, 0, topic_data + 12, 20) == 0);
                
            }

            // we need to garbage collect all their votes
            if (previous_present)
            {
                previous_member[0] = 'V';

                for (int i = 1; GUARD(32), i < 32; ++i)
                {
                    previous_member[1] = i < 2 ? 'R' : i < 12 ? 'H' : 'S';
                    previous_member[2] = 
                        i == 0 ? 'R' :
                        i == 1 ? 'D' :
                        i < 12 ? i - 2 :
                        i - 12;

                    uint8_t vote_key[32];
                    if (state(SBUF(vote_key), SBUF(previous_member)) == 32)
                    {
                        uint8_t vote_count = 0;

                        // find and decrement the vote counter
                        vote_key[0] = 'C';
                        vote_key[1] = previous_member[1];
                        vote_key[2] = previous_member[2];
                        if (state(&vote_count, 1, SBUF(vote_key)) == 1)
                        {
                            // if we're down to 1 vote then delete state
                            if (vote_count <= 1)
                            {
                                ASSERT(state_set(0,0, SBUF(vote_key)) == 0);
                                trace_num(SBUF("Decrement vote count deleted"), vote_count);
                            }
                            else    // otherwise decrement
                            {
                                vote_count--;
                                ASSERT(state_set(&vote_count, 1, SBUF(vote_key)) == 1);
                                trace_num(SBUF("Decrement vote count to"), vote_count);
                            }
                        }

                        // delete the vote entry
                        ASSERT(state_set(0,0, SBUF(previous_member)) == 0);
                        trace(SBUF("Vote entry deleted"), vote_key, 32, 1);
                    }
                }

                // delete the old member
                // reverse key 
                ASSERT(state_set(0,0, &n, 1) == 0);
                
                // forward key
                ASSERT(state_set(0, 0, previous_member + 12, 20) == 0);
            }

            if (!topic_data_zero)
            {
                // add the new member
                // reverse key 
                ASSERT(state_set(topic_data + 12, 20, &n, 1) == 20);
                
                // forward key
                ASSERT(state_set(&n, 1, topic_data + 12, 20) == 1);
            }

            DONE("Governance: Action member change.");
        }
    }

   rollback(SBUF("Governance: Internal logic error."), __LINE__); 
}
