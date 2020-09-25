// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef FUNDAMENTALNODE_PAYMENTS_H
#define FUNDAMENTALNODE_PAYMENTS_H

#include "key.h"
#include "main.h"
#include "fundamentalnode.h"


extern CCriticalSection cs_vecFundamentalnodePayments;
extern CCriticalSection cs_mapFundamentalnodeBlocks;
extern CCriticalSection cs_mapFundamentalnodePayeeVotes;

class CFundamentalnodePayments;
class CFundamentalnodePaymentWinner;
class CFundamentalnodeBlockPayees;

extern CFundamentalnodePayments fundamentalnodePayments;

#define MNPAYMENTS_SIGNATURES_REQUIRED 6
#define MNPAYMENTS_SIGNATURES_TOTAL 10

void ProcessMessageFundamentalnodePayments(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);
bool IsBlockPayeeValid(const CBlock& block, int nBlockHeight);
std::string GetFundamentalnodeRequiredPaymentsString(int nBlockHeight);
bool IsBlockValueValid(const CBlock& block, CAmount nExpectedValue, CAmount nMinted);
void FillBlockPayeeFundamentalnode(CMutableTransaction& txNew, CAmount nFees, bool fProofOfStake);

void DumpFundamentalnodePayments();

/** Save Fundamentalnode Payment Data (fnpayments.dat)
 */
class CFundamentalnodePaymentDB
{
private:
    boost::filesystem::path pathDB;
    std::string strMagicMessage;

public:
    enum ReadResult {
        Ok,
        FileError,
        HashReadError,
        IncorrectHash,
        IncorrectMagicMessage,
        IncorrectMagicNumber,
        IncorrectFormat
    };

    CFundamentalnodePaymentDB();
    bool Write(const CFundamentalnodePayments& objToSave);
    ReadResult Read(CFundamentalnodePayments& objToLoad, bool fDryRun = false);
};

class CFundamentalnodePayee
{
public:
    CScript scriptPubKey;
    int nVotes;

    CFundamentalnodePayee()
    {
        scriptPubKey = CScript();
        nVotes = 0;
    }

    CFundamentalnodePayee(CScript payee, int nVotesIn)
    {
        scriptPubKey = payee;
        nVotes = nVotesIn;
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(scriptPubKey);
        READWRITE(nVotes);
    }
};

// Keep track of votes for payees from fundamentalnodes
class CFundamentalnodeBlockPayees
{
public:
    int nBlockHeight;
    std::vector<CFundamentalnodePayee> vecPayments;

    CFundamentalnodeBlockPayees()
    {
        nBlockHeight = 0;
        vecPayments.clear();
    }
    CFundamentalnodeBlockPayees(int nBlockHeightIn)
    {
        nBlockHeight = nBlockHeightIn;
        vecPayments.clear();
    }

    void AddPayee(CScript payeeIn, int nIncrement)
    {
        LOCK(cs_vecFundamentalnodePayments);

        for (CFundamentalnodePayee& payee : vecPayments) {
            if (payee.scriptPubKey == payeeIn) {
                payee.nVotes += nIncrement;
                return;
            }
        }

        CFundamentalnodePayee c(payeeIn, nIncrement);
        vecPayments.push_back(c);
    }

    bool GetPayee(CScript& payee)
    {
        LOCK(cs_vecFundamentalnodePayments);

        int nVotes = -1;
        for (CFundamentalnodePayee& p : vecPayments) {
            if (p.nVotes > nVotes) {
                payee = p.scriptPubKey;
                nVotes = p.nVotes;
            }
        }

        return (nVotes > -1);
    }

    bool HasPayeeWithVotes(CScript payee, int nVotesReq)
    {
        LOCK(cs_vecFundamentalnodePayments);

        for (CFundamentalnodePayee& p : vecPayments) {
            if (p.nVotes >= nVotesReq && p.scriptPubKey == payee) return true;
        }

        return false;
    }

    bool IsTransactionValid(const CTransaction& txNew);
    std::string GetFundamentalnodeRequiredPaymentsString();

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(nBlockHeight);
        READWRITE(vecPayments);
    }
};

