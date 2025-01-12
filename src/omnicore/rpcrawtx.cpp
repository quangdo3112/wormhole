#include "omnicore/rpcrawtx.h"

#include "omnicore/createtx.h"
#include "omnicore/omnicore.h"
#include "omnicore/rpc.h"
#include "omnicore/errors.h"
#include "omnicore/tx.h"
#include "omnicore/utilsbitcoin.h"
#include "omnicore/rpctxobject.h"
#include "omnicore/rpcvalues.h"
#include "consensus/validation.h"

#include "coins.h"
#include "net.h"
#include "core_io.h"
#include "primitives/transaction.h"
#include "pubkey.h"
#include "rpc/server.h"
#include "sync.h"
#include "uint256.h"
#include "utilstrencodings.h"
#include "txmempool.h"
#include "config.h"

#include <univalue.h>

#include <stdint.h>
#include <stdexcept>
#include <string>

extern CCriticalSection cs_main;

using mastercore::cs_tx_cache;
using mastercore::view;


UniValue whc_decodetransaction(const Config &config,const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 3)
        throw std::runtime_error(
            "whc_decodetransaction \"rawtx\" ( \"prevtxs\" height )\n"

            "\nDecodes an Omni transaction.\n"

            "\nIf the inputs of the transaction are not in the chain, then they must be provided, because "
            "the transaction inputs are used to identify the sender of a transaction.\n"

            "\nA block height can be provided, which is used to determine the parsing rules.\n"

            "\nArguments:\n"
            "1. rawtx                (string, required) the raw transaction to decode\n"
            "2. prevtxs              (string, optional) a JSON array of transaction inputs (default: none)\n"
            "     [\n"
            "       {\n"
            "         \"txid\":\"hash\",          (string, required) the transaction hash\n"
            "         \"vout\":n,               (number, required) the output number\n"
            "         \"scriptPubKey\":\"hex\",   (string, required) the output script\n"
            "         \"value\":n.nnnnnnnn      (number, required) the output value\n"
            "       }\n"
            "       ,...\n"
            "     ]\n"
            "3. height               (number, optional) the parsing block height (default: 0 for chain height)\n"

            "\nResult:\n"
            "{\n"
            "  \"txid\" : \"hash\",                  (string) the hex-encoded hash of the transaction\n"
            "  \"fee\" : \"n.nnnnnnnn\",             (string) the transaction fee in bitcoins\n"
            "  \"sendingaddress\" : \"address\",     (string) the Bitcoin address of the sender\n"
            "  \"referenceaddress\" : \"address\",   (string) a Bitcoin address used as reference (if any)\n"
            "  \"ismine\" : true|false,            (boolean) whether the transaction involes an address in the wallet\n"
            "  \"version\" : n,                    (number) the transaction version\n"
            "  \"type_int\" : n,                   (number) the transaction type as number\n"
            "  \"type\" : \"type\",                  (string) the transaction type as string\n"
            "  [...]                             (mixed) other transaction type specific properties\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("whc_decodetransaction", "\"010000000163af14ce6d477e1c793507e32a5b7696288fa89705c0d02a3f66beb3c5b8afee0100000000ffffffff02ac020000000000004751210261ea979f6a06f9dafe00fb1263ea0aca959875a7073556a088cdfadcd494b3752102a3fd0a8a067e06941e066f78d930bfc47746f097fcd3f7ab27db8ddf37168b6b52ae22020000000000001976a914946cb2e08075bcbaf157e47bcb67eb2b2339d24288ac00000000\" \"[{\\\"txid\\\":\\\"eeafb8c5b3be663f2ad0c00597a88f2896765b2ae30735791c7e476dce14af63\\\",\\\"vout\\\":1,\\\"scriptPubKey\\\":\\\"76a9149084c0bd89289bc025d0264f7f23148fb683d56c88ac\\\",\\\"value\\\":0.0001123}]\"")
            + HelpExampleRpc("whc_decodetransaction", "\"010000000163af14ce6d477e1c793507e32a5b7696288fa89705c0d02a3f66beb3c5b8afee0100000000ffffffff02ac020000000000004751210261ea979f6a06f9dafe00fb1263ea0aca959875a7073556a088cdfadcd494b3752102a3fd0a8a067e06941e066f78d930bfc47746f097fcd3f7ab27db8ddf37168b6b52ae22020000000000001976a914946cb2e08075bcbaf157e47bcb67eb2b2339d24288ac00000000\", [{\"txid\":\"eeafb8c5b3be663f2ad0c00597a88f2896765b2ae30735791c7e476dce14af63\",\"vout\":1,\"scriptPubKey\":\"76a9149084c0bd89289bc025d0264f7f23148fb683d56c88ac\",\"value\":0.0001123}]")
        );

    CTransaction tx = ParseTransaction(request.params[0]);

    // use a dummy coins view to store the user provided transaction inputs
    CCoinsView viewDummyTemp;
    CCoinsViewCache viewTemp(&viewDummyTemp);

    if (request.params.size() > 1) {
        std::vector<PrevTxsEntry> prevTxsParsed = ParsePrevTxs(request.params[1]);
        InputsToView(prevTxsParsed, viewTemp);
    }

    int blockHeight = 0;
    if (request.params.size() > 2) {
        blockHeight = request.params[2].get_int();
    }

    UniValue txObj(UniValue::VOBJ);
    int populateResult = -3331;
    {
        LOCK2(cs_main, cs_tx_cache);
        // temporarily switch global coins view cache for transaction inputs
        std::swap(view, viewTemp);
        uint256 blockHash;
        CTransactionRef txref;
        GetTransaction(GetConfig(), tx.GetId(), txref, blockHash, true);
        populateResult = populateRPCTransactionObject(tx, blockHash, txObj, "", false, "", blockHeight);
        // and restore the original, unpolluted coins view cache
        std::swap(viewTemp, view);
    }

    if (populateResult != 0) PopulateFailure(populateResult);

    return txObj;
}

