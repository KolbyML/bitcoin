// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "fundamentalnodeman.h"

#include "addrman.h"
#include "fs.h"
#include "fundamentalnode-payments.h"
#include "fundamentalnode-sync.h"
#include "fundamentalnode.h"
#include "messagesigner.h"
#include "netbase.h"
#include "spork.h"
#include "swifttx.h"
#include "util.h"


#define MN_WINNER_MINIMUM_AGE 8000    // Age in seconds. This should be > FUNDAMENTALNODE_REMOVAL_SECONDS to avoid misconfigured new nodes in the list.

/** Fundamentalnode manager */
CFundamentalnodeMan mnodeman;
/** Keep track of the active Fundamentalnode */
CActiveFundamentalnode activeFundamentalnode;

struct CompareLastPaid {
    bool operator()(const std::pair<int64_t, CTxIn>& t1,
                    const std::pair<int64_t, CTxIn>& t2) const
    {
        return t1.first < t2.first;
    }
};

struct CompareScoreTxIn {
    bool operator()(const std::pair<int64_t, CTxIn>& t1,
                    const std::pair<int64_t, CTxIn>& t2) const
    {
        return t1.first < t2.first;
    }
};

struct CompareScoreMN {
    bool operator()(const std::pair<int64_t, CFundamentalnode>& t1,
                    const std::pair<int64_t, CFundamentalnode>& t2) const
    {
        return t1.first < t2.first;
    }
};

//
// CFundamentalnodeDB
//

CFundamentalnodeDB::CFundamentalnodeDB()
{
    pathMN = GetDataDir() / "fncache.dat";
    strMagicMessage = "FundamentalnodeCache";
}

bool CFundamentalnodeDB::Write(const CFundamentalnodeMan& mnodemanToSave)
{
    int64_t nStart = GetTimeMillis();

    // serialize, checksum data up to that point, then append checksum
    CDataStream ssFundamentalnodes(SER_DISK, CLIENT_VERSION);
    ssFundamentalnodes << strMagicMessage;                   // fundamentalnode cache file specific magic message
    ssFundamentalnodes << FLATDATA(Params().MessageStart()); // network specific magic number
    ssFundamentalnodes << mnodemanToSave;
    uint256 hash = Hash(ssFundamentalnodes.begin(), ssFundamentalnodes.end());
    ssFundamentalnodes << hash;

    // open output file, and associate with CAutoFile
    FILE* file = fsbridge::fopen(pathMN, "wb");
    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s : Failed to open file %s", __func__, pathMN.string());

    // Write and commit header, data
    try {
        fileout << ssFundamentalnodes;
    } catch (const std::exception& e) {
        return error("%s : Serialize or I/O error - %s", __func__, e.what());
    }
    //    FileCommit(fileout);
    fileout.fclose();

    LogPrint("fundamentalnode","Written info to fncache.dat  %dms\n", GetTimeMillis() - nStart);
    LogPrint("fundamentalnode","  %s\n", mnodemanToSave.ToString());

    return true;
}

