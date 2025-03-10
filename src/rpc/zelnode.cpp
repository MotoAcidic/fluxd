// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2015-2019 The PIVX developers
// Copyright (c) 2018-2019 The Zel developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "zelnode/activezelnode.h"
#include "db.h"
#include "init.h"
#include "main.h"
#include "zelnode/zelnodeconfig.h"
#include "rpc/server.h"
#include "utilmoneystr.h"
#include "key_io.h"
#include "zelnode/benchmarks.h"
#include "util.h"

#include <univalue.h>

#include <boost/tokenizer.hpp>
#include <fstream>
#include <consensus/validation.h>
#include <undo.h>

#define MICRO 0.000001
#define MILLI 0.001

UniValue rebuildzelnodedb(const UniValue& params, bool fHelp) {
    if (fHelp || params.size() > 0)
        throw runtime_error(
                "rebuildzelnodedb \n"
                "\nRescans the blockchain from the start of the zelnode transactions to rebuild the zelnodedb\n"
                "\nNote: This call can take minutes to complete\n"

                "\nExamples:\n"
                + HelpExampleCli("rebuildzelnodedb", "")
                + HelpExampleRpc("rebuildzelnodedb", "")
        );
    {
        LOCK2(cs_main, g_zelnodeCache.cs);

        int nCurrentHeight = chainActive.Height();

        g_zelnodeCache.SetNull();
        g_zelnodeCache.InitMapZelnodeList();

        delete pZelnodeDB;
        pZelnodeDB = NULL;
        pZelnodeDB = new CDeterministicZelnodeDB(0, false, true);

        CBlockIndex *rescanIndex = nullptr;

        rescanIndex = chainActive[Params().GetConsensus().vUpgrades[Consensus::UPGRADE_KAMATA].nActivationHeight - 10];

        const int nTotalBlocks = nCurrentHeight - rescanIndex->nHeight;

        int nPrintTrigger = 0;
        int nPercent = 0;
        std::set<COutPoint> setSpentOutPoints;
        CZelnodeTxBlockUndo zelnodeTxBlockUndo;

        // Main benchmarks
        static int64_t nTimeLoadBlock = 0;
        static int64_t nTimeAddPaidNode = 0;
        static int64_t nTimeLoopTx = 0;
        static int64_t nTimeUndoData = 0;
        static int64_t nTimeWriteUndo = 0;
        static int64_t nTimeFlush = 0;
        static int64_t nTimeTotal = 0;
        static int64_t nBlocksTotal = 0;

        // Inner Tx loop benchmarks
        static int64_t nLoopSpentOutputs = 0;
        static int64_t nLoopFetchTx = 0;
        static int64_t nAddStart = 0;
        static int64_t nAddNewConfirm = 0;
        static int64_t nAddUpdateConfirm = 0;


        while (rescanIndex) {
            if (nPrintTrigger <= 0) {
                nPercent = (nTotalBlocks - (nCurrentHeight - rescanIndex->nHeight)) * 100 / nTotalBlocks;
                std::cout << "     " << _("Fluxnode blocks") << " | " << nCurrentHeight - rescanIndex->nHeight - nTotalBlocks << " / ~" << nTotalBlocks << " (" << nPercent << "%)" << std::endl;
                LogPrintf("Fluxnode blocks %d / %d (%d percent)\n", (nTotalBlocks - (nCurrentHeight - rescanIndex->nHeight)), nTotalBlocks, nPercent);

                LogPrint("bench", "Read block : [%.2fs (%.2fms/blk)]\n", nTimeLoadBlock * MICRO, nTimeLoadBlock * MILLI / nBlocksTotal);
                LogPrint("bench", "dpaidNode : [%.2fs (%.2fms/blk)]\n", nTimeAddPaidNode * MICRO, nTimeAddPaidNode * MILLI / nBlocksTotal);
                LogPrint("bench", "LoopTx : [%.2fs (%.2fms/blk)]\n", nTimeLoopTx * MICRO, nTimeLoopTx * MILLI / nBlocksTotal);
                LogPrint("bench", "Undo : [%.2fs (%.2fms/blk)]\n", nTimeUndoData * MICRO, nTimeUndoData * MILLI / nBlocksTotal);
                LogPrint("bench", "Write Undo : [%.2fs (%.2fms/blk)]\n", nTimeWriteUndo * MICRO, nTimeUndoData * MILLI  / nBlocksTotal);
                LogPrint("bench", "Flush : [%.2fs (%.2fms/blk)]\n", nTimeFlush * MICRO, nTimeFlush * MILLI  / nBlocksTotal);

                LogPrint("bench", "nLoopSpentOutputs : [%.2fs (%.2fms/blk)]\n", nLoopSpentOutputs * MICRO, nLoopSpentOutputs * MILLI  / nBlocksTotal);
                LogPrint("bench", "nLoopFetchTx : [%.2fs (%.2fms/blk)]\n", nLoopFetchTx * MICRO, nLoopFetchTx * MILLI  / nBlocksTotal);
                LogPrint("bench", "nAddStart : [%.2fs (%.2fms/blk)]\n", nAddStart * MICRO, nAddStart * MILLI  / nBlocksTotal);
                LogPrint("bench", "nAddNewConfirm : [%.2fs (%.2fms/blk)]\n", nAddNewConfirm * MICRO, nAddNewConfirm * MILLI  / nBlocksTotal);
                LogPrint("bench", "nAddUpdateConfirm : [%.2fs (%.2fms/blk)]\n", nAddUpdateConfirm * MICRO, nAddUpdateConfirm * MILLI  / nBlocksTotal);

                nPrintTrigger = 10000;
            }
            nPrintTrigger--;

            zelnodeTxBlockUndo.SetNull();
            setSpentOutPoints.clear();

            ZelnodeCache zelnodeCache;
            CBlock block;

            int64_t nTimeStart = GetTimeMicros();
            ReadBlockFromDisk(block, rescanIndex, Params().GetConsensus());
            nBlocksTotal++;

            int64_t nTime1 = GetTimeMicros(); nTimeLoadBlock += nTime1 - nTimeStart;

            // Add paidnode info
            if (rescanIndex->nHeight >= Params().StartZelnodePayments()) {
                CTxDestination t_dest;
                COutPoint t_out;
                for (int currentTier = CUMULUS; currentTier != LAST; currentTier++) {
                    if (g_zelnodeCache.GetNextPayment(t_dest, currentTier, t_out)) {
                        zelnodeCache.AddPaidNode(currentTier, t_out, rescanIndex->nHeight);
                    }
                }
            }

            int64_t nTime2 = GetTimeMicros(); nTimeAddPaidNode += nTime2 - nTime1;

            for (const auto& tx : block.vtx) {

                int64_t nLoopStart = GetTimeMicros();

                if (!tx.IsCoinBase() && !tx.IsZelnodeTx()) {
                    for (const auto &input : tx.vin) {
                        setSpentOutPoints.insert(input.prevout);
                    }
                }

                int64_t nLoop1 = GetTimeMicros(); nLoopSpentOutputs += nLoop1 - nLoopStart;

                if (tx.IsZelnodeTx()) {
                    int nTier = 0;
                    CTransaction get_tx;
                    uint256 block_hash;
                    if (GetTransaction(tx.collateralOut.hash, get_tx, Params().GetConsensus(), block_hash,
                                       true)) {

                        if (!GetCoinTierFromAmount(rescanIndex->nHeight, get_tx.vout[tx.collateralOut.n].nValue, nTier)) {
                            return error("Failed to get tier from amount. This shouldn't happen tx = %s", tx.collateralOut.ToFullString());
                        }

                    } else {
                        return error("Failed to find tx: %s", tx.collateralOut.ToFullString());
                    }

                    int64_t nLoop2 = GetTimeMicros(); nLoopFetchTx += nLoop2 - nLoop1;

                    if (tx.nType == ZELNODE_START_TX_TYPE) {

                        // Add new Zelnode Start Tx into local cache
                        zelnodeCache.AddNewStart(tx, rescanIndex->nHeight, nTier, get_tx.vout[tx.collateralOut.n].nValue);
                        int64_t nLoop3 = GetTimeMicros(); nAddStart += nLoop3 - nLoop2;

                    } else if (tx.nType == ZELNODE_CONFIRM_TX_TYPE) {
                        if (tx.nUpdateType == ZelnodeUpdateType::INITIAL_CONFIRM) {

                            zelnodeCache.AddNewConfirm(tx, rescanIndex->nHeight);
                            int64_t nLoop4 = GetTimeMicros(); nAddNewConfirm += nLoop4 - nLoop2;
                        } else if (tx.nUpdateType == ZelnodeUpdateType::UPDATE_CONFIRM) {
                            zelnodeCache.AddUpdateConfirm(tx, rescanIndex->nHeight);
                            ZelnodeCacheData global_data = g_zelnodeCache.GetZelnodeData(tx.collateralOut);
                            if (global_data.IsNull()) {
                                return error("Failed to find global data on update confirm tx, %s",
                                             tx.GetHash().GetHex());
                            }
                            zelnodeTxBlockUndo.mapUpdateLastConfirmHeight.insert(
                                    std::make_pair(tx.collateralOut,
                                                   global_data.nLastConfirmedBlockHeight));
                            zelnodeTxBlockUndo.mapLastIpAddress.insert(std::make_pair(tx.collateralOut, global_data.ip));
                            int64_t nLoop5 = GetTimeMicros(); nAddUpdateConfirm += nLoop5 - nLoop2;
                        }
                    }
                }
            }

            int64_t nTime3 = GetTimeMicros(); nTimeLoopTx += nTime3 - nTime2;

            // Update the temp cache with the set of started outpoints that have now expired from the dos list
            GetUndoDataForExpiredZelnodeDosScores(zelnodeTxBlockUndo, rescanIndex->nHeight);
            zelnodeCache.AddExpiredDosTx(zelnodeTxBlockUndo, rescanIndex->nHeight);

            // Update the temp cache with the set of confirmed outpoints that have now expired
            GetUndoDataForExpiredConfirmZelnodes(zelnodeTxBlockUndo, rescanIndex->nHeight, setSpentOutPoints);
            zelnodeCache.AddExpiredConfirmTx(zelnodeTxBlockUndo);

            // Update the block undo, with the paid nodes last paid height.
            GetUndoDataForPaidZelnodes(zelnodeTxBlockUndo, zelnodeCache);

            // Check for Start tx that are going to expire
            zelnodeCache.CheckForExpiredStartTx(rescanIndex->nHeight);

            int64_t nTime4 = GetTimeMicros(); nTimeUndoData += nTime4 - nTime3;

            if (zelnodeTxBlockUndo.vecExpiredDosData.size() ||
                zelnodeTxBlockUndo.vecExpiredConfirmedData.size() ||
                zelnodeTxBlockUndo.mapUpdateLastConfirmHeight.size() ||
                zelnodeTxBlockUndo.mapLastPaidHeights.size()) {
                if (!pZelnodeDB->WriteBlockUndoZelnodeData(block.GetHash(), zelnodeTxBlockUndo))
                    return error("Failed to write zelnodetx undo data");
            }

            int64_t nTime5 = GetTimeMicros(); nTimeWriteUndo += nTime5 - nTime4;

            assert(zelnodeCache.Flush());

            int64_t nTime6 = GetTimeMicros(); nTimeFlush += nTime6 - nTime5;

            rescanIndex = chainActive.Next(rescanIndex);
        }
        g_zelnodeCache.DumpZelnodeCache();
    }

    return true;
}


