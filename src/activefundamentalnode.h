// Copyright (c) 2014-2016 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef ACTIVEFUNDAMENTALNODE_H
#define ACTIVEFUNDAMENTALNODE_H

#include "init.h"
#include "key.h"
#include "fundamentalnode.h"
#include "net.h"
#include "sync.h"
#include "wallet.h"

#define ACTIVE_FUNDAMENTALNODE_INITIAL 0 // initial state
#define ACTIVE_FUNDAMENTALNODE_SYNC_IN_PROCESS 1
#define ACTIVE_FUNDAMENTALNODE_NOT_CAPABLE 3
#define ACTIVE_FUNDAMENTALNODE_STARTED 4

// Responsible for activating the Fundamentalnode and pinging the network
class CActiveFundamentalnode
{
private:
    /// Ping Fundamentalnode
    bool SendFundamentalnodePing(std::string& errorMessage);

    int status;
    std::string notCapableReason;

public:

    CActiveFundamentalnode()
    {
        vin = boost::none;
        status = ACTIVE_FUNDAMENTALNODE_INITIAL;
    }

    // Initialized by init.cpp
    // Keys for the main Fundamentalnode
    CPubKey pubKeyFundamentalnode;

    // Initialized while registering Fundamentalnode
    boost::optional<CTxIn> vin;
    CService service;

    /// Manage status of main Fundamentalnode
    void ManageStatus();
    void ResetStatus();
    std::string GetStatusMessage() const;
    int GetStatus() const { return status; }

    /// Enable cold wallet mode (run a Fundamentalnode with no funds)
    bool EnableHotColdFundamentalNode(CTxIn& vin, CService& addr);
};

#endif
