// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "fundamentalnode.h"

#include "addrman.h"
#include "init.h"
#include "fundamentalnode-payments.h"
#include "fundamentalnode-sync.h"
#include "fundamentalnodeman.h"
#include "netbase.h"
#include "sync.h"
#include "util.h"
#include "wallet.h"

// keep track of the scanning errors I've seen
std::map<uint256, int> mapSeenFundamentalnodeScanningErrors;
// cache block hashes as we calculate them
std::map<int64_t, uint256> mapFundamentalnodeCacheBlockHashes;

//Get the last hash that matches the modulus given. Processed in reverse order
bool GetFundamentalnodeBlockHash(uint256& hash, int nBlockHeight)
{
    if (chainActive.Tip() == NULL) return false;

    if (nBlockHeight == 0)
        nBlockHeight = chainActive.Tip()->nHeight;

    if (mapFundamentalnodeCacheBlockHashes.count(nBlockHeight)) {
        hash = mapFundamentalnodeCacheBlockHashes[nBlockHeight];
        return true;
    }

    const CBlockIndex* BlockLastSolved = chainActive.Tip();
    const CBlockIndex* BlockReading = chainActive.Tip();

    if (BlockLastSolved == NULL || BlockLastSolved->nHeight == 0 || chainActive.Tip()->nHeight + 1 < nBlockHeight) return false;

    int nBlocksAgo = 0;
    if (nBlockHeight > 0) nBlocksAgo = (chainActive.Tip()->nHeight + 1) - nBlockHeight;
    assert(nBlocksAgo >= 0);

    int n = 0;
    for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
        if (n >= nBlocksAgo) {
            hash = BlockReading->GetBlockHash();
            mapFundamentalnodeCacheBlockHashes[nBlockHeight] = hash;
            return true;
        }
        n++;

        if (BlockReading->pprev == NULL) {
            assert(BlockReading);
            break;
        }
        BlockReading = BlockReading->pprev;
    }

    return false;
}

CFundamentalnode::CFundamentalnode() :
        CSignedMessage()
{
    LOCK(cs);
    vin = CTxIn();
    addr = CService();
    pubKeyCollateralAddress = CPubKey();
    pubKeyFundamentalnode = CPubKey();
    activeState = FUNDAMENTALNODE_ENABLED;
    sigTime = GetAdjustedTime();
    lastPing = CFundamentalnodePing();
    cacheInputAge = 0;
    cacheInputAgeBlock = 0;
    unitTest = false;
    allowFreeTx = true;
    nActiveState = FUNDAMENTALNODE_ENABLED;
    protocolVersion = PROTOCOL_VERSION;
    nLastDsq = 0;
    nScanningErrorCount = 0;
    nLastScanningErrorBlockHeight = 0;
    lastTimeChecked = 0;
}

CFundamentalnode::CFundamentalnode(const CFundamentalnode& other) :
        CSignedMessage(other)
{
    LOCK(cs);
    vin = other.vin;
    addr = other.addr;
    pubKeyCollateralAddress = other.pubKeyCollateralAddress;
    pubKeyFundamentalnode = other.pubKeyFundamentalnode;
    activeState = other.activeState;
    sigTime = other.sigTime;
    lastPing = other.lastPing;
    cacheInputAge = other.cacheInputAge;
    cacheInputAgeBlock = other.cacheInputAgeBlock;
    unitTest = other.unitTest;
    allowFreeTx = other.allowFreeTx;
    nActiveState = FUNDAMENTALNODE_ENABLED;
    protocolVersion = other.protocolVersion;
    nLastDsq = other.nLastDsq;
    nScanningErrorCount = other.nScanningErrorCount;
    nLastScanningErrorBlockHeight = other.nLastScanningErrorBlockHeight;
    lastTimeChecked = 0;
}

uint256 CFundamentalnode::GetSignatureHash() const
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << nMessVersion;
    ss << addr;
    ss << sigTime;
    ss << pubKeyCollateralAddress;
    ss << pubKeyFundamentalnode;
    ss << protocolVersion;
    return ss.GetHash();
}

