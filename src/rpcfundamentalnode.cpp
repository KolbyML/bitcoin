// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2018 The VITAE developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activefundamentalnode.h"
#include "db.h"
#include "init.h"
#include "main.h"
#include "fundamentalnode-payments.h"
#include "fundamentalnode-sync.h"
#include "fundamentalnodeconfig.h"
#include "fundamentalnodeman.h"
#include "netbase.h"
#include "rpcserver.h"
#include "utilmoneystr.h"

#include <univalue.h>

#include <boost/tokenizer.hpp>

UniValue listfundamentalnodes(const UniValue& params, bool fHelp)
{
    std::string strFilter = "";

    if (params.size() == 1) strFilter = params[0].get_str();

    if (fHelp || (params.size() > 1))
        throw std::runtime_error(
                "listfundamentalnodes ( \"filter\" )\n"
                "\nGet a ranked list of fundamentalnodes\n"

                "\nArguments:\n"
                "1. \"filter\"    (string, optional) Filter search text. Partial match by txhash, status, or addr.\n"

                "\nResult:\n"
                "[\n"
                "  {\n"
                "    \"rank\": n,           (numeric) Fundamentalnode Rank (or 0 if not enabled)\n"
                "    \"txhash\": \"hash\",    (string) Collateral transaction hash\n"
                "    \"outidx\": n,         (numeric) Collateral transaction output index\n"
                "    \"pubkey\": \"key\",   (string) Fundamentalnode public key used for message broadcasting\n"
                "    \"status\": s,         (string) Status (ENABLED/EXPIRED/REMOVE/etc)\n"
                "    \"addr\": \"addr\",      (string) Fundamentalnode PIVX address\n"
                "    \"version\": v,        (numeric) Fundamentalnode protocol version\n"
                "    \"lastseen\": ttt,     (numeric) The time in seconds since epoch (Jan 1 1970 GMT) of the last seen\n"
                "    \"activetime\": ttt,   (numeric) The time in seconds since epoch (Jan 1 1970 GMT) fundamentalnode has been active\n"
                "    \"lastpaid\": ttt,     (numeric) The time in seconds since epoch (Jan 1 1970 GMT) fundamentalnode was last paid\n"
                "  }\n"
                "  ,...\n"
                "]\n"

                "\nExamples:\n" +
                HelpExampleCli("listfundamentalnodes", "") + HelpExampleRpc("listfundamentalnodes", ""));

    UniValue ret(UniValue::VARR);
    int nHeight;
    {
        LOCK(cs_main);
        CBlockIndex* pindex = chainActive.Tip();
        if(!pindex) return 0;
        nHeight = pindex->nHeight;
    }
    std::vector<std::pair<int, CFundamentalnode> > vFundamentalnodeRanks = mnodeman.GetFundamentalnodeRanks(nHeight);
    for (PAIRTYPE(int, CFundamentalnode) & s : vFundamentalnodeRanks) {
        UniValue obj(UniValue::VOBJ);
        std::string strVin = s.second.vin.prevout.ToStringShort();
        std::string strTxHash = s.second.vin.prevout.hash.ToString();
        uint32_t oIdx = s.second.vin.prevout.n;

        CFundamentalnode* mn = mnodeman.Find(s.second.vin);

        if (mn != NULL) {
            if (strFilter != "" && strTxHash.find(strFilter) == std::string::npos &&
                mn->Status().find(strFilter) == std::string::npos &&
                CBitcoinAddress(mn->pubKeyCollateralAddress.GetID()).ToString().find(strFilter) == string::npos) continue;

            std::string strStatus = mn->Status();
            std::string strHost;
            int port;
            SplitHostPort(mn->addr.ToString(), port, strHost);
            CNetAddr node;
            LookupHost(strHost.c_str(), node, false);
            std::string strNetwork = GetNetworkName(node.GetNetwork());

            obj.push_back(Pair("rank", (strStatus == "ENABLED" ? s.first : 0)));
            obj.push_back(Pair("network", strNetwork));
            obj.push_back(Pair("txhash", strTxHash));
            obj.push_back(Pair("outidx", (uint64_t)oIdx));
            obj.push_back(Pair("pubkey", HexStr(mn->pubKeyFundamentalnode)));
            obj.push_back(Pair("status", strStatus));
            obj.push_back(Pair("addr", CBitcoinAddress(mn->pubKeyCollateralAddress.GetID()).ToString()));
            obj.push_back(Pair("version", mn->protocolVersion));
            obj.push_back(Pair("lastseen", (int64_t)mn->lastPing.sigTime));
            obj.push_back(Pair("activetime", (int64_t)(mn->lastPing.sigTime - mn->sigTime)));
            obj.push_back(Pair("lastpaid", (int64_t)mn->GetLastPaid()));

            ret.push_back(obj);
        }
    }

    return ret;
}