UniValue createzelnodekey(const UniValue& params, bool fHelp)
{
    if (fHelp || (params.size() != 0))
        throw runtime_error(
                "createzelnodekey\n"
                "\nCreate a new zelnode private key\n"

                "\nResult:\n"
                "\"key\"    (string) Zelnode private key\n"

                "\nExamples:\n" +
                HelpExampleCli("createzelnodekey", "") + HelpExampleRpc("createzelnodekey", ""));

    CKey secret;
    secret.MakeNewKey(false);
    return EncodeSecret(secret);
}

UniValue createsporkkeys(const UniValue& params, bool fHelp)
{
    if (fHelp || (params.size() != 0))
        throw runtime_error(
                "createsporkkeys\n"
                "\nCreate a set of private and public keys used for sporks\n"

                "\nResult:\n"
                "\"pubkey\"    (string) Spork public key\n"
                "\"privkey\"    (string) Spork private key\n"

                "\nExamples:\n" +
                HelpExampleCli("createsporkkeys", "") + HelpExampleRpc("createsporkkeys", ""));

    CKey secret;
    secret.MakeNewKey(false);

    CPubKey pubKey = secret.GetPubKey();

    std::string str;
    for (int i = 0; i < pubKey.size(); i++) {
        str += pubKey[i];
    }

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("pubkey", HexStr(str));
    ret.pushKV("privkey", EncodeSecret(secret));
    return ret;
}

UniValue getzelnodeoutputs(const UniValue& params, bool fHelp)
{
    if (fHelp || (params.size() != 0))
        throw runtime_error(
                "getzelnodeoutputs\n"
                "\nPrint all zelnode transaction outputs\n"

                "\nResult:\n"
                "[\n"
                "  {\n"
                "    \"txhash\": \"xxxx\",    (string) output transaction hash\n"
                "    \"outputidx\": n       (numeric) output index number\n"
                "  }\n"
                "  ,...\n"
                "]\n"

                "\nExamples:\n" +
                HelpExampleCli("getzelnodeoutputs", "") + HelpExampleRpc("getzelnodeoutputs", ""));

    // Find possible candidates
    vector<std::pair<COutput, CAmount>> possibleCoins = activeZelnode.SelectCoinsZelnode();

    UniValue ret(UniValue::VARR);
    for (auto& pair : possibleCoins) {
        COutput out = pair.first;
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("txhash", out.tx->GetHash().ToString());
        obj.pushKV("outputidx", out.i);
        obj.pushKV("ZEL Amount", pair.second / COIN);
        obj.pushKV("Confirmations", pair.first.nDepth);
        ret.push_back(obj);
    }

    return ret;
}

