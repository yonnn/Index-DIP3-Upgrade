#include "bip47/utils.h"
#include "bip47/paymentcode.h"
#include "secretpoint.h"
#include "primitives/transaction.h"
#include "bip47/paymentaddress.h"
#include <vector>
#include "uint256.h"
#include "wallet/wallet.h"

using namespace std;

bool CBIP47Util::getOpCodeOutput(const CTransaction& tx, CTxOut& txout) {
    for(int i = 0; i < tx.vout.size(); i++) {
        if (tx.vout[i].scriptPubKey[0] == OP_RETURN) {
            txout = tx.vout[i];
            return true;
        }
    }
    return false;
}


bool CBIP47Util::isValidNotificationTransactionOpReturn(CTxOut txout) {
    vector<unsigned char> op_date;
    return getOpCodeData(txout, op_date);
}

bool CBIP47Util::getOpCodeData(CTxOut txout, vector<unsigned char>& op_data) {
    CScript::const_iterator pc = txout.scriptPubKey.begin();
    vector<unsigned char> data;
    
    while (pc < txout.scriptPubKey.end())
    {
        opcodetype opcode;
        if (!txout.scriptPubKey.GetOp(pc, opcode, data))
        {
            LogPrintf("GetOp false in getOpCodeData\n");
            return false;
        }
        LogPrintf("Data.size() = %d,  opcode = 0x%x\n", data.size(), opcode);
        if (data.size() > 0 && opcode < OP_PUSHDATA4  )
        {
            op_data = data;
            return true;
        } 
        
    }
}

bool CBIP47Util::getPaymentCodeInNotificationTransaction(vector<unsigned char> privKeyBytes, CTransaction tx, CPaymentCode &paymentCode) {
    // tx.vin[0].scriptSig
//     CWalletTx wtx(pwalletMain, tx);

    CTxOut txout;
    if(!getOpCodeOutput(tx, txout)) {
        LogPrintf("Cannot Get OpCodeOutput\n");
        return false;
    }

    if(!isValidNotificationTransactionOpReturn(txout))
    {
        LogPrintf("Error isValidNotificationTransactionOpReturn txout\n");
        return false;
    }

    vector<unsigned char> op_data;
    if(!getOpCodeData(txout, op_data)) {
        LogPrintf("Cannot Get OpCodeData\n");
        return false;
    }

    /**
     * @Todo Get PubKeyBytes from tx script Sig
     * */
    vector<unsigned char> pubKeyBytes;

    if (!getScriptSigPubkey(tx.vin[0], pubKeyBytes))
    {
        LogPrintf("Bip47Utiles CPaymentCode ScriptSig GetPubkey error\n");
        return false;
    }
    
    LogPrintf("pubkeyBytes size = %d\n", pubKeyBytes.size());


    vector<unsigned char> outpoint(tx.vin[0].prevout.hash.begin(), tx.vin[0].prevout.hash.end());
    
    SecretPoint secretPoint(privKeyBytes, pubKeyBytes);
    
    LogPrintf("Generating Secret Point for Decode with \n privekey: %s\n pubkey: %s\n", HexStr(privKeyBytes), HexStr(pubKeyBytes));
    
    LogPrintf("output: %s\n", tx.vin[0].prevout.hash.GetHex());
    uint256 secretPBytes(secretPoint.ECDHSecretAsBytes());
    LogPrintf("secretPoint: %s\n", secretPBytes.GetHex());

    vector<unsigned char> mask = CPaymentCode::getMask(secretPoint.ECDHSecretAsBytes(), outpoint);
    vector<unsigned char> payload = CPaymentCode::blind(op_data, mask);
    CPaymentCode pcode(payload.data(), payload.size());
    paymentCode = pcode;
    return true;
}

bool CBIP47Util::getScriptSigPubkey(CTxIn txin, vector<unsigned char>& pubkeyBytes)
{

    LogPrintf("ScriptSig size = %d\n", txin.scriptSig.size());
    CScript::const_iterator pc = txin.scriptSig.begin();
    vector<unsigned char> chunk0data;
    vector<unsigned char> chunk1data;
    
    opcodetype opcode0, opcode1;
    if (!txin.scriptSig.GetOp(pc, opcode0, chunk0data))
    {
        LogPrintf("Bip47Utiles ScriptSig Chunk0 error != 2\n");
        return false;
    }
    LogPrintf("opcode0 = %x, chunk0data.size = %d\n", opcode0, chunk0data.size());

    if (!txin.scriptSig.GetOp(pc, opcode1, chunk1data))
    {
        //check whether this is a P2PK redeems cript
        CScript dest = pwalletMain->mapWallet[txin.prevout.hash].tx->vout[txin.prevout.n].scriptPubKey;
        CScript::const_iterator pc = dest.begin();
        opcodetype opcode;
        std::vector<unsigned char> vch;
        if (!dest.GetOp(pc, opcode, vch) || vch.size() < 33 || vch.size() > 65) 
            return false;
        CPubKey pubKeyOut = CPubKey(vch);
        if (!pubKeyOut.IsFullyValid())
            return false;
        if (!dest.GetOp(pc, opcode, vch) || opcode != OP_CHECKSIG || dest.GetOp(pc, opcode, vch))
            return false;

        pubkeyBytes.clear();
        std::copy(pubKeyOut.begin(), pubKeyOut.end(), std::back_inserter(pubkeyBytes));
        return true;
    } 
    LogPrintf("opcode1 = %x, chunk1data.size = %d\n", opcode1, chunk1data.size());
    
    if(!chunk0data.empty() && chunk0data.size() > 2 && !chunk1data.empty() && chunk1data.size() > 2)
    {
        pubkeyBytes = chunk1data;
        return true;
    }
    else if(opcode0 == OP_CHECKSIG && !chunk0data.empty() && chunk0data.size() > 2)
    {
        pubkeyBytes = chunk0data;
        return true;
    }

    LogPrintf("Script did not match expected form: \n");
    return false;
}

CPaymentAddress CBIP47Util::getPaymentAddress(CPaymentCode &pcode, int idx, CExtKey extkey) {
    CKey prvkey = extkey.key;
    
    vector<unsigned char> ppkeybytes(prvkey.begin(), prvkey.end());
    
    
    CPaymentAddress paddr(pcode, idx, ppkeybytes);
    return paddr;
    
}

CPaymentAddress CBIP47Util::getReceiveAddress(CWallet* pbip47Wallet, CPaymentCode &pcode_from, int idx)
{
    CPaymentAddress pm_address;
    CExtKey accEkey = pbip47Wallet->getBIP47Account(0).keyPrivAt(idx);
    if(accEkey.key.IsValid()){ //Keep Empty
    }
    pm_address = getPaymentAddress(pcode_from, 0, accEkey);
    
    return pm_address;
}

CPaymentAddress CBIP47Util::getSendAddress(CWallet* pbip47Wallet, CPaymentCode &pcode_to, int idx)
{
    CPaymentAddress pm_address;
    CExtKey accEkey = pbip47Wallet->getBIP47Account(0).keyPrivAt(0);
    if(accEkey.key.IsValid()){ //Keep Empty
    }
    pm_address = getPaymentAddress(pcode_to, idx, accEkey);
    
    return pm_address;
    
}




