#include "hookapi.h"
#define ASSERT(x)\
    if (!(x))\
        rollback(SBUF("Govern: Assertion failed."),__LINE__);

#define SEAT_COUNT 20

#define HOOK_MAX 10 // maximum number of hooks on an account

/**
 * Xahau Governance Hook
 *
 *  Hook Parameters:
 *      Parameter Name: {'I', 'M', 'C'}
 *      Parameter Value: Initial Member Count <1 byte>
 *
 *      Parameter Name: {'I', 'D', 'A'}
 *      Parameter Value: Initial Distribution Amount to each initial member <8 byte BE drops>
 *
 *      Parameter Name: {'I', 'R', 'R'}
 *      Parameter Value: Initial Reward Rate <8 byte XFL fraction between 0 and 1, LE>
 *
 *      Parameter Name: {'I', 'R', 'D'}
 *      Parameter Value: Initial Reward Delay <8 byte LE int seconds between rewards>
 *
 *      Parameter Name:  {'I', 'S', '\0'}
 *      Parameter Value: Initial seat #0's member's 20 byte Account ID.
 *
 *      ...
 *  
 *  Topics:
 *      'H[0-9]'    - Hook Hash in positions 0-9 <32 byte hash>
 *      'RR'        - reward rate  <le xfl 8 bytes>
 *      'RD'        - reward delay <le xfl 8 bytes>
 *      'S[0-19]'   - who is a governance member occupying that seat <20 byte accid>
 *
 *  Hook State:
 *      State Key: {0..0, 'M', 'C'}
 *      State Data: Current member count <1 byte>
 *
 *      State Key: {0..0, 'R', 'R'}
 *      State Data: Current reward rate <8 byte LE XFL>
 *
 *      State Key: {0..0, 'R', 'D'}
 *      State Data: Current reward delay <8 byte LE int>
 *
 *      State Key: {0..0, '\0 + seat id'}
 *      State Data: 20 byte account ID for the member who occupies this seat. If absent unoccupied.
 *
 *      State Key: {0..0, <20 byte account id>}
 *      State Data: Seat number this member occupies <1 byte>
 *
 *      State Key: {'V', 'H|R|S' <topic type>, '\0 + topic id', 0..0, <member accid>}
 *      State Data: A vote by a member for a topic and topic data
 *
 *      State Key: {'C', 'H|R|S' <topic type>, '\0 + topic id', 0*, <front truncated topic data>}
 *      State Data: The number of members who have voted for that topic data and topic combination <1 byte>
 *
 *  Hook Invocation:
 *      ttINVOKE:
 *          First time:
 *              Behaviour: Setup hook, fund initial accounts, end (accept).
 *
 *          Subsequent:
 *              Behaviour: Vote on a topic, if the votes meet the topic vote threshold, action the topic.
 *
 *              Parameter Name: {'T'}
 *              Parameter Value: The topic to vote on <2 bytes>
 *                  { 'S|H' (seat, hook), '\0 + topic id' }, or
 *                  { 'R' (reward), 'R|D' (rate, delay) }
 *
 *              Parameter Name: {'V'}
 *              Parameter Value: The data to vote for this topic (accid, hook hash, reward rate/delay)
 **/

#define SVAR(x) x, sizeof(x)

#define DONE(x)\
    accept(SVAR(x),(uint32_t)__LINE__);

#define NOPE(x)\
    rollback(SBUF(x), __LINE__);

#define DEBUG 1