UniValue createconfirmationtransaction(const UniValue& params, bool fHelp)
{
    if (fHelp || (params.size() != 0))
        throw runtime_error(
                "createconfirmationtransaction\n"
                "\nCreate a new confirmation transaction and return the raw hex\n"

                "\nResult:\n"
                "    \"hex\": \"xxxx\",    (string) output transaction hex\n"

                "\nExamples:\n" +
                HelpExampleCli("createconfirmationtransaction", "") + HelpExampleRpc("createconfirmationtransaction", ""));

    if (!fZelnode) throw runtime_error("This is not a Flux Node");

    std::string errorMessage;
    CMutableTransaction mutTx;
    mutTx.nVersion = ZELNODE_TX_VERSION;

    activeZelnode.BuildDeterministicConfirmTx(mutTx, ZelnodeUpdateType::UPDATE_CONFIRM);

    if (!activeZelnode.SignDeterministicConfirmTx(mutTx, errorMessage)) {
        throw runtime_error(strprintf("Failed to sign new confirmation transaction: %s\n", errorMessage));
    }

    CTransaction tx(mutTx);

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << tx;
    return HexStr(ss.begin(), ss.end());
}

UniValue startzelnode(const UniValue& params, bool fHelp)
{

    std::string strCommand;
    if (params.size() >= 1)
        strCommand = params[0].get_str();


    if (IsZelnodeTransactionsActive()) {
        if (fHelp || params.size() < 2 || params.size() > 3 ||
            (params.size() == 2 && (strCommand != "all")) ||
            (params.size() == 3 && strCommand != "alias"))
            throw runtime_error(
                    "startzelnode \"all|alias\" lockwallet ( \"alias\" )\n"
                    "\nAttempts to start one or more zelnode(s)\n"

                    "\nArguments:\n"
                    "1. set         (string, required) Specify which set of zelnode(s) to start.\n"
                    "2. lockwallet  (boolean, required) Lock wallet after completion.\n"
                    "3. alias       (string) Zelnode alias. Required if using 'alias' as the set.\n"

                    "\nResult: (for 'local' set):\n"
                    "\"status\"     (string) Zelnode status message\n"

                    "\nResult: (for other sets):\n"
                    "{\n"
                    "  \"overall\": \"xxxx\",     (string) Overall status message\n"
                    "  \"detail\": [\n"
                    "    {\n"
                    "      \"node\": \"xxxx\",    (string) Node name or alias\n"
                    "      \"result\": \"xxxx\",  (string) 'success' or 'failed'\n"
                    "      \"error\": \"xxxx\"    (string) Error message, if failed\n"
                    "    }\n"
                    "    ,...\n"
                    "  ]\n"
                    "}\n"

                    "\nExamples:\n" +
                    HelpExampleCli("startzelnode", "\"alias\" \"0\" \"my_zn\"") + HelpExampleRpc("startzelnode", "\"alias\" \"0\" \"my_zn\""));


        if (IsInitialBlockDownload(Params())) {
            throw runtime_error("Chain is still syncing, please wait until chain is synced\n");
        }

        bool fLock = (params[1].get_str() == "true" ? true : false);

        EnsureWalletIsUnlocked();

        bool fAlias = false;
        std::string alias = "";
        if (params.size() == 3) {
            fAlias = true;
            alias = params[2].get_str();
        }

        bool found = false;
        int successful = 0;
        int failed = 0;

        UniValue resultsObj(UniValue::VARR);

        for (ZelnodeConfig::ZelnodeEntry zne : zelnodeConfig.getEntries()) {
            UniValue zelnodeEntry(UniValue::VOBJ);

            if (fAlias && zne.getAlias() == alias) {
                found = true;
            } else if (fAlias) {
                continue;
            }

            std::string errorMessage;
            CMutableTransaction mutTransaction;

            int32_t index;
            zne.castOutputIndex(index);
            COutPoint outpoint = COutPoint(uint256S(zne.getTxHash()), index);

            zelnodeEntry.pushKV("outpoint", outpoint.ToString());
            zelnodeEntry.pushKV("alias", zne.getAlias());

            bool fChecked = false;
            if (mempool.mapZelnodeTxMempool.count(outpoint)) {
                zelnodeEntry.pushKV("result", "failed");
                zelnodeEntry.pushKV("reason", "Mempool already has a zelnode transaction using this outpoint");
            } else if (g_zelnodeCache.InStartTracker(outpoint)) {
                zelnodeEntry.pushKV("result", "failed");
                zelnodeEntry.pushKV("reason", "Zelnode already started, waiting to be confirmed");
            } else if (g_zelnodeCache.InDoSTracker(outpoint)) {
                zelnodeEntry.pushKV("result", "failed");
                zelnodeEntry.pushKV("reason", "Zelnode already started then not confirmed, in DoS tracker. Must wait until out of DoS tracker to start");
            } else if (g_zelnodeCache.InConfirmTracker(outpoint)) {
                zelnodeEntry.pushKV("result", "failed");
                zelnodeEntry.pushKV("reason", "Zelnode already confirmed and in zelnode list");
            } else {
                fChecked = true;
            }

            if (!fChecked) {
                resultsObj.push_back(zelnodeEntry);

                if (fAlias)
                    return resultsObj;
                else
                    continue;
            }

            mutTransaction.nVersion = ZELNODE_TX_VERSION;

            bool result = activeZelnode.BuildDeterministicStartTx(zne.getPrivKey(), zne.getTxHash(), zne.getOutputIndex(), errorMessage, mutTransaction);

            zelnodeEntry.pushKV("transaction_built", result ? "successful" : "failed");

            if (result) {
                CReserveKey reservekey(pwalletMain);
                std::string errorMessage;

                bool fSigned = false;
                if (activeZelnode.SignDeterministicStartTx(mutTransaction, errorMessage)) {
                    CTransaction tx(mutTransaction);
                    fSigned = true;

                    CWalletTx walletTx(pwalletMain, tx);
                    CValidationState state;
                    bool fCommited = pwalletMain->CommitTransaction(walletTx, reservekey, &state);
                    zelnodeEntry.pushKV("transaction_commited", fCommited ? "successful" : "failed");
                    if (fCommited) {
                        successful++;
                    } else {
                        errorMessage = state.GetRejectReason();
                        failed++;
                    }
                } else {
                    failed++;
                }
                zelnodeEntry.pushKV("transaction_signed", fSigned ? "successful" : "failed");
                zelnodeEntry.pushKV("errorMessage", errorMessage);
            } else {
                failed++;
                zelnodeEntry.pushKV("errorMessage", errorMessage);
            }

            resultsObj.push_back(zelnodeEntry);

            if (fAlias && found) {
                break;
            }
        }

        UniValue statusObj(UniValue::VOBJ);
        if (!found && fAlias) {
            failed++;
            statusObj.pushKV("result", "failed");
            statusObj.pushKV("error", "could not find alias in config. Verify with list-conf.");
            resultsObj.push_back(statusObj);
        }

        if (fLock)
            pwalletMain->Lock();

        UniValue returnObj(UniValue::VOBJ);
        returnObj.pushKV("overall", strprintf("Successfully started %d zelnodes, failed to start %d, total %d", successful, failed, successful + failed));
        returnObj.pushKV("detail", resultsObj);

        return returnObj;
    }
    return NullUniValue;
}