std::string CFundamentalnode::GetStrMessage() const
{
    return (addr.ToString() +
            std::to_string(sigTime) +
            pubKeyCollateralAddress.GetID().ToString() +
            pubKeyFundamentalnode.GetID().ToString() +
            std::to_string(protocolVersion)
    );
}

//
// When a new fundamentalnode broadcast is sent, update our information
//
bool CFundamentalnode::UpdateFromNewBroadcast(CFundamentalnodeBroadcast& mnb)
{
    if (mnb.sigTime > sigTime) {
        pubKeyFundamentalnode = mnb.pubKeyFundamentalnode;
        pubKeyCollateralAddress = mnb.pubKeyCollateralAddress;
        sigTime = mnb.sigTime;
        vchSig = mnb.vchSig;
        protocolVersion = mnb.protocolVersion;
        addr = mnb.addr;
        lastTimeChecked = 0;
        int nDoS = 0;
        if (mnb.lastPing == CFundamentalnodePing() || (mnb.lastPing != CFundamentalnodePing() && mnb.lastPing.CheckAndUpdate(nDoS, false))) {
            lastPing = mnb.lastPing;
            mnodeman.mapSeenFundamentalnodePing.insert(std::make_pair(lastPing.GetHash(), lastPing));
        }
        return true;
    }
    return false;
}

//
// Deterministically calculate a given "score" for a Fundamentalnode depending on how close it's hash is to
// the proof of work for that block. The further away they are the better, the furthest will win the election
// and get paid this block
//
uint256 CFundamentalnode::CalculateScore(int mod, int64_t nBlockHeight)
{
    if (chainActive.Tip() == NULL) return uint256();

    uint256 hash;
    uint256 aux = vin.prevout.hash + vin.prevout.n;

    if (!GetFundamentalnodeBlockHash(hash, nBlockHeight)) {
        LogPrint("fundamentalnode","CalculateScore ERROR - nHeight %d - Returned 0\n", nBlockHeight);
        return uint256();
    }

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << hash;
    uint256 hash2 = ss.GetHash();

    CHashWriter ss2(SER_GETHASH, PROTOCOL_VERSION);
    ss2 << hash;
    ss2 << aux;
    uint256 hash3 = ss2.GetHash();

    uint256 r = (hash3 > hash2 ? hash3 - hash2 : hash2 - hash3);

    return r;
}

void CFundamentalnode::Check(bool forceCheck)
{
    if (ShutdownRequested()) return;

    if (!forceCheck && (GetTime() - lastTimeChecked < FUNDAMENTALNODE_CHECK_SECONDS)) return;
    lastTimeChecked = GetTime();


    //once spent, stop doing the checks
    if (activeState == FUNDAMENTALNODE_VIN_SPENT) return;


    if (!IsPingedWithin(FUNDAMENTALNODE_REMOVAL_SECONDS)) {
        activeState = FUNDAMENTALNODE_REMOVE;
        return;
    }

    if (!IsPingedWithin(FUNDAMENTALNODE_EXPIRATION_SECONDS)) {
        activeState = FUNDAMENTALNODE_EXPIRED;
        return;
    }

    if(lastPing.sigTime - sigTime < FUNDAMENTALNODE_MIN_MNP_SECONDS){
        activeState = FUNDAMENTALNODE_PRE_ENABLED;
        return;
    }

    if (!unitTest) {
        /*CValidationState state;
        CMutableTransaction tx = CMutableTransaction();
        CTxOut vout = CTxOut(9999.99 * COIN, obfuScationPool.collateralPubKey);
        tx.vin.push_back(vin);
        tx.vout.push_back(vout);

        {
            TRY_LOCK(cs_main, lockMain);
            if (!lockMain) return;

            if (!AcceptableInputs(mempool, state, CTransaction(tx), false, NULL)) {
                activeState = FUNDAMENTALNODE_VIN_SPENT;
                return;
            }
        }*/
    }

    activeState = FUNDAMENTALNODE_ENABLED; // OK
}

