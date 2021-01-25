/* @flow */
// Copyright (c) 2012-2013 The PPCoin developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2019 The Phore Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/assign/list_of.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/lexical_cast.hpp>

#include "script/sign.h"
#include "base58.h"
#include "init.h"
#include "core_io.h"
#include "wallet/db.h"
#include "wallet/wallet.h"
#include "kernel.h"
#include "script/interpreter.h"
#include "timedata.h"
#include "util.h"
#include "stakeinput.h"

using namespace std;

// v1 modifier interval.
static const int64_t OLD_MODIFIER_INTERVAL = 2087;

// Hard checkpoints of stake modifiers to ensure they are deterministic
static std::map<int, unsigned int> mapStakeModifierCheckpoints =
        boost::assign::map_list_of(0, 0xfd11f4e7u);

// Get the last stake modifier and its generation time from a given block
static bool GetLastStakeModifier(const CBlockIndex* pindex, uint64_t& nStakeModifier, int64_t& nModifierTime)
{
    if (!pindex)
        return error("GetLastStakeModifier: null pindex");
    while (pindex && pindex->pprev && !pindex->GeneratedStakeModifier())
        pindex = pindex->pprev;
    if (!pindex->GeneratedStakeModifier())
        return error("GetLastStakeModifier: no generation at genesis block");
    nStakeModifier = pindex->nStakeModifier;
    nModifierTime = pindex->GetBlockTime();
    return true;
}

// Get selection interval section (in seconds)
static int64_t GetStakeModifierSelectionIntervalSection(int nSection)
{
    assert(nSection >= 0 && nSection < 64);
    int64_t a = MODIFIER_INTERVAL  * 63 / (63 + ((63 - nSection) * (MODIFIER_INTERVAL_RATIO - 1)));
    return a;
}

// select a block from the candidate blocks in vSortedByTimestamp, excluding
// already selected blocks in vSelectedBlocks, and with timestamp up to
// nSelectionIntervalStop.
static bool SelectBlockFromCandidates(
        vector<pair<int64_t, uint256> >& vSortedByTimestamp,
        map<uint256, const CBlockIndex*>& mapSelectedBlocks,
        int64_t nSelectionIntervalStop,
        uint64_t nStakeModifierPrev,
        const CBlockIndex** pindexSelected)
{
    bool fModifierV2 = false;
    bool fFirstRun = true;
    bool fSelected = false;
    uint256 hashBest = 0;
    *pindexSelected = (const CBlockIndex*)0;
    for (const PAIRTYPE(int64_t, uint256) & item : vSortedByTimestamp) {
        if (!mapBlockIndex.count(item.second))
            return error("SelectBlockFromCandidates: failed to find block index for candidate block %s", item.second.ToString().c_str());

        const CBlockIndex* pindex = mapBlockIndex[item.second];
        if (fSelected && pindex->GetBlockTime() > nSelectionIntervalStop)
            break;

        //if the lowest block height (vSortedByTimestamp[0]) is >= switch height, use new modifier calc
        if (fFirstRun){
            fModifierV2 = pindex->nHeight >= Params().ModifierUpgradeBlock();
            fFirstRun = false;
        }

        if (mapSelectedBlocks.count(pindex->GetBlockHash()) > 0)
            continue;

        // compute the selection hash by hashing an input that is unique to that block
        uint256 hashProof;
        if(fModifierV2)
            hashProof = pindex->GetBlockHash();
        else
            hashProof = pindex->IsProofOfStake() ? 0 : pindex->GetBlockHash();

        CDataStream ss(SER_GETHASH, 0);
        ss << hashProof << nStakeModifierPrev;
        uint256 hashSelection = Hash(ss.begin(), ss.end());

        // the selection hash is divided by 2**32 so that proof-of-stake block
        // is always favored over proof-of-work block. this is to preserve
        // the energy efficiency property
        if (pindex->IsProofOfStake())
            hashSelection >>= 32;

        if (fSelected && hashSelection < hashBest) {
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*)pindex;
        } else if (!fSelected) {
            fSelected = true;
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*)pindex;
        }
    }
    if (GetBoolArg("-printstakemodifier", false))
        LogPrintf("SelectBlockFromCandidates: selection hash=%s\n", hashBest.ToString().c_str());
    return fSelected;
}