UniValue startdeterministiczelnode(const UniValue& params, bool fHelp)
{
    if (!IsZelnodeTransactionsActive()) {
        throw runtime_error("deterministic zelnodes transactions is not active yet");
    }

    std::string strCommand;
    if (params.size() >= 1)
        strCommand = params[0].get_str();

    if (fHelp || params.size() != 2)
        throw runtime_error(
                "startdeterministiczelnode alias_name lockwallet\n"
                "\nAttempts to start one zelnode\n"

                "\nArguments:\n"
                "1. set         (string, required) Specify which set of zelnode(s) to start.\n"
                "2. lockwallet  (boolean, required) Lock wallet after completion.\n"
                "3. alias       (string) Zelnode alias. Required if using 'alias' as the set.\n"

                "\nResult: (for 'local' set):\n"
                "\"status\"     (string) Zelnode status message\n"

                "\nResult: (for other sets):\n"
                "{\n"
                "  \"overall\": \"xxxx\",     (string) Overall status message\n"
                "  \"detail\": [\n"
                "    {\n"
                "      \"node\": \"xxxx\",    (string) Node name or alias\n"
                "      \"result\": \"xxxx\",  (string) 'success' or 'failed'\n"
                "      \"error\": \"xxxx\"    (string) Error message, if failed\n"
                "    }\n"
                "    ,...\n"
                "  ]\n"
                "}\n"

                "\nExamples:\n" +
                HelpExampleCli("startdeterministiczelnode", "\"alias_name\" false ") + HelpExampleRpc("startdeterministiczelnode", "\"alias_name\" false"));

    bool fLock = (params[1].get_str() == "true" ? true : false);

    EnsureWalletIsUnlocked();

    std::string alias = params[0].get_str();

    bool found = false;
    int successful = 0;
    int failed = 0;

    UniValue resultsObj(UniValue::VARR);
    UniValue statusObj(UniValue::VOBJ);
    statusObj.pushKV("alias", alias);

    for (ZelnodeConfig::ZelnodeEntry zne : zelnodeConfig.getEntries()) {
        if (zne.getAlias() == alias) {
            found = true;
            std::string errorMessage;

            CMutableTransaction mutTransaction;

            int32_t index;
            zne.castOutputIndex(index);
            UniValue returnObj(UniValue::VOBJ);
            COutPoint outpoint = COutPoint(uint256S(zne.getTxHash()), index);
            if (mempool.mapZelnodeTxMempool.count(outpoint)) {
                returnObj.pushKV("result", "failed");
                returnObj.pushKV("reason", "Mempool already has a zelnode transaction using this outpoint");
                return returnObj;
            } else if (g_zelnodeCache.InStartTracker(outpoint)) {
                returnObj.pushKV("result", "failed");
                returnObj.pushKV("reason", "Zelnode already started, waiting to be confirmed");
                return returnObj;
            } else if (g_zelnodeCache.InDoSTracker(outpoint)) {
                returnObj.pushKV("result", "failed");
                returnObj.pushKV("reason", "Zelnode already started then not confirmed, in DoS tracker. Must wait until out of DoS tracker to start");
                return returnObj;
            } else if (g_zelnodeCache.InConfirmTracker(outpoint)) {
                returnObj.pushKV("result", "failed");
                returnObj.pushKV("reason", "Zelnode already confirmed and in zelnode list");
                return returnObj;
            }

            mutTransaction.nVersion = ZELNODE_TX_VERSION;

            bool result = activeZelnode.BuildDeterministicStartTx(zne.getPrivKey(), zne.getTxHash(), zne.getOutputIndex(), errorMessage, mutTransaction);

            statusObj.pushKV("result", result ? "successful" : "failed");

            if (result) {
                CReserveKey reservekey(pwalletMain);
                std::string errorMessage;

                if (activeZelnode.SignDeterministicStartTx(mutTransaction, errorMessage)) {
                    CTransaction tx(mutTransaction);

                    CWalletTx walletTx(pwalletMain, tx);
                    pwalletMain->CommitTransaction(walletTx, reservekey);
                    successful++;
                } else {
                    failed++;
                    statusObj.pushKV("errorMessage", errorMessage);
                }
            } else {
                failed++;
                statusObj.pushKV("errorMessage", errorMessage);
            }
            break;
        }
    }

    if (!found) {
        failed++;
        statusObj.pushKV("result", "failed");
        statusObj.pushKV("error", "could not find alias in config. Verify with listzelnodeconf.");
    }

    resultsObj.push_back(statusObj);

    if (fLock)
        pwalletMain->Lock();

    UniValue returnObj(UniValue::VOBJ);
    returnObj.pushKV("overall", strprintf("Successfully started %d zelnodes, failed to start %d, total %d", successful, failed, successful + failed));
    returnObj.pushKV("detail", resultsObj);

    return returnObj;
}


void GetDeterministicListData(UniValue& listData, const std::string& strFilter, const Tier tier) {
    int count = 0;
    for (const auto& item : g_zelnodeCache.mapZelnodeList.at(tier).listConfirmedZelnodes) {

        auto data = g_zelnodeCache.GetZelnodeData(item.out);

        UniValue info(UniValue::VOBJ);

        if (!data.IsNull()) {
            std::string strTxHash = data.collateralIn.GetTxHash();


            CTxDestination payment_destination;
            if (IsAP2SHFluxNodePublicKey(data.collateralPubkey)) {
                GetFluxNodeP2SHDestination(pcoinsTip, data.collateralIn, payment_destination);
            } else {
                payment_destination = data.collateralPubkey.GetID();
            }


            if (strFilter != "" && strTxHash.find(strFilter) == string::npos && HexStr(data.pubKey).find(strFilter) &&
                data.ip.find(strFilter) && EncodeDestination(payment_destination).find(strFilter) == string::npos)
                continue;

            std::string strHost = data.ip;
            CNetAddr node = CNetAddr(strHost, false);
            std::string strNetwork = GetNetworkName(node.GetNetwork());

            info.pushKV("collateral", data.collateralIn.ToFullString());
            info.pushKV("txhash", strTxHash);
            info.pushKV("outidx", data.collateralIn.GetTxIndex());
            info.pushKV("ip", data.ip);
            info.pushKV("network", strNetwork);
            info.pushKV("added_height", data.nAddedBlockHeight);
            info.pushKV("confirmed_height", data.nConfirmedBlockHeight);
            info.pushKV("last_confirmed_height", data.nLastConfirmedBlockHeight);
            info.pushKV("last_paid_height", data.nLastPaidHeight);
            info.pushKV("tier", data.TierToString());
            info.pushKV("payment_address", EncodeDestination(payment_destination));
            info.pushKV("pubkey", HexStr(data.pubKey));
            if (chainActive.Height() >= data.nAddedBlockHeight)
                info.pushKV("activesince", std::to_string(chainActive[data.nAddedBlockHeight]->nTime));
            else
                info.pushKV("activesince", 0);
            if (chainActive.Height() >= data.nLastPaidHeight)
                info.pushKV("lastpaid", std::to_string(chainActive[data.nLastPaidHeight]->nTime));
            else
                info.pushKV("lastpaid", 0);

            if (data.nCollateral > 0) {
                info.pushKV("amount", FormatMoney(data.nCollateral));
            }

            info.pushKV("rank", count++);

            listData.push_back(info);
        }
    }
}

