#ifndef RIPPLE_HOOK_XPORTWRAPPERBUILDER_H_INCLUDED
#define RIPPLE_HOOK_XPORTWRAPPERBUILDER_H_INCLUDED

#include <xrpl/basics/Expected.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/hook/Enum.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/STTx.h>

#include <cstdint>
#include <functional>

namespace hook {
namespace XportWrapperBuilder {

using HookReturnCode = ::hook_api::hook_return_code;
using FeeCalculator =
    std::function<ripple::Expected<std::uint64_t, HookReturnCode>(
        ripple::Slice)>;
using NonceGenerator =
    std::function<ripple::Expected<ripple::uint256, HookReturnCode>()>;

struct Input
{
    ripple::Slice innerTxBlob;
    ripple::AccountID exporter;
    std::uint32_t networkID = 0;
    ripple::LedgerIndex ledgerSeq = 0;
    ripple::uint256 parentTxnID;
    ripple::uint256 hookHash;
    bool hasCallback = false;
    std::uint32_t emitGeneration = 0;
    std::uint64_t emitBurden = 1;
    NonceGenerator generateNonce;
    FeeCalculator calculateFee;
    beast::Journal j;
};

struct Result
{
    ripple::STTx wrapperTx;
};

ripple::Expected<Result, HookReturnCode>
build(Input const& input);

}  // namespace XportWrapperBuilder
}  // namespace hook

#endif