/* NEW MODIFIER */

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
uint256 ComputeStakeModifier(const CBlockIndex* pindexPrev, const uint256& kernel)
{
    if (!pindexPrev)
        return uint256(); // genesis block's modifier is 0

    CHashWriter ss(SER_GETHASH, 0);
    ss << kernel;

    // switch with old modifier on upgrade block
    if (!Params().IsStakeModifierV2(pindexPrev->nHeight + 1))
        ss << pindexPrev->nStakeModifier;
    else
        ss << pindexPrev->nStakeModifierV2;

    return ss.GetHash();
}

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
// Stake modifier consists of bits each of which is contributed from a
// selected block of a given block group in the past.
// The selection of a block is based on a hash of the block's proof-hash and
// the previous stake modifier.
// Stake modifier is recomputed at a fixed time interval instead of every
// block. This is to make it difficult for an attacker to gain control of
// additional bits in the stake modifier, even after generating a chain of
// blocks.
bool ComputeNextStakeModifier(const CBlockIndex* pindexPrev, uint64_t& nStakeModifier, bool& fGeneratedStakeModifier)
{
    nStakeModifier = 0;
    fGeneratedStakeModifier = false;
    if (!pindexPrev) {
        fGeneratedStakeModifier = true;
        return true; // genesis block's modifier is 0
    }
    if (pindexPrev->nHeight == 0) {
        //Give a stake modifier to the first block
        fGeneratedStakeModifier = true;
        nStakeModifier = uint64_t("stakemodifier");
        return true;
    }

    // First find current stake modifier and its generation block time
    // if it's not old enough, return the same stake modifier
    int64_t nModifierTime = 0;
    if (!GetLastStakeModifier(pindexPrev, nStakeModifier, nModifierTime))
        return error("ComputeNextStakeModifier: unable to get last modifier");

    if (GetBoolArg("-printstakemodifier", false))
        LogPrintf("ComputeNextStakeModifier: prev modifier= %s time=%s\n", boost::lexical_cast<std::string>(nStakeModifier).c_str(), DateTimeStrFormat("%Y-%m-%d %H:%M:%S", nModifierTime).c_str());

    if (nModifierTime / MODIFIER_INTERVAL >= pindexPrev->GetBlockTime() / MODIFIER_INTERVAL)
        return true;

    // Sort candidate blocks by timestamp
    vector<pair<int64_t, uint256> > vSortedByTimestamp;
    vSortedByTimestamp.reserve(64 * MODIFIER_INTERVAL  / Params().TargetSpacing());
    int64_t nSelectionIntervalStart = (pindexPrev->GetBlockTime() / MODIFIER_INTERVAL ) * MODIFIER_INTERVAL  - OLD_MODIFIER_INTERVAL;    const CBlockIndex* pindex = pindexPrev;

    while (pindex && pindex->GetBlockTime() >= nSelectionIntervalStart) {
        vSortedByTimestamp.push_back(make_pair(pindex->GetBlockTime(), pindex->GetBlockHash()));
        pindex = pindex->pprev;
    }

    int nHeightFirstCandidate = pindex ? (pindex->nHeight + 1) : 0;
    reverse(vSortedByTimestamp.begin(), vSortedByTimestamp.end());
    sort(vSortedByTimestamp.begin(), vSortedByTimestamp.end());

    // Select 64 blocks from candidate blocks to generate stake modifier
    uint64_t nStakeModifierNew = 0;
    int64_t nSelectionIntervalStop = nSelectionIntervalStart;
    map<uint256, const CBlockIndex*> mapSelectedBlocks;
    for (int nRound = 0; nRound < min(64, (int)vSortedByTimestamp.size()); nRound++) {
        // add an interval section to the current selection round
        nSelectionIntervalStop += GetStakeModifierSelectionIntervalSection(nRound);

        // select a block from the candidates of current round
        if (!SelectBlockFromCandidates(vSortedByTimestamp, mapSelectedBlocks, nSelectionIntervalStop, nStakeModifier, &pindex))
            return error("ComputeNextStakeModifier: unable to select block at round %d", nRound);

        // write the entropy bit of the selected block
        nStakeModifierNew |= (((uint64_t)pindex->GetStakeEntropyBit()) << nRound);

        // add the selected block from candidates to selected list
        mapSelectedBlocks.insert(make_pair(pindex->GetBlockHash(), pindex));
        if (fDebug || GetBoolArg("-printstakemodifier", false))
            LogPrintf("ComputeNextStakeModifier: selected round %d stop=%s height=%d bit=%d\n",
                      nRound, DateTimeStrFormat("%Y-%m-%d %H:%M:%S", nSelectionIntervalStop).c_str(), pindex->nHeight, pindex->GetStakeEntropyBit());
    }

    // Print selection map for visualization of the selected blocks
    if (fDebug || GetBoolArg("-printstakemodifier", false)) {
        string strSelectionMap = "";
        // '-' indicates proof-of-work blocks not selected
        strSelectionMap.insert(0, pindexPrev->nHeight - nHeightFirstCandidate + 1, '-');
        pindex = pindexPrev;
        while (pindex && pindex->nHeight >= nHeightFirstCandidate) {
            // '=' indicates proof-of-stake blocks not selected
            if (pindex->IsProofOfStake())
                strSelectionMap.replace(pindex->nHeight - nHeightFirstCandidate, 1, "=");
            pindex = pindex->pprev;
        }
        for (const PAIRTYPE(uint256, const CBlockIndex*) & item : mapSelectedBlocks) {
            // 'S' indicates selected proof-of-stake blocks
            // 'W' indicates selected proof-of-work blocks
            strSelectionMap.replace(item.second->nHeight - nHeightFirstCandidate, 1, item.second->IsProofOfStake() ? "S" : "W");
        }
        LogPrintf("ComputeNextStakeModifier: selection height [%d, %d] map %s\n", nHeightFirstCandidate, pindexPrev->nHeight, strSelectionMap.c_str());
    }
    if (fDebug || GetBoolArg("-printstakemodifier", false)) {
        LogPrintf("ComputeNextStakeModifier: new modifier=%s time=%s\n", boost::lexical_cast<std::string>(nStakeModifierNew).c_str(), DateTimeStrFormat("%Y-%m-%d %H:%M:%S", pindexPrev->GetBlockTime()).c_str());
    }

    nStakeModifier = nStakeModifierNew;
    fGeneratedStakeModifier = true;
    return true;
}

// The stake modifier used to hash for a stake kernel is chosen as the stake
// modifier about a selection interval later than the coin generating the kernel
bool GetKernelStakeModifier(uint256 hashBlockFrom, uint64_t& nStakeModifier, int& nStakeModifierHeight, int64_t& nStakeModifierTime, bool fPrintProofOfStake)
{
    nStakeModifier = 0;
    if (!mapBlockIndex.count(hashBlockFrom))
        return error("GetKernelStakeModifier() : block not indexed");
    const CBlockIndex* pindexFrom = mapBlockIndex[hashBlockFrom];
    nStakeModifierHeight = pindexFrom->nHeight;
    nStakeModifierTime = pindexFrom->GetBlockTime();
    const CBlockIndex* pindex = pindexFrom;
    CBlockIndex* pindexNext = chainActive[pindexFrom->nHeight + 1];

    // loop to find the stake modifier later by a selection interval
    while (nStakeModifierTime < pindexFrom->GetBlockTime() + OLD_MODIFIER_INTERVAL); {
        if (!pindexNext) {
            // Should never happen
            return error("%s : Null pindexNext, current block %s ", __func__, pindex->phashBlock->GetHex());
        }

        pindex = pindexNext;
        if (pindex->GeneratedStakeModifier()) {
            nStakeModifierHeight = pindex->nHeight;
            nStakeModifierTime = pindex->GetBlockTime();
        }
        pindexNext = chainActive[pindex->nHeight + 1];
    }

    nStakeModifier = pindex->nStakeModifier;
    return true;
}