UniValue viewdeterministiczelnodelist(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
                "viewdeterministiczelnodelist ( \"filter\" )\n"
                "\nView the list of deterministric zelnode(s)\n"

                "\nResult:\n"
                "[\n"
                "  {\n"
                "    \"collateral\": n,                       (string) Collateral transaction\n"
                "    \"txhash\": \"hash\",                    (string) Collateral transaction hash\n"
                "    \"outidx\": n,                           (numeric) Collateral transaction output index\n"
                "    \"ip\": \"address\"                      (string) IP address\n"
                "    \"network\": \"network\"                 (string) Network type (IPv4, IPv6, onion)\n"
                "    \"added_height\": \"height\"             (string) Block height when zelnode was added\n"
                "    \"confirmed_height\": \"height\"         (string) Block height when zelnode was confirmed\n"
                "    \"last_confirmed_height\": \"height\"    (string) Last block height when zelnode was confirmed\n"
                "    \"last_paid_height\": \"height\"         (string) Last block height when zelnode was paid\n"
                "    \"tier\": \"type\",                      (string) Tier (CUMULUS/NIMBUS/STRATUS)\n"
                "    \"payment_address\": \"addr\",           (string) Zelnode ZEL address\n"
                "    \"pubkey\": \"key\",                     (string) Zelnode public key used for message broadcasting\n"
                "    \"activesince\": ttt,                    (numeric) The time in seconds since epoch (Jan 1 1970 GMT) zelnode has been active\n"
                "    \"lastpaid\": ttt,                       (numeric) The time in seconds since epoch (Jan 1 1970 GMT) zelnode was last paid\n"
                "    \"rank\": n                              (numberic) rank\n"
                "  }\n"
                "  ,...\n"
                "]\n"

                "\nExamples:\n" +
                HelpExampleCli("viewdeterministiczelnodelist", ""));

    if (IsInitialBlockDownload(Params())) {
        throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "Wait until chain is synced closer to tip");
    }

    // Get filter if any
    std::string strFilter = "";
    if (params.size() == 1) strFilter = params[0].get_str();

    // Create empty list
    UniValue deterministicList(UniValue::VARR);

    // Fill list
    for (int currentTier = CUMULUS; currentTier != LAST; currentTier++)
    {
        GetDeterministicListData(deterministicList, strFilter, (Tier)currentTier);
    }

    // Return list
    return deterministicList;
}

UniValue listzelnodes(const UniValue& params, bool fHelp)
{
    return viewdeterministiczelnodelist(params, fHelp);
}

UniValue getdoslist(const UniValue& params, bool fHelp)
{
    if (fHelp || (params.size() > 0))
        throw runtime_error(
                "getdoslist\n"
                "\nGet a list of all zelnodes in the DOS list\n"

                "\nResult:\n"
                "[\n"
                "  {\n"
                "    \"collateral\": \"hash\",  (string) Collateral transaction hash\n"
                "    \"added_height\": n,   (numeric) Height the zelnode start transaction was added to the chain\n"
                "    \"payment_address\": \"xxx\",   (string) The payment address associated with the zelnode\n"
                "    \"eligible_in\": n,     (numeric) The amount of blocks before the zelnode is eligible to be started again\n"
                "  }\n"
                "  ,...\n"
                "]\n"

                "\nExamples:\n" +
                HelpExampleCli("getdoslist", "") + HelpExampleRpc("getdoslist", ""));

    if (IsDZelnodeActive()) {
        UniValue wholelist(UniValue::VARR);

        std::map<int, std::vector<UniValue>> mapOrderedDosList;

        for (const auto& item : g_zelnodeCache.mapStartTxDosTracker) {

            // Get the data from the item in the map of dox tracking
            const ZelnodeCacheData data = item.second;

            CTxDestination payment_destination;
            if (IsAP2SHFluxNodePublicKey(data.collateralPubkey)) {
                GetFluxNodeP2SHDestination(pcoinsTip, data.collateralIn, payment_destination);
            } else {
                payment_destination = data.collateralPubkey.GetID();
            }

            UniValue info(UniValue::VOBJ);

            info.pushKV("collateral", data.collateralIn.ToFullString());
            info.pushKV("added_height", data.nAddedBlockHeight);
            info.pushKV("payment_address", EncodeDestination(payment_destination));

            int nCurrentHeight = chainActive.Height();
            int nEligibleIn = ZELNODE_DOS_REMOVE_AMOUNT - (nCurrentHeight - data.nAddedBlockHeight);
            info.pushKV("eligible_in",  nEligibleIn);

            if (data.nCollateral > 0) {
                info.pushKV("amount", FormatMoney(data.nCollateral));
            }

            mapOrderedDosList[nEligibleIn].emplace_back(info);
        }

        if (mapOrderedDosList.size()) {
            for (int i = 0; i < ZELNODE_DOS_REMOVE_AMOUNT + 1; i++) {
                if (mapOrderedDosList.count(i)) {
                    for (const auto& item : mapOrderedDosList.at(i)) {
                        wholelist.push_back(item);
                    }
                }
            }
        }

        return wholelist;
    }

    return NullUniValue;
}

UniValue getstartlist(const UniValue& params, bool fHelp)
{
    if (fHelp || (params.size() > 0))
        throw runtime_error(
                "getstartlist\n"
                "\nGet a list of all zelnodes in the start list\n"

                "\nResult:\n"
                "[\n"
                "  {\n"
                "    \"collateral\": \"hash\",  (string) Collateral transaction hash\n"
                "    \"added_height\": n,   (numeric) Height the zelnode start transaction was added to the chain\n"
                "    \"payment_address\": \"xxx\",   (string) The payment address associated with the zelnode\n"
                "    \"expires_in\": n,     (numeric) The amount of blocks before the start transaction expires, unless a confirmation transaction is added to a block\n"
                "  }\n"
                "  ,...\n"
                "]\n"

                "\nExamples:\n" +
                HelpExampleCli("getstartlist", "") + HelpExampleRpc("getstartlist", ""));

    if (IsDZelnodeActive()) {
        UniValue wholelist(UniValue::VARR);

        std::map<int, std::vector<UniValue>> mapOrderedStartList;

        for (const auto& item : g_zelnodeCache.mapStartTxTracker) {

            // Get the data from the item in the map of dox tracking
            const ZelnodeCacheData data = item.second;

            CTxDestination payment_destination;
            if (IsAP2SHFluxNodePublicKey(data.collateralPubkey)) {
                GetFluxNodeP2SHDestination(pcoinsTip, data.collateralIn, payment_destination);
            } else {
                payment_destination = data.collateralPubkey.GetID();
            }

            UniValue info(UniValue::VOBJ);

            info.pushKV("collateral", data.collateralIn.ToFullString());
            info.pushKV("added_height", data.nAddedBlockHeight);
            info.pushKV("payment_address", EncodeDestination(payment_destination));

            int nCurrentHeight = chainActive.Height();
            int nExpiresIn = ZELNODE_START_TX_EXPIRATION_HEIGHT - (nCurrentHeight - data.nAddedBlockHeight);

            info.pushKV("expires_in",  nExpiresIn);

            if (data.nCollateral > 0) {
                info.pushKV("amount", FormatMoney(data.nCollateral));
            }

            mapOrderedStartList[nExpiresIn].emplace_back(info);
        }

        if (mapOrderedStartList.size()) {
            for (int i = 0; i < ZELNODE_START_TX_EXPIRATION_HEIGHT + 1; i++) {
                if (mapOrderedStartList.count(i)) {
                    for (const auto& item : mapOrderedStartList.at(i)) {
                        wholelist.push_back(item);
                    }
                }
            }
        }

        return wholelist;
    }

    return NullUniValue;
}