CFundamentalnodeDB::ReadResult CFundamentalnodeDB::Read(CFundamentalnodeMan& mnodemanToLoad, bool fDryRun)
{
    int64_t nStart = GetTimeMillis();
    // open input file, and associate with CAutoFile
    FILE* file = fsbridge::fopen(pathMN, "rb");
    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull()) {
        error("%s : Failed to open file %s", __func__, pathMN.string());
        return FileError;
    }

    // use file size to size memory buffer
    int fileSize = fs::file_size(pathMN);
    int dataSize = fileSize - sizeof(uint256);
    // Don't try to resize to a negative number if file is small
    if (dataSize < 0)
        dataSize = 0;
    std::vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;

    // read data and checksum from file
    try {
        filein.read((char*)&vchData[0], dataSize);
        filein >> hashIn;
    } catch (const std::exception& e) {
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return HashReadError;
    }
    filein.fclose();

    CDataStream ssFundamentalnodes(vchData, SER_DISK, CLIENT_VERSION);

    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssFundamentalnodes.begin(), ssFundamentalnodes.end());
    if (hashIn != hashTmp) {
        error("%s : Checksum mismatch, data corrupted", __func__);
        return IncorrectHash;
    }

    unsigned char pchMsgTmp[4];
    std::string strMagicMessageTmp;
    try {
        // de-serialize file header (fundamentalnode cache file specific magic message) and ..

        ssFundamentalnodes >> strMagicMessageTmp;

        // ... verify the message matches predefined one
        if (strMagicMessage != strMagicMessageTmp) {
            error("%s : Invalid fundamentalnode cache magic message", __func__);
            return IncorrectMagicMessage;
        }

        // de-serialize file header (network specific magic number) and ..
        ssFundamentalnodes >> FLATDATA(pchMsgTmp);

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp))) {
            error("%s : Invalid network magic number", __func__);
            return IncorrectMagicNumber;
        }
        // de-serialize data into CFundamentalnodeMan object
        ssFundamentalnodes >> mnodemanToLoad;
    } catch (const std::exception& e) {
        mnodemanToLoad.Clear();
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return IncorrectFormat;
    }

    LogPrint("fundamentalnode","Loaded info from fncache.dat  %dms\n", GetTimeMillis() - nStart);
    LogPrint("fundamentalnode","  %s\n", mnodemanToLoad.ToString());
    if (!fDryRun) {
        LogPrint("fundamentalnode","Fundamentalnode manager - cleaning....\n");
        mnodemanToLoad.CheckAndRemove(true);
        LogPrint("fundamentalnode","Fundamentalnode manager - result:\n");
        LogPrint("fundamentalnode","  %s\n", mnodemanToLoad.ToString());
    }

    return Ok;
}

void DumpFundamentalnodes()
{
    int64_t nStart = GetTimeMillis();

    CFundamentalnodeDB mndb;
    CFundamentalnodeMan tempMnodeman;

    LogPrint("fundamentalnode","Verifying fncache.dat format...\n");
    CFundamentalnodeDB::ReadResult readResult = mndb.Read(tempMnodeman, true);
    // there was an error and it was not an error on file opening => do not proceed
    if (readResult == CFundamentalnodeDB::FileError)
        LogPrint("fundamentalnode","Missing fundamentalnode cache file - fncache.dat, will try to recreate\n");
    else if (readResult != CFundamentalnodeDB::Ok) {
        LogPrint("fundamentalnode","Error reading fncache.dat: ");
        if (readResult == CFundamentalnodeDB::IncorrectFormat)
            LogPrint("fundamentalnode","magic is ok but data has invalid format, will try to recreate\n");
        else {
            LogPrint("fundamentalnode","file format is unknown or invalid, please fix it manually\n");
            return;
        }
    }
    LogPrint("fundamentalnode","Writting info to fncache.dat...\n");
    mndb.Write(mnodeman);

    LogPrint("fundamentalnode","Fundamentalnode dump finished  %dms\n", GetTimeMillis() - nStart);
}

CFundamentalnodeMan::CFundamentalnodeMan()
{
    nDsqCount = 0;
}

bool CFundamentalnodeMan::Add(CFundamentalnode& mn)
{
    LOCK(cs);

    if (!mn.IsEnabled())
        return false;

    CFundamentalnode* pmn = Find(mn.vin);
    if (pmn == NULL) {
        LogPrint("fundamentalnode", "CFundamentalnodeMan: Adding new Fundamentalnode %s - %i now\n", mn.vin.prevout.hash.ToString(), size() + 1);
        vFundamentalnodes.push_back(mn);
        return true;
    }

    return false;
}

void CFundamentalnodeMan::AskForMN(CNode* pnode, CTxIn& vin)
{
    std::map<COutPoint, int64_t>::iterator i = mWeAskedForFundamentalnodeListEntry.find(vin.prevout);
    if (i != mWeAskedForFundamentalnodeListEntry.end()) {
        int64_t t = (*i).second;
        if (GetTime() < t) return; // we've asked recently
    }

    // ask for the fnb info once from the node that sent fnp

    LogPrint("fundamentalnode", "CFundamentalnodeMan::AskForMN - Asking node for missing entry, vin: %s\n", vin.prevout.hash.ToString());
    pnode->PushMessage("obseg", vin);
    int64_t askAgain = GetTime() + FUNDAMENTALNODE_MIN_MNP_SECONDS;
    mWeAskedForFundamentalnodeListEntry[vin.prevout] = askAgain;
}

void CFundamentalnodeMan::Check()
{
    LOCK(cs);

    for (CFundamentalnode& mn : vFundamentalnodes) {
        mn.Check();
    }
}

