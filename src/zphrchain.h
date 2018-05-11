// Copyright (c) 2018 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PHORE_ZPHRCHAIN_H
#define PHORE_ZPHRCHAIN_H

#include "libzerocoin/Coin.h"
#include "libzerocoin/Denominations.h"
#include "libzerocoin/CoinSpend.h"
#include <list>
#include <string>

class CBlock;
class CBigNum;
struct CMintMeta;
class CTransaction;
class CTxIn;
class CTxOut;
class CValidationState;
class CZerocoinMint;
class uint256;

libzerocoin::CoinSpend TxInToZerocoinSpend(const CTxIn& txin);
bool TxOutToPublicCoin(const CTxOut txout, libzerocoin::PublicCoin& pubCoin, CValidationState& state);
bool BlockToPubcoinList(const CBlock& block, list<libzerocoin::PublicCoin>& listPubcoins);
bool BlockToZerocoinMintList(const CBlock& block, std::list<CZerocoinMint>& vMints);
bool BlockToMintValueVector(const CBlock& block, const libzerocoin::CoinDenomination denom, std::vector<CBigNum>& vValues);
std::list<libzerocoin::CoinDenomination> ZerocoinSpendListFromBlock(const CBlock& block);
void FindMints(vector<CMintMeta> vMintsToFind, vector<CMintMeta>& vMintsToUpdate, vector<CMintMeta>& vMissingMints);
bool GetZerocoinMint(const CBigNum& bnPubcoin, uint256& txHash);
bool IsSerialKnown(const CBigNum& bnSerial);
bool IsSerialInBlockchain(const CBigNum& bnSerial, int& nHeightTx);
bool IsSerialInBlockchain(const uint256& hashSerial, int& nHeightTx, uint256& txidSpend);
bool IsSerialInBlockchain(const uint256& hashSerial, int& nHeightTx, uint256& txidSpend, CTransaction& tx);
bool IsPubcoinInBlockchain(const uint256& hashPubcoin, uint256& txid);
bool RemoveSerialFromDB(const CBigNum& bnSerial);
int GetZerocoinStartHeight();
libzerocoin::ZerocoinParams* GetZerocoinParams(int nHeight);
std::string ReindexZerocoinDB();

#endif //PHORE_ZPHRCHAIN_H
