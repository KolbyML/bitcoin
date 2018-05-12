// Copyright (c) 2018 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zphrchain.h"
#include "invalid.h"
#include "main.h"
#include "txdb.h"
#include "ui_interface.h"

// 6 comes from OPCODE (1) + vch.size() (1) + BIGNUM size (4)
#define SCRIPT_OFFSET 6
// For Script size (BIGNUM/Uint256 size)
#define BIGNUM_SIZE   4

int GetZerocoinStartHeight()
{
    return Params().Zerocoin_StartHeight();
}

libzerocoin::ZerocoinParams* GetZerocoinParams(int nHeight) {
    return nHeight > Params().Zerocoin_LastOldParams() ? Params().Zerocoin_Params() : Params().OldZerocoin_Params();
}

void FindMints(vector<CMintMeta> vMintsToFind, vector<CMintMeta>& vMintsToUpdate, vector<CMintMeta>& vMissingMints)
{
    // see which mints are in our public zerocoin database. The mint should be here if it exists, unless
    // something went wrong
    for (CMintMeta meta : vMintsToFind) {
        uint256 txHash;
        if (!zerocoinDB->ReadCoinMint(meta.hashPubcoin, txHash)) {
            vMissingMints.push_back(meta);
            continue;
        }

        // make sure the txhash and block height meta data are correct for this mint
        CTransaction tx;
        uint256 hashBlock;
        if (!GetTransaction(txHash, tx, hashBlock, true)) {
            LogPrintf("%s : cannot find tx %s\n", __func__, txHash.GetHex());
            vMissingMints.push_back(meta);
            continue;
        }

        if (!mapBlockIndex.count(hashBlock)) {
            LogPrintf("%s : cannot find block %s\n", __func__, hashBlock.GetHex());
            vMissingMints.push_back(meta);
            continue;
        }

        //see if this mint is spent
        uint256 hashTxSpend = 0;
        bool fSpent = zerocoinDB->ReadCoinSpend(meta.hashSerial, hashTxSpend);

        //if marked as spent, check that it actually made it into the chain
        CTransaction txSpend;
        uint256 hashBlockSpend;
        if (fSpent && !GetTransaction(hashTxSpend, txSpend, hashBlockSpend, true)) {
            LogPrintf("%s : cannot find spend tx %s\n", __func__, hashTxSpend.GetHex());
            meta.isUsed = false;
            vMintsToUpdate.push_back(meta);
            continue;
        }

        //The mint has been incorrectly labelled as spent in zerocoinDB and needs to be undone
        int nHeightTx = 0;
        uint256 hashSerial = meta.hashSerial;
        uint256 txidSpend;
        if (fSpent && !IsSerialInBlockchain(hashSerial, nHeightTx, txidSpend)) {
            LogPrintf("%s : cannot find block %s. Erasing coinspend from zerocoinDB.\n", __func__, hashBlockSpend.GetHex());
            meta.isUsed = false;
            vMintsToUpdate.push_back(meta);
            continue;
        }

        // is the denomination correct?
        for (auto& out : tx.vout) {
            if (!out.IsZerocoinMint())
                continue;
            PublicCoin pubcoin(meta.nVersion < libzerocoin::PrivateCoin::PUBKEY_VERSION ? Params().OldZerocoin_Params() : Params().Zerocoin_Params());
            CValidationState state;
            TxOutToPublicCoin(out, pubcoin, state);
            if (GetPubCoinHash(pubcoin.getValue()) == meta.hashPubcoin && pubcoin.getDenomination() != meta.denom) {
                LogPrintf("%s: found mismatched denom pubcoinhash = %s\n", __func__, meta.hashPubcoin.GetHex());
                meta.denom = pubcoin.getDenomination();
                vMintsToUpdate.emplace_back(meta);
            }
        }

        // if meta data is correct, then no need to update
        if (meta.txid == txHash && meta.nHeight == mapBlockIndex[hashBlock]->nHeight && meta.isUsed == fSpent)
            continue;

        //mark this mint for update
        meta.txid = txHash;
        meta.nHeight = mapBlockIndex[hashBlock]->nHeight;
        meta.isUsed = fSpent;
        LogPrintf("%s: found updates for pubcoinhash = %s\n", __func__, meta.hashPubcoin.GetHex());

        vMintsToUpdate.push_back(meta);
    }
}

bool GetZerocoinMint(const CBigNum& bnPubcoin, uint256& txHash)
{
    txHash = 0;
    return zerocoinDB->ReadCoinMint(bnPubcoin, txHash);
}