UniValue whc_sendrawtransaction(const Config &config, const JSONRPCRequest &request) {
    if (request.fHelp || request.params.size() < 1 ||
        request.params.size() > 2) {
        throw std::runtime_error(
                "whc_sendrawtransaction \"hexstring\" ( allowhighfees )\n"
                        "\nSubmits raw transaction (serialized, hex-encoded) to local node "
                        "and network.\n"
                        "\nAlso see createrawtransaction and signrawtransaction calls.\n"
                        "\nArguments:\n"
                        "1. \"hexstring\"    (string, required) The hex string of the raw "
                        "transaction)\n"
                        "2. allowhighfees    (boolean, optional, default=false) Allow high "
                        "fees\n"
                        "\nResult:\n"
                        "\"hex\"             (string) The transaction hash in hex\n"
                "\nSend the transaction (signed hex)\n" +
                HelpExampleCli("whc_sendrawtransaction", "\"hexstring\"") +
                "\nAs a json rpc call\n" +
                HelpExampleRpc("whc_sendrawtransaction", "\"hexstring\""));
    }

    LOCK(cs_main);
    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VBOOL});

    // parse hex string from parameter
    CMutableTransaction mtx;
    if (!DecodeHexTx(mtx, request.params[0].get_str())) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    }

    CTransactionRef tx(MakeTransactionRef(std::move(mtx)));
    const uint256 &txid = tx->GetId();

    int blockHeight = mastercore::GetHeight();
    CMPTransaction mp_obj;
    int pop_ret = ParseTransaction(*tx.get(), blockHeight, 0, mp_obj);
    if (0 == pop_ret) {
		mp_obj.unlockLogic();
        if (mp_obj.getEncodingClass() != OMNI_CLASS_C) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Not a Wormhole Protocol transaction");
        }

        if (mp_obj.getSender().empty() == true) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "The transaction no have sender");
        }

        int interp_ret = mp_obj.interpretPacket();
        if (interp_ret < 0) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, error_str(interp_ret));
        }
    } else{
		throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Not a Wormhole Protocol transaction");
	}


    bool fLimitFree = false;
    Amount nMaxRawTxFee = maxTxFee;
    if (request.params.size() > 1 && request.params[1].get_bool()) {
        nMaxRawTxFee = Amount(0);
    }

    CCoinsViewCache &viewTmp = *pcoinsTip;
    bool fHaveChain = false;
    for (size_t o = 0; !fHaveChain && o < tx->vout.size(); o++) {
        const Coin &existingCoin = viewTmp.AccessCoin(COutPoint(txid, o));
        fHaveChain = !existingCoin.IsSpent();
    }

    bool fHaveMempool = g_mempool.exists(txid);
    if (!fHaveMempool && !fHaveChain) {
        // Push to local node and sync with wallets.
        CValidationState state;
        bool fMissingInputs;
        if (!AcceptToMemoryPool(config, g_mempool, state, tx,
                                fLimitFree, &fMissingInputs, false,
                                nMaxRawTxFee)) {
            if (state.IsInvalid()) {
                throw JSONRPCError(RPC_TRANSACTION_REJECTED,
                                   strprintf("%i: %s", state.GetRejectCode(),
                                             state.GetRejectReason()));
            } else {
                if (fMissingInputs) {
                    throw JSONRPCError(RPC_TRANSACTION_ERROR, "Missing inputs");
                }

                throw JSONRPCError(RPC_TRANSACTION_ERROR,
                                   state.GetRejectReason());
            }
        }
    } else if (fHaveChain) {
        throw JSONRPCError(RPC_TRANSACTION_ALREADY_IN_CHAIN,
                           "transaction already in block chain");
    }

    if (!g_connman) {
        throw JSONRPCError(
                RPC_CLIENT_P2P_DISABLED,
                "Error: Peer-to-peer functionality missing or disabled");
    }

    CInv inv(MSG_TX, txid);
    g_connman->ForEachNode([&inv](CNode *pnode) { pnode->PushInventory(inv); });
    return txid.GetHex();
}

