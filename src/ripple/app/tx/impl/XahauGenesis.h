namespace XahauGenesis
{
    // the wasm bytecode is loaded from this file as vec<u8> GovernanceHook and RewardHook
     #include <ripple/app/hook/xahau.h>

    using namespace ripple;

    constexpr XRPAmount GenesisAmount          {    940'000 * DROPS_PER_XRP };
    constexpr XRPAmount InfraAmount            {     10'000 * DROPS_PER_XRP };
    constexpr XRPAmount ExchangeAmount         {      2'000 * DROPS_PER_XRP };

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

    inline
    std::vector<std::pair<std::string, XRPAmount>> const
    NonGovernanceDistribution
    {    
        {"nHUJbfgaAtkbuAdtrqCDerP99PcprcpyqakZTsYpkQdb67aKKyJn", ExchangeAmount},
    };

    inline
    std::vector<std::pair<std::string, XRPAmount>>
    TestNonGovernanceDistribution;

    inline
    std::vector<std::pair<std::string, XRPAmount>> const
    L1Membership
    {
        {"nHDs6fHVnhb4ZbSFWF2dTTPHoZ6Rr39i2UfLotzgf8FKUi7iZdxx", InfraAmount},  // tn4
        {"nHUvgFxT8EGrXqQZ1rqMw67UtduefbaCHiVtVahk9RXDJP1g1mB4", InfraAmount},  // tn5
        {"nHU7Vn6co7xEFMBesV7qw7FXE8ucKrhVWQiYZB5oGyMhvqrnZrnJ", InfraAmount},  // tn6
        {"nHBoJCE3wPgkTcrNPMHyTJFQ2t77EyCAqcBRspFCpL6JhwCm94VZ", InfraAmount},  // tn7
        {"nHUVv4g47bFMySAZFUKVaXUYEmfiUExSoY4FzwXULNwJRzju4XnQ", InfraAmount},  // tn8
        {"nHBvr8avSFTz4TFxZvvi4rEJZZtyqE3J6KAAcVWVtifsE7edPM7q", InfraAmount},  // tn9
        {"nHUH3Z8TRU57zetHbEPr1ynyrJhxQCwrJvNjr4j1SMjYADyW1WWe", InfraAmount},  //tn10
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
        {"nHU7Vn6co7xEFMBesV7qw7FXE8ucKrhVWQiYZB5oGyMhvqrnZrnJ",
            {
                "rN4HyiKpUuQM68pohiMPinQnyPdY8tBcWa",
                "rnsVxwrctvfiPX6wtX5tjYLPMidXFTqWc8"}},

        {"nHBoJCE3wPgkTcrNPMHyTJFQ2t77EyCAqcBRspFCpL6JhwCm94VZ",
            {
                "rLjrBkBUSRtZJnFfZAbX4SFQSpkk2HcpX8",
                "rKUt5c9zqGCsZTVhUqrTUNnAbHHo19vtoM",
                "rJY9NAbesWDGupunxyTvvtL3yWUDrbuCRF"}},

        {"nHUVv4g47bFMySAZFUKVaXUYEmfiUExSoY4FzwXULNwJRzju4XnQ",
            {
                "r38XdJQ2TKdLRAENjLoft8eDvFsUee5wbr",
                "rnVtyAEp4TGyje7ccS1SjWHVwxqqQBeft3",
                "rpzQniG7qsVi6qaS5X2QuscfpWY31j5bks",
                "rsb7Y9qE7uvftjHZPW1qBVbLdCxjGe5G8X",
                "rJeoxs1fZW78sMeamwJ27CVcXZNpQZR3t"}},

        {"nHBvr8avSFTz4TFxZvvi4rEJZZtyqE3J6KAAcVWVtifsE7edPM7q",
            {
                "rh8svn1EFs3TKEYVBZfWpNdqNr2R1uyM7y",
                "rMn7PRAiT22hytTpLBHHZ34jPe2RCi2svT",
                "rLSCctV2Q5rsduFuZk7N65mbSrf3BFknPc",
                "rn8b9tjZbKmPSoMKV8QwgcU8wq2DVCdfQN",
                "rEAeU9EDmdcJ3xPLW714bm9fygDV8XoNrj",
                "rpCLrZYhfaN7ysBNpfAZuNj49Fyg4SHFGv",
                "rafa8E9RPa5vxZ4rN8s571aWUdbE4oqv7i",
                "r37Qu8nTfdJFkE14ERAB3LH3wSdz1LbyzU",
                "rnqXJXh1mGf9BGt3aB74RscNsJiDMV1YPK",
                "rLhHTgwBbq7aVsrSPp2CDeyX3RRuXihGVv",
                "rJt6kBV8EjhN9v7U4KnYtydfFu3DaQVCvF",
                "r4YGLYBzvWTZGLKwbByfVxc8ZjCgBUUwBn",
                "rEw7zrMdCXFs3CzxtC3NFpjmM2HBLTigVL",
                "rwrqQBN88MeT3QDipCfJSeJ9sZMZA54xkD",
                "rpmAcuJAWVgS1zL3R1ob8F5ZSJ9d4jEAoj",
                "rwGMc2FXtvPitSppNwJaSxqSfEfrLVRtMm",
                "rUrAvfQTv16EETc3Q2sgCTAoKS9C49crx2",
                "rBDsW6p9Xak9b2ye2eAgh9JjpubTzeV1ti",
                "rhGbC5n1qK3Cq3sBbdtKGV5AR3kboXi41K",
                "rNu4sdVz6d2H37okUeH47ASMwckhzSx3k5"}},
    };

    inline
    std::vector<std::pair<std::string, std::vector<std::string>>>
    TestL2Membership;
}