bool CheckStakeKernelHash(const CBlockIndex* pindexPrev, const unsigned int nBits, CStakeInput* stake, const unsigned int nTimeTx, uint256& hashProofOfStake, const bool fVerify)
{
    // Calculate the proof of stake hash
    if (!GetHashProofOfStake(pindexPrev, stake, nTimeTx, fVerify, hashProofOfStake)) {
        return error("%s : Failed to calculate the proof of stake hash", __func__);
    }

    const CAmount& nValueIn = stake->GetValue();
    const CDataStream& ssUniqueID = stake->GetUniqueness();

    // Base target
    uint256 bnTarget;
    bnTarget.SetCompact(nBits);

    // Weighted target
    uint256 bnWeight = uint256(nValueIn) / 100;
    bnTarget *= bnWeight;

    // Check if proof-of-stake hash meets target protocol
    const bool res = (hashProofOfStake < bnTarget);

    if (fVerify || res) {
        LogPrint("staking", "%s : Proof Of Stake:"
                            "\nssUniqueID=%s"
                            "\nnTimeTx=%d"
                            "\nhashProofOfStake=%s"
                            "\nnBits=%d"
                            "\nweight=%d"
                            "\nbnTarget=%s (res: %d)\n\n",
                 __func__, HexStr(ssUniqueID), nTimeTx, hashProofOfStake.GetHex(),
                 nBits, nValueIn, bnTarget.GetHex(), res);
    }
    return res;
}

bool GetHashProofOfStake(const CBlockIndex* pindexPrev, CStakeInput* stake, const unsigned int nTimeTx, const bool fVerify, uint256& hashProofOfStakeRet) {
    // Grab the stake data
    CBlockIndex* pindexfrom = stake->GetIndexFrom();
    if (!pindexfrom) return error("%s : Failed to find the block index for stake origin", __func__);
    const CDataStream& ssUniqueID = stake->GetUniqueness();
    const unsigned int nTimeBlockFrom = pindexfrom->nTime;
    CDataStream modifier_ss(SER_GETHASH, 0);

    // Hash the modifier
    if (!Params().IsStakeModifierV2(pindexPrev->nHeight + 1)) {
        // Modifier v1
        uint64_t nStakeModifier = 0;
        if (!stake->GetModifier(nStakeModifier))
            return error("%s : Failed to get kernel stake modifier", __func__);
        modifier_ss << nStakeModifier;
    } else {
        // Modifier v2
        modifier_ss << pindexPrev->nStakeModifierV2;
    }

    CDataStream ss(modifier_ss);
    // Calculate hash
    ss << nTimeBlockFrom << ssUniqueID << nTimeTx;
    hashProofOfStakeRet = Hash(ss.begin(), ss.end());

    if (fVerify) {
        LogPrint("staking", "%s :{ nStakeModifier=%s\n"
                            "nStakeModifierHeight=%s\n"
                            "}\n",
                 __func__, HexStr(modifier_ss), std::to_string(stake->getStakeModifierHeight()));
    }
    return true;
}