bool IsPubcoinInBlockchain(const uint256& hashPubcoin, uint256& txid)
{
    txid = 0;
    return zerocoinDB->ReadCoinMint(hashPubcoin, txid);
}

bool IsSerialKnown(const CBigNum& bnSerial)
{
    uint256 txHash = 0;
    return zerocoinDB->ReadCoinSpend(bnSerial, txHash);
}

bool IsSerialInBlockchain(const CBigNum& bnSerial, int& nHeightTx) {
    uint256 txHash = 0;
    // if not in zerocoinDB then its not in the blockchain
    if (!zerocoinDB->ReadCoinSpend(bnSerial, txHash))
        return false;

    return IsTransactionInChain(txHash, nHeightTx);
}

bool IsSerialInBlockchain(const uint256& hashSerial, int& nHeightTx, uint256& txidSpend) {
    CTransaction tx;
    return IsSerialInBlockchain(hashSerial, nHeightTx, txidSpend, tx);
}

bool IsSerialInBlockchain(const uint256& hashSerial, int& nHeightTx, uint256& txidSpend, CTransaction& tx) {
    txidSpend = 0;
    // if not in zerocoinDB then its not in the blockchain
    if (!zerocoinDB->ReadCoinSpend(hashSerial, txidSpend))
        return false;
    return IsTransactionInChain(txidSpend, nHeightTx, tx);
}

bool RemoveSerialFromDB(const CBigNum& bnSerial)
{
    return zerocoinDB->EraseCoinSpend(bnSerial);
}

/** zerocoin transaction checks */
bool RecordMintToDB(PublicCoin publicZerocoin, const uint256& txHash)
{
    //Check the pubCoinValue didn't already store in the zerocoin database. todo: pubcoin memory map?
    //write the zerocoinmint to db if we don't already have it
    //note that many of the mint parameters are not set here because those params are private to the minter
    CZerocoinMint pubCoinTx;
    uint256 hashFromDB;
    if (zerocoinDB->ReadCoinMint(publicZerocoin.getValue(), hashFromDB)) {
        if(hashFromDB == txHash)
            return true;

        LogPrintf("RecordMintToDB: failed, we already have this public coin recorded\n");
        return false;
    }

    if (!zerocoinDB->WriteCoinMint(publicZerocoin, txHash)) {
        LogPrintf("RecordMintToDB: failed to record public coin to DB\n");
        return false;
    }

    return true;
}

bool TxOutToPublicCoin(const CTxOut txout, PublicCoin& pubCoin, CValidationState& state)
{
    CBigNum publicZerocoin;
    vector<unsigned char> vchZeroMint;
    vchZeroMint.insert(vchZeroMint.end(), txout.scriptPubKey.begin() + SCRIPT_OFFSET,
                           txout.scriptPubKey.begin() + txout.scriptPubKey.size());
    publicZerocoin.setvch(vchZeroMint);

    CoinDenomination denomination = AmountToZerocoinDenomination(txout.nValue);
    LogPrint("zero", "%s ZCPRINT denomination %d pubcoin %s\n", __func__, denomination, publicZerocoin.GetHex());
    if (denomination == ZQ_ERROR)
        return state.DoS(100, error("TxOutToPublicCoin : txout.nValue is not correct"));

    PublicCoin checkPubCoin(Params().Zerocoin_Params(), publicZerocoin, denomination);
    pubCoin = checkPubCoin;

    return true;
}

bool BlockToPubcoinList(const CBlock& block, list<PublicCoin>& listPubcoins)
{
    for (const CTransaction& tx : block.vtx) {
        if(!tx.IsZerocoinMint())
            continue;

        for (unsigned int i = 0; i < tx.vout.size(); i++) {

            const CTxOut txOut = tx.vout[i];
            if(!txOut.scriptPubKey.IsZerocoinMint())
                continue;

            CValidationState state;
            PublicCoin pubCoin(Params().Zerocoin_Params());
            if(!TxOutToPublicCoin(txOut, pubCoin, state))
                return false;

            listPubcoins.emplace_back(pubCoin);
        }
    }

    return true;
}

//return a list of zerocoin mints contained in a specific block
bool BlockToZerocoinMintList(const CBlock& block, std::list<CZerocoinMint>& vMints)
{
    for (const CTransaction& tx : block.vtx) {
        if(!tx.IsZerocoinMint())
            continue;

        for (unsigned int i = 0; i < tx.vout.size(); i++) {
            const CTxOut txOut = tx.vout[i];
            if(!txOut.scriptPubKey.IsZerocoinMint())
                continue;

            CValidationState state;
            PublicCoin pubCoin(Params().Zerocoin_Params());
            if(!TxOutToPublicCoin(txOut, pubCoin, state))
                return false;

            CZerocoinMint mint = CZerocoinMint(pubCoin.getDenomination(), pubCoin.getValue(), 0, 0, false, 1, nullptr);
            mint.SetTxHash(tx.GetHash());
            vMints.push_back(mint);
        }
    }

    return true;
}