int64_t CFundamentalnode::SecondsSincePayment()
{
    CScript pubkeyScript;
    pubkeyScript = GetScriptForDestination(pubKeyCollateralAddress.GetID());

    int64_t sec = (GetAdjustedTime() - GetLastPaid());
    int64_t month = 60 * 60 * 24 * 30;
    if (sec < month) return sec; //if it's less than 30 days, give seconds

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << vin;
    ss << sigTime;
    uint256 hash = ss.GetHash();

    // return some deterministic value for unknown/unpaid but force it to be more than 30 days old
    return month + hash.GetCompact(false);
}

int64_t CFundamentalnode::GetLastPaid()
{
    CBlockIndex* pindexPrev = chainActive.Tip();
    if (pindexPrev == NULL) return false;

    CScript mnpayee;
    mnpayee = GetScriptForDestination(pubKeyCollateralAddress.GetID());

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << vin;
    ss << sigTime;
    uint256 hash = ss.GetHash();

    // use a deterministic offset to break a tie -- 2.5 minutes
    int64_t nOffset = hash.GetCompact(false) % 150;

    if (chainActive.Tip() == NULL) return false;

    const CBlockIndex* BlockReading = chainActive.Tip();

    int nMnCount = mnodeman.CountEnabled() * 1.25;
    int n = 0;
    for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
        if (n >= nMnCount) {
            return 0;
        }
        n++;

        if (fundamentalnodePayments.mapFundamentalnodeBlocks.count(BlockReading->nHeight)) {
            /*
                Search for this payee, with at least 2 votes. This will aid in consensus allowing the network
                to converge on the same payees quickly, then keep the same schedule.
            */
            if (fundamentalnodePayments.mapFundamentalnodeBlocks[BlockReading->nHeight].HasPayeeWithVotes(mnpayee, 2)) {
                return BlockReading->nTime + nOffset;
            }
        }

        if (BlockReading->pprev == NULL) {
            assert(BlockReading);
            break;
        }
        BlockReading = BlockReading->pprev;
    }

    return 0;
}

std::string CFundamentalnode::GetStatus()
{
    switch (nActiveState) {
        case CFundamentalnode::FUNDAMENTALNODE_PRE_ENABLED:
            return "PRE_ENABLED";
        case CFundamentalnode::FUNDAMENTALNODE_ENABLED:
            return "ENABLED";
        case CFundamentalnode::FUNDAMENTALNODE_EXPIRED:
            return "EXPIRED";
        case CFundamentalnode::FUNDAMENTALNODE_REMOVE:
            return "REMOVE";
        case CFundamentalnode::FUNDAMENTALNODE_WATCHDOG_EXPIRED:
            return "WATCHDOG_EXPIRED";
        case CFundamentalnode::FUNDAMENTALNODE_POSE_BAN:
            return "POSE_BAN";
        default:
            return "UNKNOWN";
    }
}

bool CFundamentalnode::IsValidNetAddr()
{
    // TODO: regtest is fine with any addresses for now,
    // should probably be a bit smarter if one day we start to implement tests for this
    return Params().NetworkID() == CBaseChainParams::REGTEST ||
           (IsReachable(addr) && addr.IsRoutable());
}

bool CFundamentalnode::IsInputAssociatedWithPubkey() const
{
    CScript payee;
    payee = GetScriptForDestination(pubKeyCollateralAddress.GetID());

    CTransaction txVin;
    uint256 hash;
    if(GetTransaction(vin.prevout.hash, txVin, hash, true)) {
        for (CTxOut out : txVin.vout) {
            if (out.nValue == 10000 * COIN && out.scriptPubKey == payee) return true;
        }
    }

    return false;
}

CFundamentalnodeBroadcast::CFundamentalnodeBroadcast() :
        CFundamentalnode()
{ }

CFundamentalnodeBroadcast::CFundamentalnodeBroadcast(CService newAddr, CTxIn newVin, CPubKey pubKeyCollateralAddressNew, CPubKey pubKeyFundamentalnodeNew, int protocolVersionIn) :
        CFundamentalnode()
{
    vin = newVin;
    addr = newAddr;
    pubKeyCollateralAddress = pubKeyCollateralAddressNew;
    pubKeyFundamentalnode = pubKeyFundamentalnodeNew;
    protocolVersion = protocolVersionIn;
}

CFundamentalnodeBroadcast::CFundamentalnodeBroadcast(const CFundamentalnode& mn) :
        CFundamentalnode(mn)
{ }