UniValue getfundamentalnodecount (const UniValue& params, bool fHelp)
{
    if (fHelp || (params.size() > 0))
        throw std::runtime_error(
                "getfundamentalnodecount\n"
                "\nGet fundamentalnode count values\n"

                "\nResult:\n"
                "{\n"
                "  \"total\": n,        (numeric) Total fundamentalnodes\n"
                "  \"stable\": n,       (numeric) Stable count\n"
                "  \"enabled\": n,      (numeric) Enabled fundamentalnodes\n"
                "  \"inqueue\": n       (numeric) Fundamentalnodes in queue\n"
                "}\n"

                "\nExamples:\n" +
                HelpExampleCli("getfundamentalnodecount", "") + HelpExampleRpc("getfundamentalnodecount", ""));

    UniValue obj(UniValue::VOBJ);
    int nCount = 0;
    int ipv4 = 0, ipv6 = 0, onion = 0;

    if (chainActive.Tip())
        mnodeman.GetNextFundamentalnodeInQueueForPayment(chainActive.Tip()->nHeight, true, nCount);

    mnodeman.CountNetworks(ActiveProtocol(), ipv4, ipv6, onion);

    obj.push_back(Pair("total", mnodeman.size()));
    obj.push_back(Pair("stable", mnodeman.stable_size()));
    obj.push_back(Pair("enabled", mnodeman.CountEnabled()));
    obj.push_back(Pair("inqueue", nCount));
    obj.push_back(Pair("ipv4", ipv4));
    obj.push_back(Pair("ipv6", ipv6));
    obj.push_back(Pair("onion", onion));

    return obj;
}

UniValue fundamentalnodecurrent (const UniValue& params, bool fHelp)
{
    if (fHelp || (params.size() != 0))
        throw std::runtime_error(
                "fundamentalnodecurrent\n"
                "\nGet current fundamentalnode winner (scheduled to be paid next).\n"

                "\nResult:\n"
                "{\n"
                "  \"protocol\": xxxx,        (numeric) Protocol version\n"
                "  \"txhash\": \"xxxx\",      (string) Collateral transaction hash\n"
                "  \"pubkey\": \"xxxx\",      (string) MN Public key\n"
                "  \"lastseen\": xxx,         (numeric) Time since epoch of last seen\n"
                "  \"activeseconds\": xxx,    (numeric) Seconds MN has been active\n"
                "}\n"

                "\nExamples:\n" +
                HelpExampleCli("fundamentalnodecurrent", "") + HelpExampleRpc("fundamentalnodecurrent", ""));

    const int nHeight = WITH_LOCK(cs_main, return chainActive.Height() + 1);
    int nCount = 0;
    CFundamentalnode* winner = mnodeman.GetNextFundamentalnodeInQueueForPayment(nHeight, true, nCount);
    if (winner) {
        UniValue obj(UniValue::VOBJ);
        obj.push_back(Pair("protocol", (int64_t)winner->protocolVersion));
        obj.push_back(Pair("txhash", winner->vin.prevout.hash.ToString()));
        obj.push_back(Pair("pubkey", CBitcoinAddress(winner->pubKeyCollateralAddress.GetID()).ToString()));
        obj.push_back(Pair("lastseen", (winner->lastPing == CFundamentalnodePing()) ? winner->sigTime : (int64_t)winner->lastPing.sigTime));
        obj.push_back(Pair("activeseconds", (winner->lastPing == CFundamentalnodePing()) ? 0 : (int64_t)(winner->lastPing.sigTime - winner->sigTime)));
        return obj;
    }

    throw std::runtime_error("unknown");
}

bool StartFundamentalnodeEntry(UniValue& statusObjRet, CFundamentalnodeBroadcast& mnbRet, bool& fSuccessRet, const CFundamentalnodeConfig::CFundamentalnodeEntry& mne, std::string& errorMessage, std::string strCommand = "")
{
    int nIndex;
    if(!mne.castOutputIndex(nIndex)) {
        return false;
    }

    CTxIn vin = CTxIn(uint256S(mne.getTxHash()), uint32_t(nIndex));
    CFundamentalnode* pmn = mnodeman.Find(vin);
    if (pmn != NULL) {
        if (strCommand == "missing") return false;
        if (strCommand == "disabled" && pmn->IsEnabled()) return false;
    }

    fSuccessRet = CFundamentalnodeBroadcast::Create(mne.getIp(), mne.getPrivKey(), mne.getTxHash(), mne.getOutputIndex(), errorMessage, mnbRet);

    statusObjRet.push_back(Pair("alias", mne.getAlias()));
    statusObjRet.push_back(Pair("result", fSuccessRet ? "success" : "failed"));
    statusObjRet.push_back(Pair("error", fSuccessRet ? "" : errorMessage));

    return true;
}