void CFundamentalnodeMan::CheckAndRemove(bool forceExpiredRemoval)
{
    Check();

    LOCK(cs);

    //remove inactive and outdated
    std::vector<CFundamentalnode>::iterator it = vFundamentalnodes.begin();
    while (it != vFundamentalnodes.end()) {
        if ((*it).activeState == CFundamentalnode::FUNDAMENTALNODE_REMOVE ||
            (*it).activeState == CFundamentalnode::FUNDAMENTALNODE_VIN_SPENT ||
            (forceExpiredRemoval && (*it).activeState == CFundamentalnode::FUNDAMENTALNODE_EXPIRED) ||
            (*it).protocolVersion < fundamentalnodePayments.GetMinFundamentalnodePaymentsProto()) {
            LogPrint("fundamentalnode", "CFundamentalnodeMan: Removing inactive Fundamentalnode %s - %i now\n", (*it).vin.prevout.hash.ToString(), size() - 1);

            //erase all of the broadcasts we've seen from this vin
            // -- if we missed a few pings and the node was removed, this will allow is to get it back without them
            //    sending a brand new fnb
            std::map<uint256, CFundamentalnodeBroadcast>::iterator it3 = mapSeenFundamentalnodeBroadcast.begin();
            while (it3 != mapSeenFundamentalnodeBroadcast.end()) {
                if ((*it3).second.vin == (*it).vin) {
                    fundamentalnodeSync.mapSeenSyncMNB.erase((*it3).first);
                    mapSeenFundamentalnodeBroadcast.erase(it3++);
                } else {
                    ++it3;
                }
            }

            // allow us to ask for this fundamentalnode again if we see another ping
            std::map<COutPoint, int64_t>::iterator it2 = mWeAskedForFundamentalnodeListEntry.begin();
            while (it2 != mWeAskedForFundamentalnodeListEntry.end()) {
                if ((*it2).first == (*it).vin.prevout) {
                    mWeAskedForFundamentalnodeListEntry.erase(it2++);
                } else {
                    ++it2;
                }
            }

            it = vFundamentalnodes.erase(it);
        } else {
            ++it;
        }
    }

    // check who's asked for the Fundamentalnode list
    std::map<CNetAddr, int64_t>::iterator it1 = mAskedUsForFundamentalnodeList.begin();
    while (it1 != mAskedUsForFundamentalnodeList.end()) {
        if ((*it1).second < GetTime()) {
            mAskedUsForFundamentalnodeList.erase(it1++);
        } else {
            ++it1;
        }
    }

    // check who we asked for the Fundamentalnode list
    it1 = mWeAskedForFundamentalnodeList.begin();
    while (it1 != mWeAskedForFundamentalnodeList.end()) {
        if ((*it1).second < GetTime()) {
            mWeAskedForFundamentalnodeList.erase(it1++);
        } else {
            ++it1;
        }
    }

    // check which Fundamentalnodes we've asked for
    std::map<COutPoint, int64_t>::iterator it2 = mWeAskedForFundamentalnodeListEntry.begin();
    while (it2 != mWeAskedForFundamentalnodeListEntry.end()) {
        if ((*it2).second < GetTime()) {
            mWeAskedForFundamentalnodeListEntry.erase(it2++);
        } else {
            ++it2;
        }
    }

    // remove expired mapSeenFundamentalnodeBroadcast
    std::map<uint256, CFundamentalnodeBroadcast>::iterator it3 = mapSeenFundamentalnodeBroadcast.begin();
    while (it3 != mapSeenFundamentalnodeBroadcast.end()) {
        if ((*it3).second.lastPing.sigTime < GetTime() - (FUNDAMENTALNODE_REMOVAL_SECONDS * 2)) {
            mapSeenFundamentalnodeBroadcast.erase(it3++);
            fundamentalnodeSync.mapSeenSyncMNB.erase((*it3).second.GetHash());
        } else {
            ++it3;
        }
    }

    // remove expired mapSeenFundamentalnodePing
    std::map<uint256, CFundamentalnodePing>::iterator it4 = mapSeenFundamentalnodePing.begin();
    while (it4 != mapSeenFundamentalnodePing.end()) {
        if ((*it4).second.sigTime < GetTime() - (FUNDAMENTALNODE_REMOVAL_SECONDS * 2)) {
            mapSeenFundamentalnodePing.erase(it4++);
        } else {
            ++it4;
        }
    }
}