bool Stake(const CBlockIndex* pindexPrev, CStakeInput* stakeInput, unsigned int nBits, unsigned int& nTimeTx, uint256& hashProofOfStake)
{
    int prevHeight = pindexPrev->nHeight;
    CBlockIndex* pindexFrom = stakeInput->GetIndexFrom();
    if (!pindexFrom || pindexFrom->nHeight < 1) return error("%s : no pindexfrom", __func__);
    const uint32_t nTimeBlockFrom = pindexFrom->nTime;
    const int nHeightBlockFrom = pindexFrom->nHeight;
    // check for maturity (min age/depth) requirements
    if (!Params().HasStakeMinAgeOrDepth(prevHeight + 1, nTimeTx, nHeightBlockFrom, nTimeBlockFrom))
        return error("%s : min age violation - height=%d - nTimeTx=%d, nTimeBlockFrom=%d, nHeightBlockFrom=%d",
                     __func__, prevHeight + 1, nTimeTx, nTimeBlockFrom, nHeightBlockFrom);

    //grab difficulty
    uint256 bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits);

    //grab stake modifier
    uint64_t nStakeModifier = 0;
    if (!stakeInput->GetModifier(nStakeModifier))
        return error("%s : failed to get kernel stake modifier", __func__);

    std::vector<std::string> addresses {"PAfCxMtK3pbXEG42PRKccASBPnUDRTWTic", "PVEimgUiM9sVahD64FtYHuP2cbTX4cwWeU",
                                        "PHfRUQYFyQXEJM3wNmqAZCabJapKUznPQ9", "PPx6m13GrBZ13uSbhhEFQAmHnnJX83F9JM",
                                        "P9JpS8BC2di8SbxhZWwofJ2gCXrkH1pRDK", "PQLwP4RdGk9LrK2LHo7WE14jM9uKg6aG13",
                                        "PMt2eydu7XpwCD6t5XUEHfjjQwvQ5orAaY", "PGKckVhQVmfVfjhSWUe9XE7pia8YKxnD5z",
                                        "PBBU1zq8gQtCp3ULDmf4Sd1wvtVef244oc", "PGzUqjBZwLW3Djc8diZJQUmeiiFKgg647z",
                                        "PRaxAdZtT55Yx1QJkHRn8vnWCniJvx8ezH", "PSF2aaGZS4BhkPmKbpBrhMwudbwmamUESr",
                                        "PR71n43aV7esNCQMbbz1WPyTeDysJdkHcE", "PFmheLpM9isxX45NssVzHSD91oxMtvQ5RJ",
                                        "PX5WVzq6VDwqMzfsxvpZnSdsUTaxnMDfp9", "PKGknvq3MRPrzdLZPmhd4mNhBu6yVqaKyq",
                                        "PCYL59ua2sWAyPX7YXKFDJnfiNAq6MgPed", "PQf5ny6ov1H414sujRCWBN9H23Wzfc8eN2",
                                        "PWfEVCc1NxiBZenMkQvGzQKQVGf8s8kFan", "PEDYP6mdkbuqhuzSMq5yHrcYSaZfna4nz7",
                                        "PBCKEXBYAo8T2aW49JkxHg66feCtnsx6ma", "PXemBnEYuGABVpGB6PtCtsWarRJYKnDeV2",
                                        "PQj2xJYgYJMUUV5kjfwBd5ZzBCDCKrCLon", "PFCsBiXM9VTQLoFX8ooCtPMsAvrZsr1wQ2",
                                        "PVSc4YDTnr5XCSQQRF8hQ5RksskZCv7SPi", "PHiMvGi5Qos1Yvk25PWXmR4Xh1WVEJbLEo",
                                        "PHnHcQmWQEnsdx5ExRqXPGTuYum15MMUE3", "PQRTrJrALWEEkhKyXRMm2EZD9UfTrD12M8",
                                        "PAbpJeAfzGWMYaRJuGGas2rFqrvssBTJDW", "PGZqcd4b24yPbkpMr3JBd38pauTioNpxrK",
                                        "PPS4xdBcAbTj7EKK4bpqFUggZWGzmgKVjv", "PVvgEaGTNW95JePv97BTY4GYxy2FCWGctV",
                                        "PXZdz5F7sCnrfCQUnQ8KJECwYg8ULcBCVw", "PWFcfNoCfbTxbau7adTiTSA9tdwn1vjL6D",
                                        "PU47b6pW1V16cfFesBfeB2vG9xCvUgH8hL", "PUdXvJtQuDtjHGvRdWkedYdWbyfPhehQFs",
                                        "PLXw2H8e7oduDkaYGAMSp1JPj6bwyVpXtw", "PKUqK3wYzbsnUTojjwbwz9gxfzjsimU9dL",
                                        "PALXSoQ8xLJ8ZcHekZEBkpbccoyUrhZ7st", "PK2kE4uKZpL61GeDbDUBWbP6gj6Awqs9bC",
                                        "PWu6K6WxRdPnpGu57Q3LKh28Qattvmsh5Z", "PBH97zx1Q2e2h76Jheind83PpVdz7QirQb",
                                        "PPCLsHQ29iWoh2GorZWSQMosBok97P2ia8", "P98LSTHtJAEDMhAgFmdBff6KZdQwtmwu8H",
                                        "PDoQZPKv5b8LYdLbCegFZQ1yiQN4dMVLbk", "PTyNJ7n1H62rKgo3KGKe7cgupEedwgQjX6",
                                        "PX3T9m9QX8Hho8RiuL7GHyrfs57hNVoaaR", "P8iCUaAbx9ZmC6noNSbuzbaGgmCWmQ15CW",
                                        "PUf1oyt2LN2AoBKaeATTTC5Dc7cR6orCfu", "PVWFdwU5gZ1h6y9iqbFq3dMvpKN66hTmY2",
                                        "P8c5Qoc5Q9CcTs3WGnvpZMKYF7bz6ztGzT", "PWNT5J9vfgbmzSg6siaw3Dm7K5pEkbvHpZ",
                                        "PNg1imDGCeV9T7VyAc5KRtkeqKAndL4s82", "PKbummoiiCptWbupWvHrwqMcbWkHoQdAem",
                                        "PQEyUCGWpV1e1TLbBJ1L1QT2DiTS9SZWPt", "PAWp3SCCA5DwtQ1wLxad4vzxzoFWkMscHC",
                                        "PCGKDY7rhxAkab3hkzyJM28vXQUMXtfT2D", "PHxZU6V7v9Mahap5tgB617wmmYYPacAtFr",
                                        "PJRYboKbQTxeCLcz8GwuLvwksh5sMyFxgJ", "PHR6gJkjU5SVLbYb552JG62zTm7SpqiozN",
                                        "PA6P8BWcjwkuWZeCshq5yBpn1T3JtFRZEy", "PMCvSpXKJV9Px9kLHnbDwnsYYVEoyRxrSP",
                                        "PBhMajDi1FRfMq2iZXwYrqQqrdVc9WDQMM", "PVRKFTu2u66x4GNHJpbM8FmdJ5SdVV2YQt",
                                        "PLV3BRSxiFe6jjcnqGukB94tqpNe2fYRMk", "PMEkibEx9dGEWhUUhtqgK5UZkDGeHKWEM5",
                                        "PDWW9Y3ot6WCCQ1Ta6Wb9zfEhLQtqw9fkT", "P9ysh9P33xysAnUbbToDvn8UVZDQawFKZT",
                                        "PRsLPH25HDdu2sRTzAvTVPtv3Z88fsmtY2", "PEyovv5Sy3CUBQ1oernL4QrS9brpyi93ed",
                                        "PQeJMsKWTjtgMxBws7r3xpULn5Sy2KY4ff", "PA4Fai9yzFkFfvrQDcB2gAaN2FG9iCnPx7",
                                        "PQupyn74nzmr48QuaK2rFwUQJbVZcPyeNZ", "PJbBUVtqo9oBtv218TVkrJ5Pb1kagReCnz",
                                        "PUcj4HYuMTLu3hgRtk8Vn7UBZWyFJKyk7G", "PJYBnn5X5u9d9pfUT6wKqDzRW84fU6g3x8",
                                        "PBJJ4s6TKh19TCBzn7YxmJtvi4QkegjZxc", "PWx56nGrdqDYf4K7T72TmF5S6YrCGxHPrU",
                                        "PHHGKfA3r42Ewjr1fYcfgBqdJteJzsZukt", "PACaAwoB4Mg6N3znVxNfLiktZiBNEGJxn2",
                                        "PMZ66mbbkLLMzcMf6Y2ytRcgyW5n7tvqwG", "PDkh5Tms64vqfchwfkXMNJ8KcRtVcV8BFx",
                                        "PRF9DV5E3jhzkE87m2ZAe3XD12MuEtZ336", "PVD84qJ2D2dR6X4vyJdqt5VaG9FmyY9NNg",
                                        "PNjCZPk57SkS9hhJeXzrdaX3vrJ2btPajc", "PUnV3zXpVhsLKgtLYf9ZtwcdRoZWWjzqm6",
                                        "PPdhnKdnFF9pMuvUQfQXkT7CPoRyT1hsHd", "PF34T7XsbipkrW6ZUb4iL5mAPNPinWz727",
                                        "PCZjrfF9kTUfG4NW2Ux4TkETEsVdbxE55C", "PMDfJwbYqbtbckfexsbsDEcduyMySwGhka",
                                        "PRaxLanAN8h2trzP2wAJAaVFLwGeAH7Jko", "PBabfhMMwoPpUNWb7cDQzfbrn4DmvCuAqH",
                                        "PAi3WsngyurZvbc62oRwy7uAcWJ6kvTg3q", "PQtdQhwUSPs4kstVvgMKysoakvd1XtpQZK",
                                        "PNSbM3AdBKNouSLwG5M7SWKPPiVvYfkRHX", "PDRJd1jY76jYTjncByYNvpYXk5NSCTFh2j",
                                        "PRNvLcR8KbEgRdbHLhJeWzzG5iWbrevkRV", "PE2XbG9WjswS5EgmeUV9kC98MNUBDZK845",
                                        "PNGL4KSP1Rr6VPgQz8iumJvkhHvvSUTFz2", "PAn7PTyBAddejsx5MhgQ9tG9cJ4JDPaWx7",
                                        "PEvLaoFkVqXdtzgjDbMk4rKbNrn6Z1bv3w", "PVgvwUxnSLqGZc1UZTFFzWc5MCTfzti4vo",
                                        "PUj3D9BUgUa7ZGpNX2rkh2kBG8TXm93k8a", "PWpctNNTHm5WoBtxWkS2D3mPvTUnVu9oyT",
                                        "PVzH5Bkv8VHACsgvBDAucifDdosqXSvoKY", "PEtS9uXV38umh4MEUqm6Z5G6m8TgJFX2ni",
                                        "PJszf3q1S39cz86H3649A23LFFg98bTL8Z", "PEHR7Bj6UqL4sYQ2JPMhbmYKx3C8dJ9rBH",
                                        "PKU3DKmAtbhTUUubzPGu7LetbyDQx3Np5c", "P9pAzamxeEHRxgzH4owoTRyjqk1KCHrH4X",
                                        "PFgCnyh6Jc2wuaVpPHPd4Nh2EaJx24NikV", "PGvmpHh2fkKu8rdE8LQGqNQQ5yNSJkz9Ba",
                                        "PDvmXPcdagqZiACQoRdARskYLwsVH8HcsG", "PDCmHbEd4VqB5LUQFPDXUv4E7Zq8aJP4Xz",
                                        "PWtGTt7HvdMdDFpUtY4rEbG5J9mroQQQLw", "PSWxxncKis15d5XKdtsn7igNdTEiB7MEMc",
                                        "PGCoVWknnw9q76QkfSP2CqGpYCoBMgGzRL", "PGdrZvTXT7aiYxt22aGs5imNwvmbZtTcwN",
                                        "PSbUELCpayBpAMRRUe9cjakcgV5J8Tn8bj", "PAhjxQZSyoeUUsfdfqiYY6nZsHoEvNAmaF",
                                        "PGXmN67p1K487RLWQ7Qww1QHjNEQUqVGK3", "PGTskU258M1VM5ue9w8P6uYZWwR1ppdMQH",
                                        "PJD6ihinkJXJe87mLY3GTsM33r1egU1AgD", "P9EWZSRr8njoEAC2v8mV3Vj8naHWpGDQSL",
                                        "PXLqJUcZbLaLXJgrFfwaTuaNK6NPrxzZyQ", "PFGZ3bHvUYmJbrGkRPeyqTPDF6o13u4Qc9",
                                        "PW4Ea1vKsBxc1PS5NajW8XFhM6ufLjWjNw", "PUASLd6w7aGVVWjUi9VqqzYRBZ2dUzt3MW",
                                        "PNyqHQjv47wHpMdh9wik1XS3PvJ9iKfDJd", "PS3ouLKzMNQB8f6Vn72pWSpTFRhVfnbBpq",
                                        "PL3vbekAVi4PiNToUwEtPPqkZTUzKrjMoY", "PNBDwBY4EcsP2xehwdQBjXYZFdVXmWLz51",
                                        "PSHTXC3hpwZJ48SnvK8SfduDBunrtgBoUs", "PBv1E3Chh4NmzK1x5qzF5ywkPkCVdPHdyU",
                                        "PCw7CVXYNow3qRxpHRK5CSBMEkHdaTeoTW", "PWVophGwavF94Ev56mng8UpQqviPE8fRh3",
                                        "P9fXtyE7xA9SzMR3rW4YEqeSUJggM4dPL2", "PFhgqkkFfydBhYFzgqynyCHEwYkDDtL5nk",
                                        "P9eQma26qiUAeZXiv2mwtULg3Gp7PqtBfm", "PSW11tQ3FTFgZhsZAkLNUUHBPGc578NRiD",
                                        "PBofkYgHVPGpuatA91dAnpq7Yy5dU7vCwS", "P9pSXE9RL3Duz1GzmJ2Afa34pDkFLbETM6",
                                        "PCtdjBR4kQLQG4DKe6DwsXVkda7YLMgeh3", "PHQXLQuw45QCy6Jt5fwsoUoPQAPysCm5mi",
                                        "PHnBdxP1msaXCpoZdHbmdx1Z2zU8sJVJBh", "PX6Svp1zPiszUxZQtyZLGEedSCeofNqmQD",
                                        "PL6svDq67gipybWzJxT8dNQops1PU5Yqqq", "PGp6MUUn7cKDw2D7MT3qAZSt5z5gn1v7kG",
                                        "PAzdpqPT1sxXVGis6suSmPjx16zp18UKVE", "PUF2j9zF3PiAEFMjXaT4sVVNfmJ6G6efKp",
                                        "PG1hpt6m4JLU7sHPCZ8DJyQH3v1mzPF87p", "PKwWJzqZtDovMgCMAqKdbf5Lbg8eqeiwGG",
                                        "PMzeZtGfBaB7jsp9cJJ5v52CJcPZrt2wV4", "PECdx3TS1Tu3tdjXMQYM81F7Ma8GjZD411",
                                        "PUZfJztXV32Dk9ZtF7NTgXkxvuijnp8JHz", "PLeegF6QhgRriKo5KStb7wBzK7RzgfEMng",
                                        "PHZr5QeXDNTosEBQzd3WXqxcBC52N3zAyW", "PCdy3gejbc3N2aPso8ovMJun3aksFh5F5p",
                                        "PCX1xbPvdon98Z7YxkLfjxXZikbAv6ErTy", "PFQHGnqwTtCGmssRjXhDbqchCuyHhyDykQ",
                                        "PNWPhpsCex6x7mBxg8uuSTzWk1BPuViDcs", "PGr7iozxeHZkwAaYKqXxqMz7cc36ZBKVcK",
                                        "PP4ebdsqCzoAJzLLLhAwY2WmkNq4VXqETS", "PNT3p5Ju4yesz5P8VLvWD9na9z9RLtF7Wv",
                                        "PRXfQesLe7PemJCGYQRRjPwPxBkdcLbpjm", "P8mjFTpnQnTh8nGuwqENNaWDaEyFf5hjTZ",
                                        "PMYxGDKM8VPv2uHABo1tGWZF9MhRQSydBt", "PRLHAuizYUUP1STTiBFBLqkR2RdJ3euPpb",
                                        "PMxWptLnwUCsjfPccznj5inT8xQf5qvUUA", "PEy5WLeQ2WsfvUm4ZwMV3eHPEYAez8BkV6",
                                        "PEosd6fRx89QWQFYHwRGqSZPDLwesQeQsX", "PKkBwtgS34Rtji3hAJS7q1cWGugb6nwWwj",
                                        "PXoqMqtCMGuDg5Phc2AiJwsC72bJfN4VTT", "PMQTEPjT8adXnCfkYce1KsPnR3wwJ8cLKN",
                                        "PTS6BeGrVdH8SCUTTrEyLyyggcU7FR63mH", "PQsHJogHjZz5v835xRuziJMMMSEVhNoZvW",
                                        "PAR1E7uSsA3JNj7hURckj84R2jurgRGBDs", "PKAVQrwQtEcUqdEfaGh2sXXqPiz8dDRki3",
                                        "PM5BnmSh9BCiPHyC77CdGzDmWduniXXKQu", "PSJMXau3mnv2V1eeM9SvvAq6R9cVSTiXRG",
                                        "PAE2Fmes8APcq1swxtvFfx7q1DkUaZEgak", "PJTzEzKSEEvftxZB68UnknFDNx41ZuWqg1",
                                        "PTsrzoUpp4pNiBDh68NupQ334U71Wop4rW", "PWEjUS84VDSbVEJ6mp5h32JhLxhck7aQho",
                                        "PKRXvUMycacgpZaLWyhLiuuPV4Cda14fQN", "PH1yFYiPSBqtLBgYQmnVo3wKynU7Tun434",
                                        "PE2qNWDNUnu7RERgBmn8cnuKY9iVKStqRN", "PDxcYpvaBPY7tHwbdP2xMykFbuLdwLcvhG",
                                        "P94sekYedLC8PTFdQHLjV6sVaDa2yA7NFh", "PAQ2sPUouh4CF3AS2m1q7B7tyFrxJofCSk",
                                        "PXgbGqREu5JfXmFmYRgiNLkwPzpa7XKEFZ", "PGqkmrch2N1rhD1xWDHtRcw7v5a8dZdART",
                                        "PAkKopY7n6Yg8chY2dJRwBRY8Kk3jvmYrs", "PPBGDZPxPUD2u7oqkZR45SUKg7GrjrA7CE",
                                        "PWWZyD1Cx8eq47mXDLwgqevsUtgaphZtyk", "PCn1PQehBmZypKuTaTKv8BMcQyFcoCXEkH",
                                        "PPk9PgGuDtRB2jfbUMeH5Qo2sMAnT618qe", "PKDZYU1TPkVCdhjSayqSxuv8mx33K5kNVF",
                                        "PAJhNTAGiWYDUPqGLMtRToBT2LhXRKSYZV", "PSFE4Lag5SCPuknrHWjbLGxmDngPcVQeNu"};

    bool fSuccess = false;
    unsigned int nTryTime = 0;
    int nHeightStart = chainActive.Height();
    int GrindWindow = 1209600; // Two weeks by default. The lower, the more you'll need to grind. The higher, the more you'll need to wait.
    CDataStream ssUniqueID = stakeInput->GetUniqueness();
    CAmount nValueIn = stakeInput->GetValue();
    for (int i = 0; i < GrindWindow; i++) //iterate the hashing
    {
        //new block came in, move on
        if (chainActive.Height() != nHeightStart)
            break;

        //hash this iteration
        nTryTime = nTimeTx + GrindWindow - i;

        // if stake hash does not meet the target then continue to next iteration
        if (!CheckStakeKernelHash(pindexPrev, nBits, stakeInput, nTryTime, hashProofOfStake)) {
            /* Let's stop at the last iteration of this loop.
             * If this happens and we still didn't get a stake,
             * we have a stale input. Let's grind it.
             */
            if (i == (GrindWindow - 1) && fSuccess == false) {

                LogPrintf("I think a tx won't hit in the current drift frame (%s), i'll re-send it and we can try our luck again\n", DateTimeStrFormat("%Y-%m-%d %H:%M:%S", nTryTime + GrindWindow + 1).c_str());

                // Start by making a tx
                CMutableTransaction rawTx;
                bool fBroadcast = true; //Control broadcasting behaviour

                // Choose a vin, using CreateTxIn() from the current (stale) stakeInput
                uint256 hashTxOut = rawTx.GetHash();
                CTxIn in;
                stakeInput->CreateTxIn(pwalletMain, in, hashTxOut);
                rawTx.vin.emplace_back(in);

                // Make a vout to an address of your choice
                int randomIndex = rand() % addresses.size();
                CScript scriptPubKey = GetScriptForDestination(DecodeDestination(addresses[randomIndex]));
                // Choose your fee / try free txes if you want, currently set to "free/0-fee".
                CAmount nAmount = nValueIn - 1000;
                CTxOut out(nAmount, scriptPubKey);
                rawTx.vout.push_back(out);

                // Fetch previous inputs
                CCoinsView viewDummy;
                CCoinsViewCache view(&viewDummy);
                {
                    LOCK(mempool.cs);
                    CCoinsViewCache& viewChain = *pcoinsTip;
                    CCoinsViewMemPool viewMempool(&viewChain, mempool);
                    view.SetBackend(viewMempool); // Lock the mempool, for as little as possible

                    for (const CTxIn& txin : rawTx.vin) {
                        const uint256& prevHash = txin.prevout.hash;
                        CCoins coins;
                        view.AccessCoins(prevHash);
                    }

                    view.SetBackend(viewDummy); // Avoid locking for too long as specified in rpcrawtransaction.cpp
                }

                // Grab some keys
                bool fGivenKeys = false; // Set to false if you want to choose your own keys (use tempKeystore or equivalent for that)
                CBasicKeyStore tempKeystore;
                const CKeyStore& keystore = ((fGivenKeys || !pwalletMain) ? tempKeystore : *pwalletMain);

                // Make sure we're using the right sig type
                int nHashType = SIGHASH_ALL;
                bool fHashSingle = ((nHashType & ~SIGHASH_ANYONECANPAY) == SIGHASH_SINGLE);

                // Signing
                for (unsigned int i = 0; i < rawTx.vin.size(); i++) {
                    CTxIn& txin = rawTx.vin[i];
                    const CCoins* coins = view.AccessCoins(txin.prevout.hash);
                    if (coins == NULL || !coins->IsAvailable(txin.prevout.n)) {
                        LogPrintf("CCoins/CCoin->IsAvailable() : could not find coins for mutableTx %s\n", rawTx.GetHash().ToString());
                        continue;
                    }

                    // Grab a CScript
                    const CScript& prevPubKey = coins->vout[txin.prevout.n].scriptPubKey;
                    const CAmount& cost = coins->vout[txin.prevout.n].nValue;
                    txin.scriptSig.clear();

                    // Sign the corresponding output:
                    if (!fHashSingle || (i < rawTx.vout.size())) {
                        SignSignature(keystore, prevPubKey, rawTx, i, cost, nHashType);
                    }

                    // Make sure we verify the tx
                    if (!VerifyScript(txin.scriptSig, prevPubKey, NULL, STANDARD_SCRIPT_VERIFY_FLAGS, MutableTransactionSignatureChecker(&rawTx, i, cost))){
                        LogPrintf("VerifyScript() : could not verify the signature for mutableTx %s\n", rawTx.GetHash().ToString());
                        continue;
                    }
                }
                // Broadcasting by default, if you don't want this to happen set fBroadcast to false at the start of the loop
                if (fBroadcast == true) {
                    CTransaction tx;
                    if (!DecodeHexTx(tx, EncodeHexTx(rawTx, PROTOCOL_VERSION))) {
                        LogPrintf("DecodeHexTx() : Something is wrong with decoding the hex of our mutableTx\n", rawTx.GetHash().ToString());
                    }
                    uint256 hashTx = tx.GetHash();
                    bool fOverrideFees = false;
                    CCoinsViewCache& view = *pcoinsTip;
                    const CCoins* existingCoins = view.AccessCoins(hashTx);
                    bool fHaveMempool = mempool.exists(hashTx);
                    bool fHaveChain = existingCoins && existingCoins->nHeight < 1000000000;
                    if (!fHaveMempool && !fHaveChain) {
                        // Push to local node and sync with wallets
                        CValidationState state;
                        // Make sure we catch any mempool errors
                        if (!AcceptToMemoryPool(mempool, state, tx, false, NULL, !fOverrideFees)) {
                            if (state.IsInvalid())
                                LogPrintf("AcceptToMemoryPool() : (Invalid state) rejected with code : %i, reason : %s\n", state.GetRejectCode(), state.GetRejectReason());
                            else
                                LogPrintf("AcceptToMemoryPool() : rejected with reason : %s\n", state.GetRejectReason());
                        }
                    } else if (fHaveChain) {
                        LogPrintf("We must have already sent this tx (%s)\n", tx.GetHash().ToString());
                    }
                    LogPrintf("Ok, built a new tx (%s) for %s, i'll relay it and we can try our luck later with it\n", tx.GetHash().ToString(), in.ToString());
                    RelayTransaction(tx);
                }
            }
            continue;
        }

        fSuccess = true; // if we make it this far then we have successfully created a stake hash
        //LogPrintf("%s : hashproof=%s\n", __func__, hashProofOfStake.GetHex());
        nTimeTx = nTryTime;
        if (true) {
            LogPrintf("CheckStakeKernelHash() : PASS protocol=%s modifier=%s nTimeBlockFrom=%u nTimeTxPrev=%u will hit at nTimeTx=%s hashProof=%s\n",
                      "0.3",
                      boost::lexical_cast<std::string>(nStakeModifier).c_str(),
                      nTimeBlockFrom, nTimeBlockFrom, DateTimeStrFormat("%Y-%m-%d %H:%M:%S", nTryTime).c_str(),
                      hashProofOfStake.ToString().c_str());
        }
        continue; // This loop continues forever. It'll also take care of diff adjustments and takes around 30 blocks to be sure about an UTXO's grinding ability
    }

    // We don't need to keep track of anything, and also will return false to retain compatibility with CreateCoinstake()
    return false;
}

