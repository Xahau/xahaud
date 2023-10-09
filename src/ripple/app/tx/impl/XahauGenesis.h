namespace XahauGenesis
{
    // the wasm bytecode is loaded from this file as vec<u8> GovernanceHook and RewardHook
     #include <ripple/app/hook/xahau.h>

    using namespace ripple;

   
    constexpr XRPAmount GenesisAmount          {   1'000'000 * DROPS_PER_XRP };
    constexpr XRPAmount IntPropAmount          { 172'000'000 * DROPS_PER_XRP };
    constexpr XRPAmount InfraAmount            {  12'000'000 * DROPS_PER_XRP };
    constexpr XRPAmount ExchangeAmount         {  12'000'000 * DROPS_PER_XRP };
    constexpr XRPAmount MarketMakerAmount      {  28'000'000 * DROPS_PER_XRP };

    // Foundation amount is 600MM less everything else
    //    600'000'000
    //---------------
    //-     1'000'000   Genesis Acc Bal
    //-  6*12'000'000   Infra Amounts
    //-   172'000'000   Intellectual Property Amount
    //-    28'000'000   Market Maker Amount
    //-  3*12'000'000   Exchange Amounts
    //===============  
    //    291'000'000   

    constexpr XRPAmount FoundationAmount       { 291'000'000 * DROPS_PER_XRP };

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

    // These are initial distributions made to contributors to the network outside of
    // and beyond the Governance Game.
    inline
    std::vector<std::pair<std::string, XRPAmount>> const
    NonGovernanceDistribution
    {    
        {"nHB6Y311qufjwrpPbkiXc6SrEM2BbsqjJD2GE4xnhabRgYT3qMQb", FoundationAmount},  // Foundation
        {"rs2dgzYeqYqsk8bvkQR5YPyqsXYcA24MP2",                   ExchangeAmount},    // MEXC           
        {"rfKsmLP6sTfVGDvga6rW6XbmSFUzc3G9f3",                   ExchangeAmount},    // Bitrue
        {"rQrQMKhcw3WnptGeWiYSwX5Tz3otyJqPnq",                   ExchangeAmount},    // Uphold
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
        {"nHBVzNWFu2yT373tTto5HT1Tybdq2GE8hsdhHdowUDdj9wBieNFo", InfraAmount},      // L2 Table Projects
        {"nHUzXFEQwBkqHWp71E23fY9QKvEGQqVncJfm8fPxn4bRBYQ3Eee3", InfraAmount},      // L2 Table Community
        {"nHB1Sjz33Aizc4cdKWvKJDhh5BJAKe19PW7hsveZQq83RUcaxoXA", InfraAmount},      // L2 Table Auditors & Enterprise
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

        {"nHUzXFEQwBkqHWp71E23fY9QKvEGQqVncJfm8fPxn4bRBYQ3Eee3",    // L2 Table Community
            {
                "r42noEGvAqfwXnDBebeEPfYVswZVe6CKPo",               // Bithomp
                "r47tpT8LUoymNgRWzfUq2LdkPRfo4UZSkB",               // Reto
                "rscan6NzxxSFxEQST8qALrc5v9mpM8fU1j",               // XRPScan
                "rWiNRBZaEHFptxtkdohBbk36UxoHVwvEa",                // XRPLWin
                "rQQQrUdN1cLdNmxH4dHfKgmX5P4kf3ZrM",                // Tequ
                "rxah6E9kpp1Zw98MxYFMoWMQ1NHCZSmyx"}},              // xTokenize

        {"nHB1Sjz33Aizc4cdKWvKJDhh5BJAKe19PW7hsveZQq83RUcaxoXA",    // L2 Table Auditors & Enterprise
            {
                "r9EzMEYr5zuRasrfVKdry9JWLbw9mBe6Jg",               // Fyeo
                "rpuQonHVeMk1qEz9bWMhDRBDSjvLu2gU1B"}},             // Geveo
    };

    inline
    std::vector<std::pair<std::string, std::vector<std::string>>>
    TestL2Membership;
}
