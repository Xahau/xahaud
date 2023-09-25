namespace XahauGenesis
{
    // the wasm bytecode is loaded from this file as vec<u8> GovernanceHook and RewardHook
     #include <ripple/app/hook/xahau.h>

    using namespace ripple;

    constexpr XRPAmount GenesisAmount          {  1'000'000 * DROPS_PER_XRP };
    constexpr XRPAmount InfraAmount            { 10'000'000 * DROPS_PER_XRP };
    constexpr XRPAmount ExchangeAmount         {  2'000'000 * DROPS_PER_XRP };

    inline
    std::vector<std::pair<std::string, XRPAmount>> const
    Distribution
    {
        {"rMYm3TY5D3rXYVAz6Zr2PDqEcjsTYbNiAT", InfraAmount},
        {"nHUG6WyZX5C6YPhxDEUiFdvRsWvbxdXuUcMkMxuqS9C3akrJtJQA", InfraAmount},
        {"nHDDs26hxCgh74A6QE31CR5QoC17yXdJQXNDXezp8HW93mCYGPG7", InfraAmount},
        {"nHUNFRAhqbfqBHYxfiAtJDxruSgbsEBUHR6v55MhdUtzTNyXLcR4", InfraAmount},
        {"nHB4MVtevJBZF4vfdLTecKBxj5KsxERkfk7UNyL9iYtTZvjmMBXw", InfraAmount},
        {"nHUB9Fh1JXvyMY4NhiCKgg6pkGrB3FoBTAz4dpvKC1fwCMjY1w5N", InfraAmount},
        {"nHUdqajJr8S1ecKwqVkX4gQNUzQP9RTonZeEZH8vwg7664CZP3QF", InfraAmount},
        {"nHDnr7GgwZWS7Qb517e5is3pxwVxsNgmmpmQYvrc1ngbPiURBa6B", InfraAmount},
        {"nHBv6AqLDgWgEVLoNE7jEViv4XG17jj6tpuzTFm664Cc4mcpEgwb", InfraAmount},
        {"nHUxeL9jgcjhTWepmFnbWpmobZmFBduLkceQddCJnAPghJejdRix", InfraAmount},
        {"nHUubQ7fqxkwPtwS4pQb2ENZ6fdUcAt7aJRiYcPXjxbbkC778Zjk", InfraAmount},
    };

    // this data structure is used for testing the amendment only,
    // if the ttEnableAmendment has an optional flag (that cannot be added in production)
    // then whatever is in this array is used
    inline
    std::vector<std::pair<std::string, XRPAmount>> TestDistribution;


    // For the Governance Hook: HookOn is set to ttINVOKE only    
    inline
    ripple::uint256 const
    GovernanceHookOn("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF7FFFFFFFFFFFFFFFFFFBFFFFF");

    inline
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> const
    GovernanceParameters
    {
            // 1.003274 -xfl-> 6089869970204910592 -le-> 0x00E461EE78908354
            {{'I', 'R', 'R'}, {0x00U, 0xE4U, 0x61U, 0xEEU, 0x78U, 0x90U, 0x83U, 0x54U}},

            // 2600000 -xfl-> 6199553087261802496 -le-> 0x00806AACAF3C0956
            {{'I', 'R', 'D'}, {0x00U, 0x80U, 0x6AU, 0xACU, 0xAFU, 0x3CU, 0x09U, 0x56U}}

    };


    inline
    std::vector<std::pair<std::string, std::vector<std::string>>>
    TestL2Membership;

    inline
    std::vector<std::pair<
        std::string,                // L2 table account
        std::vector<std::string>>>  // list of initial members
    const
    L2Membership
    {
        {"nHUubQ7fqxkwPtwS4pQb2ENZ6fdUcAt7aJRiYcPXjxbbkC778Zjk",
            {"rG9QZQDR8XqCU1x2VARPuZwYxqcb3J9bYa", "rPV6jx8QoQBCR1AcmVLDnu98QBu4QakwhU"}}
    };

    // For the Reward Hook: HookOn is set to ttCLAIM_REWARD only
    inline
    ripple::uint256 const
    RewardHookOn("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFBFFFFFFFFFFFFFFFFFFBFFFFF");
    
}