bool BlockToMintValueVector(const CBlock& block, const CoinDenomination denom, vector<CBigNum>& vValues)
{
    for (const CTransaction& tx : block.vtx) {
        if(!tx.IsZerocoinMint())
            continue;

        for (const CTxOut& txOut : tx.vout) {
            if(!txOut.scriptPubKey.IsZerocoinMint())
                continue;

            CValidationState state;
            PublicCoin coin(Params().Zerocoin_Params());
            if(!TxOutToPublicCoin(txOut, coin, state))
                return false;

            if (coin.getDenomination() != denom)
                continue;

            vValues.push_back(coin.getValue());
        }
    }

    return true;
}

CoinSpend TxInToZerocoinSpend(const CTxIn& txin)
{
    // extract the spend from the txin
    std::vector<char, zero_after_free_allocator<char> > dataTxIn;
    dataTxIn.insert(dataTxIn.end(), txin.scriptSig.begin() + BIGNUM_SIZE, txin.scriptSig.end());

    CDataStream serializedCoinSpend(dataTxIn, SER_NETWORK, PROTOCOL_VERSION);
    return CoinSpend(Params().Zerocoin_Params(), GetZerocoinParams(chainActive.Height()), serializedCoinSpend);
}

//return a list of zerocoin spends contained in a specific block, list may have many denominations
std::list<libzerocoin::CoinDenomination> ZerocoinSpendListFromBlock(const CBlock& block)
{
    std::list<libzerocoin::CoinDenomination> vSpends;
    for (const CTransaction& tx : block.vtx) {
        if (!tx.IsZerocoinSpend())
            continue;

        for (const CTxIn txin : tx.vin) {
            if (!txin.scriptSig.IsZerocoinSpend())
                continue;

            libzerocoin::CoinDenomination c = libzerocoin::IntToZerocoinDenomination(txin.nSequence);
            vSpends.push_back(c);
        }
    }
    return vSpends;
}

std::string ReindexZerocoinDB()
{
    if (!zerocoinDB->WipeCoins("spends") || !zerocoinDB->WipeCoins("mints")) {
        return _("Failed to wipe zerocoinDB");
    }

    uiInterface.ShowProgress(_("Reindexing zerocoin database..."), 0);

    CBlockIndex *pindex = chainActive[Params().Zerocoin_StartHeight()];
    while (pindex) {
        uiInterface.ShowProgress(_("Reindexing zerocoin database..."), std::max(1, std::min(99, (int)((double)(pindex->nHeight - Params().Zerocoin_StartHeight()) / (double)(chainActive.Height() - Params().Zerocoin_StartHeight()) * 100))));

        if (pindex->nHeight % 1000 == 0)
            LogPrintf("Reindexing zerocoin : block %d...\n", pindex->nHeight);

        CBlock block;
        if (!ReadBlockFromDisk(block, pindex)) {
            strLoadError = _("Reindexing zerocoin failed");
            break;
        }

        for (const CTransaction &tx : block.vtx) {
            for (unsigned int i = 0; i < tx.vin.size(); i++) {
                if (tx.IsCoinBase())
                    break;

                if (tx.ContainsZerocoins()) {
                    uint256 txid = tx.GetHash();
                    //Record Serials
                    if (tx.IsZerocoinSpend()) {
                        for (auto &in : tx.vin) {
                            if (!in.scriptSig.IsZerocoinSpend())
                                continue;

                            libzerocoin::CoinSpend spend = TxInToZerocoinSpend(in);
                            zerocoinDB->WriteCoinSpend(spend.getCoinSerialNumber(), txid);
                        }
                    }

                    //Record mints
                    if (tx.IsZerocoinMint()) {
                        for (auto &out : tx.vout) {
                            if (!out.IsZerocoinMint())
                                continue;

                            CValidationState state;
                            libzerocoin::PublicCoin coin(GetZerocoinParams(pindex->nHeight));
                            TxOutToPublicCoin(out, coin, state);
                            zerocoinDB->WriteCoinMint(coin, txid);
                        }
                    }
                }
            }
        }
        pindex = chainActive.Next(pindex);
    }
    uiInterface.ShowProgress("", 100);
    return "";
}