UniValue whc_createrawtx_opreturn(const Config &config,const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 2)
        throw std::runtime_error(
            "whc_createrawtx_opreturn \"rawtx\" \"payload\"\n"

            "\nAdds a payload with class C (op-return) encoding to the transaction.\n"

            "\nIf no raw transaction is provided, a new transaction is created.\n"

            "\nIf the data encoding fails, then the transaction is not modified.\n"

            "\nArguments:\n"
            "1. rawtx                (string, required) the raw transaction to extend (can be null)\n"
            "2. payload              (string, required) the hex-encoded payload to add\n"

            "\nResult:\n"
            "\"rawtx\"                 (string) the hex-encoded modified raw transaction\n"

            "\nExamples\n"
            + HelpExampleCli("whc_createrawtx_opreturn", "\"01000000000000000000\" \"00000000000000020000000006dac2c0\"")
            + HelpExampleRpc("whc_createrawtx_opreturn", "\"01000000000000000000\", \"00000000000000020000000006dac2c0\"")
        );

    CMutableTransaction tx = ParseMutableTransaction(request.params[0]);
    std::vector<unsigned char> payload = ParseHexV(request.params[1], "payload");

    // extend the transaction
    tx = OmniTxBuilder(tx)
            .addOpReturn(payload)
            .build();

    return EncodeHexTx(CTransaction(tx));
}