bool CFundamentalnodeBroadcast::Create(std::string strService, std::string strKeyFundamentalnode, std::string strTxHash, std::string strOutputIndex, std::string& strErrorRet, CFundamentalnodeBroadcast& mnbRet, bool fOffline)
{
    CTxIn txin;
    CPubKey pubKeyCollateralAddressNew;
    CKey keyCollateralAddressNew;
    CPubKey pubKeyFundamentalnodeNew;
    CKey keyFundamentalnodeNew;

    //need correct blocks to send ping
    if (!fOffline && !fundamentalnodeSync.IsBlockchainSynced()) {
        strErrorRet = "Sync in progress. Must wait until sync is complete to start Fundamentalnode";
        LogPrint("fundamentalnode","CFundamentalnodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    if (!CMessageSigner::GetKeysFromSecret(strKeyFundamentalnode, keyFundamentalnodeNew, pubKeyFundamentalnodeNew)) {
        strErrorRet = strprintf("Invalid fundamentalnode key %s", strKeyFundamentalnode);
        LogPrint("fundamentalnode","CFundamentalnodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    if (!pwalletMain->GetFundamentalnodeVinAndKeys(txin, pubKeyCollateralAddressNew, keyCollateralAddressNew, strTxHash, strOutputIndex)) {
        LogPrint("fundamentalnode","CFundamentalnodeBroadcast::Create -- %s\n", strprintf("Could not allocate txin %s:%s for fundamentalnode %s", strTxHash, strOutputIndex, strService));
        return false;
    }

    int nPort;
    int nDefaultPort = Params().GetDefaultPort();
    std::string strHost;
    SplitHostPort(strService, nPort, strHost);
    if (nPort == 0) nPort = nDefaultPort;
    CService _service(LookupNumeric(strHost.c_str(), nPort));

    // The service needs the correct default port to work properly
    if (!CheckDefaultPort(_service, strErrorRet, "CFundamentalnodeBroadcast::Create"))
        return false;

    return Create(txin, _service, keyCollateralAddressNew, pubKeyCollateralAddressNew, keyFundamentalnodeNew, pubKeyFundamentalnodeNew, strErrorRet, mnbRet);
}

bool CFundamentalnodeBroadcast::Create(CTxIn txin, CService service, CKey keyCollateralAddressNew, CPubKey pubKeyCollateralAddressNew, CKey keyFundamentalnodeNew, CPubKey pubKeyFundamentalnodeNew, std::string& strErrorRet, CFundamentalnodeBroadcast& mnbRet)
{
    // wait for reindex and/or import to finish
    if (fImporting || fReindex) return false;

    LogPrint("fundamentalnode", "CFundamentalnodeBroadcast::Create -- pubKeyCollateralAddressNew = %s, pubKeyFundamentalnodeNew.GetID() = %s\n",
             CBitcoinAddress(pubKeyCollateralAddressNew.GetID()).ToString(),
             pubKeyFundamentalnodeNew.GetID().ToString());

    CFundamentalnodePing mnp(txin);
    if (!mnp.Sign(keyFundamentalnodeNew, pubKeyFundamentalnodeNew)) {
        strErrorRet = strprintf("Failed to sign ping, fundamentalnode=%s", txin.prevout.hash.ToString());
        LogPrint("fundamentalnode","CFundamentalnodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CFundamentalnodeBroadcast();
        return false;
    }

    mnbRet = CFundamentalnodeBroadcast(service, txin, pubKeyCollateralAddressNew, pubKeyFundamentalnodeNew, PROTOCOL_VERSION);

    if (!mnbRet.IsValidNetAddr()) {
        strErrorRet = strprintf("Invalid IP address %s, fundamentalnode=%s", mnbRet.addr.ToStringIP (), txin.prevout.hash.ToString());
        LogPrint("fundamentalnode","CFundamentalnodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CFundamentalnodeBroadcast();
        return false;
    }

    mnbRet.lastPing = mnp;
    if (!mnbRet.Sign(keyCollateralAddressNew, pubKeyCollateralAddressNew)) {
        strErrorRet = strprintf("Failed to sign broadcast, fundamentalnode=%s", txin.prevout.hash.ToString());
        LogPrint("fundamentalnode","CFundamentalnodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CFundamentalnodeBroadcast();
        return false;
    }

    return true;
}

bool CFundamentalnodeBroadcast::Sign(const CKey& key, const CPubKey& pubKey)
{
    std::string strError = "";
    nMessVersion = MessageVersion::MESS_VER_HASH;
    const std::string strMessage = GetSignatureHash().GetHex();

    if (!CMessageSigner::SignMessage(strMessage, vchSig, key)) {
        return error("%s : SignMessage() (nMessVersion=%d) failed", __func__, nMessVersion);
    }

    if (!CMessageSigner::VerifyMessage(pubKey, vchSig, strMessage, strError)) {
        return error("%s : VerifyMessage() (nMessVersion=%d) failed, error: %s\n",
                     __func__, nMessVersion, strError);
    }

    return true;
}

bool CFundamentalnodeBroadcast::Sign(const std::string strSignKey)
{
    CKey key;
    CPubKey pubkey;

    if (!CMessageSigner::GetKeysFromSecret(strSignKey, key, pubkey)) {
        return error("%s : Invalid strSignKey", __func__);
    }

    return Sign(key, pubkey);
}

bool CFundamentalnodeBroadcast::CheckSignature() const
{
    std::string strError = "";
    std::string strMessage = (
            nMessVersion == MessageVersion::MESS_VER_HASH ?
            GetSignatureHash().GetHex() :
            GetStrMessage()
    );

    if(!CMessageSigner::VerifyMessage(pubKeyCollateralAddress, vchSig, strMessage, strError))
        return error("%s : VerifyMessage (nMessVersion=%d) failed: %s", __func__, nMessVersion, strError);

    return true;
}

bool CFundamentalnodeBroadcast::CheckDefaultPort(CService service, std::string& strErrorRet, const std::string& strContext)
{
    int nDefaultPort = Params().GetDefaultPort();

    if (service.GetPort() != nDefaultPort) {
        strErrorRet = strprintf("Invalid port %u for fundamentalnode %s, only %d is supported on %s-net.",
                                service.GetPort(), service.ToString(), nDefaultPort, Params().NetworkIDString());
        LogPrint("fundamentalnode", "%s - %s\n", strContext, strErrorRet);
        return false;
    }

    return true;
}

bool CFundamentalnodeBroadcast::CheckAndUpdate(int& nDos)
{
    // make sure signature isn't in the future (past is OK)
    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrint("fundamentalnode","mnb - Signature rejected, too far into the future %s\n", vin.prevout.hash.ToString());
        nDos = 1;
        return false;
    }

    // incorrect ping or its sigTime
    if(lastPing == CFundamentalnodePing() || !lastPing.CheckAndUpdate(nDos, false, true))
        return false;

    if (protocolVersion < fundamentalnodePayments.GetMinFundamentalnodePaymentsProto()) {
        LogPrint("fundamentalnode","mnb - ignoring outdated Fundamentalnode %s protocol version %d\n", vin.prevout.hash.ToString(), protocolVersion);
        return false;
    }

    CScript pubkeyScript;
    pubkeyScript = GetScriptForDestination(pubKeyCollateralAddress.GetID());

    if (pubkeyScript.size() != 25) {
        LogPrint("fundamentalnode","mnb - pubkey the wrong size\n");
        nDos = 100;
        return false;
    }

    CScript pubkeyScript2;
    pubkeyScript2 = GetScriptForDestination(pubKeyFundamentalnode.GetID());

    if (pubkeyScript2.size() != 25) {
        LogPrint("fundamentalnode","mnb - pubkey2 the wrong size\n");
        nDos = 100;
        return false;
    }

    if (!vin.scriptSig.empty()) {
        LogPrint("fundamentalnode","mnb - Ignore Not Empty ScriptSig %s\n", vin.prevout.hash.ToString());
        return false;
    }

    std::string strError = "";
    if (!CheckSignature())
    {
        // don't ban for old fundamentalnodes, their sigs could be broken because of the bug
        nDos = protocolVersion < MIN_PEER_MNANNOUNCE ? 0 : 100;
        return error("%s : Got bad Fundamentalnode address signature", __func__);
    }

    if (Params().NetworkID() == CBaseChainParams::MAIN) {
        if (addr.GetPort() != 8765) return false;
    } else if (addr.GetPort() == 8765)
        return false;

    //search existing Fundamentalnode list, this is where we update existing Fundamentalnodes with new mnb broadcasts
    CFundamentalnode* pmn = mnodeman.Find(vin);

    // no such fundamentalnode, nothing to update
    if (pmn == NULL) return true;

    // this broadcast is older or equal than the one that we already have - it's bad and should never happen
    // unless someone is doing something fishy
    // (mapSeenFundamentalnodeBroadcast in CFundamentalnodeMan::ProcessMessage should filter legit duplicates)
    if(pmn->sigTime >= sigTime) {
        return error("%s : Bad sigTime %d for Fundamentalnode %20s %105s (existing broadcast is at %d)",
                     __func__, sigTime, addr.ToString(), vin.ToString(), pmn->sigTime);
    }

    // fundamentalnode is not enabled yet/already, nothing to update
    if (!pmn->IsEnabled()) return true;

    // mn.pubkey = pubkey, IsVinAssociatedWithPubkey is validated once below,
    //   after that they just need to match
    if (pmn->pubKeyCollateralAddress == pubKeyCollateralAddress && !pmn->IsBroadcastedWithin(FUNDAMENTALNODE_MIN_MNB_SECONDS)) {
        //take the newest entry
        LogPrint("fundamentalnode","mnb - Got updated entry for %s\n", vin.prevout.hash.ToString());
        if (pmn->UpdateFromNewBroadcast((*this))) {
            pmn->Check();
            if (pmn->IsEnabled()) Relay();
        }
        fundamentalnodeSync.AddedFundamentalnodeList(GetHash());
    }

    return true;
}

bool CFundamentalnodeBroadcast::CheckInputsAndAdd(int& nDoS)
{
    // we are a fundamentalnode with the same vin (i.e. already activated) and this mnb is ours (matches our Fundamentalnode privkey)
    // so nothing to do here for us
    if (fFundamentalNode && activeFundamentalnode.vin != boost::none &&
        vin.prevout == activeFundamentalnode.vin->prevout && pubKeyFundamentalnode == activeFundamentalnode.pubKeyFundamentalnode)
        return true;

    // incorrect ping or its sigTime
    if(lastPing == CFundamentalnodePing() || !lastPing.CheckAndUpdate(nDoS, false, true)) return false;

    // search existing Fundamentalnode list
    CFundamentalnode* pmn = mnodeman.Find(vin);

    if (pmn != NULL) {
        // nothing to do here if we already know about this fundamentalnode and it's enabled
        if (pmn->IsEnabled()) return true;
            // if it's not enabled, remove old MN first and continue
        else
            mnodeman.Remove(pmn->vin);
    }

    CValidationState state;
    uint256 hashBlock = 0;
    CTransaction tx2, tx1;
    GetTransaction(vin.prevout.hash, tx2, hashBlock, true);

    {
        TRY_LOCK(cs_main, lockMain);
        if (!lockMain) {
            // not mnb fault, let it to be checked again later
            mnodeman.mapSeenFundamentalnodeBroadcast.erase(GetHash());
            fundamentalnodeSync.mapSeenSyncMNB.erase(GetHash());
            return false;
        }

        int64_t nValueIn = 0;

        BOOST_FOREACH (const CTxIn& txin, tx2.vin) {
            // First try finding the previous transaction in database
            CTransaction txPrev;
            uint256 hashBlockPrev;
            if (!GetTransaction(txin.prevout.hash, txPrev, hashBlockPrev, true)) {
                LogPrintf("CheckInputsAndAdd: failed to find vin transaction \n");
                continue; // previous transaction not in main chain
            }

            nValueIn += txPrev.vout[txin.prevout.n].nValue;

        }

        if(nValueIn - tx2.GetValueOut() < FUNDAMENTALNODE_AMOUNT - FN_MAGIC_AMOUNT){
            state.IsInvalid(nDoS);
            return false;
        }
    }

    LogPrint("fundamentalnode", "mnb - Accepted Fundamentalnode entry\n");

    if (GetInputAge(vin) < FUNDAMENTALNODE_MIN_CONFIRMATIONS) {
        LogPrint("fundamentalnode","mnb - Input must have at least %d confirmations\n", FUNDAMENTALNODE_MIN_CONFIRMATIONS);
        // maybe we miss few blocks, let this mnb to be checked again later
        mnodeman.mapSeenFundamentalnodeBroadcast.erase(GetHash());
        fundamentalnodeSync.mapSeenSyncMNB.erase(GetHash());
        return false;
    }

    // verify that sig time is legit in past
    // should be at least not earlier than block when 1000 PIV tx got FUNDAMENTALNODE_MIN_CONFIRMATIONS
    GetTransaction(vin.prevout.hash, tx2, hashBlock, true);
    BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi != mapBlockIndex.end() && (*mi).second) {
        CBlockIndex* pMNIndex = (*mi).second;                                                        // block for 1000 PIVX tx -> 1 confirmation
        CBlockIndex* pConfIndex = chainActive[pMNIndex->nHeight + FUNDAMENTALNODE_MIN_CONFIRMATIONS - 1]; // block where tx got FUNDAMENTALNODE_MIN_CONFIRMATIONS
        if (pConfIndex->GetBlockTime() > sigTime) {
            LogPrint("fundamentalnode","mnb - Bad sigTime %d for Fundamentalnode %s (%i conf block is at %d)\n",
                     sigTime, vin.prevout.hash.ToString(), FUNDAMENTALNODE_MIN_CONFIRMATIONS, pConfIndex->GetBlockTime());
            return false;
        }
    }

    LogPrint("fundamentalnode","mnb - Got NEW Fundamentalnode entry - %s - %lli \n", vin.prevout.hash.ToString(), sigTime);
    CFundamentalnode mn(*this);
    mnodeman.Add(mn);

    // if it matches our Fundamentalnode privkey, then we've been remotely activated
    if (pubKeyFundamentalnode == activeFundamentalnode.pubKeyFundamentalnode && protocolVersion == PROTOCOL_VERSION) {
        activeFundamentalnode.EnableHotColdFundamentalNode(vin, addr);
    }

    bool isLocal = addr.IsRFC1918() || addr.IsLocal();
    if (Params().NetworkID() == CBaseChainParams::REGTEST) isLocal = false;

    if (!isLocal) Relay();

    return true;
}

void CFundamentalnodeBroadcast::Relay()
{
    CInv inv(MSG_FUNDAMENTALNODE_ANNOUNCE, GetHash());
    RelayInv(inv);
}

uint256 CFundamentalnodeBroadcast::GetHash() const
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << sigTime;
    ss << pubKeyCollateralAddress;
    return ss.GetHash();
}

CFundamentalnodePing::CFundamentalnodePing() :
        CSignedMessage(),
        vin(),
        blockHash(),
        sigTime(GetAdjustedTime())
{ }

CFundamentalnodePing::CFundamentalnodePing(CTxIn& newVin) :
        CSignedMessage(),
        vin(newVin),
        sigTime(GetAdjustedTime())
{
    int nHeight;
    {
        LOCK(cs_main);
        nHeight = chainActive.Height();
        if (nHeight > 12)
            blockHash = chainActive[nHeight - 12]->GetBlockHash();
    }
}

uint256 CFundamentalnodePing::GetHash() const
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << vin;
    if (nMessVersion == MessageVersion::MESS_VER_HASH) ss << blockHash;
    ss << sigTime;
    return ss.GetHash();
}

std::string CFundamentalnodePing::GetStrMessage() const
{
    return vin.ToString() + blockHash.ToString() + std::to_string(sigTime);
}

bool CFundamentalnodePing::CheckAndUpdate(int& nDos, bool fRequireEnabled, bool fCheckSigTimeOnly)
{
    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrint("fundamentalnode","CFundamentalnodePing::CheckAndUpdate - Signature rejected, too far into the future %s\n", vin.prevout.hash.ToString());
        nDos = 1;
        return false;
    }

    if (sigTime <= GetAdjustedTime() - 60 * 60) {
        LogPrint("fundamentalnode","CFundamentalnodePing::CheckAndUpdate - Signature rejected, too far into the past %s - %d %d \n", vin.prevout.hash.ToString(), sigTime, GetAdjustedTime());
        nDos = 1;
        return false;
    }

    // see if we have this Fundamentalnode
    CFundamentalnode* pmn = mnodeman.Find(vin);
    const bool isFundamentalnodeFound = (pmn != nullptr);
    const bool isSignatureValid = (isFundamentalnodeFound && CheckSignature(pmn->pubKeyFundamentalnode));

    if(fCheckSigTimeOnly) {
        if (isFundamentalnodeFound && !isSignatureValid) {
            nDos = 33;
            return false;
        }
        return true;
    }

    LogPrint("fundamentalnode", "CFundamentalnodePing::CheckAndUpdate - New Ping - %s - %s - %lli\n", GetHash().ToString(), blockHash.ToString(), sigTime);

    if (isFundamentalnodeFound && pmn->protocolVersion >= fundamentalnodePayments.GetMinFundamentalnodePaymentsProto()) {
        if (fRequireEnabled && !pmn->IsEnabled()) return false;

        // LogPrint("fundamentalnode","mnping - Found corresponding mn for vin: %s\n", vin.ToString());
        // update only if there is no known ping for this fundamentalnode or
        // last ping was more then FUNDAMENTALNODE_MIN_MNP_SECONDS-60 ago comparing to this one
        if (!pmn->IsPingedWithin(FUNDAMENTALNODE_MIN_MNP_SECONDS - 60, sigTime)) {
            if (!isSignatureValid) {
                nDos = 33;
                return false;
            }

            // Check if the ping block hash exists in disk
            BlockMap::iterator mi = mapBlockIndex.find(blockHash);
            if (mi == mapBlockIndex.end() || !(*mi).second) {
                LogPrint("fundamentalnode","CFundamentalnodePing::CheckAndUpdate - ping block not in disk. Fundamentalnode %s block hash %s\n", vin.prevout.hash.ToString(), blockHash.ToString());
                return false;
            }

            // Verify ping block hash in main chain and in the [ tip > x > tip - 24 ] range.
            {
                LOCK(cs_main);
                if (!chainActive.Contains((*mi).second) || (chainActive.Height() - (*mi).second->nHeight > 24)) {
                    LogPrint("fundamentalnode","CFundamentalnodePing::CheckAndUpdate - Fundamentalnode %s block hash %s is too old or has an invalid block hash\n", vin.prevout.hash.ToString(), blockHash.ToString());
                    // Do nothing here (no Fundamentalnode update, no mnping relay)
                    // Let this node to be visible but fail to accept mnping
                    return false;
                }
            }

            pmn->lastPing = *this;

            //mnodeman.mapSeenFundamentalnodeBroadcast.lastPing is probably outdated, so we'll update it
            CFundamentalnodeBroadcast mnb(*pmn);
            uint256 hash = mnb.GetHash();
            if (mnodeman.mapSeenFundamentalnodeBroadcast.count(hash)) {
                mnodeman.mapSeenFundamentalnodeBroadcast[hash].lastPing = *this;
            }

            pmn->Check(true);
            if (!pmn->IsEnabled()) return false;

            LogPrint("fundamentalnode", "CFundamentalnodePing::CheckAndUpdate - Fundamentalnode ping accepted, vin: %s\n", vin.prevout.hash.ToString());

            Relay();
            return true;
        }
        LogPrint("fundamentalnode", "CFundamentalnodePing::CheckAndUpdate - Fundamentalnode ping arrived too early, vin: %s\n", vin.prevout.hash.ToString());
        //nDos = 1; //disable, this is happening frequently and causing banned peers
        return false;
    }
    LogPrint("fundamentalnode", "CFundamentalnodePing::CheckAndUpdate - Couldn't find compatible Fundamentalnode entry, vin: %s\n", vin.prevout.hash.ToString());

    return false;
}

void CFundamentalnodePing::Relay()
{
    CInv inv(MSG_FUNDAMENTALNODE_PING, GetHash());
    RelayInv(inv);
}