void CFundamentalnodeMan::Clear()
{
    LOCK(cs);
    vFundamentalnodes.clear();
    mAskedUsForFundamentalnodeList.clear();
    mWeAskedForFundamentalnodeList.clear();
    mWeAskedForFundamentalnodeListEntry.clear();
    mapSeenFundamentalnodeBroadcast.clear();
    mapSeenFundamentalnodePing.clear();
    nDsqCount = 0;
}

int CFundamentalnodeMan::stable_size ()
{
    int nStable_size = 0;
    int nMinProtocol = ActiveProtocol();
    int64_t nFundamentalnode_Min_Age = MN_WINNER_MINIMUM_AGE;
    int64_t nFundamentalnode_Age = 0;

    for (CFundamentalnode& mn : vFundamentalnodes) {
        if (mn.protocolVersion < nMinProtocol) {
            continue; // Skip obsolete versions
        }
        if (sporkManager.IsSporkActive (SPORK_8_FUNDAMENTALNODE_PAYMENT_ENFORCEMENT)) {
            nFundamentalnode_Age = GetAdjustedTime() - mn.sigTime;
            if ((nFundamentalnode_Age) < nFundamentalnode_Min_Age) {
                continue; // Skip fundamentalnodes younger than (default) 8000 sec (MUST be > FUNDAMENTALNODE_REMOVAL_SECONDS)
            }
        }
        mn.Check ();
        if (!mn.IsEnabled ())
            continue; // Skip not-enabled fundamentalnodes

        nStable_size++;
    }

    return nStable_size;
}

int CFundamentalnodeMan::CountEnabled(int protocolVersion)
{
    int i = 0;
    protocolVersion = protocolVersion == -1 ? fundamentalnodePayments.GetMinFundamentalnodePaymentsProto() : protocolVersion;

    for (CFundamentalnode& mn : vFundamentalnodes) {
        mn.Check();
        if (mn.protocolVersion < protocolVersion || !mn.IsEnabled()) continue;
        i++;
    }

    return i;
}

void CFundamentalnodeMan::CountNetworks(int protocolVersion, int& ipv4, int& ipv6, int& onion)
{
    protocolVersion = protocolVersion == -1 ? fundamentalnodePayments.GetMinFundamentalnodePaymentsProto() : protocolVersion;

    for (CFundamentalnode& mn : vFundamentalnodes) {
        mn.Check();
        std::string strHost;
        int port;
        SplitHostPort(mn.addr.ToString(), port, strHost);
        CNetAddr node;
        LookupHost(strHost.c_str(), node, false);
        int nNetwork = node.GetNetwork();
        switch (nNetwork) {
            case 1 :
                ipv4++;
                break;
            case 2 :
                ipv6++;
                break;
            case 3 :
                onion++;
                break;
        }
    }
}

void CFundamentalnodeMan::DsegUpdate(CNode* pnode)
{
    LOCK(cs);

    if (Params().NetworkID() == CBaseChainParams::MAIN) {
        if (!(pnode->addr.IsRFC1918() || pnode->addr.IsLocal())) {
            std::map<CNetAddr, int64_t>::iterator it = mWeAskedForFundamentalnodeList.find(pnode->addr);
            if (it != mWeAskedForFundamentalnodeList.end()) {
                if (GetTime() < (*it).second) {
                    LogPrint("fundamentalnode", "obseg - we already asked peer %i for the list; skipping...\n", pnode->GetId());
                    return;
                }
            }
        }
    }

    pnode->PushMessage("obseg", CTxIn());
    int64_t askAgain = GetTime() + FUNDAMENTALNODES_DSEG_SECONDS;
    mWeAskedForFundamentalnodeList[pnode->addr] = askAgain;
}

CFundamentalnode* CFundamentalnodeMan::Find(const CScript& payee)
{
    LOCK(cs);
    CScript payee2;

    for (CFundamentalnode& mn : vFundamentalnodes) {
        payee2 = GetScriptForDestination(mn.pubKeyCollateralAddress.GetID());
        if (payee2 == payee)
            return &mn;
    }
    return NULL;
}