void RelayMNB(CFundamentalnodeBroadcast& mnb, const bool fSuccess, int& successful, int& failed)
{
    if (fSuccess) {
        successful++;
        mnodeman.UpdateFundamentalnodeList(mnb);
        mnb.Relay();
    } else {
        failed++;
    }
}

void RelayMNB(CFundamentalnodeBroadcast& mnb, const bool fSucces)
{
    int successful = 0, failed = 0;
    return RelayMNB(mnb, fSucces, successful, failed);
}

void SerializeMNB(UniValue& statusObjRet, const CFundamentalnodeBroadcast& mnb, const bool fSuccess, int& successful, int& failed)
{
    if(fSuccess) {
        successful++;
        CDataStream ssMnb(SER_NETWORK, PROTOCOL_VERSION);
        ssMnb << mnb;
        statusObjRet.push_back(Pair("hex", HexStr(ssMnb.begin(), ssMnb.end())));
    } else {
        failed++;
    }
}

void SerializeMNB(UniValue& statusObjRet, const CFundamentalnodeBroadcast& mnb, const bool fSuccess)
{
    int successful = 0, failed = 0;
    return SerializeMNB(statusObjRet, mnb, fSuccess, successful, failed);
}

UniValue startfundamentalnode (const UniValue& params, bool fHelp)
{
    std::string strCommand;
    if (params.size() >= 1) {
        strCommand = params[0].get_str();

        // Backwards compatibility with legacy 'fundamentalnode' super-command forwarder
        if (strCommand == "start") strCommand = "local";
        if (strCommand == "start-alias") strCommand = "alias";
        if (strCommand == "start-all") strCommand = "all";
        if (strCommand == "start-many") strCommand = "many";
        if (strCommand == "start-missing") strCommand = "missing";
        if (strCommand == "start-disabled") strCommand = "disabled";
    }

    if (fHelp || params.size() < 2 || params.size() > 3 ||
        (params.size() == 2 && (strCommand != "local" && strCommand != "all" && strCommand != "many" && strCommand != "missing" && strCommand != "disabled")) ||
        (params.size() == 3 && strCommand != "alias"))
        throw std::runtime_error(
                "startfundamentalnode \"local|all|many|missing|disabled|alias\" lockwallet ( \"alias\" )\n"
                "\nAttempts to start one or more fundamentalnode(s)\n"

                "\nArguments:\n"
                "1. set         (string, required) Specify which set of fundamentalnode(s) to start.\n"
                "2. lockwallet  (boolean, required) Lock wallet after completion.\n"
                "3. alias       (string) Fundamentalnode alias. Required if using 'alias' as the set.\n"

                "\nResult: (for 'local' set):\n"
                "\"status\"     (string) Fundamentalnode status message\n"

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
                HelpExampleCli("startfundamentalnode", "\"alias\" \"0\" \"my_mn\"") + HelpExampleRpc("startfundamentalnode", "\"alias\" \"0\" \"my_mn\""));

    bool fLock = (params[1].get_str() == "true" ? true : false);

    EnsureWalletIsUnlocked();

    if (strCommand == "local") {
        if (!fFundamentalNode) throw std::runtime_error("you must set fundamentalnode=1 in the configuration\n");

        if (activeFundamentalnode.GetStatus() != ACTIVE_FUNDAMENTALNODE_STARTED) {
            activeFundamentalnode.ResetStatus();
            if (fLock)
                pwalletMain->Lock();
        }

        return activeFundamentalnode.GetStatusMessage();
    }

    if (strCommand == "all" || strCommand == "many" || strCommand == "missing" || strCommand == "disabled") {
        if ((strCommand == "missing" || strCommand == "disabled") &&
            (fundamentalnodeSync.RequestedFundamentalnodeAssets <= FUNDAMENTALNODE_SYNC_LIST ||
             fundamentalnodeSync.RequestedFundamentalnodeAssets == FUNDAMENTALNODE_SYNC_FAILED)) {
            throw std::runtime_error("You can't use this command until fundamentalnode list is synced\n");
        }

        std::vector<CFundamentalnodeConfig::CFundamentalnodeEntry> mnEntries;
        mnEntries = fundamentalnodeConfig.getEntries();

        int successful = 0;
        int failed = 0;

        UniValue resultsObj(UniValue::VARR);

        for (CFundamentalnodeConfig::CFundamentalnodeEntry mne : fundamentalnodeConfig.getEntries()) {
            UniValue statusObj(UniValue::VOBJ);
            CFundamentalnodeBroadcast mnb;
            std::string errorMessage;
            bool fSuccess = false;
            if (!StartFundamentalnodeEntry(statusObj, mnb, fSuccess, mne, errorMessage, strCommand))
                continue;
            resultsObj.push_back(statusObj);
            RelayMNB(mnb, fSuccess, successful, failed);
        }
        if (fLock)
            pwalletMain->Lock();

        UniValue returnObj(UniValue::VOBJ);
        returnObj.push_back(Pair("overall", strprintf("Successfully started %d fundamentalnodes, failed to start %d, total %d", successful, failed, successful + failed)));
        returnObj.push_back(Pair("detail", resultsObj));

        return returnObj;
    }

    if (strCommand == "alias") {
        std::string alias = params[2].get_str();

        bool found = false;

        UniValue resultsObj(UniValue::VARR);
        UniValue statusObj(UniValue::VOBJ);

        for (CFundamentalnodeConfig::CFundamentalnodeEntry mne : fundamentalnodeConfig.getEntries()) {
            if (mne.getAlias() == alias) {
                CFundamentalnodeBroadcast mnb;
                found = true;
                std::string errorMessage;
                bool fSuccess = false;
                if (!StartFundamentalnodeEntry(statusObj, mnb, fSuccess, mne, errorMessage, strCommand))
                    continue;
                RelayMNB(mnb, fSuccess);
                break;
            }
        }

        if (fLock)
            pwalletMain->Lock();

        if(!found) {
            statusObj.push_back(Pair("success", false));
            statusObj.push_back(Pair("error_message", "Could not find alias in config. Verify with listfundamentalnodeconf."));
        }

        return statusObj;
    }
    return NullUniValue;
}