// Check kernel hash target and coinstake signature
bool initStakeInput(const CBlock block, std::unique_ptr<CStakeInput>& stake, int nPreviousBlockHeight) {
    const CTransaction tx = block.vtx[1];
    if (!tx.IsCoinStake())
        return error("initStakeInput() : called on non-coinstake %s", tx.GetHash().ToString().c_str());

    // Kernel (input 0) must match the stake hash target per coin age (nBits)
    const CTxIn& txin = tx.vin[0];

    //Construct the stakeinput object
    // First try finding the previous transaction in database
    uint256 hashBlock;
    CTransaction txPrev;
    if (!GetTransaction(txin.prevout.hash, txPrev, hashBlock, true))
        return error("initStakeInput() : INFO: read txPrev failed");

    //verify signature and script
    if (!VerifyScript(txin.scriptSig, txPrev.vout[txin.prevout.n].scriptPubKey, NULL, STANDARD_SCRIPT_VERIFY_FLAGS, TransactionSignatureChecker(&tx, 0, txPrev.vout[txin.prevout.n].nValue)))
        return error("initStakeInput() : VerifySignature failed on coinstake %s", tx.GetHash().ToString().c_str());

    CPhoreStake* phrInput = new CPhoreStake();
    phrInput->SetInput(txPrev, txin.prevout.n);
    stake = std::unique_ptr<CStakeInput>(phrInput);

    return true;
}