CFundamentalnode* CFundamentalnodeMan::Find(const CTxIn& vin)
{
    LOCK(cs);

    for (CFundamentalnode& mn : vFundamentalnodes) {
        if (mn.vin.prevout == vin.prevout)
            return &mn;
    }
    return NULL;
}


CFundamentalnode* CFundamentalnodeMan::Find(const CPubKey& pubKeyFundamentalnode)
{
    LOCK(cs);

    for (CFundamentalnode& mn : vFundamentalnodes) {
        if (mn.pubKeyFundamentalnode == pubKeyFundamentalnode)
            return &mn;
    }
    return NULL;
}

//
// Deterministically select the oldest/best fundamentalnode to pay on the network
//
CFundamentalnode* CFundamentalnodeMan::GetNextFundamentalnodeInQueueForPayment(int nBlockHeight, bool fFilterSigTime, int& nCount)
{
    LOCK(cs);

    CFundamentalnode* pBestFundamentalnode = NULL;
    std::vector<std::pair<int64_t, CTxIn> > vecFundamentalnodeLastPaid;

    /*
        Make a vector with all of the last paid times
    */

    int nMnCount = CountEnabled();
    for (CFundamentalnode& mn : vFundamentalnodes) {
        mn.Check();
        if (!mn.IsEnabled()) continue;

        // //check protocol version
        if (mn.protocolVersion < fundamentalnodePayments.GetMinFundamentalnodePaymentsProto()) continue;

        //it's in the list (up to 8 entries ahead of current block to allow propagation) -- so let's skip it
        if (fundamentalnodePayments.IsScheduled(mn, nBlockHeight)) continue;

        //it's too new, wait for a cycle
        if (fFilterSigTime && mn.sigTime + (nMnCount * 2.6 * 60) > GetAdjustedTime()) continue;

        //make sure it has as many confirmations as there are fundamentalnodes
        if (mn.GetFundamentalnodeInputAge() < nMnCount) continue;

        vecFundamentalnodeLastPaid.push_back(std::make_pair(mn.SecondsSincePayment(), mn.vin));
    }

    nCount = (int)vecFundamentalnodeLastPaid.size();

    //when the network is in the process of upgrading, don't penalize nodes that recently restarted
    if (fFilterSigTime && nCount < nMnCount / 3) return GetNextFundamentalnodeInQueueForPayment(nBlockHeight, false, nCount);

    // Sort them high to low
    sort(vecFundamentalnodeLastPaid.rbegin(), vecFundamentalnodeLastPaid.rend(), CompareLastPaid());

    // Look at 1/10 of the oldest nodes (by last payment), calculate their scores and pay the best one
    //  -- This doesn't look at who is being paid in the +8-10 blocks, allowing for double payments very rarely
    //  -- 1/100 payments should be a double payment on mainnet - (1/(3000/10))*2
    //  -- (chance per block * chances before IsScheduled will fire)
    int nTenthNetwork = CountEnabled() / 10;
    int nCountTenth = 0;
    uint256 nHigh;
    for (PAIRTYPE(int64_t, CTxIn) & s : vecFundamentalnodeLastPaid) {
        CFundamentalnode* pmn = Find(s.second);
        if (!pmn) break;

        uint256 n = pmn->CalculateScore(1, nBlockHeight - 100);
        if (n > nHigh) {
            nHigh = n;
            pBestFundamentalnode = pmn;
        }
        nCountTenth++;
        if (nCountTenth >= nTenthNetwork) break;
    }
    return pBestFundamentalnode;
}

CFundamentalnode* CFundamentalnodeMan::GetCurrentFundamentalNode(int mod, int64_t nBlockHeight, int minProtocol)
{
    int64_t score = 0;
    CFundamentalnode* winner = NULL;

    // scan for winner
    for (CFundamentalnode& mn : vFundamentalnodes) {
        mn.Check();
        if (mn.protocolVersion < minProtocol || !mn.IsEnabled()) continue;

        // calculate the score for each Fundamentalnode
        uint256 n = mn.CalculateScore(mod, nBlockHeight);
        int64_t n2 = n.GetCompact(false);

        // determine the winner
        if (n2 > score) {
            score = n2;
            winner = &mn;
        }
    }

    return winner;
}

