// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef FUNDAMENTALNODE_H
#define FUNDAMENTALNODE_H

#include "base58.h"
#include "key.h"
#include "main.h"
#include "messagesigner.h"
#include "net.h"
#include "sync.h"
#include "timedata.h"
#include "util.h"

#define FUNDAMENTALNODE_MIN_CONFIRMATIONS 15
#define FUNDAMENTALNODE_MIN_MNP_SECONDS (10 * 60)
#define FUNDAMENTALNODE_MIN_MNB_SECONDS (5 * 60)
#define FUNDAMENTALNODE_PING_SECONDS (5 * 60)
#define FUNDAMENTALNODE_EXPIRATION_SECONDS (120 * 60)
#define FUNDAMENTALNODE_REMOVAL_SECONDS (130 * 60)
#define FUNDAMENTALNODE_CHECK_SECONDS 5

static const CAmount FUNDAMENTALNODE_AMOUNT = 10000* COIN;
static const CAmount FN_MAGIC_AMOUNT = 0.1234 *COIN;

class CFundamentalnode;
class CFundamentalnodeBroadcast;
class CFundamentalnodePing;
extern std::map<int64_t, uint256> mapCacheBlockHashes;

bool GetFundamentalnodeBlockHash(uint256& hash, int nBlockHeight);


//
// The Fundamentalnode Ping Class : Contains a different serialize method for sending pings from fundamentalnodes throughout the network
//

class CFundamentalnodePing : public CSignedMessage
{
public:
    CTxIn vin;
    uint256 blockHash;
    int64_t sigTime; //mnb message times

    CFundamentalnodePing();
    CFundamentalnodePing(CTxIn& newVin);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(vin);
        READWRITE(blockHash);
        READWRITE(sigTime);
        READWRITE(vchSig);
        try
        {
            READWRITE(nMessVersion);
        } catch (...) {
            nMessVersion = MessageVersion::MESS_VER_STRMESS;
        }
    }

    uint256 GetHash() const;

    // override CSignedMessage functions
    uint256 GetSignatureHash() const override { return GetHash(); }
    std::string GetStrMessage() const override;
    const CTxIn GetVin() const override  { return vin; };

    bool CheckAndUpdate(int& nDos, bool fRequireEnabled = true, bool fCheckSigTimeOnly = false);
    void Relay();

    void swap(CFundamentalnodePing& first, CFundamentalnodePing& second) // nothrow
    {
        CSignedMessage::swap(first, second);

        // enable ADL (not necessary in our case, but good practice)
        using std::swap;

        // by swapping the members of two classes,
        // the two classes are effectively swapped
        swap(first.vin, second.vin);
        swap(first.blockHash, second.blockHash);
        swap(first.sigTime, second.sigTime);
    }

    CFundamentalnodePing& operator=(CFundamentalnodePing from)
    {
        swap(*this, from);
        return *this;
    }
    friend bool operator==(const CFundamentalnodePing& a, const CFundamentalnodePing& b)
    {
        return a.vin == b.vin && a.blockHash == b.blockHash;
    }
    friend bool operator!=(const CFundamentalnodePing& a, const CFundamentalnodePing& b)
    {
        return !(a == b);
    }
};

//
// The Fundamentalnode Class. It contains the input of the 10000 PIV, signature to prove
// it's the one who own that ip address and code for calculating the payment election.
//
class CFundamentalnode : public CSignedMessage
{
private:
    // critical section to protect the inner data structures
    mutable CCriticalSection cs;
    int64_t lastTimeChecked;

public:
    enum state {
        FUNDAMENTALNODE_PRE_ENABLED,
        FUNDAMENTALNODE_ENABLED,
        FUNDAMENTALNODE_EXPIRED,
        FUNDAMENTALNODE_REMOVE,
        FUNDAMENTALNODE_WATCHDOG_EXPIRED,
        FUNDAMENTALNODE_POSE_BAN,
        FUNDAMENTALNODE_VIN_SPENT,
        FUNDAMENTALNODE_POS_ERROR,
        FUNDAMENTALNODE_MISSING
    };

