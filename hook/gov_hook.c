#include "hookapi.h"
#define ASSERT(x)\
    if (!(x))\
        rollback(0,0,__LINE__);

#define INIT_MEMBER_COUNT 5
#define MAX_MEMBER_COUNT 20
#define INIT_AMOUNT 50000000000000ULL // 50MM

// test accounts only, must be replaced with real accounts prior to launch
uint8_t initial_members[] =
{
    // the first 12 bytes of each is padding to allow a reverse key
    // to be easily set for the account id

    // acc 0 - rJmkqLL4mqrc9kbP5Ztkq4TvVamYNetedp
    0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U, 0x00U, 0x00U,
    0xC2U,0xF1U,0x07U,0xE6U,0xE8U,0x64U,0xD3U,0x90U,0x6DU,0x0AU,
    0x08U,0x84U,0x46U,0xFDU,0xDFU,0x8AU,0x7BU,0x2FU,0x56U,0x9CU,

    // acc 1 - rEVGdPT6ACPPcXGNwNeeYYtoRTKFaqhEi4
    0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U, 0x00U, 0x00U,
    0x9EU,0xEAU,0x73U,0xF5U,0xF0U,0x62U,0x7EU,0x69U,0x39U,0x7EU,
    0xC7U,0x2EU,0x9AU,0x3CU,0x78U,0x04U,0xC0U,0xF2U,0xBFU,0x69U,

    // acc 2 - rJi1kZsQthVqVPLmQdTmG8AtiAKk26J9ST
    0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U, 0x00U, 0x00U,
    0xC3U,0xE8U,0xE2U,0x9AU,0xB6U,0x28U,0x47U,0x27U,0x5CU,0xEDU,
    0x36U,0xEBU,0xF4U,0xE9U,0x28U,0xDCU,0x25U,0xA0U,0x7FU,0x24U,

    // acc 3 - rHm34nx2QSJKXhe32NiNbsnrUSeZqDZMkg
    0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U, 0x00U, 0x00U,
    0xB7U,0xDAU,0x76U,0x2DU,0xB9U,0x90U,0x2EU,0x85U,0x19U,0x96U,
    0x66U,0xB2U,0xE6U,0xC3U,0x00U,0x9CU,0x5EU,0x27U,0x57U,0x69U,

    // acc 4 - rLc3URyuFpTqBAcPFK5J7c73h488ew79dH
    0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U,0x00U, 0x00U, 0x00U,
    0xD7U,0x0EU,0xF4U,0xD5U,0x02U,0x1CU,0x7CU,0x64U,0x6AU,0x98U,
    0xE8U,0x4FU,0x60U,0xFEU,0xD3U,0x64U,0xA0U,0x04U,0x45U,0x32U,
};

// this is the xfl for 0.00333333333
uint8_t initial_reward[] =    // on key FF, 0..0 
{
    0x53U,0xCBU,0xD7U,0xA6U,0x25U,0x0DU,0x78U,0x80U
};

uint8_t key[32];
uint8_t account_field[32];
uint8_t zero[32];

#define DONE()\
{\
    accept(0,0,(uint32_t)__LINE__);\
}

#define TRACELINE()\
{\
    uint8_t out[4];\
    out[0] = (__LINE__ >> 24U) & 0xFFU;\
    out[1] = (__LINE__ >> 16U) & 0xFFU;\
    out[2] = (__LINE__ >>  8U) & 0xFFU;\
    out[3] = (__LINE__ >>  0U) & 0xFFU;\
    trace(SBUF("Line upto:"), out, 4, 1);\
}