UniValue createfundamentalnodekey (const UniValue& params, bool fHelp)
{
    if (fHelp || (params.size() != 0))
        throw std::runtime_error(
                "createfundamentalnodekey\n"
                "\nCreate a new fundamentalnode private key\n"

                "\nResult:\n"
                "\"key\"    (string) Fundamentalnode private key\n"

                "\nExamples:\n" +
                HelpExampleCli("createfundamentalnodekey", "") + HelpExampleRpc("createfundamentalnodekey", ""));

    CKey secret;
    secret.MakeNewKey(false);

    return CBitcoinSecret(secret).ToString();
}

UniValue getfundamentalnodeoutputs (const UniValue& params, bool fHelp)
{
    if (fHelp || (params.size() != 0))
        throw std::runtime_error(
                "getfundamentalnodeoutputs\n"
                "\nPrint all fundamentalnode transaction outputs\n"

                "\nResult:\n"
                "[\n"
                "  {\n"
                "    \"txhash\": \"xxxx\",    (string) output transaction hash\n"
                "    \"outputidx\": n       (numeric) output index number\n"
                "  }\n"
                "  ,...\n"
                "]\n"

                "\nExamples:\n" +
                HelpExampleCli("getfundamentalnodeoutputs", "") + HelpExampleRpc("getfundamentalnodeoutputs", ""));

    // Find possible candidates
    std::vector<COutput> possibleCoins = activeFundamentalnode.SelectCoinsFundamentalnode();

    UniValue ret(UniValue::VARR);
    for (COutput& out : possibleCoins) {
        UniValue obj(UniValue::VOBJ);
        obj.push_back(Pair("txhash", out.tx->GetHash().ToString()));
        obj.push_back(Pair("outputidx", out.i));
        ret.push_back(obj);
    }

    return ret;
}

UniValue listfundamentalnodeconf (const UniValue& params, bool fHelp)
{
    std::string strFilter = "";

    if (params.size() == 1) strFilter = params[0].get_str();

    if (fHelp || (params.size() > 1))
        throw std::runtime_error(
                "listfundamentalnodeconf ( \"filter\" )\n"
                "\nPrint fundamentalnode.conf in JSON format\n"

                "\nArguments:\n"
                "1. \"filter\"    (string, optional) Filter search text. Partial match on alias, address, txHash, or status.\n"

                "\nResult:\n"
                "[\n"
                "  {\n"
                "    \"alias\": \"xxxx\",        (string) fundamentalnode alias\n"
                "    \"address\": \"xxxx\",      (string) fundamentalnode IP address\n"
                "    \"privateKey\": \"xxxx\",   (string) fundamentalnode private key\n"
                "    \"txHash\": \"xxxx\",       (string) transaction hash\n"
                "    \"outputIndex\": n,       (numeric) transaction output index\n"
                "    \"status\": \"xxxx\"        (string) fundamentalnode status\n"
                "  }\n"
                "  ,...\n"
                "]\n"

                "\nExamples:\n" +
                HelpExampleCli("listfundamentalnodeconf", "") + HelpExampleRpc("listfundamentalnodeconf", ""));

    std::vector<CFundamentalnodeConfig::CFundamentalnodeEntry> mnEntries;
    mnEntries = fundamentalnodeConfig.getEntries();

    UniValue ret(UniValue::VARR);

    for (CFundamentalnodeConfig::CFundamentalnodeEntry mne : fundamentalnodeConfig.getEntries()) {
        int nIndex;
        if(!mne.castOutputIndex(nIndex))
            continue;
        CTxIn vin = CTxIn(uint256S(mne.getTxHash()), uint32_t(nIndex));
        CFundamentalnode* pmn = mnodeman.Find(vin);

        std::string strStatus = pmn ? pmn->Status() : "MISSING";

        if (strFilter != "" && mne.getAlias().find(strFilter) == std::string::npos &&
            mne.getIp().find(strFilter) == std::string::npos &&
            mne.getTxHash().find(strFilter) == std::string::npos &&
            strStatus.find(strFilter) == std::string::npos) continue;

        UniValue mnObj(UniValue::VOBJ);
        mnObj.push_back(Pair("alias", mne.getAlias()));
        mnObj.push_back(Pair("address", mne.getIp()));
        mnObj.push_back(Pair("privateKey", mne.getPrivKey()));
        mnObj.push_back(Pair("txHash", mne.getTxHash()));
        mnObj.push_back(Pair("outputIndex", mne.getOutputIndex()));
        mnObj.push_back(Pair("status", strStatus));
        ret.push_back(mnObj);
    }

    return ret;
}