UniValue getzelnodestatus (const UniValue& params, bool fHelp)
{
    if (fHelp || (params.size() != 0))
        throw runtime_error(
                "getzelnodestatus\n"
                "\nPrint zelnode status\n"

                "\nResult:\n"
                "{\n"
                "  \"status\": \"xxxx\",                    (string) Zelnode status\n"
                "  \"collateral\": n,                       (string) Collateral transaction\n"
                "  \"txhash\": \"xxxx\",                    (string) Collateral transaction hash\n"
                "  \"outidx\": n,                           (numeric) Collateral transaction output index number\n"
                "  \"ip\": \"xxxx\",                        (string) Zelnode network address\n"
                "  \"network\": \"network\",                (string) Network type (IPv4, IPv6, onion)\n"
                "  \"added_height\": \"height\",            (string) Block height when zelnode was added\n"
                "  \"confirmed_height\": \"height\",        (string) Block height when zelnode was confirmed\n"
                "  \"last_confirmed_height\": \"height\",   (string) Last block height when zelnode was confirmed\n"
                "  \"last_paid_height\": \"height\",        (string) Last block height when zelnode was paid\n"
                "  \"tier\": \"type\",                      (string) Tier (CUMULUS/NIMBUS/STRATUS)\n"
                "  \"payment_address\": \"xxxx\",           (string) ZEL address for zelnode payments\n"
                "  \"pubkey\": \"key\",                     (string) Zelnode public key used for message broadcasting\n"
                "  \"activesince\": ttt,                    (numeric) The time in seconds since epoch (Jan 1 1970 GMT) zelnode has been active\n"
                "  \"lastpaid\": ttt,                       (numeric) The time in seconds since epoch (Jan 1 1970 GMT) zelnode was last paid\n"
                "}\n"

                "\nExamples:\n" +
                HelpExampleCli("getzelnodestatus", "") + HelpExampleRpc("getzelnodestatus", ""));

    if (!fZelnode) throw runtime_error("This is not a Flux Node");

    if (IsDZelnodeActive()) {
        int nLocation = ZELNODE_TX_ERROR;
        auto data = g_zelnodeCache.GetZelnodeData(activeZelnode.deterministicOutPoint, &nLocation);

        UniValue info(UniValue::VOBJ);

        if (data.IsNull()) {
            info.pushKV("status", "expired");
            info.pushKV("collateral", activeZelnode.deterministicOutPoint.ToFullString());
        } else {
            std::string strTxHash = data.collateralIn.GetTxHash();
            std::string strHost = data.ip;
            CNetAddr node = CNetAddr(strHost, false);
            std::string strNetwork = GetNetworkName(node.GetNetwork());

            info.pushKV("status", ZelnodeLocationToString(nLocation));
            info.pushKV("collateral", data.collateralIn.ToFullString());
            info.pushKV("txhash", strTxHash);
            info.pushKV("outidx", data.collateralIn.GetTxIndex());
            info.pushKV("ip", data.ip);
            info.pushKV("network", strNetwork);
            info.pushKV("added_height", data.nAddedBlockHeight);
            info.pushKV("confirmed_height", data.nConfirmedBlockHeight);
            info.pushKV("last_confirmed_height", data.nLastConfirmedBlockHeight);
            info.pushKV("last_paid_height", data.nLastPaidHeight);
            info.pushKV("tier", data.TierToString());
            info.pushKV("payment_address", EncodeDestination(data.collateralPubkey.GetID()));
            info.pushKV("pubkey", HexStr(data.pubKey));
            if (chainActive.Height() >= data.nAddedBlockHeight)
                info.pushKV("activesince", std::to_string(chainActive[data.nAddedBlockHeight]->nTime));
            else
                info.pushKV("activesince", 0);
            if (chainActive.Height() >= data.nLastPaidHeight)
                info.pushKV("lastpaid", std::to_string(chainActive[data.nLastPaidHeight]->nTime));
            else
                info.pushKV("lastpaid", 0);

            if (data.nCollateral > 0) {
                info.pushKV("amount", FormatMoney(data.nCollateral));
            }
        }

        return info;
    }

    return NullUniValue;

}

UniValue zelnodecurrentwinner (const UniValue& params, bool fHelp)
{
    if (fHelp || (params.size() != 0))
        throw runtime_error(
                "zelnodecurrentwinner\n"
                "\nGet current zelnode winner\n"

                "\nResult:\n"
                "{\n"
                "  \"protocol\": xxxx,        (numeric) Protocol version\n"
                "  \"txhash\": \"xxxx\",      (string) Collateral transaction hash\n"
                "  \"pubkey\": \"xxxx\",      (string) ZN Public key\n"
                "  \"lastseen\": xxx,       (numeric) Time since epoch of last seen\n"
                "  \"activeseconds\": xxx,  (numeric) Seconds ZN has been active\n"
                "}\n"

                "\nExamples:\n" +
                HelpExampleCli("zelnodecurrentwinner", "") + HelpExampleRpc("zelnodecurrentwinner", ""));

    if (IsDZelnodeActive()) {
        UniValue ret(UniValue::VOBJ);

        for (int currentTier = CUMULUS; currentTier != LAST; currentTier++) {
            CTxDestination dest;
            COutPoint outpoint;
            string strWinner = TierToString(currentTier) + " Winner";
            if (g_zelnodeCache.GetNextPayment(dest, currentTier, outpoint)) {
                UniValue obj(UniValue::VOBJ);
                auto data = g_zelnodeCache.GetZelnodeData(outpoint);
                obj.pushKV("collateral", data.collateralIn.ToFullString());
                obj.pushKV("ip", data.ip);
                obj.pushKV("added_height", data.nAddedBlockHeight);
                obj.pushKV("confirmed_height", data.nConfirmedBlockHeight);
                obj.pushKV("last_confirmed_height", data.nLastConfirmedBlockHeight);
                obj.pushKV("last_paid_height", data.nLastPaidHeight);
                obj.pushKV("tier", TierToString(data.nTier));
                obj.pushKV("payment_address", EncodeDestination(dest));
                ret.pushKV(strWinner, obj);
            }
        }

        return ret;
    }

    return NullUniValue;
}