int CFundamentalnodeMan::GetFundamentalnodeRank(const CTxIn& vin, int64_t nBlockHeight, int minProtocol, bool fOnlyActive)
{
    std::vector<std::pair<int64_t, CTxIn> > vecFundamentalnodeScores;
    int64_t nFundamentalnode_Min_Age = MN_WINNER_MINIMUM_AGE;
    int64_t nFundamentalnode_Age = 0;

    //make sure we know about this block
    uint256 hash;
    if (!GetBlockHash(hash, nBlockHeight)) return -1;

    // scan for winner
    for (CFundamentalnode& mn : vFundamentalnodes) {
        if (mn.protocolVersion < minProtocol) {
            LogPrint("fundamentalnode","Skipping Fundamentalnode with obsolete version %d\n", mn.protocolVersion);
            continue;                                                       // Skip obsolete versions
        }

        if (sporkManager.IsSporkActive(SPORK_8_FUNDAMENTALNODE_PAYMENT_ENFORCEMENT)) {
            nFundamentalnode_Age = GetAdjustedTime() - mn.sigTime;
            if ((nFundamentalnode_Age) < nFundamentalnode_Min_Age) {
                LogPrint("fundamentalnode","Skipping just activated Fundamentalnode. Age: %ld\n", nFundamentalnode_Age);
                continue;                                                   // Skip fundamentalnodes younger than (default) 1 hour
            }
        }
        if (fOnlyActive) {
            mn.Check();
            if (!mn.IsEnabled()) continue;
        }
        uint256 n = mn.CalculateScore(1, nBlockHeight);
        int64_t n2 = n.GetCompact(false);

        vecFundamentalnodeScores.push_back(std::make_pair(n2, mn.vin));
    }

    sort(vecFundamentalnodeScores.rbegin(), vecFundamentalnodeScores.rend(), CompareScoreTxIn());

    int rank = 0;
    for (PAIRTYPE(int64_t, CTxIn) & s : vecFundamentalnodeScores) {
        rank++;
        if (s.second.prevout == vin.prevout) {
            return rank;
        }
    }

    return -1;
}

std::vector<std::pair<int, CFundamentalnode> > CFundamentalnodeMan::GetFundamentalnodeRanks(int64_t nBlockHeight, int minProtocol)
{
    std::vector<std::pair<int64_t, CFundamentalnode> > vecFundamentalnodeScores;
    std::vector<std::pair<int, CFundamentalnode> > vecFundamentalnodeRanks;

    //make sure we know about this block
    uint256 hash;
    if (!GetBlockHash(hash, nBlockHeight)) return vecFundamentalnodeRanks;

    // scan for winner
    for (CFundamentalnode& mn : vFundamentalnodes) {
        mn.Check();

        if (mn.protocolVersion < minProtocol) continue;

        if (!mn.IsEnabled()) {
            vecFundamentalnodeScores.push_back(std::make_pair(9999, mn));
            continue;
        }

        uint256 n = mn.CalculateScore(1, nBlockHeight);
        int64_t n2 = n.GetCompact(false);

        vecFundamentalnodeScores.push_back(std::make_pair(n2, mn));
    }

    sort(vecFundamentalnodeScores.rbegin(), vecFundamentalnodeScores.rend(), CompareScoreMN());

    int rank = 0;
    for (PAIRTYPE(int64_t, CFundamentalnode) & s : vecFundamentalnodeScores) {
        rank++;
        vecFundamentalnodeRanks.push_back(std::make_pair(rank, s.second));
    }

    return vecFundamentalnodeRanks;
}

CFundamentalnode* CFundamentalnodeMan::GetFundamentalnodeByRank(int nRank, int64_t nBlockHeight, int minProtocol, bool fOnlyActive)
{
    std::vector<std::pair<int64_t, CTxIn> > vecFundamentalnodeScores;

    // scan for winner
    for (CFundamentalnode& mn : vFundamentalnodes) {
        if (mn.protocolVersion < minProtocol) continue;
        if (fOnlyActive) {
            mn.Check();
            if (!mn.IsEnabled()) continue;
        }

        uint256 n = mn.CalculateScore(1, nBlockHeight);
        int64_t n2 = n.GetCompact(false);

        vecFundamentalnodeScores.push_back(std::make_pair(n2, mn.vin));
    }

    sort(vecFundamentalnodeScores.rbegin(), vecFundamentalnodeScores.rend(), CompareScoreTxIn());

    int rank = 0;
    for (PAIRTYPE(int64_t, CTxIn) & s : vecFundamentalnodeScores) {
        rank++;
        if (rank == nRank) {
            return Find(s.second);
        }
    }

    return NULL;
}