UniValue getfundamentalnodestatus (const UniValue& params, bool fHelp)
{
    if (fHelp || (params.size() != 0))
        throw std::runtime_error(
                "getfundamentalnodestatus\n"
                "\nPrint fundamentalnode status\n"

                "\nResult:\n"
                "{\n"
                "  \"txhash\": \"xxxx\",      (string) Collateral transaction hash\n"
                "  \"outputidx\": n,          (numeric) Collateral transaction output index number\n"
                "  \"netaddr\": \"xxxx\",     (string) Fundamentalnode network address\n"
                "  \"addr\": \"xxxx\",        (string) PIVX address for fundamentalnode payments\n"
                "  \"status\": \"xxxx\",      (string) Fundamentalnode status\n"
                "  \"message\": \"xxxx\"      (string) Fundamentalnode status message\n"
                "}\n"

                "\nExamples:\n" +
                HelpExampleCli("getfundamentalnodestatus", "") + HelpExampleRpc("getfundamentalnodestatus", ""));

    if (!fFundamentalNode)
        throw JSONRPCError(RPC_MISC_ERROR, _("This is not a fundamentalnode."));

    if (activeFundamentalnode.vin == boost::none)
        throw JSONRPCError(RPC_MISC_ERROR, _("Active Fundamentalnode not initialized."));

    CFundamentalnode* pmn = mnodeman.Find(*(activeFundamentalnode.vin));

    if (pmn) {
        UniValue mnObj(UniValue::VOBJ);
        mnObj.push_back(Pair("txhash", activeFundamentalnode.vin->prevout.hash.ToString()));
        mnObj.push_back(Pair("outputidx", (uint64_t)activeFundamentalnode.vin->prevout.n));
        mnObj.push_back(Pair("netaddr", activeFundamentalnode.service.ToString()));
        mnObj.push_back(Pair("addr", CBitcoinAddress(pmn->pubKeyCollateralAddress.GetID()).ToString()));
        mnObj.push_back(Pair("status", activeFundamentalnode.GetStatus()));
        mnObj.push_back(Pair("message", activeFundamentalnode.GetStatusMessage()));
        return mnObj;
    }
    throw std::runtime_error("Fundamentalnode not found in the list of available fundamentalnodes. Current status: "
                             + activeFundamentalnode.GetStatusMessage());
}