UniValue getzelnodecount (const UniValue& params, bool fHelp)
{
    if (fHelp || (params.size() > 0))
        throw runtime_error(
                "getzelnodecount\n"
                "\nGet zelnode count values\n"

                "\nResult:\n"
                "{\n"
                "  \"total\": n,        (numeric) Total zelnodes\n"
                "  \"stable\": n,       (numeric) Stable count\n"
                "  \"enabled\": n,      (numeric) Enabled zelnodes\n"
                "  \"inqueue\": n       (numeric) Zelnodes in queue\n"
                "}\n"

                "\nExamples:\n" +
                HelpExampleCli("getzelnodecount", "") + HelpExampleRpc("getzelnodecount", ""));

    UniValue obj(UniValue::VOBJ);

    if (IsDZelnodeActive())
    {
        int ipv4 = 0, ipv6 = 0, onion = 0, nTotal = 0;
        std::vector<int> vNodeCount(GetNumberOfTiers());
        {
            LOCK(g_zelnodeCache.cs);

            g_zelnodeCache.CountNetworks(ipv4, ipv6, onion, vNodeCount);

            nTotal = g_zelnodeCache.mapConfirmedZelnodeData.size();
        }

        obj.pushKV("total", nTotal);
        obj.pushKV("stable", nTotal);

        std::map<int,pair<string,string> > words;
        words.insert(make_pair(0, make_pair("basic-enabled", "cumulus-enabled")));
        words.insert(make_pair(1, make_pair("super-enabled", "nimbus-enabled")));
        words.insert(make_pair(2, make_pair("bamf-enabled", "stratus-enabled")));
        for (int i = 0; i < vNodeCount.size(); i++) {
            if (words.count(i)) {
                obj.pushKV(words.at(i).first, vNodeCount[i]);
            } else {
                obj.pushKV("unnamed-enabled", vNodeCount[i]);
            }
        }

        for (int i = 0; i < vNodeCount.size(); i++) {
            if (words.count(i)) {
                obj.pushKV(words.at(i).second, vNodeCount[i]);
            } else {
                obj.pushKV("unnamed-enabled", vNodeCount[i]);
            }
        }

        obj.pushKV("ipv4", ipv4);
        obj.pushKV("ipv6", ipv6);
        obj.pushKV("onion", onion);

        return obj;
    }

    return NullUniValue;
}

UniValue getmigrationcount (const UniValue& params, bool fHelp)
{
    if (fHelp || (params.size() > 0))
        throw runtime_error(
                "getmigrationcount\n"
                "\nGet zelnode migration count values\n"

                "\nResult:\n"
                "{\n"
                "  \"total-old\": n,        (numeric) Total zelnodes\n"
                "  \"total-new\": n,        (numeric) Total zelnodes\n"
                "}\n"

                "\nExamples:\n" +
                HelpExampleCli("getmigrationcount", "") + HelpExampleRpc("getmigrationcount", ""));

    if (IsDZelnodeActive())
    {
        int nTotalOld = 0;
        int nTotalNew = 0;
        std::vector<int> vOldNodeCount(GetNumberOfTiers());
        std::vector<int> vNewNodeCount(GetNumberOfTiers());
        {
            LOCK(g_zelnodeCache.cs);
            g_zelnodeCache.CountMigration(nTotalOld, nTotalNew,vOldNodeCount, vNewNodeCount);
        }

        std::map<int,pair<string,string> > words;
        words.insert(make_pair(0, make_pair("basic-enabled", "cumulus-enabled")));
        words.insert(make_pair(1, make_pair("super-enabled", "nimbus-enabled")));
        words.insert(make_pair(2, make_pair("bamf-enabled", "stratus-enabled")));

        UniValue oldTierCount(UniValue::VOBJ);
        oldTierCount.pushKV("total-old", nTotalOld);
        for (int i = 0; i < vOldNodeCount.size(); i++) {
            if (words.count(i)) {
                oldTierCount.pushKV(words.at(i).second + "-old", vOldNodeCount[i]);
            } else {
                oldTierCount.pushKV("unnamed-enabled-old", vOldNodeCount[i]);
            }
        }

        UniValue newTierCount(UniValue::VOBJ);
        newTierCount.pushKV("total-new", nTotalNew);
        for (int i = 0; i < vNewNodeCount.size(); i++) {
            if (words.count(i)) {
                newTierCount.pushKV(words.at(i).second + "-new", vNewNodeCount[i]);
            } else {
                newTierCount.pushKV("unnamed-enabled-new", vNewNodeCount[i]);
            }
        }

        UniValue result(UniValue::VARR);

        result.push_back(oldTierCount);
        result.push_back(newTierCount);
        return result;
    }

    return NullUniValue;
}