void CFundamentalnodeMan::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if (fLiteMode) return; //disable all Fundamentalnode related functionality
    if (!fundamentalnodeSync.IsBlockchainSynced()) return;

    LOCK(cs_process_message);

    if (strCommand == "fnb") { //Fundamentalnode Broadcast
        CFundamentalnodeBroadcast fnb;
        vRecv >> fnb;

        if (mapSeenFundamentalnodeBroadcast.count(fnb.GetHash())) { //seen
            fundamentalnodeSync.AddedFundamentalnodeList(fnb.GetHash());
            return;
        }
        mapSeenFundamentalnodeBroadcast.insert(std::make_pair(fnb.GetHash(), fnb));

        int nDoS = 0;
        if (!fnb.CheckAndUpdate(nDoS)) {
            if (nDoS > 0)
                Misbehaving(pfrom->GetId(), nDoS);

            //failed
            return;
        }

        // make sure the vout that was signed is related to the transaction that spawned the Fundamentalnode
        //  - this is expensive, so it's only done once per Fundamentalnode
        if (!fnb.IsInputAssociatedWithPubkey()) {
            LogPrintf("CFundamentalnodeMan::ProcessMessage() : fnb - Got mismatched pubkey and vin\n");
            Misbehaving(pfrom->GetId(), 33);
            return;
        }

        // make sure it's still unspent
        //  - this is checked later by .check() in many places and by ThreadCheckObfuScationPool()
        if (fnb.CheckInputsAndAdd(nDoS)) {
            // use this as a peer
            addrman.Add(CAddress(fnb.addr, NODE_NETWORK), pfrom->addr, 2 * 60 * 60);
            fundamentalnodeSync.AddedFundamentalnodeList(fnb.GetHash());
        } else {
            LogPrint("fundamentalnode","fnb - Rejected Fundamentalnode entry %s\n", fnb.vin.prevout.hash.ToString());

            if (nDoS > 0)
                Misbehaving(pfrom->GetId(), nDoS);
        }
    }

    else if (strCommand == "fnp") { //Fundamentalnode Ping
        CFundamentalnodePing fnp;
        vRecv >> fnp;

        LogPrint("fundamentalnode", "fnp - Fundamentalnode ping, vin: %s\n", fnp.vin.prevout.hash.ToString());

        if (mapSeenFundamentalnodePing.count(fnp.GetHash())) return; //seen
        mapSeenFundamentalnodePing.insert(std::make_pair(fnp.GetHash(), fnp));

        int nDoS = 0;
        if (fnp.CheckAndUpdate(nDoS)) return;

        if (nDoS > 0) {
            // if anything significant failed, mark that node
            Misbehaving(pfrom->GetId(), nDoS);
        } else {
            // if nothing significant failed, search existing Fundamentalnode list
            CFundamentalnode* pmn = Find(fnp.vin);
            // if it's known, don't ask for the fnb, just return
            if (pmn != NULL) return;
        }

        // something significant is broken or mn is unknown,
        // we might have to ask for a fundamentalnode entry once
        AskForMN(pfrom, fnp.vin);

    } else if (strCommand == "obseg") { //Get Fundamentalnode list or specific entry

        CTxIn vin;
        vRecv >> vin;

        if (vin == CTxIn()) { //only should ask for this once
            //local network
            bool isLocal = (pfrom->addr.IsRFC1918() || pfrom->addr.IsLocal());

            if (!isLocal && Params().NetworkID() == CBaseChainParams::MAIN) {
                std::map<CNetAddr, int64_t>::iterator i = mAskedUsForFundamentalnodeList.find(pfrom->addr);
                if (i != mAskedUsForFundamentalnodeList.end()) {
                    int64_t t = (*i).second;
                    if (GetTime() < t) {
                        Misbehaving(pfrom->GetId(), 34);
                        LogPrint("fundamentalnode","obseg - peer already asked me for the list\n");
                        return;
                    }
                }
                int64_t askAgain = GetTime() + FUNDAMENTALNODES_DSEG_SECONDS;
                mAskedUsForFundamentalnodeList[pfrom->addr] = askAgain;
            }
        } //else, asking for a specific node which is ok


        int nInvCount = 0;

        for (CFundamentalnode& mn : vFundamentalnodes) {
            if (mn.addr.IsRFC1918()) continue; //local network

            if (mn.IsEnabled()) {
                LogPrint("fundamentalnode", "obseg - Sending Fundamentalnode entry - %s \n", mn.vin.prevout.hash.ToString());
                if (vin == CTxIn() || vin == mn.vin) {
                    CFundamentalnodeBroadcast fnb = CFundamentalnodeBroadcast(mn);
                    uint256 hash = fnb.GetHash();
                    pfrom->PushInventory(CInv(MSG_FUNDAMENTALNODE_ANNOUNCE, hash));
                    nInvCount++;

                    if (!mapSeenFundamentalnodeBroadcast.count(hash)) mapSeenFundamentalnodeBroadcast.insert(std::make_pair(hash, fnb));

                    if (vin == mn.vin) {
                        LogPrint("fundamentalnode", "obseg - Sent 1 Fundamentalnode entry to peer %i\n", pfrom->GetId());
                        return;
                    }
                }
            }
        }

        if (vin == CTxIn()) {
            pfrom->PushMessage("ssc", FUNDAMENTALNODE_SYNC_LIST, nInvCount);
            LogPrint("fundamentalnode", "obseg - Sent %d Fundamentalnode entries to peer %i\n", nInvCount, pfrom->GetId());
        }
    }
}