UniValue getfundamentalnodewinners (const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 3)
        throw std::runtime_error(
                "getfundamentalnodewinners ( blocks \"filter\" )\n"
                "\nPrint the fundamentalnode winners for the last n blocks\n"

                "\nArguments:\n"
                "1. blocks      (numeric, optional) Number of previous blocks to show (default: 10)\n"
                "2. filter      (string, optional) Search filter matching MN address\n"

                "\nResult (single winner):\n"
                "[\n"
                "  {\n"
                "    \"nHeight\": n,           (numeric) block height\n"
                "    \"winner\": {\n"
                "      \"address\": \"xxxx\",    (string) PIVX MN Address\n"
                "      \"nVotes\": n,          (numeric) Number of votes for winner\n"
                "    }\n"
                "  }\n"
                "  ,...\n"
                "]\n"

                "\nResult (multiple winners):\n"
                "[\n"
                "  {\n"
                "    \"nHeight\": n,           (numeric) block height\n"
                "    \"winner\": [\n"
                "      {\n"
                "        \"address\": \"xxxx\",  (string) PIVX MN Address\n"
                "        \"nVotes\": n,        (numeric) Number of votes for winner\n"
                "      }\n"
                "      ,...\n"
                "    ]\n"
                "  }\n"
                "  ,...\n"
                "]\n"

                "\nExamples:\n" +
                HelpExampleCli("getfundamentalnodewinners", "") + HelpExampleRpc("getfundamentalnodewinners", ""));

    int nHeight;
    {
        LOCK(cs_main);
        CBlockIndex* pindex = chainActive.Tip();
        if(!pindex) return 0;
        nHeight = pindex->nHeight;
    }

    int nLast = 10;
    std::string strFilter = "";

    if (params.size() >= 1)
        nLast = atoi(params[0].get_str());

    if (params.size() == 2)
        strFilter = params[1].get_str();

    UniValue ret(UniValue::VARR);

    for (int i = nHeight - nLast; i < nHeight + 20; i++) {
        UniValue obj(UniValue::VOBJ);
        obj.push_back(Pair("nHeight", i));

        std::string strPayment = GetFundamentalnodeRequiredPaymentsString(i);
        if (strFilter != "" && strPayment.find(strFilter) == std::string::npos) continue;

        if (strPayment.find(',') != std::string::npos) {
            UniValue winner(UniValue::VARR);
            boost::char_separator<char> sep(",");
            boost::tokenizer< boost::char_separator<char> > tokens(strPayment, sep);
            for (const std::string& t : tokens) {
                UniValue addr(UniValue::VOBJ);
                std::size_t pos = t.find(":");
                std::string strAddress = t.substr(0,pos);
                uint64_t nVotes = atoi(t.substr(pos+1));
                addr.push_back(Pair("address", strAddress));
                addr.push_back(Pair("nVotes", nVotes));
                winner.push_back(addr);
            }
            obj.push_back(Pair("winner", winner));
        } else if (strPayment.find("Unknown") == std::string::npos) {
            UniValue winner(UniValue::VOBJ);
            std::size_t pos = strPayment.find(":");
            std::string strAddress = strPayment.substr(0,pos);
            uint64_t nVotes = atoi(strPayment.substr(pos+1));
            winner.push_back(Pair("address", strAddress));
            winner.push_back(Pair("nVotes", nVotes));
            obj.push_back(Pair("winner", winner));
        } else {
            UniValue winner(UniValue::VOBJ);
            winner.push_back(Pair("address", strPayment));
            winner.push_back(Pair("nVotes", 0));
            obj.push_back(Pair("winner", winner));
        }

        ret.push_back(obj);
    }

    return ret;
}

UniValue getfundamentalnodescores (const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw std::runtime_error(
                "getfundamentalnodescores ( blocks )\n"
                "\nPrint list of winning fundamentalnode by score\n"

                "\nArguments:\n"
                "1. blocks      (numeric, optional) Show the last n blocks (default 10)\n"

                "\nResult:\n"
                "{\n"
                "  xxxx: \"xxxx\"   (numeric : string) Block height : Fundamentalnode hash\n"
                "  ,...\n"
                "}\n"

                "\nExamples:\n" +
                HelpExampleCli("getfundamentalnodescores", "") + HelpExampleRpc("getfundamentalnodescores", ""));

    int nLast = 10;

    if (params.size() == 1) {
        try {
            nLast = std::stoi(params[0].get_str());
        } catch (const std::invalid_argument&) {
            throw std::runtime_error("Exception on param 2");
        }
    }
    UniValue obj(UniValue::VOBJ);

    std::vector<CFundamentalnode> vFundamentalnodes = mnodeman.GetFullFundamentalnodeVector();
    for (int nHeight = chainActive.Tip()->nHeight - nLast; nHeight < chainActive.Tip()->nHeight + 20; nHeight++) {
        uint256 nHigh;
        CFundamentalnode* pBestFundamentalnode = NULL;
        for (CFundamentalnode& mn : vFundamentalnodes) {
            uint256 n = mn.CalculateScore(1, nHeight - 100);
            if (n > nHigh) {
                nHigh = n;
                pBestFundamentalnode = &mn;
            }
        }
        if (pBestFundamentalnode)
            obj.push_back(Pair(strprintf("%d", nHeight), pBestFundamentalnode->vin.prevout.hash.ToString().c_str()));
    }

    return obj;
}