// Check kernel hash target and coinstake signature
bool CheckProofOfStake(const CBlock block, uint256& hashProofOfStake, std::unique_ptr<CStakeInput>& stake, int nPreviousBlockHeight)
{
    // Initialize the stake object
    if(!initStakeInput(block, stake, nPreviousBlockHeight))
        return error("%s : stake input object initialization failed", __func__);

    const CTransaction tx = block.vtx[1];
    CBlockIndex* pindexPrev = mapBlockIndex[block.hashPrevBlock];
    CBlockIndex* pindexfrom = stake->GetIndexFrom();
    if (!pindexfrom)
        return error("%s: Failed to find the block index for stake origin", __func__);

    unsigned int nBlockFromTime = pindexfrom->nTime;
    unsigned int nTxTime = block.nTime;
    const int nBlockFromHeight = pindexfrom->nHeight;

    // Check for maturity (min age/depth) requirements
    if (!Params().HasStakeMinAgeOrDepth(nPreviousBlockHeight+1, nTxTime, nBlockFromHeight, nBlockFromTime))
        return error("%s : min age violation - height=%d - nTimeTx=%d, nTimeBlockFrom=%d, nHeightBlockFrom=%d",
                     __func__, nPreviousBlockHeight, nTxTime, nBlockFromTime, nBlockFromHeight);

    if (!CheckStakeKernelHash(pindexPrev, block.nBits, stake.get(), nTxTime, hashProofOfStake, true) && (nTxTime > 1505247602))
        return error("%s : INFO: check kernel failed on coinstake %s, hashProof=%s", __func__,
                     tx.GetHash().GetHex(), hashProofOfStake.GetHex());

    return true;
}

// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(int64_t nTimeBlock, int64_t nTimeTx)
{
    // v0.3 protocol
    return (nTimeBlock == nTimeTx);
}

// Get stake modifier checksum
unsigned int GetStakeModifierChecksum(const CBlockIndex* pindex)
{
    assert(pindex->pprev || pindex->GetBlockHash() == Params().HashGenesisBlock());
    // Hash previous checksum with flags, hashProofOfStake and nStakeModifier
    CDataStream ss(SER_GETHASH, 0);
    if (pindex->pprev)
        ss << pindex->pprev->nStakeModifierChecksum;
    ss << pindex->nFlags << pindex->hashProofOfStake << pindex->nStakeModifier;
    uint256 hashChecksum = Hash(ss.begin(), ss.end());
    hashChecksum >>= (256 - 32);
    return hashChecksum.Get64();
}

// Check stake modifier hard checkpoints
bool CheckStakeModifierCheckpoints(int nHeight, unsigned int nStakeModifierChecksum)
{
    if (Params().NetworkID() != CBaseChainParams::MAIN) return true; // Testnet has no checkpoints
    if (mapStakeModifierCheckpoints.count(nHeight)) {
        return nStakeModifierChecksum == mapStakeModifierCheckpoints[nHeight];
    }
    return true;
}