UniValue whc_createrawtx_multisig(const Config &config,const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 4)
        throw std::runtime_error(
            "whc_createrawtx_multisig \"rawtx\" \"payload\" \"seed\" \"redeemkey\"\n"

            "\nAdds a payload with class B (bare-multisig) encoding to the transaction.\n"

            "\nIf no raw transaction is provided, a new transaction is created.\n"

            "\nIf the data encoding fails, then the transaction is not modified.\n"

            "\nArguments:\n"
            "1. rawtx                (string, required) the raw transaction to extend (can be null)\n"
            "2. payload              (string, required) the hex-encoded payload to add\n"
            "3. seed                 (string, required) the seed for obfuscation\n"
            "4. redeemkey            (string, required) a public key or address for dust redemption\n"

            "\nResult:\n"
            "\"rawtx\"                 (string) the hex-encoded modified raw transaction\n"

            "\nExamples\n"
            + HelpExampleCli("whc_createrawtx_multisig", "\"0100000001a7a9402ecd77f3c9f745793c9ec805bfa2e14b89877581c734c774864247e6f50400000000ffffffff01aa0a0000000000001976a9146d18edfe073d53f84dd491dae1379f8fb0dfe5d488ac00000000\" \"00000000000000020000000000989680\" \"1LifmeXYHeUe2qdKWBGVwfbUCMMrwYtoMm\" \"0252ce4bdd3ce38b4ebbc5a6e1343608230da508ff12d23d85b58c964204c4cef3\"")
            + HelpExampleRpc("whc_createrawtx_multisig", "\"0100000001a7a9402ecd77f3c9f745793c9ec805bfa2e14b89877581c734c774864247e6f50400000000ffffffff01aa0a0000000000001976a9146d18edfe073d53f84dd491dae1379f8fb0dfe5d488ac00000000\", \"00000000000000020000000000989680\", \"1LifmeXYHeUe2qdKWBGVwfbUCMMrwYtoMm\", \"0252ce4bdd3ce38b4ebbc5a6e1343608230da508ff12d23d85b58c964204c4cef3\"")
        );

    CMutableTransaction tx = ParseMutableTransaction(request.params[0]);
    std::vector<unsigned char> payload = ParseHexV(request.params[1], "payload");
    std::string obfuscationSeed = ParseAddressOrEmpty(request.params[2]);
    CPubKey redeemKey = ParsePubKeyOrAddress(request.params[3]);

    // extend the transaction
    tx = OmniTxBuilder(tx)
            .addMultisig(payload, obfuscationSeed, redeemKey)
            .build();

    return EncodeHexTx(CTransaction(tx));
}

UniValue whc_createrawtx_input(const Config &config,const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() != 3)
        throw std::runtime_error(
            "whc_createrawtx_input \"rawtx\" \"txid\" n\n"

            "\nAdds a transaction input to the transaction.\n"

            "\nIf no raw transaction is provided, a new transaction is created.\n"

            "\nArguments:\n"
            "1. rawtx                (string, required) the raw transaction to extend (can be null)\n"
            "2. txid                 (string, required) the hash of the input transaction\n"
            "3. n                    (number, required) the index of the transaction output used as input\n"

            "\nResult:\n"
            "\"rawtx\"                 (string) the hex-encoded modified raw transaction\n"

            "\nExamples\n"
            + HelpExampleCli("whc_createrawtx_input", "\"01000000000000000000\" \"b006729017df05eda586df9ad3f8ccfee5be340aadf88155b784d1fc0e8342ee\" 0")
            + HelpExampleRpc("whc_createrawtx_input", "\"01000000000000000000\", \"b006729017df05eda586df9ad3f8ccfee5be340aadf88155b784d1fc0e8342ee\", 0")
        );

    CMutableTransaction tx = ParseMutableTransaction(request.params[0]);
    uint256 txid = ParseHashV(request.params[1], "txid");
    uint32_t nOut = ParseOutputIndex(request.params[2]);

    // extend the transaction
    tx = OmniTxBuilder(tx)
            .addInput(txid, nOut)
            .build();

    return EncodeHexTx(CTransaction(tx));
}

