// Generated using generate_lsflags.sh

#ifndef HOOKLSFLAGS_INCLUDED
#define HOOKLSFLAGS_INCLUDED 1

enum ltACCOUNT_ROOT {
    lsfPasswordSpent = 0x00010000,
    lsfRequireDestTag = 0x00020000,
    lsfRequireAuth = 0x00040000,
    lsfDisallowXRP = 0x00080000,
    lsfDisableMaster = 0x00100000,
    lsfNoFreeze = 0x00200000,
    lsfGlobalFreeze = 0x00400000,
    lsfDefaultRipple = 0x00800000,
    lsfDepositAuth = 0x01000000,
    lsfTshCollect = 0x02000000,
    lsfDisallowIncomingNFTokenOffer = 0x04000000,
    lsfDisallowIncomingCheck = 0x08000000,
    lsfDisallowIncomingPayChan = 0x10000000,
    lsfDisallowIncomingTrustline = 0x20000000,
    lsfURITokenIssuer = 0x40000000,
    lsfDisallowIncomingRemit = 0x80000000,
    lsfAllowTrustLineClawback = 0x00001000,
};
enum ltOFFER {
    lsfPassive = 0x00010000,
    lsfSell = 0x00020000,
};
enum ltRIPPLE_STATE {
    lsfLowReserve = 0x00010000,
    lsfHighReserve = 0x00020000,
    lsfLowAuth = 0x00040000,
    lsfHighAuth = 0x00080000,
    lsfLowNoRipple = 0x00100000,
    lsfHighNoRipple = 0x00200000,
    lsfLowFreeze = 0x00400000,
    lsfHighFreeze = 0x00800000,
    lsfLowDeepFreeze = 0x02000000,
    lsfHighDeepFreeze = 0x04000000,
    lsfAMMNode = 0x01000000,
};
enum ltSIGNER_LIST {
    lsfOneOwnerCount = 0x00010000,
};
enum ltDIR_NODE {
    lsfNFTokenBuyOffers = 0x00000001,
    lsfNFTokenSellOffers = 0x00000002,
    lsfEmittedDir = 0x00000004,
};
enum ltNFTOKEN_OFFER {
    lsfSellNFToken = 0x00000001,
};
enum ltURI_TOKEN {
    lsfBurnable = 0x00000001,
};
enum remarks {
    lsfImmutable = 1,
};
enum ltMPTOKEN_ISSUANCE {
    lsfMPTLocked = 0x00000001,
    lsfMPTCanLock = 0x00000002,
    lsfMPTRequireAuth = 0x00000004,
    lsfMPTCanEscrow = 0x00000008,
    lsfMPTCanTrade = 0x00000010,
    lsfMPTCanTransfer = 0x00000020,
    lsfMPTCanClawback = 0x00000040,
};
enum ltMPTOKEN {
    lsfMPTAuthorized = 0x00000002,
};
enum ltCREDENTIAL {
    lsfAccepted = 0x00010000,
};

#endif // HOOKLSFLAGS_INCLUDED