void CFundamentalnodeMan::Remove(CTxIn vin)
{
    LOCK(cs);

    std::vector<CFundamentalnode>::iterator it = vFundamentalnodes.begin();
    while (it != vFundamentalnodes.end()) {
        if ((*it).vin == vin) {
            LogPrint("fundamentalnode", "CFundamentalnodeMan: Removing Fundamentalnode %s - %i now\n", (*it).vin.prevout.hash.ToString(), size() - 1);
            vFundamentalnodes.erase(it);
            break;
        }
        ++it;
    }
}

void CFundamentalnodeMan::UpdateFundamentalnodeList(CFundamentalnodeBroadcast fnb)
{
    mapSeenFundamentalnodePing.insert(std::make_pair(fnb.lastPing.GetHash(), fnb.lastPing));
    mapSeenFundamentalnodeBroadcast.insert(std::make_pair(fnb.GetHash(), fnb));
    fundamentalnodeSync.AddedFundamentalnodeList(fnb.GetHash());

    LogPrint("fundamentalnode","CFundamentalnodeMan::UpdateFundamentalnodeList() -- fundamentalnode=%s\n", fnb.vin.prevout.ToString());

    CFundamentalnode* pmn = Find(fnb.vin);
    if (pmn == NULL) {
        CFundamentalnode mn(fnb);
        Add(mn);
    } else {
        pmn->UpdateFromNewBroadcast(fnb);
    }
}

std::string CFundamentalnodeMan::ToString() const
{
    std::ostringstream info;

    info << "Fundamentalnodes: " << (int)vFundamentalnodes.size() << ", peers who asked us for Fundamentalnode list: " << (int)mAskedUsForFundamentalnodeList.size() << ", peers we asked for Fundamentalnode list: " << (int)mWeAskedForFundamentalnodeList.size() << ", entries in Fundamentalnode list we asked for: " << (int)mWeAskedForFundamentalnodeListEntry.size();

    return info.str();
}

void ThreadCheckFundamentalnodes()
{
    if (fLiteMode) return; //disable all Fundamentalnode related functionality

    // Make this thread recognisable as the wallet flushing thread
    RenameThread("pivx-fundamentalnodeman");
    LogPrintf("Fundamentalnodes thread started\n");

    unsigned int c = 0;

    while (true) {
        MilliSleep(1000);

        // try to sync from all available nodes, one step at a time
        fundamentalnodeSync.Process();

        if (fundamentalnodeSync.IsBlockchainSynced()) {
            c++;

            // check if we should activate or ping every few minutes,
            // start right after sync is considered to be done
            if (c % FUNDAMENTALNODE_PING_SECONDS == 1) activeFundamentalnode.ManageStatus();

            if (c % 60 == 0) {
                mnodeman.CheckAndRemove();
                fundamentalnodePayments.CleanPaymentList();
                CleanTransactionLocksList();
            }
        }
    }
}