int64_t hook(uint32_t r)
{
    _g(1,1);

    uint8_t ttbuf[2];
    int64_t br = otxn_field(SBUF(ttbuf), sfTransactionType);
    uint32_t txntype = ((uint32_t)(ttbuf[0]) << 16U) + ((uint32_t)(ttbuf[1]));

    if (txntype != 99)  // ttINVOKE
        DONE();

    // get the account id
    ASSERT(otxn_field(account_field + 12, 20, sfAccount) == 20);

    uint8_t hook_accid[20];
    hook_account(SBUF(hook_accid));

    // start of hook proper


    int64_t member_count = state(0,0, SBUF(key));

    TRACEVAR(member_count);
    
    // initial execution, setup hook
    if (member_count == DOESNT_EXIST)
    {
        etxn_reserve(INIT_MEMBER_COUNT);

        // set member count
        int8_t imc = INIT_MEMBER_COUNT;
        ASSERT(state_set(&imc, 1, SBUF(key)));                  // member count is on the zero key

        // set reward rate
        key[0] = 0xFFU;
        ASSERT(state_set(SBUF(initial_reward), SBUF(key)));      // interest rate is on the the FF, 0...0 key
        key[0] = 0;

        for (uint32_t i = 0; GUARD(INIT_MEMBER_COUNT), i < INIT_MEMBER_COUNT; ++i)
        {
            key[31] = i+1;

                                                                // 0... X where X is member id started from 1
                                                                // maps to the member's account ID
            // reverse key 
            ASSERT(state_set(initial_members + (i * 32) + 12, 20, SBUF(key)) == 20);
            
                                                                // 0, 0... ACCOUNT ID maps to member_id (as above)
            // forward key
            ASSERT(state_set(key + 31, 1, initial_members + (i * 32), 32) == 1);

            // emit initial
            uint8_t tx[PREPARE_PAYMENT_SIMPLE_SIZE];
            PREPARE_PAYMENT_SIMPLE(tx, INIT_AMOUNT, (initial_members + (i * 32) + 12), 0, 0);

            // emit the transaction
            uint8_t emithash[32];
            int64_t emit_result = emit(SBUF(emithash), SBUF(tx));
            ASSERT(emit_result > 0);
            TRACEVAR(emit_result);

        }

        DONE();
    }

    

    // outgoing txns allowed
    if (BUFFER_EQUAL_20(hook_accid, account_field + 12))
        DONE();


    

    // otherwise a normal execution (not initial)
    // first let's check if the invoking party is a member
    
    int64_t member_id = state(0,0,SBUF(account_field));
    ASSERT(member_id >= 0);

    // the only thing you can do is vote for a topic
    // so lets process their vote
    ASSERT(otxn_slot(1) == 1);
    ASSERT(slot_subfield(1, sfHookParameters, 2) == 2);
    // first parameter must contain the topic as key and the topic data as value
    ASSERT(slot_subarray(2, 0, 2) == 2);
    ASSERT(slot_subfield(2, sfHookParameterName, 3) == 3);
    ASSERT(slot_subfield(2, sfHookParameterValue, 4) == 4);

    
    uint8_t dump[1024];
    uint64_t dumpsize = slot(SBUF(dump), 4);
    trace(SBUF("dump"), dump, dumpsize, 1);

    int64_t topic = slot(0,0, 3) & 0xFFU; // there's a high "size" byte because it's a VL
    TRACEVAR(topic);
    ASSERT(topic >= 1 && topic <= 25);

    
    uint8_t topic_data_buffer[44];  // this gives us some bytes on the front to play with to avoid buffer copies
    uint8_t* topic_data = topic_data_buffer + 12;;
    uint8_t topic_size = 
        topic == 1 ? 8 :
        topic >= 2 && topic <= 5 ? 32 : 20;
    
    // reuse account_field to record vote
    account_field[0] = topic;

    // read canidate from ttINVOKE parameter 
    ASSERT(slot(topic_data - 1, topic_size + 1, 4) == topic_size + 1);
    *(topic_data - 1) = 0; // this is the size byte for the VL

    
    // get previous vote if any on this topic (1-25)
    uint8_t previous_topic_data[32];
    int64_t previous_topic_size = state(previous_topic_data, topic_size, SBUF(account_field));

    // write vote to voting key
    ASSERT(state_set(topic_data, topic_size, SBUF(account_field)) == topic_size);

    // check if the vote they're making has already been cast before,
    // if it is identical to their existing vote for this topic then just end with tesSUCCESS
    if (previous_topic_size == topic_size && BUFFER_EQUAL_32(previous_topic_data, topic_data))
        DONE();

    
    // execution to here means the vote is different
    // we might have to decrement the old voting if they voted previously
    // and we will have to increment the new voting


    // decrement old counter
    if (previous_topic_size > 0)
    {
        uint8_t votes = 0;
        previous_topic_data[31] = topic;
        if (state(&votes, 1, SBUF(previous_topic_data)) && votes > 0)
        {
            votes--;
            ASSERT(state_set(&votes, 1, SBUF(previous_topic_data)));
        }
    }


    
    // increment new counter
    uint8_t votes = 0;
    uint8_t last_byte = *(topic_data + 31);
    *(topic_data + 31) = topic;
    if (state(&votes, 1, SBUF(previous_topic_data)) && votes > 0)
    {
        votes++;
        ASSERT(state_set(&votes, 1, topic_data, 32));
    }
    *(topic_data + 31) = last_byte;

    
    // set this flag if the topic data is all zeros, it's important in some cases
    int topic_data_zero = BUFFER_EQUAL_32(topic_data, zero); 

    
    // now check if we hit threshold
    if (votes == member_count || // 100% required for topics 1 - 5 (interest rate, hooks0-3)
        (topic > 5 && votes >= (member_count * 0.8))) // 80% required for membership voting
    {
        // 100%/80% threshold as needed is reached
        // action vote

        if (topic == 1)
        {
            // change reward %
            key[0] = 0xFFU;
            ASSERT(state_set(topic_data, 8, SBUF(key)));      // interest rate is on the the FF, 0...0 key
        }
        else if (topic <= 5)
        {
            // set hook hash
            uint8_t pos = topic - 2;
        
            // first get the hook ledget object
            uint8_t keylet[34];
            ASSERT(util_keylet(SBUF(keylet), KEYLET_HOOK, SBUF(hook_accid), 0,0,0,0) == 34);
            ASSERT(slot_set(SBUF(keylet), 5) == 5);

            // now get the hooks array
            ASSERT(slot_subfield(5, sfHooks, 6) == 6);

            // now check the entry
            if (slot_subarray(6, pos, 7) == 7)
            {
                // it exists
                // check if its identical
                uint8_t existing_hook[32];
                ASSERT(slot_subfield(7, sfHookHash, 8) == 8);
                ASSERT(slot(SBUF(existing_hook), 8) == 32);

                // if it is then do nothing
                if (BUFFER_EQUAL_32(existing_hook, topic_data))
                    DONE();
            }

            // generate the hook definition keylet
            ASSERT(util_keylet(SBUF(keylet), KEYLET_HOOK_DEFINITION, topic_data, 32, 0,0,0,0) == 34);

            // check if the ledger contains such a hook definition
            ASSERT(slot_set(SBUF(keylet), 9) == 9);

            // it does so now we can do the emit
            etxn_reserve(1);

            // RH UPTO: do hookset emit

            uint8_t* hookhash = 
                topic_data_zero 
                ? ((uint8_t*)0xFFFFFFFFU) // if the topic data is all zero then it's a delete operation
                : topic_data;             // otherwise it's an install operation

            uint8_t* h[4] =
            {
                pos == 0 ? hookhash : 0,
                pos == 1 ? hookhash : 0,
                pos == 2 ? hookhash : 0,
                pos == 3 ? hookhash : 0
            };

            uint8_t emit_buf[1024];
            uint32_t emit_size = 0;
            PREPARE_HOOKSET(emit_buf, sizeof(emit_buf), h[0], h[1], h[2], h[3], emit_size);

            uint8_t emithash[32];
            int64_t emit_result = emit(SBUF(emithash), emit_buf, emit_size);
            TRACEVAR(emit_result);

        }
        else
        {
            // add / change member
            key[31] = topic - 6;


            uint8_t previous_member[32];
            int previous_present = (state(previous_member + 12, 20, SBUF(key)) == 20);

            if (previous_present && !topic_data_zero)
            {
                // we will not change member count, we're adding a member and removing a member
            }
            else
            {
                // decrement member count
                ASSERT(member_count > 0);   // just bail out if the last member is trying to self remove
                
                if (previous_present)
                    member_count--;
                else
                    member_count++;

                ASSERT(state_set(&member_count, 1, SBUF(zero)) == 1);
            }

            // we need to garbage collect all their votes
            if (previous_present)
            {
                for (int i = 1; GUARD(25), i <= 25; ++i)
                {
                    previous_member[0] = i;
                    uint8_t vote_key[32];
                    if (state(SBUF(vote_key), SBUF(previous_member)) == 32)
                    {
                        uint8_t vote_count = 0;
                        if (state(&vote_count, 1, SBUF(vote_key)) == 1)
                        {
                            if (vote_count <= 1)
                            {
                                ASSERT(state_set(0,0, SBUF(vote_key)) == 0);
                            }
                            else
                            {
                                vote_count--;
                                ASSERT(state_set(&vote_count, 1, SBUF(vote_key)) == 1);
                            }
                        }

                        // delete the entry
                        ASSERT(state_set(0,0, SBUF(previous_member)) == 0);
                    }
                }
            }

            if (!topic_data_zero)
            {
                // add the new member
                // reverse key 
                key[31] = topic - 6;
                ASSERT(state_set(topic_data, 20, SBUF(key)) == 20);
                
                // forward key
                ASSERT(state_set(key + 31, 1, topic_data_buffer, 32) == 20);
            }

            // done!
        }
    }
    
    DONE();
}
