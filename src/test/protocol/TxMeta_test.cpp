//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/TxMeta.h>
#include <xrpl/protocol/digest.h>

#include <cstring>

namespace ripple {
namespace test {
namespace {

uint256
makeHash(char const* label)
{
    return sha512Half(Slice(label, std::strlen(label)));
}

STObject
makeExportResult()
{
    STObject exportResult(sfExportResult);
    exportResult.setFieldU32(sfLedgerSequence, 4321);
    exportResult.setFieldH256(sfLedgerHash, makeHash("export-ledger"));
    exportResult.setFieldH256(sfTransactionHash, makeHash("exported-tx"));
    exportResult.setFieldH256(
        sfExportSignatureHash, makeHash("export-signature-witness"));
    return exportResult;
}

void
expectExportResult(beast::unit_test::suite& suite, STObject const& obj)
{
    suite.expect(obj.isFieldPresent(sfExportResult), __FILE__, __LINE__);
    auto const& exportResult =
        obj.peekAtField(sfExportResult).downcast<STObject>();
    suite.expect(
        exportResult.getFieldU32(sfLedgerSequence) == 4321, __FILE__, __LINE__);
    suite.expect(
        exportResult.getFieldH256(sfLedgerHash) == makeHash("export-ledger"),
        __FILE__,
        __LINE__);
    suite.expect(
        exportResult.getFieldH256(sfTransactionHash) == makeHash("exported-tx"),
        __FILE__,
        __LINE__);
    suite.expect(
        exportResult.getFieldH256(sfExportSignatureHash) ==
            makeHash("export-signature-witness"),
        __FILE__,
        __LINE__);
}

}  // namespace

class TxMeta_test : public beast::unit_test::suite
{
public:
    void
    testExportResultRoundTrip()
    {
        testcase("export result round-trip");

        auto const txID = makeHash("meta-tx");
        TxMeta meta{txID, 12};
        meta.setResult(tesSUCCESS, 3);
        BEAST_EXPECT(!meta.hasExportResult());

        meta.setExportResult(makeExportResult());
        BEAST_EXPECT(meta.hasExportResult());

        auto const obj = meta.getAsObject();
        expectExportResult(*this, obj);

        TxMeta fromObject{txID, 12, obj};
        BEAST_EXPECT(fromObject.hasExportResult());
        expectExportResult(*this, fromObject.getAsObject());

        Serializer s;
        obj.add(s);
        Blob const blob{s.begin(), s.end()};

        TxMeta fromBlob{txID, 12, blob};
        BEAST_EXPECT(fromBlob.hasExportResult());
        expectExportResult(*this, fromBlob.getAsObject());

        std::string const serialized{blob.begin(), blob.end()};
        TxMeta fromString{txID, 12, serialized};
        BEAST_EXPECT(fromString.hasExportResult());
        expectExportResult(*this, fromString.getAsObject());
    }

    void
    run() override
    {
        testExportResultRoundTrip();
    }
};

BEAST_DEFINE_TESTSUITE(TxMeta, protocol, ripple);

}  // namespace test
}  // namespace ripple