UniValue whc_createrawtx_reference(const Config &config,const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 2 || request.params.size() > 3)
        throw std::runtime_error(
            "whc_createrawtx_reference \"rawtx\" \"destination\" ( amount )\n"

            "\nAdds a reference output to the transaction.\n"

            "\nIf no raw transaction is provided, a new transaction is created.\n"

            "\nThe output value is set to at least the dust threshold.\n"

            "\nArguments:\n"
            "1. rawtx                (string, required) the raw transaction to extend (can be null)\n"
            "2. destination          (string, required) the reference address or destination\n"
            "3. amount               (number, optional) the optional reference amount (minimal by default)\n"

            "\nResult:\n"
            "\"rawtx\"                 (string) the hex-encoded modified raw transaction\n"

            "\nExamples\n"
            + HelpExampleCli("whc_createrawtx_reference", "\"0100000001a7a9402ecd77f3c9f745793c9ec805bfa2e14b89877581c734c774864247e6f50400000000ffffffff03aa0a0000000000001976a9146d18edfe073d53f84dd491dae1379f8fb0dfe5d488ac5c0d0000000000004751210252ce4bdd3ce38b4ebbc5a6e1343608230da508ff12d23d85b58c964204c4cef3210294cc195fc096f87d0f813a337ae7e5f961b1c8a18f1f8604a909b3a5121f065b52aeaa0a0000000000001976a914946cb2e08075bcbaf157e47bcb67eb2b2339d24288ac00000000\" \"1CE8bBr1dYZRMnpmyYsFEoexa1YoPz2mfB\" 0.005")
            + HelpExampleRpc("whc_createrawtx_reference", "\"0100000001a7a9402ecd77f3c9f745793c9ec805bfa2e14b89877581c734c774864247e6f50400000000ffffffff03aa0a0000000000001976a9146d18edfe073d53f84dd491dae1379f8fb0dfe5d488ac5c0d0000000000004751210252ce4bdd3ce38b4ebbc5a6e1343608230da508ff12d23d85b58c964204c4cef3210294cc195fc096f87d0f813a337ae7e5f961b1c8a18f1f8604a909b3a5121f065b52aeaa0a0000000000001976a914946cb2e08075bcbaf157e47bcb67eb2b2339d24288ac00000000\", \"1CE8bBr1dYZRMnpmyYsFEoexa1YoPz2mfB\", 0.005")
        );

    CMutableTransaction tx = ParseMutableTransaction(request.params[0]);
    std::string destination = ParseAddress(request.params[1]);
    int64_t amount = (request.params.size() > 2) ? AmountFromValue(request.params[2]).GetSatoshis() : 0;

    // extend the transaction
    tx = OmniTxBuilder(tx)
            .addReference(destination, amount)
            .build();

    return EncodeHexTx(CTransaction(tx));
}

