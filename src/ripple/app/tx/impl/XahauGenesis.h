namespace XahauGenesis
{
    // the wasm bytecode is loaded from this file as vec<u8> GovernanceHook and RewardHook
     #include <ripple/app/hook/xahau.h>

    using namespace ripple;

   
    constexpr XRPAmount IntPropAmount          { 172'000'000 * DROPS_PER_XRP };
    constexpr XRPAmount MarketMakerAmount      {  28'000'000 * DROPS_PER_XRP };
    constexpr XRPAmount InfraAmount            {  12'000'000 * DROPS_PER_XRP };
    constexpr XRPAmount DevTableAmount         {   8'600'000 * DROPS_PER_XRP };
    constexpr XRPAmount ProjectAmount          {   6'000'000 * DROPS_PER_XRP };
    constexpr XRPAmount ExchangeAmount         {   4'000'000 * DROPS_PER_XRP };
    constexpr XRPAmount AuditorAmount          {   3'000'000 * DROPS_PER_XRP };
    constexpr XRPAmount GenesisAmount          {   1'000'000 * DROPS_PER_XRP };
    constexpr XRPAmount DevAmount              {     500'000 * DROPS_PER_XRP };
    constexpr XRPAmount TableAmount            {     100'000 * DROPS_PER_XRP };

    // Foundation amount is 600MM less everything else
    //    600'000'000
    //---------------
    //-     1'000'000   Genesis Acc Bal
    //-  5*12'000'000   Infra Amounts
    //-  2* 6'000'000   Project Amounts
    //-  3* 4'000'000   Exhange Amounts
    //-  4* 3'000'000   Auditor Amounts
    //-  7*   500'000   Dev Amounts
    //-  1* 8'600'000   Dev Table
    //-  3*   100'000   L2 Table Amounts
    //-   172'000'000   Intellectual Property Amount
    //-    28'000'000   Market Maker Amount
    //===============  
    //    290'600'000   

    constexpr XRPAmount FoundationAmount       { 290'600'000 * DROPS_PER_XRP };

    // For the Governance Hook: HookOn is set to ttINVOKE only    
    inline
    ripple::uint256 const
    GovernanceHookOn("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF7FFFFFFFFFFFFFFFFFFBFFFFF");

    // For the Reward Hook: HookOn is set to ttCLAIM_REWARD only
    inline
    ripple::uint256 const
    RewardHookOn("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFBFFFFFFFFFFFFFFFFFFBFFFFF");

    inline
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> const
    GovernanceParameters
    {
            // 0.003333 -xfl-> 6038156834009797973 -le-> 0x55554025A6D7CB53
            {{'I', 'R', 'R'}, {0x55U, 0x55U, 0x40U, 0x25U, 0xA6U, 0xD7U, 0xCBU, 0x53U}},

            // 2600000 -xfl-> 6199553087261802496 -le-> 0x00806AACAF3C0956
            {{'I', 'R', 'D'}, {0x00U, 0x80U, 0x6AU, 0xACU, 0xAFU, 0x3CU, 0x09U, 0x56U}}
    };

    // Despite the name this category is now used for both non-governance participants AND l2 seats
    // specific distributions
    inline
    std::vector<std::pair<std::string, XRPAmount>> const
    NonGovernanceDistribution
    {    
        // XRPLF
        {"nHB6Y311qufjwrpPbkiXc6SrEM2BbsqjJD2GE4xnhabRgYT3qMQb", FoundationAmount},  // Foundation

        // L2 Project seats
        {"rHMtqVuvEESUhPrsgb8tSa5ghjyoQySfVC",                   ProjectAmount},     // xSpectar
        {"rJFxdrd1BuMeJshRAZBuHP3hex9DjH1nnr",                   ProjectAmount},     // CSC

        // L2 Exchange seats
        {"rfKsmLP6sTfVGDvga6rW6XbmSFUzc3G9f3",                   InfraAmount},       // Bitrue
        {"rB3egB3cm51DpFENKH2CameyQJf2fmvN72",                   ExchangeAmount},    // Coinstore
        {"rGshbE2xPc2Jw66iTAXuX5RjXmJW4ohbrY",                   ExchangeAmount},    // Unannounced Exchange 1
        {"r4vv7gFjtWAUxPWfj5puGNeW9U8FGSn7iu",                   ExchangeAmount},    // Unannounced Exchange 2

        // L2 Auditor seats
        {"r9EzMEYr5zuRasrfVKdry9JWLbw9mBe6Jg",                   InfraAmount},       // FYEO
        {"rpuQonHVeMk1qEz9bWMhDRBDSjvLu2gU1B",                   AuditorAmount},     // Geveo
        {"rHrptekd18qAGCADzK1kg2QyREiRPuVpTJ",                   AuditorAmount},     // Danish Blockchain Labs
        {"rpZuvdsDzCLxii1ag9TAyf11Wc43qg4QAG",                   AuditorAmount},     // Quantstamp
        {"rwcL4h6ix5VrxjE6GXNq2svJjnj6H3ZJjv",                   AuditorAmount},     // Ottersec

        // L2 Dev Seats
        {"r42noEGvAqfwXnDBebeEPfYVswZVe6CKPo",                   DevAmount},         // Viacheslav
        {"r47tpT8LUoymNgRWzfUq2LdkPRfo4UZSkB",                   DevAmount},         // Reto
        {"rscan6NzxxSFxEQST8qALrc5v9mpM8fU1j",                   DevAmount},         // Anurag
        {"rWiNRBZaEHFptxtkdohBbk36UxoHVwvEa",                    DevAmount},         // Zvjezdan
        {"rtequsfcSxEerbj18TdS6hUd89vTbaWec",                    DevAmount},         // Tequ
        {"rxah6E9kpp1Zw98MxYFMoWMQ1NHCZSmyx",                    DevAmount},         // Michael
        {"riLD4RiZcmFLijuBkBr1qLa5tXbojgNSN",                    DevAmount},         // Ildar
    };

    inline
    std::vector<std::pair<std::string, XRPAmount>>
    TestNonGovernanceDistribution;

    inline
    std::vector<std::pair<std::string, XRPAmount>> const
    L1Membership
    {
        {"nHB45nBNgjKMssrRqaNVr2tpCq3t55J5APRRDD6ov1U41JfVFjr6", IntPropAmount},    // XRPL-Labs
        {"nHB2tHvDXE2GM3Cp9ivyAXU3NDLkf8mzYREQkcZ7wFJyBiaVLu24", InfraAmount},      // Titanium
        {"nHUetpkzTgf8c6A1DTGemRrhL4nMjsiJrtcsmmg75ufzV5EDMbWU", InfraAmount},      // Evernode
        {"nHBcpUteNtfXeEquLuXVSAAPKxPigmkLAvmpnPjiiPQ4dWZvkoxi", InfraAmount},      // Digital Governance
        {"nHB1BVMv96GUa272VehmoCeGM61uLRHZVbxAG1i9kQ9Xzd1KVrh1", MarketMakerAmount},// GateHub

        {"nHBVzNWFu2yT373tTto5HT1Tybdq2GE8hsdhHdowUDdj9wBieNFo", TableAmount},      // L2 Table Projects
        {"nHUzXFEQwBkqHWp71E23fY9QKvEGQqVncJfm8fPxn4bRBYQ3Eee3", DevTableAmount},   // L2 Table Community
        {"nHB1Sjz33Aizc4cdKWvKJDhh5BJAKe19PW7hsveZQq83RUcaxoXA", TableAmount},      // L2 Table Auditors
        {"nHD9nhqSzzUFEmu8DuRpKrgF1rQzBkjVpMmYhtjmddhGoDLAF63i", TableAmount},      // L2 Table Exchanges
    };

    // this data structure is used for testing the amendment only,
    // if the ttEnableAmendment has an optional flag (that cannot be added in production)
    // then whatever is in this array is used
    inline
    std::vector<std::pair<std::string, XRPAmount>> TestL1Membership;

    inline
    std::vector<std::pair<
        std::string,                // L2 table account
        std::vector<std::string>>>  // list of initial members
    const
    L2Membership
    {
        {"nHBVzNWFu2yT373tTto5HT1Tybdq2GE8hsdhHdowUDdj9wBieNFo",    // L2 Table Projects
            {
                "rJFxdrd1BuMeJshRAZBuHP3hex9DjH1nnr",               // Casino Coin
                "rHMtqVuvEESUhPrsgb8tSa5ghjyoQySfVC"}},             // xSpectar

        {"nHD9nhqSzzUFEmu8DuRpKrgF1rQzBkjVpMmYhtjmddhGoDLAF63i",    // L2 Table Exchanges
            {
                "rfKsmLP6sTfVGDvga6rW6XbmSFUzc3G9f3",               // Bitrue
                "rB3egB3cm51DpFENKH2CameyQJf2fmvN72",               // Coinstore
                "rGshbE2xPc2Jw66iTAXuX5RjXmJW4ohbrY",               // Unannounced Exchange 1
                "r4vv7gFjtWAUxPWfj5puGNeW9U8FGSn7iu"}},             // Unannounced Exchange 2
        
        {"nHB1Sjz33Aizc4cdKWvKJDhh5BJAKe19PW7hsveZQq83RUcaxoXA",    // L2 Table Auditors & Enterprise
            {
                "r9EzMEYr5zuRasrfVKdry9JWLbw9mBe6Jg",               // Fyeo
                "rpuQonHVeMk1qEz9bWMhDRBDSjvLu2gU1B",               // Geveo
                "rHrptekd18qAGCADzK1kg2QyREiRPuVpTJ",               // Danish Blockchain Labs
                "rpZuvdsDzCLxii1ag9TAyf11Wc43qg4QAG",               // Quantstamp
                "rwcL4h6ix5VrxjE6GXNq2svJjnj6H3ZJjv"}},             // Ottersec

        {"nHUzXFEQwBkqHWp71E23fY9QKvEGQqVncJfm8fPxn4bRBYQ3Eee3",    // L2 Table Community
            {
                "r42noEGvAqfwXnDBebeEPfYVswZVe6CKPo",               // Viacheslav
                "r47tpT8LUoymNgRWzfUq2LdkPRfo4UZSkB",               // Reto
                "rscan6NzxxSFxEQST8qALrc5v9mpM8fU1j",               // Anurag
                "rWiNRBZaEHFptxtkdohBbk36UxoHVwvEa",                // Zvjezdan
                "rtequsfcSxEerbj18TdS6hUd89vTbaWec",                // Tequ
                "rxah6E9kpp1Zw98MxYFMoWMQ1NHCZSmyx",                // Michael
                "riLD4RiZcmFLijuBkBr1qLa5tXbojgNSN"}},              // Ildar
    };

    inline
    std::vector<std::pair<std::string, std::vector<std::string>>>
    TestL2Membership;
}