    CTxIn vin;
    CService addr;
    CPubKey pubKeyCollateralAddress;
    CPubKey pubKeyFundamentalnode;
    CPubKey pubKeyCollateralAddress1;
    CPubKey pubKeyFundamentalnode1;
    int activeState;
    int64_t sigTime; //mnb message time
    int cacheInputAge;
    int cacheInputAgeBlock;
    bool unitTest;
    bool allowFreeTx;
    int protocolVersion;
    int nActiveState;
    int64_t nLastDsq; //the dsq count from the last dsq broadcast of this node
    int nScanningErrorCount;
    int nLastScanningErrorBlockHeight;
    CFundamentalnodePing lastPing;

    CFundamentalnode();
    CFundamentalnode(const CFundamentalnode& other);

    // override CSignedMessage functions
    uint256 GetSignatureHash() const override;
    std::string GetStrMessage() const override;
    const CTxIn GetVin() const override { return vin; };
    const CPubKey GetPublicKey(std::string& strErrorRet) const override { return pubKeyCollateralAddress; }

    void swap(CFundamentalnode& first, CFundamentalnode& second) // nothrow
    {
        CSignedMessage::swap(first, second);

        // enable ADL (not necessary in our case, but good practice)
        using std::swap;

        // by swapping the members of two classes,
        // the two classes are effectively swapped
        swap(first.vin, second.vin);
        swap(first.addr, second.addr);
        swap(first.pubKeyCollateralAddress, second.pubKeyCollateralAddress);
        swap(first.pubKeyFundamentalnode, second.pubKeyFundamentalnode);
        swap(first.activeState, second.activeState);
        swap(first.sigTime, second.sigTime);
        swap(first.lastPing, second.lastPing);
        swap(first.cacheInputAge, second.cacheInputAge);
        swap(first.cacheInputAgeBlock, second.cacheInputAgeBlock);
        swap(first.unitTest, second.unitTest);
        swap(first.allowFreeTx, second.allowFreeTx);
        swap(first.protocolVersion, second.protocolVersion);
        swap(first.nLastDsq, second.nLastDsq);
        swap(first.nScanningErrorCount, second.nScanningErrorCount);
        swap(first.nLastScanningErrorBlockHeight, second.nLastScanningErrorBlockHeight);
    }

    CFundamentalnode& operator=(CFundamentalnode from)
    {
        swap(*this, from);
        return *this;
    }
    friend bool operator==(const CFundamentalnode& a, const CFundamentalnode& b)
    {
        return a.vin == b.vin;
    }
    friend bool operator!=(const CFundamentalnode& a, const CFundamentalnode& b)
    {
        return !(a.vin == b.vin);
    }

    uint256 CalculateScore(int mod = 1, int64_t nBlockHeight = 0);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        LOCK(cs);