bool DecodeHexMnb(CFundamentalnodeBroadcast& mnb, std::string strHexMnb) {

    if (!IsHex(strHexMnb))
        return false;

    std::vector<unsigned char> mnbData(ParseHex(strHexMnb));
    CDataStream ssData(mnbData, SER_NETWORK, PROTOCOL_VERSION);
    try {
        ssData >> mnb;
    }
    catch (const std::exception&) {
        return false;
    }

    return true;
}
UniValue createfundamentalnodebroadcast(const UniValue& params, bool fHelp)
{
    std::string strCommand;
    if (params.size() >= 1)
        strCommand = params[0].get_str();
    if (fHelp || (strCommand != "alias" && strCommand != "all") || (strCommand == "alias" && params.size() < 2))
        throw std::runtime_error(
                "createfundamentalnodebroadcast \"command\" ( \"alias\")\n"
                "\nCreates a fundamentalnode broadcast message for one or all fundamentalnodes configured in fundamentalnode.conf\n" +
                HelpRequiringPassphrase() + "\n"

                                            "\nArguments:\n"
                                            "1. \"command\"      (string, required) \"alias\" for single fundamentalnode, \"all\" for all fundamentalnodes\n"
                                            "2. \"alias\"        (string, required if command is \"alias\") Alias of the fundamentalnode\n"

                                            "\nResult (all):\n"
                                            "{\n"
                                            "  \"overall\": \"xxx\",        (string) Overall status message indicating number of successes.\n"
                                            "  \"detail\": [                (array) JSON array of broadcast objects.\n"
                                            "    {\n"
                                            "      \"alias\": \"xxx\",      (string) Alias of the fundamentalnode.\n"
                                            "      \"success\": true|false, (boolean) Success status.\n"
                                            "      \"hex\": \"xxx\"         (string, if success=true) Hex encoded broadcast message.\n"
                                            "      \"error_message\": \"xxx\"   (string, if success=false) Error message, if any.\n"
                                            "    }\n"
                                            "    ,...\n"
                                            "  ]\n"
                                            "}\n"

                                            "\nResult (alias):\n"
                                            "{\n"
                                            "  \"alias\": \"xxx\",      (string) Alias of the fundamentalnode.\n"
                                            "  \"success\": true|false, (boolean) Success status.\n"
                                            "  \"hex\": \"xxx\"         (string, if success=true) Hex encoded broadcast message.\n"
                                            "  \"error_message\": \"xxx\"   (string, if success=false) Error message, if any.\n"
                                            "}\n"

                                            "\nExamples:\n" +
                HelpExampleCli("createfundamentalnodebroadcast", "alias mymn1") + HelpExampleRpc("createfundamentalnodebroadcast", "alias mymn1"));

    EnsureWalletIsUnlocked();

    if (strCommand == "alias")
    {
        // wait for reindex and/or import to finish
        if (fImporting || fReindex)
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Wait for reindex and/or import to finish");

        std::string alias = params[1].get_str();
        bool found = false;

        UniValue statusObj(UniValue::VOBJ);
        statusObj.push_back(Pair("alias", alias));

        for (CFundamentalnodeConfig::CFundamentalnodeEntry mne : fundamentalnodeConfig.getEntries()) {
            if(mne.getAlias() == alias) {
                CFundamentalnodeBroadcast mnb;
                found = true;
                std::string errorMessage;
                bool fSuccess = false;
                if (!StartFundamentalnodeEntry(statusObj, mnb, fSuccess, mne, errorMessage, strCommand))
                    continue;
                SerializeMNB(statusObj, mnb, fSuccess);
                break;
            }
        }

        if(!found) {
            statusObj.push_back(Pair("success", false));
            statusObj.push_back(Pair("error_message", "Could not find alias in config. Verify with listfundamentalnodeconf."));
        }

        return statusObj;
    }

    if (strCommand == "all")
    {
        // wait for reindex and/or import to finish
        if (fImporting || fReindex)
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Wait for reindex and/or import to finish");

        std::vector<CFundamentalnodeConfig::CFundamentalnodeEntry> mnEntries;
        mnEntries = fundamentalnodeConfig.getEntries();

        int successful = 0;
        int failed = 0;

        UniValue resultsObj(UniValue::VARR);

        for (CFundamentalnodeConfig::CFundamentalnodeEntry mne : fundamentalnodeConfig.getEntries()) {
            UniValue statusObj(UniValue::VOBJ);
            CFundamentalnodeBroadcast mnb;
            std::string errorMessage;
            bool fSuccess = false;
            if (!StartFundamentalnodeEntry(statusObj, mnb, fSuccess, mne, errorMessage, strCommand))
                continue;
            SerializeMNB(statusObj, mnb, fSuccess, successful, failed);
            resultsObj.push_back(statusObj);
        }

        UniValue returnObj(UniValue::VOBJ);
        returnObj.push_back(Pair("overall", strprintf("Successfully created broadcast messages for %d fundamentalnodes, failed to create %d, total %d", successful, failed, successful + failed)));
        returnObj.push_back(Pair("detail", resultsObj));

        return returnObj;
    }
    return NullUniValue;
}

