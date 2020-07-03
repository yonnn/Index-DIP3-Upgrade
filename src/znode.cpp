// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activeznode.h"
#include "consensus/consensus.h"
#include "consensus/validation.h"
#include "init.h"
#include "znode.h"
#include "znode-sync.h"
#include "util.h"
#include "net.h"
#include "netbase.h"

#include <boost/lexical_cast.hpp>

CZnodeTimings::CZnodeTimings()
{
    if(Params().GetConsensus().IsRegtest()) {
        minMnp = Regtest::ZnodeMinMnpSeconds;
        newStartRequired = Regtest::ZnodeNewStartRequiredSeconds;
    } else {
        minMnp = Mainnet::ZnodeMinMnpSeconds;
        newStartRequired = Mainnet::ZnodeNewStartRequiredSeconds;
    }
}

CZnodeTimings & CZnodeTimings::Inst() {
    static CZnodeTimings inst;
    return inst;
}

int CZnodeTimings::MinMnpSeconds() {
    return Inst().minMnp;
}

int CZnodeTimings::NewStartRequiredSeconds() {
    return Inst().newStartRequired;
}


CZnode::CZnode() :
        vin(),
        addr(),
        pubKeyCollateralAddress(),
        pubKeyZnode(),
        lastPing(),
        vchSig(),
        sigTime(GetAdjustedTime()),
        nLastDsq(0),
        nTimeLastChecked(0),
        nTimeLastPaid(0),
        nTimeLastWatchdogVote(0),
        nActiveState(ZNODE_ENABLED),
        nCacheCollateralBlock(0),
        nBlockLastPaid(0),
        nProtocolVersion(LEGACY_ZNODES_PROTOCOL_VERSION),
        nPoSeBanScore(0),
        nPoSeBanHeight(0),
        fAllowMixingTx(true),
        fUnitTest(false) {}

CZnode::CZnode(CService addrNew, CTxIn vinNew, CPubKey pubKeyCollateralAddressNew, CPubKey pubKeyZnodeNew, int nProtocolVersionIn) :
        vin(vinNew),
        addr(addrNew),
        pubKeyCollateralAddress(pubKeyCollateralAddressNew),
        pubKeyZnode(pubKeyZnodeNew),
        lastPing(),
        vchSig(),
        sigTime(GetAdjustedTime()),
        nLastDsq(0),
        nTimeLastChecked(0),
        nTimeLastPaid(0),
        nTimeLastWatchdogVote(0),
        nActiveState(ZNODE_ENABLED),
        nCacheCollateralBlock(0),
        nBlockLastPaid(0),
        nProtocolVersion(nProtocolVersionIn),
        nPoSeBanScore(0),
        nPoSeBanHeight(0),
        fAllowMixingTx(true),
        fUnitTest(false) {}

CZnode::CZnode(const CZnode &other) :
        vin(other.vin),
        addr(other.addr),
        pubKeyCollateralAddress(other.pubKeyCollateralAddress),
        pubKeyZnode(other.pubKeyZnode),
        lastPing(other.lastPing),
        vchSig(other.vchSig),
        sigTime(other.sigTime),
        nLastDsq(other.nLastDsq),
        nTimeLastChecked(other.nTimeLastChecked),
        nTimeLastPaid(other.nTimeLastPaid),
        nTimeLastWatchdogVote(other.nTimeLastWatchdogVote),
        nActiveState(other.nActiveState),
        nCacheCollateralBlock(other.nCacheCollateralBlock),
        nBlockLastPaid(other.nBlockLastPaid),
        nProtocolVersion(other.nProtocolVersion),
        nPoSeBanScore(other.nPoSeBanScore),
        nPoSeBanHeight(other.nPoSeBanHeight),
        fAllowMixingTx(other.fAllowMixingTx),
        fUnitTest(other.fUnitTest) {}

CZnode::CZnode(const CZnodeBroadcast &mnb) :
        vin(mnb.vin),
        addr(mnb.addr),
        pubKeyCollateralAddress(mnb.pubKeyCollateralAddress),
        pubKeyZnode(mnb.pubKeyZnode),
        lastPing(mnb.lastPing),
        vchSig(mnb.vchSig),
        sigTime(mnb.sigTime),
        nLastDsq(0),
        nTimeLastChecked(0),
        nTimeLastPaid(0),
        nTimeLastWatchdogVote(mnb.sigTime),
        nActiveState(mnb.nActiveState),
        nCacheCollateralBlock(0),
        nBlockLastPaid(0),
        nProtocolVersion(mnb.nProtocolVersion),
        nPoSeBanScore(0),
        nPoSeBanHeight(0),
        fAllowMixingTx(true),
        fUnitTest(false) {}