UniValue listzelnodeconf (const UniValue& params, bool fHelp)
{
    std::string strFilter = "";

    if (params.size() == 1) strFilter = params[0].get_str();

    if (fHelp || (params.size() > 1))
        throw runtime_error(
                "listzelnodeconf ( \"filter\" )\n"
                "\nPrint zelnode.conf in JSON format\n"

                "\nArguments:\n"
                "1. \"filter\"    (string, optional) Filter search text. Partial match on alias, address, txHash, or status.\n"

                "\nResult:\n"
                "[\n"
                "  {\n"
                "    \"alias\": \"xxxx\",                       (string) zelnode alias\n"
                "    \"status\": \"xxxx\",                      (string) zelnode status\n"
                "    \"collateral\": n,                         (string) Collateral transaction\n"
                "    \"txHash\": \"xxxx\",                      (string) transaction hash\n"
                "    \"outputIndex\": n,                        (numeric) transaction output index\n"
                "    \"privateKey\": \"xxxx\",                  (string) zelnode private key\n"
                "    \"address\": \"xxxx\",                     (string) zelnode IP address\n"
                "    \"ip\": \"xxxx\",                          (string) Zelnode network address\n"
                "    \"network\": \"network\",                  (string) Network type (IPv4, IPv6, onion)\n"
                "    \"added_height\": \"height\",              (string) Block height when zelnode was added\n"
                "    \"confirmed_height\": \"height\",          (string) Block height when zelnode was confirmed\n"
                "    \"last_confirmed_height\": \"height\",     (string) Last block height when zelnode was confirmed\n"
                "    \"last_paid_height\": \"height\",          (string) Last block height when zelnode was paid\n"
                "    \"tier\": \"type\",                        (string) Tier (CUMULUS/NIMBUS/STRATUS)\n"
                "    \"payment_address\": \"xxxx\",             (string) ZEL address for zelnode payments\n"
                "    \"activesince\": ttt,                      (numeric) The time in seconds since epoch (Jan 1 1970 GMT) zelnode has been active\n"
                "    \"lastpaid\": ttt,                         (numeric) The time in seconds since epoch (Jan 1 1970 GMT) zelnode was last paid\n"
                "  }\n"
                "  ,...\n"
                "]\n"

                "\nExamples:\n" +
                HelpExampleCli("listzelnodeconf", "") + HelpExampleRpc("listzelnodeconf", ""));

    std::vector<ZelnodeConfig::ZelnodeEntry> zelnodeEntries;
    zelnodeEntries = zelnodeConfig.getEntries();

    UniValue ret(UniValue::VARR);

    for (ZelnodeConfig::ZelnodeEntry zelnode : zelnodeEntries) {
        if (IsDZelnodeActive()) {
            int nIndex;
            if (!zelnode.castOutputIndex(nIndex))
                continue;
            COutPoint out = COutPoint(uint256S(zelnode.getTxHash()), uint32_t(nIndex));

            int nLocation = ZELNODE_TX_ERROR;
            auto data = g_zelnodeCache.GetZelnodeData(out, &nLocation);

            UniValue info(UniValue::VOBJ);
            info.pushKV("alias", zelnode.getAlias());
            info.pushKV("status", ZelnodeLocationToString(nLocation));
            info.pushKV("collateral", out.ToFullString());
            info.pushKV("txHash", zelnode.getTxHash());
            info.pushKV("outputIndex", zelnode.getOutputIndex());
            info.pushKV("privateKey", zelnode.getPrivKey());
            info.pushKV("address", zelnode.getIp());

            if (data.IsNull()) {
                info.pushKV("ip", "UNKNOWN");
                info.pushKV("network", "UNKOWN");
                info.pushKV("added_height", 0);
                info.pushKV("confirmed_height", 0);
                info.pushKV("last_confirmed_height", 0);
                info.pushKV("last_paid_height", 0);
                info.pushKV("tier", "UNKNOWN");
                info.pushKV("payment_address", "UNKNOWN");
                info.pushKV("activesince", 0);
                info.pushKV("lastpaid", 0);
            } else {
                std::string strHost = data.ip;
                CNetAddr node = CNetAddr(strHost, false);
                std::string strNetwork = GetNetworkName(node.GetNetwork());
                info.pushKV("ip", data.ip);
                info.pushKV("network", strNetwork);
                info.pushKV("added_height", data.nAddedBlockHeight);
                info.pushKV("confirmed_height", data.nConfirmedBlockHeight);
                info.pushKV("last_confirmed_height", data.nLastConfirmedBlockHeight);
                info.pushKV("last_paid_height", data.nLastPaidHeight);
                info.pushKV("tier", TierToString(data.nTier));
                info.pushKV("payment_address", EncodeDestination(data.collateralPubkey.GetID()));
                if (chainActive.Height() >= data.nAddedBlockHeight)
                    info.push_back(
                            std::make_pair("activesince", std::to_string(chainActive[data.nAddedBlockHeight]->nTime)));
                else
                    info.pushKV("activesince", 0);
                if (chainActive.Height() >= data.nLastPaidHeight)
                    info.push_back(
                            std::make_pair("lastpaid", std::to_string(chainActive[data.nLastPaidHeight]->nTime)));
                else
                    info.pushKV("lastpaid", 0);
            }

            ret.push_back(info);
            continue;
        }
    }

    return ret;
}


UniValue getbenchmarks(const UniValue& params, bool fHelp)
{
    if (!fZelnode) throw runtime_error("This is not a Flux Node");

    if (fHelp || params.size() != 0)
        throw runtime_error(
                "getbenchmarks\n"
                "\nCommand to test node benchmarks\n"

                "\nExamples:\n" +
                HelpExampleCli("getbenchmarks", "") + HelpExampleRpc("getbenchmarks", ""));

    return GetBenchmarks();
}

UniValue getbenchstatus(const UniValue& params, bool fHelp)
{
    if (!fZelnode) throw runtime_error("This is not a Flux Node");

    if (fHelp || params.size() != 0)
        throw runtime_error(
                "getbenchstatus\n"
                "\nCommand to get status of zelbenchd\n"

                "\nExamples:\n" +
                HelpExampleCli("getbenchstatus", "") + HelpExampleRpc("getbenchstatus", ""));

    return GetZelBenchdStatus();
}


UniValue stopzelbenchd(const UniValue& params, bool fHelp)
{
    if (!fZelnode) throw runtime_error("This is not a Flux Node");

    if (fHelp || params.size() != 0)
        throw runtime_error(
                "stopzelbenchd\n"
                "\nStop zelbenchd\n"

                "\nExamples:\n" +
                HelpExampleCli("stopzelbenchd", "") + HelpExampleRpc("stopzelbenchd", ""));

    if (IsZelBenchdRunning()) {
        StopZelBenchd();
        return "Stopping process";
    }

    return "Not running";
}

UniValue startzelbenchd(const UniValue& params, bool fHelp)
{
    if (!fZelnode) throw runtime_error("This is not a Flux Node");

    if (fHelp || params.size() != 0)
        throw runtime_error(
                "startzelbenchd\n"
                "\nStart zelbenchd\n"

                "\nExamples:\n" +
                HelpExampleCli("startzelbenchd", "") + HelpExampleRpc("startzelbenchd", ""));

    if (!IsZelBenchdRunning()) {
        StartZelBenchd();
        return "Starting process";
    }

    return "Already running";
}




static const CRPCCommand commands[] =
        { //  category              name                      actor (function)         okSafeMode
                //  --------------------- ------------------------  -----------------------  ----------
                { "zelnode",    "createzelnodekey",       &createzelnodekey,       false  },
                { "zelnode",    "getzelnodeoutputs",      &getzelnodeoutputs,      false  },
                { "zelnode",    "startzelnode",           &startzelnode,           false  },
                { "zelnode",    "listzelnodes",           &listzelnodes,           false  },
                { "zelnode",    "getdoslist",             &getdoslist,             false  },
                { "zelnode",    "getstartlist",           &getstartlist,           false  },
                { "zelnode",    "getzelnodecount",        &getzelnodecount,        false  },
                { "zelnode",    "getmigrationcount",      &getmigrationcount,        false },
                { "zelnode",    "zelnodecurrentwinner",   &zelnodecurrentwinner,   false  }, /* uses wallet if enabled */
                { "zelnode",    "getzelnodestatus",       &getzelnodestatus,       false  },
                { "zelnode",    "listzelnodeconf",        &listzelnodeconf,        false  },
                { "hidden",    "rebuildzelnodedb",       &rebuildzelnodedb,       false  },

                {"zelnode",     "startdeterministiczelnode", &startdeterministiczelnode, false },
                {"zelnode",     "viewdeterministiczelnodelist", &viewdeterministiczelnodelist, false },

                { "benchmarks", "getbenchmarks",         &getbenchmarks,           false  },
                { "benchmarks", "getbenchstatus",        &getbenchstatus,          false  },
                { "benchmarks", "stopzelbenchd",        &stopzelbenchd,          false  },
                { "benchmarks", "startzelbenchd",       &startzelbenchd,         false  },

                /** Not shown in help menu */
                { "hidden",    "createsporkkeys",        &createsporkkeys,         false  },
                { "hidden",    "createconfirmationtransaction",        &createconfirmationtransaction,         false  }




        };


void RegisterZelnodeRPCCommands(CRPCTable &tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
