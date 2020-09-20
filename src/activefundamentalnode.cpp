// Copyright (c) 2014-2016 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activefundamentalnode.h"

#include "addrman.h"
#include "fundamentalnode-sync.h"
#include "fundamentalnode.h"
#include "fundamentalnodeconfig.h"
#include "fundamentalnodeman.h"
#include "messagesigner.h"
#include "netbase.h"
#include "protocol.h"

//
// Bootup the Fundamentalnode, look for a 10000 PIVX input and register on the network
//
void CActiveFundamentalnode::ManageStatus()
{
    std::string errorMessage;

    if (!fFundamentalNode) return;

    if (fDebug) LogPrintf("CActiveFundamentalnode::ManageStatus() - Begin\n");

    //need correct blocks to send ping
    if (Params().NetworkID() != CBaseChainParams::REGTEST && !fundamentalnodeSync.IsBlockchainSynced()) {
        status = ACTIVE_FUNDAMENTALNODE_SYNC_IN_PROCESS;
        LogPrintf("CActiveFundamentalnode::ManageStatus() - %s\n", GetStatusMessage());
        return;
    }

    if (status == ACTIVE_FUNDAMENTALNODE_SYNC_IN_PROCESS) status = ACTIVE_FUNDAMENTALNODE_INITIAL;

    if (status == ACTIVE_FUNDAMENTALNODE_INITIAL) {
        CFundamentalnode* pmn;
        pmn = mnodeman.Find(pubKeyFundamentalnode);
        if (pmn != nullptr) {
            pmn->Check();
            if (pmn->IsEnabled() && pmn->protocolVersion == PROTOCOL_VERSION)
                EnableHotColdFundamentalNode(pmn->vin, pmn->addr);
        }
    }

    if (status != ACTIVE_FUNDAMENTALNODE_STARTED) {
        // Set defaults
        status = ACTIVE_FUNDAMENTALNODE_NOT_CAPABLE;
        notCapableReason = "";

        if (pwalletMain->IsLocked()) {
            notCapableReason = "Wallet is locked.";
            LogPrintf("CActiveFundamentalnode::ManageStatus() - not capable: %s\n", notCapableReason);
            return;
        }

        if (pwalletMain->GetBalance() == 0) {
            notCapableReason = "Hot node, waiting for remote activation.";
            LogPrintf("CActiveFundamentalnode::ManageStatus() - not capable: %s\n", notCapableReason);
            return;
        }

        if (strFundamentalNodeAddr.empty()) {
            if (!GetLocal(service)) {
                notCapableReason = "Can't detect external address. Please use the fundamentalnodeaddr configuration option.";
                LogPrintf("CActiveFundamentalnode::ManageStatus() - not capable: %s\n", notCapableReason);
                return;
            }
        } else {
            int nPort;
            std::string strHost;
            SplitHostPort(strFundamentalNodeAddr, nPort, strHost);
            service = LookupNumeric(strHost.c_str(), nPort);
        }

        // The service needs the correct default port to work properly
        if (!CFundamentalnodeBroadcast::CheckDefaultPort(service, errorMessage, "CActiveFundamentalnode::ManageStatus()"))
            return;

        LogPrintf("CActiveFundamentalnode::ManageStatus() - Checking inbound connection to '%s'\n", service.ToString());

        CAddress addr(service, NODE_NETWORK);
        if (!OpenNetworkConnection(addr, nullptr)) {
            notCapableReason = "Could not connect to " + service.ToString();
            LogPrintf("CActiveFundamentalnode::ManageStatus() - not capable: %s\n", notCapableReason);
            return;
        }
    }

    //send to all peers
    if (!SendFundamentalnodePing(errorMessage)) {
        LogPrintf("CActiveFundamentalnode::ManageStatus() - Error on Ping: %s\n", errorMessage);
    }
}

void CActiveFundamentalnode::ResetStatus()
{
    status = ACTIVE_FUNDAMENTALNODE_INITIAL;
    ManageStatus();
}

std::string CActiveFundamentalnode::GetStatusMessage() const
{
    switch (status) {
        case ACTIVE_FUNDAMENTALNODE_INITIAL:
            return "Node just started, not yet activated";
        case ACTIVE_FUNDAMENTALNODE_SYNC_IN_PROCESS:
            return "Sync in progress. Must wait until sync is complete to start Fundamentalnode";
        case ACTIVE_FUNDAMENTALNODE_NOT_CAPABLE:
            return "Not capable fundamentalnode: " + notCapableReason;
        case ACTIVE_FUNDAMENTALNODE_STARTED:
            return "Fundamentalnode successfully started";
        default:
            return "unknown";
    }
}