int64_t hook(uint32_t r)
{
    _g(1,1);

    if (otxn_type() != 99)  // ttINVOKE
        DONE("Governance: Passing non-Invoke txn. HookOn should be changed to avoid this.");

    // get the account id
    uint8_t account_field[32];
    otxn_field(account_field + 12, 20, sfAccount);

    uint8_t hook_accid[20];
    hook_account(SBUF(hook_accid));
    
    int64_t member_count = state(0,0, "MC", 2);
    if (DEBUG)
        TRACEVAR(member_count);
    
    // outgoing txns to other hooks allowed
    // but a self ttINVOKE means rewards hook is trying to get the governance hook to distribute governance rewards
    int64_t is_distribution = 0;
    int64_t is_setup = member_count == DOESNT_EXIST;

    if (BUFFER_EQUAL_20(hook_accid, account_field + 12))
    {
        uint8_t dest_acc[20];
        if (otxn_field(SBUF(dest_acc), sfDestination) == 20 && !BUFFER_EQUAL_20(hook_accid, dest_acc))
            DONE("Goverance: Passing outgoing txn.");
    
        is_distribution = 1;
    }

    // initial execution, setup hook

    int64_t distribution_amount;


    if (is_setup)
    {
        // gather hook parameters

        int64_t imc, ida, irr, ird;
        if (hook_param(SVAR(imc), "IMC", 3) < 0)
            NOPE("Governance: Initial Member Count Parameter missing (IMC).");

        if (hook_param(SVAR(ida), "IDA", 3) < 0)
            NOPE("Governance: Initial Distribution Amount Prameter missing (IDA).");

        if (hook_param(SVAR(irr), "IRR", 3) < 0)
            NOPE("Governance: Initial Reward Rate Parameter missing (IRR).");

        if (hook_param(SVAR(ird), "IRD", 3) < 0)
            NOPE("Governance: Initial Reward Delay Parameter miss (IRD).");

        // sanity check parameters
        if (imc == 0)
            NOPE("Governance: Initial Member Count must be > 0.");

        if (imc > SEAT_COUNT)
            NOPE("Governance: Initial Member Count must be <= Seat Count (20).");

        if (ird == 0)
            NOPE("Governance: Initial Reward Delay must be > 0.");

        etxn_reserve(imc);

        // set member count
        ASSERT(state_set(SBUF(imc), "MC", 2));

        // set reward rate
        ASSERT(state_set(SVAR(irr), "RR", 2));
    
        // set reward delay
        ASSERT(state_set(SVAR(ird), "RD", 2));

        distribution_amount = ida;
        member_count = imc;
    }
    else if (is_distribution)
    {
        ASSERT(member_count > 0);

        uint8_t amt[8];
        ASSERT(otxn_param(SBUF(amt), "reward", 5) == 8);
        distribution_amount = INT64_FROM_BUF(amt);
        distribution_amount /= member_count;
        ASSERT(distribution_amount > 0);
    }

    if (is_setup || is_distribution)
    {
        for (uint8_t i = 0; GUARD(SEAT_COUNT), i < member_count; ++i)
        {
            uint8_t member_acc[20];
            if (is_setup)
            {
                uint8_t member_pkey[3] = {'I', 'S', i};
                if (hook_param(SBUF(member_acc), member_pkey, 3) != 20)
                    NOPE("Governance: One or more initial member account ID's is missing");

                                                                // 0... X where X is member id started from 1
                                                                // maps to the member's account ID
                // reverse key 
                ASSERT(state_set(SBUF(member_acc), SVAR(i)) == 20);
            
                                                                // 0, 0... ACCOUNT ID maps to member_id (as above)
                // forward key
                ASSERT(state_set(SVAR(i), SBUF(member_acc)) == 1);
            }
            else if (state(SBUF(member_acc), SVAR(i)) != 20)
                break;

            // emit initial
            uint8_t tx[PREPARE_PAYMENT_SIMPLE_SIZE];
            PREPARE_PAYMENT_SIMPLE(tx, distribution_amount, member_acc, 0, 0);

            // emit the transaction
            uint8_t emithash[32];
            int64_t emit_result = emit(SBUF(emithash), SBUF(tx));
            ASSERT(emit_result > 0);

            if (DEBUG)
                TRACEVAR(emit_result);

        }

        DONE(is_setup
            ? "Governance: Setup completed successfully."
            : "Governance: Reward distribution completed successfully.");
    }

    // otherwise a normal execution (not initial)
    // first let's check if the invoking party is a member
    
    int64_t member_id = state(0,0,account_field + 12, 20);
    if (member_id < 0)
        NOPE("Governance: You are not currently a governance member.");

    // the only thing a member can do is vote for a topic
    // so lets process their vote
  
    // { 'S|H|R', '\0 + topicid' }
    uint8_t topic[2];
    int64_t result = otxn_param(SBUF(topic), "T", 1);
    if (result != 2 || (
                topic[0] != 'S' &&      // topic type: seat
                topic[0] != 'H' &&      // topic type: hook
                topic[0] != 'R'))       // topic type: reward 
        NOPE("Governance: Valid TOPIC must be specified as otxn parameter.");

   
    if (topic[0] == 'S' && topic[1] > (SEAT_COUNT - 1))
        NOPE("Governance: Valid seat topics are 0 through 19.");

    if (topic[0] == 'H' && topic[1] > HOOK_MAX)
        NOPE("Governance: Valid hook topics are 0 through 9.");

    if (topic[0] == 'R' && topic[1] != 'R' && topic[1] != 'D')
        NOPE("Governance: Valid reward topics are R (rate) and D (delay).");
   
    // RH TODO: validate RR/RD xfl > 0

    uint8_t topic_data[32];
    uint8_t topic_size = 
        topic[0] == 'H' ? 32 :      // hook topics are a 32 byte hook hash
        topic[0] == 'S' ? 20 :      // account topics are a 20 byte account ID
                          8;        // reward topics are an 8 byte le xfl
    
    uint8_t padding = 32 - topic_size;

    result = otxn_param(topic_data + padding, topic_size, "V", 1);
    if (result != topic_size)
        NOPE("Governance: Missing or incorrect size of VOTE data for TOPIC type.");


    // reuse account_field to create vote key
    account_field[0] = 'V';
    account_field[1] = topic[0];
    account_field[2] = topic[1];

    // get their previous vote if any on this topic
    uint8_t previous_topic_data[32];
    int64_t previous_topic_size = state(previous_topic_data + padding, topic_size, SBUF(account_field));

    // check if the vote they're making has already been cast before,
    // if it is identical to their existing vote for this topic then just end with tesSUCCESS
    if (previous_topic_size == topic_size && BUFFER_EQUAL_32(previous_topic_data, topic_data))
        DONE("Governance: Your vote is already cast this way for this topic.");

    // execution to here means the vote is different
    // we might have to decrement the old voting if they voted previously
    // and we will have to increment the new voting

    // write vote to their voting key
    ASSERT(state_set(topic_data, topic_size, SBUF(account_field)) == topic_size);
    
    // decrement old vote counter for this option
    if (previous_topic_size > 0)
    {
        uint8_t votes = 0;
        // override the first two bytes to turn it into a vote count key
        previous_topic_data[0] = 'C';
        previous_topic_data[1] = topic[0];
        previous_topic_data[2] = topic[1];

        if (state(&votes, 1, SBUF(previous_topic_data)) && votes > 0)
        {
            votes--;
            // delete the state entry if votes hit zero
            ASSERT(state_set(votes == 0 ? 0 : &votes, votes == 0 ? 0 : 1, SBUF(previous_topic_data)));
        }
    }
    
    // increment new counter 
    uint8_t votes = 0;
    {
        // we're going to clobber the topic data to turn it into a vote count key
        // so store the first bytes 
        uint64_t saved_data = *((uint64_t*)topic_data);
        topic_data[0] = 'C';
        topic_data[1] = topic[0];
        topic_data[2] = topic[1];

        state(&votes, 1, SBUF(topic_data));
        votes++;
        ASSERT(state_set(&votes, 1, SBUF(topic_data)));

        // restore the saved bytes
        *((uint64_t*)topic_data) = saved_data;
    }
   

    // set this flag if the topic data is all zeros
    uint8_t zero[32];
    int topic_data_zero = BUFFER_EQUAL_32(topic_data, zero); 

    if (DEBUG)
    {
        TRACEVAR(topic_data_zero);
        TRACEVAR(votes);
        TRACEVAR(member_count);
        trace(SBUF("topic"), topic, 2, 1);
    }
    
    // now check if we hit threshold
    if (votes < (topic[0] == 'S' ? (member_count * 0.8) : member_count))
        DONE("Governance: Vote recorded. Not yet enough votes to action.");

    // 100%/80% threshold as needed is reached
    // action vote

    if (DEBUG)
        TRACESTR("Actioning votes");

    switch (topic[0])
    {
        case 'R':
        {
            // reward topics
            ASSERT(state_set(topic_data, 8, SBUF(topic)));      // interest rate is on the the FF, 0...0 key
            if (topic[1] == 'R')
                DONE("Governance: Reward rate change actioned!");
            
            DONE("Governance: Reward delay change actioned!");
        }

        case 'H':
        {
            // hook topics

            // first get the hook ledget object
            uint8_t keylet[34];
            util_keylet(SBUF(keylet), KEYLET_HOOK, SBUF(hook_accid), 0,0,0,0);
            slot_set(SBUF(keylet), 5);

            // now get the hooks array
            slot_subfield(5, sfHooks, 6);

            // now check the entry
            if (slot_subarray(6, topic[1], 7) == 7)
            {
                // it exists
                // check if its identical
                uint8_t existing_hook[32];
                slot_subfield(7, sfHookHash, 8);
                ASSERT(slot(SBUF(existing_hook), 8) == 32);

                // if it is then do nothing
                if (BUFFER_EQUAL_32(existing_hook, topic_data))
                    DONE("Goverance: Target hook is already the same as actioned hook.");
            }

            // generate the hook definition keylet
            util_keylet(SBUF(keylet), KEYLET_HOOK_DEFINITION, topic_data, 32, 0,0,0,0);

            // check if the ledger contains such a hook definition
            if (slot_set(SBUF(keylet), 9) != 9)
                NOPE("Goverance: Hook Hash doesn't exist on ledger while actioning hook.");

            // it does so now we can do the emit
            etxn_reserve(1);

            uint8_t* hookhash = 
                topic_data_zero 
                ? ((uint8_t*)0xFFFFFFFFU) // if the topic data is all zero then it's a delete operation
                : topic_data;             // otherwise it's an install operation


            uint8_t* h[10];
            h[topic[1]] = hookhash;

            uint8_t emit_buf[1024];
            uint32_t emit_size = 0;
            PREPARE_HOOKSET(emit_buf, sizeof(emit_buf), h, emit_size);

            uint8_t emithash[32];
            int64_t emit_result = emit(SBUF(emithash), emit_buf, emit_size);

            if (DEBUG)
                TRACEVAR(emit_result);

            if (emit_result != emit_size)
                NOPE("Governance: Emit failed during hook actioning.");

            DONE("Governance: Hook actioned.");
        }

        case 'S':
        {
            // add / change member
            uint8_t previous_member[32];
            int previous_present = (state(previous_member + 12, 20, topic[1], 1) == 20);

            if (previous_present && !topic_data_zero)
            {
                // we will not change member count, we're adding a member and removing a member
                
                // pass
            }
            else
            {
                // adjust member count
                if (previous_present)
                    member_count--;
                else
                    member_count++;
                
                ASSERT(member_count > 0);   // just bail out if the last member is trying to self remove

                ASSERT(state_set(&member_count, 1, SBUF(zero)) == 1);
            }

            // we need to garbage collect all their votes
            if (previous_present)
            {
                previous_member[0] = 'V';

                for (int i = 1; GUARD(32), i < 32; ++i)
                {
                    previous_member[1] = i < 2 ? 'R' : i < 12 ? 'H' : 'S';
                    previous_member[2] = i < 2 ? i : i < 12 ? i - 2 : i - 12;

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
                            }
                            else    // otherwise decrement
                            {
                                vote_count--;
                                ASSERT(state_set(&vote_count, 1, SBUF(vote_key)) == 1);
                            }
                        }

                        // delete the vote entry
                        ASSERT(state_set(0,0, SBUF(previous_member)) == 0);
                    }
                }
            }

            if (!topic_data_zero)
            {
                // add the new member
                // reverse key 
                ASSERT(state_set(topic_data, 20, topic[1], 1) == 20);
                
                // forward key
                ASSERT(state_set(topic[1], 1, SBUF(topic_data)) == 20);
            }

            DONE("Governance: Action member change.");
        }
    }

   rollback(SBUF("Governance: Internal logic error."), __LINE__); 
}