// for storing the winning payments
class CFundamentalnodePaymentWinner : public CSignedMessage
{
public:
    CTxIn vinFundamentalnode;

    int nBlockHeight;
    CScript payee;

    CFundamentalnodePaymentWinner() :
            CSignedMessage(),
            vinFundamentalnode(),
            nBlockHeight(0),
            payee()
    {}

    CFundamentalnodePaymentWinner(CTxIn vinIn) :
            CSignedMessage(),
            vinFundamentalnode(vinIn),
            nBlockHeight(0),
            payee()
    {}

    uint256 GetHash() const;

    // override CSignedMessage functions
    uint256 GetSignatureHash() const override { return GetHash(); }
    std::string GetStrMessage() const override;
    const CTxIn GetVin() const override { return vinFundamentalnode; };

    bool IsValid(CNode* pnode, std::string& strError);
    void Relay();

    void AddPayee(CScript payeeIn)
    {
        payee = payeeIn;
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(vinFundamentalnode);
        READWRITE(nBlockHeight);
        READWRITE(payee);
        READWRITE(vchSig);
        try
        {
            READWRITE(nMessVersion);
        } catch (...) {
            nMessVersion = MessageVersion::MESS_VER_STRMESS;
        }
    }

    std::string ToString()
    {
        std::string ret = "";
        ret += vinFundamentalnode.ToString();
        ret += ", " + std::to_string(nBlockHeight);
        ret += ", " + HexStr(payee);
        ret += ", " + std::to_string((int)vchSig.size());
        return ret;
    }
};

//
// fundamentalnode Payments Class
// Keeps track of who should get paid for which blocks
//

class CFundamentalnodePayments
{
private:
    int nSyncedFromPeer;
    int nLastBlockHeight;

public:
    std::map<uint256, CFundamentalnodePaymentWinner> mapFundamentalnodePayeeVotes;
    std::map<int, CFundamentalnodeBlockPayees> mapFundamentalnodeBlocks;
    std::map<COutPoint, int> mapFundamentalnodesLastVote; //prevout, nBlockHeight

    CFundamentalnodePayments()
    {
        nSyncedFromPeer = 0;
        nLastBlockHeight = 0;
    }

    void Clear()
    {
        LOCK2(cs_mapFundamentalnodeBlocks, cs_mapFundamentalnodePayeeVotes);
        mapFundamentalnodeBlocks.clear();
        mapFundamentalnodePayeeVotes.clear();
    }

    bool AddWinningFundamentalnode(CFundamentalnodePaymentWinner& winner);
    bool ProcessBlock(int nBlockHeight);

    void Sync(CNode* node, int nCountNeeded);
    void CleanPaymentList();
    int LastPayment(CFundamentalnode& mn);

    bool GetBlockPayee(int nBlockHeight, CScript& payee);
    bool IsTransactionValid(const CTransaction& txNew, int nBlockHeight);
    bool IsScheduled(CFundamentalnode& mn, int nNotBlockHeight);

    bool CanVote(COutPoint outFundamentalnode, int nBlockHeight)
    {
        LOCK(cs_mapFundamentalnodePayeeVotes);

        if (mapFundamentalnodesLastVote.count(outFundamentalnode)) {
            if (mapFundamentalnodesLastVote[outFundamentalnode] == nBlockHeight) {
                return false;
            }
        }

        //record this fundamentalnode voted
        mapFundamentalnodesLastVote[outFundamentalnode] = nBlockHeight;
        return true;
    }

    int GetMinFundamentalnodePaymentsProto();
    void ProcessMessageFundamentalnodePayments(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);
    std::string GetFundamentalnodeRequiredPaymentsString(int nBlockHeight);
    void FillBlockPayeeFundamentalnode(CMutableTransaction& txNew, int64_t nFees, bool fProofOfStake);
    std::string ToString() const;
    int GetOldestBlock();
    int GetNewestBlock();

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(mapFundamentalnodePayeeVotes);
        READWRITE(mapFundamentalnodeBlocks);
    }
};


#endif