bool CActiveFundamentalnode::SendFundamentalnodePing(std::string& errorMessage)
{
    if (vin == boost::none) {
        errorMessage = "Active Fundamentalnode not initialized";
        return false;
    }

    if (status != ACTIVE_FUNDAMENTALNODE_STARTED) {
        errorMessage = "Fundamentalnode is not in a running status";
        return false;
    }

    CPubKey pubKeyFundamentalnode;
    CKey keyFundamentalnode;

    if (!CMessageSigner::GetKeysFromSecret(strFundamentalNodePrivKey, keyFundamentalnode, pubKeyFundamentalnode)) {
        errorMessage = "Error upon calling GetKeysFromSecret.\n";
        return false;
    }

    LogPrintf("CActiveFundamentalnode::SendFundamentalnodePing() - Relay Fundamentalnode Ping vin = %s\n", vin->ToString());

    CFundamentalnodePing mnp(*vin);
    if (!mnp.Sign(keyFundamentalnode, pubKeyFundamentalnode)) {
        errorMessage = "Couldn't sign Fundamentalnode Ping";
        return false;
    }

    // Update lastPing for our fundamentalnode in Fundamentalnode list
    CFundamentalnode* pmn = mnodeman.Find(*vin);
    if (pmn != NULL) {
        if (pmn->IsPingedWithin(FUNDAMENTALNODE_PING_SECONDS, mnp.sigTime)) {
            errorMessage = "Too early to send Fundamentalnode Ping";
            return false;
        }

        pmn->lastPing = mnp;
        mnodeman.mapSeenFundamentalnodePing.insert(std::make_pair(mnp.GetHash(), mnp));

        //mnodeman.mapSeenFundamentalnodeBroadcast.lastPing is probably outdated, so we'll update it
        CFundamentalnodeBroadcast mnb(*pmn);
        uint256 hash = mnb.GetHash();
        if (mnodeman.mapSeenFundamentalnodeBroadcast.count(hash)) mnodeman.mapSeenFundamentalnodeBroadcast[hash].lastPing = mnp;

        mnp.Relay();
        return true;

    } else {
        // Seems like we are trying to send a ping while the Fundamentalnode is not registered in the network
        errorMessage = "Fundamentalnode List doesn't include our Fundamentalnode, shutting down Fundamentalnode pinging service! " + vin->ToString();
        status = ACTIVE_FUNDAMENTALNODE_NOT_CAPABLE;
        notCapableReason = errorMessage;
        return false;
    }
}

// get all possible outputs for running Fundamentalnode
std::vector<COutput> CActiveFundamentalnode::SelectCoinsFundamentalnode()
{
    std::vector<COutput> vCoins;
    std::vector<COutput> filteredCoins;
    std::vector<COutPoint> confLockedCoins;

    // Temporary unlock MN coins from fundamentalnode.conf
    if (GetBoolArg("-mnconflock", true)) {
        uint256 mnTxHash;
        for (CFundamentalnodeConfig::CFundamentalnodeEntry mne : fundamentalnodeConfig.getEntries()) {
            mnTxHash.SetHex(mne.getTxHash());

            int nIndex;
            if(!mne.castOutputIndex(nIndex))
                continue;

            COutPoint outpoint = COutPoint(mnTxHash, nIndex);
            confLockedCoins.push_back(outpoint);
            pwalletMain->UnlockCoin(outpoint);
        }
    }

    // Retrieve all possible outputs
    pwalletMain->AvailableCoins(vCoins);

    // Lock MN coins from fundamentalnode.conf back if they where temporary unlocked
    if (!confLockedCoins.empty()) {
        for (COutPoint outpoint : confLockedCoins)
            pwalletMain->LockCoin(outpoint);
    }

    // Filter
    for (const COutput& out : vCoins) {
        if (out.tx->vout[out.i].nValue == 10000 * COIN) { //exactly
            filteredCoins.push_back(out);
        }
    }
    return filteredCoins;
}

// when starting a Fundamentalnode, this can enable to run as a hot wallet with no funds
bool CActiveFundamentalnode::EnableHotColdFundamentalNode(CTxIn& newVin, CService& newService)
{
    if (!fFundamentalNode) return false;

    status = ACTIVE_FUNDAMENTALNODE_STARTED;

    //The values below are needed for signing mnping messages going forward
    vin = newVin;
    service = newService;

    LogPrintf("CActiveFundamentalnode::EnableHotColdFundamentalNode() - Enabled! You may shut down the cold daemon.\n");

    return true;
}