//CSporkManager sporkManager;
//
// When a new znode broadcast is sent, update our information
//
bool CZnode::UpdateFromNewBroadcast(CZnodeBroadcast &mnb) {
    return true;
}

//
// Deterministically calculate a given "score" for a Znode depending on how close it's hash is to
// the proof of work for that block. The further away they are the better, the furthest will win the election
// and get paid this block
//
arith_uint256 CZnode::CalculateScore(const uint256 &blockHash) {
    return arith_uint256();
}

void CZnode::Check(bool fForce) {
    LOCK(cs);

    if (ShutdownRequested()) return;

    if (!fForce && (GetTime() - nTimeLastChecked < ZNODE_CHECK_SECONDS)) return;
    nTimeLastChecked = GetTime();

    LogPrint("znode", "CZnode::Check -- Znode %s is in %s state\n", vin.prevout.ToStringShort(), GetStateString());

    //once spent, stop doing the checks
    if (IsOutpointSpent()) return;

    int nHeight = 0;
    if (!fUnitTest) {
        TRY_LOCK(cs_main, lockMain);
        if (!lockMain) return;

        Coin coin;
        if (!pcoinsTip->GetCoin(vin.prevout, coin) || coin.out.IsNull() || coin.IsSpent()) {
            nActiveState = ZNODE_OUTPOINT_SPENT;
            LogPrint("znode", "CZnode::Check -- Failed to find Znode UTXO, znode=%s\n", vin.prevout.ToStringShort());
            return;
        }

        nHeight = chainActive.Height();
    }

    if (IsPoSeBanned()) {
        if (nHeight < nPoSeBanHeight) return; // too early?
        // Otherwise give it a chance to proceed further to do all the usual checks and to change its state.
        // Znode still will be on the edge and can be banned back easily if it keeps ignoring mnverify
        // or connect attempts. Will require few mnverify messages to strengthen its position in mn list.
        LogPrintf("CZnode::Check -- Znode %s is unbanned and back in list now\n", vin.prevout.ToStringShort());
        DecreasePoSeBanScore();
    } else if (nPoSeBanScore >= ZNODE_POSE_BAN_MAX_SCORE) {
        nActiveState = ZNODE_POSE_BAN;
        // ban for the whole payment cycle
        LogPrintf("CZnode::Check -- Znode %s is banned till block %d now\n", vin.prevout.ToStringShort(), nPoSeBanHeight);
        return;
    }

    int nActiveStatePrev = nActiveState;
    bool fOurZnode = fMasternodeMode && activeZnode.pubKeyZnode == pubKeyZnode;

    // znode doesn't meet payment protocol requirements ...
/*    bool fRequireUpdate = nProtocolVersion < znpayments.GetMinZnodePaymentsProto() ||
                          // or it's our own node and we just updated it to the new protocol but we are still waiting for activation ...
                          (fOurZnode && nProtocolVersion < PROTOCOL_VERSION); */

    // keep old znodes on start, give them a chance to receive updates...
    bool fWaitForPing = !znodeSync.IsZnodeListSynced() && !IsPingedWithin(ZNODE_MIN_MNP_SECONDS);

    if (fWaitForPing && !fOurZnode) {
        // ...but if it was already expired before the initial check - return right away
        if (IsExpired() || IsWatchdogExpired() || IsNewStartRequired()) {
            LogPrint("znode", "CZnode::Check -- Znode %s is in %s state, waiting for ping\n", vin.prevout.ToStringShort(), GetStateString());
            return;
        }
    }

    // don't expire if we are still in "waiting for ping" mode unless it's our own znode
    if (!fWaitForPing || fOurZnode) {

        if (!IsPingedWithin(ZNODE_NEW_START_REQUIRED_SECONDS)) {
            nActiveState = ZNODE_NEW_START_REQUIRED;
            if (nActiveStatePrev != nActiveState) {
                LogPrint("znode", "CZnode::Check -- Znode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
            }
            return;
        }

        bool fWatchdogActive = znodeSync.IsSynced();
        bool fWatchdogExpired = (fWatchdogActive && ((GetTime() - nTimeLastWatchdogVote) > ZNODE_WATCHDOG_MAX_SECONDS));

//        LogPrint("znode", "CZnode::Check -- outpoint=%s, nTimeLastWatchdogVote=%d, GetTime()=%d, fWatchdogExpired=%d\n",
//                vin.prevout.ToStringShort(), nTimeLastWatchdogVote, GetTime(), fWatchdogExpired);

        if (fWatchdogExpired) {
            nActiveState = ZNODE_WATCHDOG_EXPIRED;
            if (nActiveStatePrev != nActiveState) {
                LogPrint("znode", "CZnode::Check -- Znode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
            }
            return;
        }

        if (!IsPingedWithin(ZNODE_EXPIRATION_SECONDS)) {
            nActiveState = ZNODE_EXPIRED;
            if (nActiveStatePrev != nActiveState) {
                LogPrint("znode", "CZnode::Check -- Znode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
            }
            return;
        }
    }

    if (Params().NetworkIDString() != CBaseChainParams::REGTEST && lastPing.sigTime - sigTime < ZNODE_MIN_MNP_SECONDS) {
        nActiveState = ZNODE_PRE_ENABLED;
        if (nActiveStatePrev != nActiveState) {
            LogPrint("znode", "CZnode::Check -- Znode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
        }
        return;
    }

    nActiveState = ZNODE_ENABLED; // OK
    if (nActiveStatePrev != nActiveState) {
        LogPrint("znode", "CZnode::Check -- Znode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
    }
}

bool CZnode::IsLegacyWindow(int height) {
    const Consensus::Params& params = ::Params().GetConsensus();
    return height >= params.DIP0003Height && height < params.DIP0003EnforcementHeight;
}

bool CZnode::IsValidNetAddr() {
    return IsValidNetAddr(addr);
}

bool CZnode::IsValidForPayment() {
    if (nActiveState == ZNODE_ENABLED) {
        return true;
    }
//    if(!sporkManager.IsSporkActive(SPORK_14_REQUIRE_SENTINEL_FLAG) &&
//       (nActiveState == ZNODE_WATCHDOG_EXPIRED)) {
//        return true;
//    }

    return false;
}

bool CZnode::IsValidNetAddr(CService addrIn) {
    // TODO: regtest is fine with any addresses for now,
    // should probably be a bit smarter if one day we start to implement tests for this
    return Params().NetworkIDString() == CBaseChainParams::REGTEST ||
           (addrIn.IsIPv4() && IsReachable(addrIn) && addrIn.IsRoutable());
}

znode_info_t CZnode::GetInfo() {
    znode_info_t info;
    info.vin = vin;
    info.addr = addr;
    info.pubKeyCollateralAddress = pubKeyCollateralAddress;
    info.pubKeyZnode = pubKeyZnode;
    info.sigTime = sigTime;
    info.nLastDsq = nLastDsq;
    info.nTimeLastChecked = nTimeLastChecked;
    info.nTimeLastPaid = nTimeLastPaid;
    info.nTimeLastWatchdogVote = nTimeLastWatchdogVote;
    info.nTimeLastPing = lastPing.sigTime;
    info.nActiveState = nActiveState;
    info.nProtocolVersion = nProtocolVersion;
    info.fInfoValid = true;
    return info;
}

std::string CZnode::StateToString(int nStateIn) {
    switch (nStateIn) {
        case ZNODE_PRE_ENABLED:
            return "PRE_ENABLED";
        case ZNODE_ENABLED:
            return "ENABLED";
        case ZNODE_EXPIRED:
            return "EXPIRED";
        case ZNODE_OUTPOINT_SPENT:
            return "OUTPOINT_SPENT";
        case ZNODE_UPDATE_REQUIRED:
            return "UPDATE_REQUIRED";
        case ZNODE_WATCHDOG_EXPIRED:
            return "WATCHDOG_EXPIRED";
        case ZNODE_NEW_START_REQUIRED:
            return "NEW_START_REQUIRED";
        case ZNODE_POSE_BAN:
            return "POSE_BAN";
        default:
            return "UNKNOWN";
    }
}

std::string CZnode::GetStateString() const {
    return StateToString(nActiveState);
}

std::string CZnode::GetStatus() const {
    // TODO: return smth a bit more human readable here
    return GetStateString();
}

std::string CZnode::ToString() const {
    std::string str;
    str += "znode{";
    str += addr.ToString();
    str += " ";
    str += std::to_string(nProtocolVersion);
    str += " ";
    str += vin.prevout.ToStringShort();
    str += " ";
    str += CBitcoinAddress(pubKeyCollateralAddress.GetID()).ToString();
    str += " ";
    str += std::to_string(lastPing == CZnodePing() ? sigTime : lastPing.sigTime);
    str += " ";
    str += std::to_string(lastPing == CZnodePing() ? 0 : lastPing.sigTime - sigTime);
    str += " ";
    str += std::to_string(nBlockLastPaid);
    str += "}\n";
    return str;
}

int CZnode::GetCollateralAge() {
    int nHeight;
    {
        TRY_LOCK(cs_main, lockMain);
        if (!lockMain || !chainActive.Tip()) return -1;
        nHeight = chainActive.Height();
    }

    if (nCacheCollateralBlock == 0) {
        int nInputAge = GetInputAge(vin);
        if (nInputAge > 0) {
            nCacheCollateralBlock = nHeight - nInputAge;
        } else {
            return nInputAge;
        }
    }

    return nHeight - nCacheCollateralBlock;
}

void CZnode::UpdateLastPaid(const CBlockIndex *pindex, int nMaxBlocksToScanBack) {
}

bool CZnodeBroadcast::Create(std::string strService, std::string strKeyZnode, std::string strTxHash, std::string strOutputIndex, std::string &strErrorRet, CZnodeBroadcast &mnbRet, bool fOffline) {
    LogPrintf("CZnodeBroadcast::Create\n");
    CTxIn txin;
    CPubKey pubKeyCollateralAddressNew;
    CKey keyCollateralAddressNew;
    CPubKey pubKeyZnodeNew;
    CKey keyZnodeNew;
    //need correct blocks to send ping
    if (!fOffline && !znodeSync.IsBlockchainSynced()) {
        strErrorRet = "Sync in progress. Must wait until sync is complete to start Znode";
        LogPrintf("CZnodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    if (!pwalletMain->GetZnodeVinAndKeys(txin, pubKeyCollateralAddressNew, keyCollateralAddressNew, strTxHash, strOutputIndex)) {
        strErrorRet = strprintf("Could not allocate txin %s:%s for znode %s", strTxHash, strOutputIndex, strService);
        LogPrintf("CZnodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    // TODO: upgrade dash

    CService service = LookupNumeric(strService.c_str());
    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (service.GetPort() != mainnetDefaultPort) {
            strErrorRet = strprintf("Invalid port %u for znode %s, only %d is supported on mainnet.", service.GetPort(), strService, mainnetDefaultPort);
            LogPrintf("CZnodeBroadcast::Create -- %s\n", strErrorRet);
            return false;
        }
    } else if (service.GetPort() == mainnetDefaultPort) {
        strErrorRet = strprintf("Invalid port %u for znode %s, %d is the only supported on mainnet.", service.GetPort(), strService, mainnetDefaultPort);
        LogPrintf("CZnodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    return Create(txin, LookupNumeric(strService.c_str()), keyCollateralAddressNew, pubKeyCollateralAddressNew, keyZnodeNew, pubKeyZnodeNew, strErrorRet, mnbRet);
}

bool CZnodeBroadcast::Create(CTxIn txin, CService service, CKey keyCollateralAddressNew, CPubKey pubKeyCollateralAddressNew, CKey keyZnodeNew, CPubKey pubKeyZnodeNew, std::string &strErrorRet, CZnodeBroadcast &mnbRet) {
    // wait for reindex and/or import to finish
    if (fImporting || fReindex) return false;

    LogPrint("znode", "CZnodeBroadcast::Create -- pubKeyCollateralAddressNew = %s, pubKeyZnodeNew.GetID() = %s\n",
             CBitcoinAddress(pubKeyCollateralAddressNew.GetID()).ToString(),
             pubKeyZnodeNew.GetID().ToString());


    CZnodePing mnp(txin);
    if (!mnp.Sign(keyZnodeNew, pubKeyZnodeNew)) {
        strErrorRet = strprintf("Failed to sign ping, znode=%s", txin.prevout.ToStringShort());
        LogPrintf("CZnodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CZnodeBroadcast();
        return false;
    }

    mnbRet = CZnodeBroadcast(service, txin, pubKeyCollateralAddressNew, pubKeyZnodeNew, LEGACY_ZNODES_PROTOCOL_VERSION);

    if (!mnbRet.IsValidNetAddr()) {
        strErrorRet = strprintf("Invalid IP address, znode=%s", txin.prevout.ToStringShort());
        LogPrintf("CZnodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CZnodeBroadcast();
        return false;
    }

    mnbRet.lastPing = mnp;
    if (!mnbRet.Sign(keyCollateralAddressNew)) {
        strErrorRet = strprintf("Failed to sign broadcast, znode=%s", txin.prevout.ToStringShort());
        LogPrintf("CZnodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CZnodeBroadcast();
        return false;
    }

    return true;
}

bool CZnodeBroadcast::SimpleCheck(int &nDos) {
    nDos = 0;

    // make sure addr is valid
    if (!IsValidNetAddr()) {
        LogPrintf("CZnodeBroadcast::SimpleCheck -- Invalid addr, rejected: znode=%s  addr=%s\n",
                  vin.prevout.ToStringShort(), addr.ToString());
        return false;
    }

    // make sure signature isn't in the future (past is OK)
    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrintf("CZnodeBroadcast::SimpleCheck -- Signature rejected, too far into the future: znode=%s\n", vin.prevout.ToStringShort());
        nDos = 1;
        return false;
    }

    // empty ping or incorrect sigTime/unknown blockhash
    if (lastPing == CZnodePing() || !lastPing.SimpleCheck(nDos)) {
        // one of us is probably forked or smth, just mark it as expired and check the rest of the rules
        nActiveState = ZNODE_EXPIRED;
    }

    CScript pubkeyScript;
    pubkeyScript = GetScriptForDestination(pubKeyCollateralAddress.GetID());

    if (pubkeyScript.size() != 25) {
        LogPrintf("CZnodeBroadcast::SimpleCheck -- pubKeyCollateralAddress has the wrong size\n");
        nDos = 100;
        return false;
    }

    CScript pubkeyScript2;
    pubkeyScript2 = GetScriptForDestination(pubKeyZnode.GetID());

    if (pubkeyScript2.size() != 25) {
        LogPrintf("CZnodeBroadcast::SimpleCheck -- pubKeyZnode has the wrong size\n");
        nDos = 100;
        return false;
    }

    if (!vin.scriptSig.empty()) {
        LogPrintf("CZnodeBroadcast::SimpleCheck -- Ignore Not Empty ScriptSig %s\n", vin.ToString());
        nDos = 100;
        return false;
    }

    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (addr.GetPort() != mainnetDefaultPort) return false;
    } else if (addr.GetPort() == mainnetDefaultPort) return false;

    return true;
}

bool CZnodeBroadcast::Update(CZnode *pmn, int &nDos) {
    nDos = 0;

    if (pmn->sigTime == sigTime && !fRecovery) {
        // mapSeenZnodeBroadcast in CZnodeMan::CheckMnbAndUpdateZnodeList should filter legit duplicates
        // but this still can happen if we just started, which is ok, just do nothing here.
        return false;
    }

    // this broadcast is older than the one that we already have - it's bad and should never happen
    // unless someone is doing something fishy
    if (pmn->sigTime > sigTime) {
        LogPrintf("CZnodeBroadcast::Update -- Bad sigTime %d (existing broadcast is at %d) for Znode %s %s\n",
                  sigTime, pmn->sigTime, vin.prevout.ToStringShort(), addr.ToString());
        return false;
    }

    pmn->Check();

    // znode is banned by PoSe
    if (pmn->IsPoSeBanned()) {
        LogPrintf("CZnodeBroadcast::Update -- Banned by PoSe, znode=%s\n", vin.prevout.ToStringShort());
        return false;
    }

    // IsVnAssociatedWithPubkey is validated once in CheckOutpoint, after that they just need to match
    if (pmn->pubKeyCollateralAddress != pubKeyCollateralAddress) {
        LogPrintf("CZnodeBroadcast::Update -- Got mismatched pubKeyCollateralAddress and vin\n");
        nDos = 33;
        return false;
    }

    if (!CheckSignature(nDos)) {
        LogPrintf("CZnodeBroadcast::Update -- CheckSignature() failed, znode=%s\n", vin.prevout.ToStringShort());
        return false;
    }

    // if ther was no znode broadcast recently or if it matches our Znode privkey...
    if (!pmn->IsBroadcastedWithin(ZNODE_MIN_MNB_SECONDS) || (fMasternodeMode && pubKeyZnode == activeZnode.pubKeyZnode)) {
        // take the newest entry
        LogPrintf("CZnodeBroadcast::Update -- Got UPDATED Znode entry: addr=%s\n", addr.ToString());
        if (pmn->UpdateFromNewBroadcast((*this))) {
            pmn->Check();
            RelayZNode();
        }
        znodeSync.AddedZnodeList();
    }

    return true;
}

bool CZnodeBroadcast::CheckOutpoint(int &nDos) {
    // we are a znode with the same vin (i.e. already activated) and this mnb is ours (matches our Znode privkey)
    // so nothing to do here for us
    if (fMasternodeMode && vin.prevout == activeZnode.vin.prevout && pubKeyZnode == activeZnode.pubKeyZnode) {
        return false;
    }

    if (!CheckSignature(nDos)) {
        LogPrintf("CZnodeBroadcast::CheckOutpoint -- CheckSignature() failed, znode=%s\n", vin.prevout.ToStringShort());
        return false;
    }

    {
        TRY_LOCK(cs_main, lockMain);
        if (!lockMain) {
            return false;
        }

        Coin coin;
        if (!pcoinsTip->GetCoin(vin.prevout, coin) || coin.out.IsNull() || coin.IsSpent()) {
            LogPrint("znode", "CZnodeBroadcast::CheckOutpoint -- Failed to find Znode UTXO, znode=%s\n", vin.prevout.ToStringShort());
            return false;
        }
        if (coin.out.nValue != ZNODE_COIN_REQUIRED * COIN) {
            LogPrint("znode", "CZnodeBroadcast::CheckOutpoint -- Znode UTXO should have 1000 XZC, znode=%s\n", vin.prevout.ToStringShort());
            return false;
        }
        if (chainActive.Height() - coin.nHeight + 1 < Params().GetConsensus().nZnodeMinimumConfirmations) {
            LogPrintf("CZnodeBroadcast::CheckOutpoint -- Znode UTXO must have at least %d confirmations, znode=%s\n",
                      Params().GetConsensus().nZnodeMinimumConfirmations, vin.prevout.ToStringShort());
            // maybe we miss few blocks, let this mnb to be checked again later
            return false;
        }
    }

    LogPrint("znode", "CZnodeBroadcast::CheckOutpoint -- Znode UTXO verified\n");

    // verify that sig time is legit in past
    // should be at least not earlier than block when 1000 XZC tx got nZnodeMinimumConfirmations
    uint256 hashBlock = uint256();
    CTransactionRef tx2;
    GetTransaction(vin.prevout.hash, tx2, Params().GetConsensus(), hashBlock, true);
    {
        LOCK(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
        if (mi != mapBlockIndex.end() && (*mi).second) {
            CBlockIndex *pMNIndex = (*mi).second; // block for 1000 XZC tx -> 1 confirmation
            CBlockIndex *pConfIndex = chainActive[pMNIndex->nHeight + Params().GetConsensus().nZnodeMinimumConfirmations - 1]; // block where tx got nZnodeMinimumConfirmations
            if (pConfIndex->GetBlockTime() > sigTime) {
                LogPrintf("CZnodeBroadcast::CheckOutpoint -- Bad sigTime %d (%d conf block is at %d) for Znode %s %s\n",
                          sigTime, Params().GetConsensus().nZnodeMinimumConfirmations, pConfIndex->GetBlockTime(), vin.prevout.ToStringShort(), addr.ToString());
                return false;
            }
        }
    }

    return true;
}

bool CZnodeBroadcast::Sign(CKey &keyCollateralAddress) {
    std::string strError;
    std::string strMessage;

    sigTime = GetAdjustedTime();

    strMessage = addr.ToString() + boost::lexical_cast<std::string>(sigTime) +
                 pubKeyCollateralAddress.GetID().ToString() + pubKeyZnode.GetID().ToString() +
                 boost::lexical_cast<std::string>(nProtocolVersion);

    return true;
}

bool CZnodeBroadcast::CheckSignature(int &nDos) {
    std::string strMessage;
    std::string strError = "";
    nDos = 0;

    strMessage = addr.ToString() + boost::lexical_cast<std::string>(sigTime) +
                 pubKeyCollateralAddress.GetID().ToString() + pubKeyZnode.GetID().ToString() +
                 boost::lexical_cast<std::string>(nProtocolVersion);

    LogPrint("znode", "CZnodeBroadcast::CheckSignature -- strMessage: %s  pubKeyCollateralAddress address: %s  sig: %s\n", strMessage, CBitcoinAddress(pubKeyCollateralAddress.GetID()).ToString(), EncodeBase64(&vchSig[0], vchSig.size()));

    return true;
}

void CZnodeBroadcast::RelayZNode() {
}

CZnodePing::CZnodePing(CTxIn &vinNew) {
    LOCK(cs_main);
    if (!chainActive.Tip() || chainActive.Height() < 12) return;

    vin = vinNew;
    blockHash = chainActive[chainActive.Height() - 12]->GetBlockHash();
    sigTime = GetAdjustedTime();
    vchSig = std::vector < unsigned char > ();
}

bool CZnodePing::Sign(CKey &keyZnode, CPubKey &pubKeyZnode) {
    std::string strError;
    std::string strZNodeSignMessage;

    sigTime = GetAdjustedTime();
    std::string strMessage = vin.ToString() + blockHash.ToString() + boost::lexical_cast<std::string>(sigTime);

    return true;
}

bool CZnodePing::CheckSignature(CPubKey &pubKeyZnode, int &nDos) {
    std::string strMessage = vin.ToString() + blockHash.ToString() + boost::lexical_cast<std::string>(sigTime);
    std::string strError = "";
    nDos = 0;

    return true;
}

bool CZnodePing::SimpleCheck(int &nDos) {
    // don't ban by default
    nDos = 0;

    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrintf("CZnodePing::SimpleCheck -- Signature rejected, too far into the future, znode=%s\n", vin.prevout.ToStringShort());
        nDos = 1;
        return false;
    }

    {
//        LOCK(cs_main);
        AssertLockHeld(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(blockHash);
        if (mi == mapBlockIndex.end()) {
            LogPrint("znode", "CZnodePing::SimpleCheck -- Znode ping is invalid, unknown block hash: znode=%s blockHash=%s\n", vin.prevout.ToStringShort(), blockHash.ToString());
            // maybe we stuck or forked so we shouldn't ban this node, just fail to accept this ping
            // TODO: or should we also request this block?
            return false;
        }
    }
    LogPrint("znode", "CZnodePing::SimpleCheck -- Znode ping verified: znode=%s  blockHash=%s  sigTime=%d\n", vin.prevout.ToStringShort(), blockHash.ToString(), sigTime);
    return true;
}

bool CZnodePing::CheckAndUpdate(CZnode *pmn, bool fFromNewBroadcast, int &nDos) {
    // don't ban by default
    nDos = 0;

    if (!SimpleCheck(nDos)) {
        return false;
    }

    if (pmn == NULL) {
        LogPrint("znode", "CZnodePing::CheckAndUpdate -- Couldn't find Znode entry, znode=%s\n", vin.prevout.ToStringShort());
        return false;
    }

    if (!fFromNewBroadcast) {
        if (pmn->IsUpdateRequired()) {
            LogPrint("znode", "CZnodePing::CheckAndUpdate -- znode protocol is outdated, znode=%s\n", vin.prevout.ToStringShort());
            return false;
        }

        if (pmn->IsNewStartRequired()) {
            LogPrint("znode", "CZnodePing::CheckAndUpdate -- znode is completely expired, new start is required, znode=%s\n", vin.prevout.ToStringShort());
            return false;
        }
    }

    {
        LOCK(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(blockHash);
        if ((*mi).second && (*mi).second->nHeight < chainActive.Height() - 24) {
            // LogPrintf("CZnodePing::CheckAndUpdate -- Znode ping is invalid, block hash is too old: znode=%s  blockHash=%s\n", vin.prevout.ToStringShort(), blockHash.ToString());
            // nDos = 1;
            return false;
        }
    }

    LogPrint("znode", "CZnodePing::CheckAndUpdate -- New ping: znode=%s  blockHash=%s  sigTime=%d\n", vin.prevout.ToStringShort(), blockHash.ToString(), sigTime);

    // LogPrintf("mnping - Found corresponding mn for vin: %s\n", vin.prevout.ToStringShort());
    // update only if there is no known ping for this znode or
    // last ping was more then ZNODE_MIN_MNP_SECONDS-60 ago comparing to this one
    if (pmn->IsPingedWithin(ZNODE_MIN_MNP_SECONDS - 60, sigTime)) {
        LogPrint("znode", "CZnodePing::CheckAndUpdate -- Znode ping arrived too early, znode=%s\n", vin.prevout.ToStringShort());
        //nDos = 1; //disable, this is happening frequently and causing banned peers
        return false;
    }

    if (!CheckSignature(pmn->pubKeyZnode, nDos)) return false;

    // so, ping seems to be ok

    // if we are still syncing and there was no known ping for this mn for quite a while
    // (NOTE: assuming that ZNODE_EXPIRATION_SECONDS/2 should be enough to finish mn list sync)
    if (!znodeSync.IsZnodeListSynced() && !pmn->IsPingedWithin(ZNODE_EXPIRATION_SECONDS / 2)) {
        // let's bump sync timeout
        LogPrint("znode", "CZnodePing::CheckAndUpdate -- bumping sync timeout, znode=%s\n", vin.prevout.ToStringShort());
        znodeSync.AddedZnodeList();
    }

    // let's store this ping as the last one
    LogPrint("znode", "CZnodePing::CheckAndUpdate -- Znode ping accepted, znode=%s\n", vin.prevout.ToStringShort());
    pmn->lastPing = *this;

    // and update mnodeman.mapSeenZnodeBroadcast.lastPing which is probably outdated
    CZnodeBroadcast mnb(*pmn);
    uint256 hash = mnb.GetHash();

    pmn->Check(true); // force update, ignoring cache
    if (!pmn->IsEnabled()) return false;

    LogPrint("znode", "CZnodePing::CheckAndUpdate -- Znode ping acceepted and relayed, znode=%s\n", vin.prevout.ToStringShort());
    Relay();

    return true;
}

void CZnodePing::Relay() {
}

//void CZnode::AddGovernanceVote(uint256 nGovernanceObjectHash)
//{
//    if(mapGovernanceObjectsVotedOn.count(nGovernanceObjectHash)) {
//        mapGovernanceObjectsVotedOn[nGovernanceObjectHash]++;
//    } else {
//        mapGovernanceObjectsVotedOn.insert(std::make_pair(nGovernanceObjectHash, 1));
//    }
//}

//void CZnode::RemoveGovernanceObject(uint256 nGovernanceObjectHash)
//{
//    std::map<uint256, int>::iterator it = mapGovernanceObjectsVotedOn.find(nGovernanceObjectHash);
//    if(it == mapGovernanceObjectsVotedOn.end()) {
//        return;
//    }
//    mapGovernanceObjectsVotedOn.erase(it);
//}

void CZnode::UpdateWatchdogVoteTime() {
    LOCK(cs);
    nTimeLastWatchdogVote = GetTime();
}

/**
*   FLAG GOVERNANCE ITEMS AS DIRTY
*
*   - When znode come and go on the network, we must flag the items they voted on to recalc it's cached flags
*
*/
//void CZnode::FlagGovernanceItemsAsDirty()
//{
//    std::vector<uint256> vecDirty;
//    {
//        std::map<uint256, int>::iterator it = mapGovernanceObjectsVotedOn.begin();
//        while(it != mapGovernanceObjectsVotedOn.end()) {
//            vecDirty.push_back(it->first);
//            ++it;
//        }
//    }
//    for(size_t i = 0; i < vecDirty.size(); ++i) {
//        mnodeman.AddDirtyGovernanceObjectHash(vecDirty[i]);
//    }
//}