UniValue decodefundamentalnodebroadcast(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw std::runtime_error(
                "decodefundamentalnodebroadcast \"hexstring\"\n"
                "\nCommand to decode fundamentalnode broadcast messages\n"

                "\nArgument:\n"
                "1. \"hexstring\"        (string) The hex encoded fundamentalnode broadcast message\n"

                "\nResult:\n"
                "{\n"
                "  \"vin\": \"xxxx\"                (string) The unspent output which is holding the fundamentalnode collateral\n"
                "  \"addr\": \"xxxx\"               (string) IP address of the fundamentalnode\n"
                "  \"pubkeycollateral\": \"xxxx\"   (string) Collateral address's public key\n"
                "  \"pubkeyfundamentalnode\": \"xxxx\"   (string) Fundamentalnode's public key\n"
                "  \"vchsig\": \"xxxx\"             (string) Base64-encoded signature of this message (verifiable via pubkeycollateral)\n"
                "  \"sigtime\": \"nnn\"             (numeric) Signature timestamp\n"
                "  \"sigvalid\": \"xxx\"            (string) \"true\"/\"false\" whether or not the mnb signature checks out.\n"
                "  \"protocolversion\": \"nnn\"     (numeric) Fundamentalnode's protocol version\n"
                "  \"nlastdsq\": \"nnn\"            (numeric) The last time the fundamentalnode sent a DSQ message (for mixing) (DEPRECATED)\n"
                "  \"nMessVersion\": \"nnn\"        (numeric) MNB Message version number\n"
                "  \"lastping\" : {                 (object) JSON object with information about the fundamentalnode's last ping\n"
                "      \"vin\": \"xxxx\"            (string) The unspent output of the fundamentalnode which is signing the message\n"
                "      \"blockhash\": \"xxxx\"      (string) Current chaintip blockhash minus 12\n"
                "      \"sigtime\": \"nnn\"         (numeric) Signature time for this ping\n"
                "      \"sigvalid\": \"xxx\"        (string) \"true\"/\"false\" whether or not the mnp signature checks out.\n"
                "      \"vchsig\": \"xxxx\"         (string) Base64-encoded signature of this ping (verifiable via pubkeyfundamentalnode)\n"
                "      \"nMessVersion\": \"nnn\"    (numeric) MNP Message version number\n"
                "  }\n"
                "}\n"

                "\nExamples:\n" +
                HelpExampleCli("decodefundamentalnodebroadcast", "hexstring") + HelpExampleRpc("decodefundamentalnodebroadcast", "hexstring"));

    CFundamentalnodeBroadcast mnb;

    if (!DecodeHexMnb(mnb, params[0].get_str()))
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Fundamentalnode broadcast message decode failed");

    UniValue resultObj(UniValue::VOBJ);

    resultObj.push_back(Pair("vin", mnb.vin.prevout.ToString()));
    resultObj.push_back(Pair("addr", mnb.addr.ToString()));
    resultObj.push_back(Pair("pubkeycollateral", CBitcoinAddress(mnb.pubKeyCollateralAddress.GetID()).ToString()));
    resultObj.push_back(Pair("pubkeyfundamentalnode", CBitcoinAddress(mnb.pubKeyFundamentalnode.GetID()).ToString()));
    resultObj.push_back(Pair("vchsig", mnb.GetSignatureBase64()));
    resultObj.push_back(Pair("sigtime", mnb.sigTime));
    resultObj.push_back(Pair("sigvalid", mnb.CheckSignature() ? "true" : "false"));
    resultObj.push_back(Pair("protocolversion", mnb.protocolVersion));
    resultObj.push_back(Pair("nlastdsq", mnb.nLastDsq));
    resultObj.push_back(Pair("nMessVersion", mnb.nMessVersion));

    UniValue lastPingObj(UniValue::VOBJ);
    lastPingObj.push_back(Pair("vin", mnb.lastPing.vin.prevout.ToString()));
    lastPingObj.push_back(Pair("blockhash", mnb.lastPing.blockHash.ToString()));
    lastPingObj.push_back(Pair("sigtime", mnb.lastPing.sigTime));
    lastPingObj.push_back(Pair("sigvalid", mnb.lastPing.CheckSignature(mnb.pubKeyFundamentalnode) ? "true" : "false"));
    lastPingObj.push_back(Pair("vchsig", mnb.lastPing.GetSignatureBase64()));
    lastPingObj.push_back(Pair("nMessVersion", mnb.lastPing.nMessVersion));

    resultObj.push_back(Pair("lastping", lastPingObj));

    return resultObj;
}

UniValue relayfundamentalnodebroadcast(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw std::runtime_error(
                "relayfundamentalnodebroadcast \"hexstring\"\n"
                "\nCommand to relay fundamentalnode broadcast messages\n"

                "\nArguments:\n"
                "1. \"hexstring\"        (string) The hex encoded fundamentalnode broadcast message\n"

                "\nExamples:\n" +
                HelpExampleCli("relayfundamentalnodebroadcast", "hexstring") + HelpExampleRpc("relayfundamentalnodebroadcast", "hexstring"));


    CFundamentalnodeBroadcast mnb;

    if (!DecodeHexMnb(mnb, params[0].get_str()))
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Fundamentalnode broadcast message decode failed");

    if(!mnb.CheckSignature())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Fundamentalnode broadcast signature verification failed");

    mnodeman.UpdateFundamentalnodeList(mnb);
    mnb.Relay();

    return strprintf("Fundamentalnode broadcast sent (service %s, vin %s)", mnb.addr.ToString(), mnb.vin.ToString());
}