        READWRITE(vin);
        READWRITE(addr);
        READWRITE(pubKeyCollateralAddress);
        READWRITE(pubKeyFundamentalnode);
        READWRITE(vchSig);
        READWRITE(sigTime);
        READWRITE(protocolVersion);
        READWRITE(activeState);
        READWRITE(lastPing);
        READWRITE(cacheInputAge);
        READWRITE(cacheInputAgeBlock);
        READWRITE(unitTest);
        READWRITE(allowFreeTx);
        READWRITE(nLastDsq);
        READWRITE(nScanningErrorCount);
        READWRITE(nLastScanningErrorBlockHeight);
    }

    int64_t SecondsSincePayment();

    bool UpdateFromNewBroadcast(CFundamentalnodeBroadcast& mnb);

    void Check(bool forceCheck = false);

    bool IsBroadcastedWithin(int seconds)
    {
        return (GetAdjustedTime() - sigTime) < seconds;
    }

    bool IsPingedWithin(int seconds, int64_t now = -1)
    {
        now == -1 ? now = GetAdjustedTime() : now;

        return (lastPing == CFundamentalnodePing()) ? false : now - lastPing.sigTime < seconds;
    }

    void Disable()
    {
        sigTime = 0;
        lastPing = CFundamentalnodePing();
    }

    bool IsEnabled()
    {
        return activeState == FUNDAMENTALNODE_ENABLED;
    }

    int GetFundamentalnodeInputAge()
    {
        if (chainActive.Tip() == NULL) return 0;

        if (cacheInputAge == 0) {
            cacheInputAge = GetInputAge(vin);
            cacheInputAgeBlock = chainActive.Tip()->nHeight;
        }

        return cacheInputAge + (chainActive.Tip()->nHeight - cacheInputAgeBlock);
    }
    
    std::string GetStatus();

    std::string Status()
    {
        std::string strStatus = "ACTIVE";

        if (activeState == CFundamentalnode::FUNDAMENTALNODE_ENABLED) strStatus = "ENABLED";
        if (activeState == CFundamentalnode::FUNDAMENTALNODE_EXPIRED) strStatus = "EXPIRED";
        if (activeState == CFundamentalnode::FUNDAMENTALNODE_VIN_SPENT) strStatus = "VIN_SPENT";
        if (activeState == CFundamentalnode::FUNDAMENTALNODE_REMOVE) strStatus = "REMOVE";
        if (activeState == CFundamentalnode::FUNDAMENTALNODE_POS_ERROR) strStatus = "POS_ERROR";
        if (activeState == CFundamentalnode::FUNDAMENTALNODE_MISSING) strStatus = "MISSING";

        return strStatus;
    }

    int64_t GetLastPaid();
    bool IsValidNetAddr();

    /// Is the input associated with collateral public key? (and there is 10000 PIV - checking if valid masternode)
    bool IsInputAssociatedWithPubkey() const;
};


//
// The Fundamentalnode Broadcast Class : Contains a different serialize method for sending fundamentalnodes through the network
//

class CFundamentalnodeBroadcast : public CFundamentalnode
{
public:
    CFundamentalnodeBroadcast();
    CFundamentalnodeBroadcast(CService newAddr, CTxIn newVin, CPubKey newPubkey, CPubKey newPubkey2, int protocolVersionIn);
    CFundamentalnodeBroadcast(const CFundamentalnode& mn);

    bool CheckAndUpdate(int& nDoS);
    bool CheckInputsAndAdd(int& nDos);

    uint256 GetHash() const;

    void Relay();

    // special sign/verify
    bool Sign(const CKey& key, const CPubKey& pubKey);
    bool Sign(const std::string strSignKey);
    bool CheckSignature() const;

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(vin);
        READWRITE(addr);
        READWRITE(pubKeyCollateralAddress);
        READWRITE(pubKeyFundamentalnode);
        READWRITE(vchSig);
        READWRITE(sigTime);
        READWRITE(protocolVersion);
        READWRITE(lastPing);
        READWRITE(nMessVersion);    // abuse nLastDsq (which will be removed) for old serialization
        if (ser_action.ForRead())
            nLastDsq = 0;
    }

    /// Create Fundamentalnode broadcast, needs to be relayed manually after that
    static bool Create(CTxIn vin, CService service, CKey keyCollateralAddressNew, CPubKey pubKeyCollateralAddressNew, CKey keyFundamentalnodeNew, CPubKey pubKeyFundamentalnodeNew, std::string& strErrorRet, CFundamentalnodeBroadcast& mnbRet);
    static bool Create(std::string strService, std::string strKey, std::string strTxHash, std::string strOutputIndex, std::string& strErrorRet, CFundamentalnodeBroadcast& mnbRet, bool fOffline = false);
    static bool CheckDefaultPort(CService service, std::string& strErrorRet, const std::string& strContext);
};

#endif