UniValue whc_createrawtx_change(const Config &config,const JSONRPCRequest &request)
{
    if (request.fHelp || request.params.size() < 4 || request.params.size() > 5)
        throw std::runtime_error(
            "whc_createrawtx_change \"rawtx\" \"prevtxs\" \"destination\" fee ( position )\n"

            "\nAdds a change output to the transaction.\n"

            "\nThe provided inputs are not added to the transaction, but only used to "
            "determine the change. It is assumed that the inputs were previously added, "
            "for example via \"createrawtransaction\".\n"

            "\nOptionally a position can be provided, where the change output should be "
            "inserted, starting with 0. If the number of outputs is smaller than the position, "
            "then the change output is added to the end. Change outputs should be inserted "
            "before reference outputs, and as per default, the change output is added to the "
            "first position.\n"

            "\nIf the change amount would be considered as dust, then no change output is added.\n"

            "\nArguments:\n"
            "1. rawtx                (string, required) the raw transaction to extend\n"
            "2. prevtxs              (string, required) a JSON array of transaction inputs\n"
            "     [\n"
            "       {\n"
            "         \"txid\":\"hash\",          (string, required) the transaction hash\n"
            "         \"vout\":n,               (number, required) the output number\n"
            "         \"scriptPubKey\":\"hex\",   (string, required) the output script\n"
            "         \"value\":n.nnnnnnnn      (number, required) the output value\n"
            "       }\n"
            "       ,...\n"
            "     ]\n"
            "3. destination          (string, required) the destination for the change\n"
            "4. fee                  (number, required) the desired transaction fees\n"
            "5. position             (number, optional) the position of the change output (default: first position)\n"

            "\nResult:\n"
            "\"rawtx\"                 (string) the hex-encoded modified raw transaction\n"

            "\nExamples\n"
            + HelpExampleCli("whc_createrawtx_change", "\"0100000001b15ee60431ef57ec682790dec5a3c0d83a0c360633ea8308fbf6d5fc10a779670400000000ffffffff025c0d00000000000047512102f3e471222bb57a7d416c82bf81c627bfcd2bdc47f36e763ae69935bba4601ece21021580b888ff56feb27f17f08802ebed26258c23697d6a462d43fc13b565fda2dd52aeaa0a0000000000001976a914946cb2e08075bcbaf157e47bcb67eb2b2339d24288ac00000000\" \"[{\\\"txid\\\":\\\"6779a710fcd5f6fb0883ea3306360c3ad8c0a3c5de902768ec57ef3104e65eb1\\\",\\\"vout\\\":4,\\\"scriptPubKey\\\":\\\"76a9147b25205fd98d462880a3e5b0541235831ae959e588ac\\\",\\\"value\\\":0.00068257}]\" \"1CE8bBr1dYZRMnpmyYsFEoexa1YoPz2mfB\" 0.00003500 1")
            + HelpExampleRpc("whc_createrawtx_change", "\"0100000001b15ee60431ef57ec682790dec5a3c0d83a0c360633ea8308fbf6d5fc10a779670400000000ffffffff025c0d00000000000047512102f3e471222bb57a7d416c82bf81c627bfcd2bdc47f36e763ae69935bba4601ece21021580b888ff56feb27f17f08802ebed26258c23697d6a462d43fc13b565fda2dd52aeaa0a0000000000001976a914946cb2e08075bcbaf157e47bcb67eb2b2339d24288ac00000000\", [{\"txid\":\"6779a710fcd5f6fb0883ea3306360c3ad8c0a3c5de902768ec57ef3104e65eb1\",\"vout\":4,\"scriptPubKey\":\"76a9147b25205fd98d462880a3e5b0541235831ae959e588ac\",\"value\":0.00068257}], \"1CE8bBr1dYZRMnpmyYsFEoexa1YoPz2mfB\", 0.00003500, 1")
        );

    CMutableTransaction tx = ParseMutableTransaction(request.params[0]);
    std::vector<PrevTxsEntry> prevTxsParsed = ParsePrevTxs(request.params[1]);
    std::string destination = ParseAddress(request.params[2]);
    int64_t txFee = AmountFromValue(request.params[3]).GetSatoshis();
    uint32_t nOut = request.params.size() > 4 ? request.params[4].get_int64() : 0;

    // use a dummy coins view to store the user provided transaction inputs
    CCoinsView viewDummy;
    CCoinsViewCache viewTemp(&viewDummy);
    InputsToView(prevTxsParsed, viewTemp);

    // extend the transaction
    tx = OmniTxBuilder(tx)
            .addChange(destination, viewTemp, txFee, nOut)
            .build();

    return EncodeHexTx(CTransaction(tx));
}

static const ContextFreeRPCCommand commands[] =
{ //  category                         name                          actor (function)             okSafeMode
  //  -------------------------------- ----------------------------- ---------------------------- ----------
    { "omni layer (raw transactions)", "whc_decodetransaction",     &whc_decodetransaction, {}},
    { "omni layer (raw transactions)", "whc_createrawtx_opreturn",  &whc_createrawtx_opreturn, {}},
    { "omni layer (raw transactions)", "whc_sendrawtransaction",    &whc_sendrawtransaction, {}},
    { "omni layer (raw transactions)", "whc_createrawtx_input",     &whc_createrawtx_input, {}},
    { "omni layer (raw transactions)", "whc_createrawtx_reference", &whc_createrawtx_reference, {}},
    { "omni layer (raw transactions)", "whc_createrawtx_change",    &whc_createrawtx_change, {}},

};

void RegisterOmniRawTransactionRPCCommands(CRPCTable &tableRPCVal)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        tableRPCVal.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
