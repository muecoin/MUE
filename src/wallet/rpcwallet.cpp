// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin developers
// Copyright (c) 2012-2013 The PPCoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX developers
// Copyright (c) 2019 The Phore Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "amount.h"
#include "base58.h"
#include "core_io.h"
#include "init.h"
#include "net.h"
#include "netbase.h"
#include "rpc/server.h"
#include "timedata.h"
#include "util.h"
#include "utilmoneystr.h"
#include "wallet/wallet.h"
#include "wallet/walletdb.h"

#include <stdint.h>

#include "libzerocoin/Coin.h"
#include "primitives/deterministicmint.h"
#include "spork.h"
#include <boost/assign/list_of.hpp>
#include <boost/thread/thread.hpp>

#include <univalue.h>

using namespace std;
using namespace boost;
using namespace boost::assign;

int64_t nWalletUnlockTime;
static CCriticalSection cs_nWalletUnlockTime;

std::string HelpRequiringPassphrase()
{
    return pwalletMain && pwalletMain->IsCrypted() ? "\nRequires wallet passphrase to be set with walletpassphrase call." : "";
}

void EnsureWalletIsUnlocked(bool fAllowAnonOnly)
{
    if (pwalletMain->IsLocked() || (!fAllowAnonOnly && pwalletMain->fWalletUnlockAnonymizeOnly))
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");
}

void WalletTxToJSON(const CWalletTx& wtx, UniValue& entry)
{
    int confirms = wtx.GetDepthInMainChain(false);
    int confirmsTotal = GetIXConfirmations(wtx.GetHash()) + confirms;
    entry.push_back(Pair("confirmations", confirmsTotal));
    entry.push_back(Pair("bcconfirmations", confirms));
    if (wtx.IsCoinBase() || wtx.IsCoinStake())
        entry.push_back(Pair("generated", true));
    if (confirms > 0) {
        entry.push_back(Pair("blockhash", wtx.hashBlock.GetHex()));
        entry.push_back(Pair("blockindex", wtx.nIndex));
        entry.push_back(Pair("blocktime", mapBlockIndex[wtx.hashBlock]->GetBlockTime()));
    }
    uint256 hash = wtx.GetHash();
    entry.push_back(Pair("txid", hash.GetHex()));
    UniValue conflicts(UniValue::VARR);
    BOOST_FOREACH (const uint256& conflict, wtx.GetConflicts())
        conflicts.push_back(conflict.GetHex());
    entry.push_back(Pair("walletconflicts", conflicts));
    entry.push_back(Pair("time", wtx.GetTxTime()));
    entry.push_back(Pair("timereceived", (int64_t)wtx.nTimeReceived));
    BOOST_FOREACH (const PAIRTYPE(string, string) & item, wtx.mapValue)
        entry.push_back(Pair(item.first, item.second));
}

string AccountFromValue(const UniValue& value)
{
    string strAccount = value.get_str();
    if (strAccount == "*")
        throw JSONRPCError(RPC_WALLET_INVALID_ACCOUNT_NAME, "Invalid account name");
    return strAccount;
}

UniValue getnewaddress(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw runtime_error(
            "getnewaddress ( \"account\" \"address_type\" )\n"
            "\nReturns a new Phore address for receiving payments.\n"
            "If 'account' is specified (recommended), it is added to the address book \n"
            "so payments received with the address will be credited to 'account'.\n"
            "\nArguments:\n"
            "1. \"account\"        (string, optional) The account name for the address to be linked to. if not provided, the default account \"\" is used. It can also be set to the empty string \"\" to represent the default account. The account does not need to exist, it will be created if there is no account by the given name.\n"
            "2. \"address_type\"   (string, optional) The address type to use. Options are \"legacy\", \"p2sh-segwit\", and \"bech32\". Default is set by -addresstype.\n"
            "\nResult:\n"
            "\"phoreaddress\"    (string) The new phore address\n"
            "\nExamples:\n" +
            HelpExampleCli("getnewaddress", "") + HelpExampleCli("getnewaddress", "\"\"") + HelpExampleCli("getnewaddress", "\"myaccount\"") + HelpExampleRpc("getnewaddress", "\"myaccount\""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    // Parse the account first so we don't generate a key if there's an error
    string strAccount;
    if (params.size() > 0)
        strAccount = AccountFromValue(params[0]);

    OutputType output_type = g_address_type;
    if (!params[1].isNull()) {
        output_type = ParseOutputType(params[1].get_str(), g_address_type);
        if (output_type == OUTPUT_TYPE_NONE) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Unknown address type '%s'", params[1].get_str()));
        }
    }

    if (!pwalletMain->IsLocked())
        pwalletMain->TopUpKeyPool();

    // Generate a new key that is added to wallet
    CPubKey newKey;
    if (!pwalletMain->GetKeyFromPool(newKey, false))
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");
 
    pwalletMain->LearnRelatedScripts(newKey, output_type);
    CTxDestination dest = GetDestinationForKey(newKey, output_type);

    pwalletMain->SetAddressBook(dest, strAccount, "receive");

    return EncodeDestination(dest);
}


CTxDestination GetAccountDestination(string strAccount, bool bForceNew = false)
{
    CWalletDB walletdb(pwalletMain->strWalletFile);

    CAccount account;
    walletdb.ReadAccount(strAccount, account);

    bool bKeyUsed = false;

    // Check if the current key has been used
    if (account.vchPubKey.IsValid()) {
        CScript scriptPubKey = GetScriptForDestination(account.vchPubKey.GetID());
        for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin();
             it != pwalletMain->mapWallet.end() && account.vchPubKey.IsValid();
             ++it) {
            const CWalletTx& wtx = (*it).second;
            BOOST_FOREACH (const CTxOut& txout, wtx.vout)
                if (txout.scriptPubKey == scriptPubKey)
                    bKeyUsed = true;
        }
    }

    // Generate a new key
    if (!account.vchPubKey.IsValid() || bForceNew || bKeyUsed) {
        if (!pwalletMain->GetKeyFromPool(account.vchPubKey, false))
            throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");

        pwalletMain->SetAddressBook(account.vchPubKey.GetID(), strAccount, "receive");
        walletdb.WriteAccount(strAccount, account);
    }

    return CTxDestination(account.vchPubKey.GetID());
}

UniValue getaccountaddress(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "getaccountaddress \"account\"\n"
            "\nReturns the current Phore address for receiving payments to this account.\n"
            "\nArguments:\n"
            "1. \"account\"       (string, required) The account name for the address. It can also be set to the empty string \"\" to represent the default account. The account does not need to exist, it will be created and a new address created  if there is no account by the given name.\n"
            "\nResult:\n"
            "\"phoreaddress\"   (string) The account phore address\n"
            "\nExamples:\n" +
            HelpExampleCli("getaccountaddress", "") + HelpExampleCli("getaccountaddress", "\"\"") + HelpExampleCli("getaccountaddress", "\"myaccount\"") + HelpExampleRpc("getaccountaddress", "\"myaccount\""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    // Parse the account first so we don't generate a key if there's an error
    string strAccount = AccountFromValue(params[0]);

    UniValue ret(UniValue::VSTR);

    ret = EncodeDestination(GetAccountDestination(strAccount));
    return ret;
}


UniValue getrawchangeaddress(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "getrawchangeaddress ( \"address_type\" )\n"
            "\nReturns a new Phore address, for receiving change.\n"
            "This is for use with raw transactions, NOT normal use.\n"
            "\nArguments:\n"
            "1. \"address_type\"           (string, optional) The address type to use. Options are \"legacy\", \"p2sh\", and \"bech32\". Default is set by -changetype.\n"
            "\nResult:\n"
            "\"address\"    (string) The address\n"
            "\nExamples:\n" +
            HelpExampleCli("getrawchangeaddress", "") + HelpExampleRpc("getrawchangeaddress", ""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    if (!pwalletMain->IsLocked())
        pwalletMain->TopUpKeyPool();
 
    OutputType output_type = g_change_type;
    if (!params[0].isNull()) {
        output_type = ParseOutputType(params[0].get_str(), g_change_type);
        if (output_type == OUTPUT_TYPE_NONE) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Unknown address type '%s'", params[0].get_str()));
        }
    }

    CReserveKey reservekey(pwalletMain);
    CPubKey vchPubKey;
    if (!reservekey.GetReservedKey(vchPubKey, true))
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");

    reservekey.KeepKey();

    pwalletMain->LearnRelatedScripts(vchPubKey, output_type);
    CTxDestination dest = GetDestinationForKey(vchPubKey, output_type);

    return EncodeDestination(dest);
}


UniValue setaccount(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "setaccount \"phoreaddress\" \"account\"\n"
            "\nSets the account associated with the given address.\n"
            "\nArguments:\n"
            "1. \"phoreaddress\"  (string, required) The phore address to be associated with an account.\n"
            "2. \"account\"         (string, required) The account to assign the address to.\n"
            "\nExamples:\n" +
            HelpExampleCli("setaccount", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\" \"tabby\"") + HelpExampleRpc("setaccount", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\", \"tabby\""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    if (!IsValidDestinationString(params[0].get_str()))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Phore address");

    CTxDestination address = DecodeDestination(params[0].get_str());

    string strAccount;
    if (params.size() > 1)
        strAccount = AccountFromValue(params[1]);

    // Only add the account if the address is yours.
    if (IsMine(*pwalletMain, address)) {
        // Detect when changing the account of an address that is the 'unused current key' of another account:
        if (pwalletMain->mapAddressBook.count(address)) {
            string strOldAccount = pwalletMain->mapAddressBook[address].name;
            if (address == GetAccountDestination(strOldAccount))
                GetAccountDestination(strOldAccount, true);
        }
        pwalletMain->SetAddressBook(address, strAccount, "receive");
    } else
        throw JSONRPCError(RPC_MISC_ERROR, "setaccount can only be used with own address");

    return NullUniValue;
}


UniValue getaccount(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "getaccount \"phoreaddress\"\n"
            "\nReturns the account associated with the given address.\n"
            "\nArguments:\n"
            "1. \"phoreaddress\"  (string, required) The phore address for account lookup.\n"
            "\nResult:\n"
            "\"accountname\"        (string) the account address\n"
            "\nExamples:\n" +
            HelpExampleCli("getaccount", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\"") + HelpExampleRpc("getaccount", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    if (!IsValidDestinationString(params[0].get_str()))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Phore address");

    CTxDestination address = DecodeDestination(params[0].get_str());

    string strAccount;
    map<CTxDestination, CAddressBookData>::iterator mi = pwalletMain->mapAddressBook.find(address);
    if (mi != pwalletMain->mapAddressBook.end() && !(*mi).second.name.empty())
        strAccount = (*mi).second.name;
    return strAccount;
}


UniValue getaddressesbyaccount(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "getaddressesbyaccount \"account\"\n"
            "\nReturns the list of addresses for the given account.\n"
            "\nArguments:\n"
            "1. \"account\"  (string, required) The account name.\n"
            "\nResult:\n"
            "[                     (json array of string)\n"
            "  \"phoreaddress\"  (string) a phore address associated with the given account\n"
            "  ,...\n"
            "]\n"
            "\nExamples:\n" +
            HelpExampleCli("getaddressesbyaccount", "\"tabby\"") + HelpExampleRpc("getaddressesbyaccount", "\"tabby\""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    string strAccount = AccountFromValue(params[0]);

    // Find all addresses that have the given account
    UniValue ret(UniValue::VARR);
    BOOST_FOREACH (const PAIRTYPE(CTxDestination, CAddressBookData) & item, pwalletMain->mapAddressBook) {
        const CTxDestination& address = item.first;
        const string& strName = item.second.name;
        if (strName == strAccount)
            ret.push_back(EncodeDestination(address));
    }
    return ret;
}

void SendMoney(const CTxDestination& address, CAmount nValue, CWalletTx& wtxNew, bool fUseIX = false)
{
    // Check amount
    if (nValue <= 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid amount");

    if (nValue > pwalletMain->GetBalance())
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");

    string strError;
    if (pwalletMain->IsLocked()) {
        strError = "Error: Wallet locked, unable to create transaction!";
        LogPrintf("SendMoney() : %s", strError);
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }

    // Parse Phore address
    CScript scriptPubKey = GetScriptForDestination(address);

    // Create and send the transaction
    CReserveKey reservekey(pwalletMain);
    CAmount nFeeRequired;
    if (!pwalletMain->CreateTransaction(scriptPubKey, nValue, wtxNew, reservekey, nFeeRequired, strError, NULL, ALL_COINS, fUseIX, (CAmount)0)) {
        if (nValue + nFeeRequired > pwalletMain->GetBalance())
            strError = strprintf("Error: This transaction requires a transaction fee of at least %s because of its amount, complexity, or use of recently received funds!", FormatMoney(nFeeRequired));
        LogPrintf("SendMoney() : %s\n", strError);
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }
    if (!pwalletMain->CommitTransaction(wtxNew, reservekey, (!fUseIX ? NetMsgType::TX : NetMsgType::IX)))
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: The transaction was rejected! This might happen if some of the coins in your wallet were already spent, such as if you used a copy of wallet.dat and coins were spent in the copy but not marked as spent here.");
}

UniValue sendtoaddress(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 4)
        throw runtime_error(
            "sendtoaddress \"phoreaddress\" amount ( \"comment\" \"comment-to\" )\n"
            "\nSend an amount to a given address. The amount is a real and is rounded to the nearest 0.00000001\n" +
            HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"phoreaddress\"  (string, required) The phore address to send to.\n"
            "2. \"amount\"      (numeric, required) The amount in phore to send. e.g. 0.1\n"
            "3. \"comment\"     (string, optional) A comment used to store what the transaction is for. \n"
            "                             This is not part of the transaction, just kept in your wallet.\n"
            "4. \"comment-to\"  (string, optional) A comment to store the name of the person or organization \n"
            "                             to which you're sending the transaction. This is not part of the \n"
            "                             transaction, just kept in your wallet.\n"
            "\nResult:\n"
            "\"transactionid\"  (string) The transaction id.\n"
            "\nExamples:\n" +
            HelpExampleCli("sendtoaddress", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\" 0.1") + HelpExampleCli("sendtoaddress", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\" 0.1 \"donation\" \"seans outpost\"") + HelpExampleRpc("sendtoaddress", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\", 0.1, \"donation\", \"seans outpost\""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    if (!IsValidDestinationString(params[0].get_str()))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Phore address");
    
    CTxDestination address = DecodeDestination(params[0].get_str());

    // Amount
    CAmount nAmount = AmountFromValue(params[1]);

    // Wallet comments
    CWalletTx wtx;
    if (params.size() > 2 && !params[2].isNull() && !params[2].get_str().empty())
        wtx.mapValue["comment"] = params[2].get_str();
    if (params.size() > 3 && !params[3].isNull() && !params[3].get_str().empty())
        wtx.mapValue["to"] = params[3].get_str();

    EnsureWalletIsUnlocked();

    SendMoney(address, nAmount, wtx);

    return wtx.GetHash().GetHex();
}

UniValue sendtoaddressix(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 4)
        throw runtime_error(
            "sendtoaddressix \"phoreaddress\" amount ( \"comment\" \"comment-to\" )\n"
            "\nSend an amount to a given address. The amount is a real and is rounded to the nearest 0.00000001\n" +
            HelpRequiringPassphrase() +
            "\nArguments:\n"
            "1. \"phoreaddress\"  (string, required) The phore address to send to.\n"
            "2. \"amount\"      (numeric, required) The  to send. e.g. 0.1\n"
            "3. \"comment\"     (string, optional) A comment used to store what the transaction is for. \n"
            "                             This is not part of the transaction, just kept in your wallet.\n"
            "4. \"comment-to\"  (string, optional) A comment to store the name of the person or organization \n"
            "                             to which you're sending the transaction. This is not part of the \n"
            "                             transaction, just kept in your wallet.\n"
            "\nResult:\n"
            "\"transactionid\"  (string) The transaction id.\n"
            "\nExamples:\n" +
            HelpExampleCli("sendtoaddressix", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\" 0.1") + HelpExampleCli("sendtoaddressix", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\" 0.1 \"donation\" \"seans outpost\"") + HelpExampleRpc("sendtoaddressix", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\", 0.1, \"donation\", \"seans outpost\""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    if (!IsValidDestinationString(params[0].get_str()))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Phore address");

    CTxDestination address = DecodeDestination(params[0].get_str());

    // Amount
    CAmount nAmount = AmountFromValue(params[1]);

    // Wallet comments
    CWalletTx wtx;
    if (params.size() > 2 && !params[2].isNull() && !params[2].get_str().empty())
        wtx.mapValue["comment"] = params[2].get_str();
    if (params.size() > 3 && !params[3].isNull() && !params[3].get_str().empty())
        wtx.mapValue["to"] = params[3].get_str();

    EnsureWalletIsUnlocked();

    SendMoney(address, nAmount, wtx, true);

    return wtx.GetHash().GetHex();
}

UniValue listaddressgroupings(const UniValue& params, bool fHelp)
{
    if (fHelp)
        throw runtime_error(
            "listaddressgroupings\n"
            "\nLists groups of addresses which have had their common ownership\n"
            "made public by common use as inputs or as the resulting change\n"
            "in past transactions\n"
            "\nResult:\n"
            "[\n"
            "  [\n"
            "    [\n"
            "      \"phoreaddress\",     (string) The phore address\n"
            "      amount,                 (numeric) The amount in phore\n"
            "      \"account\"             (string, optional) The account\n"
            "    ]\n"
            "    ,...\n"
            "  ]\n"
            "  ,...\n"
            "]\n"
            "\nExamples:\n" +
            HelpExampleCli("listaddressgroupings", "") + HelpExampleRpc("listaddressgroupings", ""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    UniValue jsonGroupings(UniValue::VARR);
    map<CTxDestination, CAmount> balances = pwalletMain->GetAddressBalances();
    BOOST_FOREACH (set<CTxDestination> grouping, pwalletMain->GetAddressGroupings()) {
        UniValue jsonGrouping(UniValue::VARR);
        BOOST_FOREACH (CTxDestination address, grouping) {
            UniValue addressInfo(UniValue::VARR);
            addressInfo.push_back(EncodeDestination(address));
            addressInfo.push_back(ValueFromAmount(balances[address]));
            {
                if (pwalletMain->mapAddressBook.find(address) != pwalletMain->mapAddressBook.end())
                    addressInfo.push_back(pwalletMain->mapAddressBook.find(address)->second.name);
            }
            jsonGrouping.push_back(addressInfo);
        }
        jsonGroupings.push_back(jsonGrouping);
    }
    return jsonGroupings;
}

UniValue signmessage(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 2)
        throw runtime_error(
            "signmessage \"phoreaddress\" \"message\"\n"
            "\nSign a message with the private key of an address" +
            HelpRequiringPassphrase() + "\n"
                                        "\nArguments:\n"
                                        "1. \"phoreaddress\"  (string, required) The phore address to use for the private key.\n"
                                        "2. \"message\"         (string, required) The message to create a signature of.\n"
                                        "\nResult:\n"
                                        "\"signature\"          (string) The signature of the message encoded in base 64\n"
                                        "\nExamples:\n"
                                        "\nUnlock the wallet for 30 seconds\n" +
            HelpExampleCli("walletpassphrase", "\"mypassphrase\" 30") +
            "\nCreate the signature\n" + HelpExampleCli("signmessage", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\" \"my message\"") +
            "\nVerify the signature\n" + HelpExampleCli("verifymessage", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\" \"signature\" \"my message\"") +
            "\nAs json rpc\n" + HelpExampleRpc("signmessage", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\", \"my message\""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    EnsureWalletIsUnlocked();

    string strAddress = params[0].get_str();
    string strMessage = params[1].get_str();

    if (!IsValidDestinationString(strAddress))
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");
    
    CTxDestination addr = DecodeDestination(strAddress);

    CKeyID *keyID = boost::get<CKeyID>(&addr);
    if (!keyID)
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");

    CKey key;
    if (!pwalletMain->GetKey(*keyID, key))
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");

    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    vector<unsigned char> vchSig;
    if (!key.SignCompact(ss.GetHash(), vchSig))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");

    return EncodeBase64(&vchSig[0], vchSig.size());
}

UniValue getreceivedbyaddress(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "getreceivedbyaddress \"phoreaddress\" ( minconf )\n"
            "\nReturns the total amount received by the given phoreaddress in transactions with at least minconf confirmations.\n"
            "\nArguments:\n"
            "1. \"phoreaddress\"  (string, required) The phore address for transactions.\n"
            "2. minconf             (numeric, optional, default=1) Only include transactions confirmed at least this many times.\n"
            "\nResult:\n"
            "amount   (numeric) The total amount in phore received at this address.\n"
            "\nExamples:\n"
            "\nThe amount from transactions with at least 1 confirmation\n" +
            HelpExampleCli("getreceivedbyaddress", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\"") +
            "\nThe amount including unconfirmed transactions, zero confirmations\n" + HelpExampleCli("getreceivedbyaddress", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\" 0") +
            "\nThe amount with at least 6 confirmation, very safe\n" + HelpExampleCli("getreceivedbyaddress", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\" 6") +
            "\nAs a json rpc call\n" + HelpExampleRpc("getreceivedbyaddress", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\", 6"));

    LOCK2(cs_main, pwalletMain->cs_wallet);
    
    // phore address
    if (!IsValidDestinationString(params[0].get_str()))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Phore address");

    CTxDestination address = DecodeDestination(params[0].get_str());
    CScript scriptPubKey = GetScriptForDestination(address);
    if (!IsMine(*pwalletMain, scriptPubKey))
        return (double)0.0;

    // Minimum confirmations
    int nMinDepth = 1;
    if (params.size() > 1)
        nMinDepth = params[1].get_int();

    // Tally
    CAmount nAmount = 0;
    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it) {
        const CWalletTx& wtx = (*it).second;
        if (wtx.IsCoinBase() || !IsFinalTx(wtx))
            continue;

        BOOST_FOREACH (const CTxOut& txout, wtx.vout)
            if (txout.scriptPubKey == scriptPubKey)
                if (wtx.GetDepthInMainChain() >= nMinDepth)
                    nAmount += txout.nValue;
    }

    return ValueFromAmount(nAmount);
}


UniValue getreceivedbyaccount(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "getreceivedbyaccount \"account\" ( minconf )\n"
            "\nReturns the total amount received by addresses with <account> in transactions with at least [minconf] confirmations.\n"
            "\nArguments:\n"
            "1. \"account\"      (string, required) The selected account, may be the default account using \"\".\n"
            "2. minconf          (numeric, optional, default=1) Only include transactions confirmed at least this many times.\n"
            "\nResult:\n"
            "amount              (numeric) The total amount in phore received for this account.\n"
            "\nExamples:\n"
            "\nAmount received by the default account with at least 1 confirmation\n" +
            HelpExampleCli("getreceivedbyaccount", "\"\"") +
            "\nAmount received at the tabby account including unconfirmed amounts with zero confirmations\n" + HelpExampleCli("getreceivedbyaccount", "\"tabby\" 0") +
            "\nThe amount with at least 6 confirmation, very safe\n" + HelpExampleCli("getreceivedbyaccount", "\"tabby\" 6") +
            "\nAs a json rpc call\n" + HelpExampleRpc("getreceivedbyaccount", "\"tabby\", 6"));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    // Minimum confirmations
    int nMinDepth = 1;
    if (params.size() > 1)
        nMinDepth = params[1].get_int();

    // Get the set of pub keys assigned to account
    string strAccount = AccountFromValue(params[0]);
    set<CTxDestination> setAddress = pwalletMain->GetAccountAddresses(strAccount);

    // Tally
    CAmount nAmount = 0;
    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it) {
        const CWalletTx& wtx = (*it).second;
        if (wtx.IsCoinBase() || !IsFinalTx(wtx))
            continue;

        BOOST_FOREACH (const CTxOut& txout, wtx.vout) {
            CTxDestination address;
            if (ExtractDestination(txout.scriptPubKey, address) && IsMine(*pwalletMain, address) && setAddress.count(address))
                if (wtx.GetDepthInMainChain() >= nMinDepth)
                    nAmount += txout.nValue;
        }
    }

    return (double)nAmount / (double)COIN;
}


CAmount GetAccountBalance(CWalletDB& walletdb, const string& strAccount, int nMinDepth, const isminefilter& filter)
{
    CAmount nBalance = 0;

    // Tally wallet transactions
    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it) {
        const CWalletTx& wtx = (*it).second;
        if (!IsFinalTx(wtx) || wtx.GetBlocksToMaturity() > 0 || wtx.GetDepthInMainChain() < 0)
            continue;

        CAmount nReceived, nSent, nFee;
        wtx.GetAccountAmounts(strAccount, nReceived, nSent, nFee, filter);

        if (nReceived != 0 && wtx.GetDepthInMainChain() >= nMinDepth)
            nBalance += nReceived;
        nBalance -= nSent + nFee;
    }

    // Tally internal accounting entries
    nBalance += walletdb.GetAccountCreditDebit(strAccount);

    return nBalance;
}

CAmount GetAccountBalance(const string& strAccount, int nMinDepth, const isminefilter& filter)
{
    CWalletDB walletdb(pwalletMain->strWalletFile);
    return GetAccountBalance(walletdb, strAccount, nMinDepth, filter);
}


UniValue getbalance(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 3)
        throw runtime_error(
            "getbalance ( \"account\" minconf includeWatchonly )\n"
            "\nIf account is not specified, returns the server's total available balance (excluding zerocoins).\n"
            "If account is specified, returns the balance in the account.\n"
            "Note that the account \"\" is not the same as leaving the parameter out.\n"
            "The server total may be different to the balance in the default \"\" account.\n"
            "\nArguments:\n"
            "1. \"account\"      (string, optional) The selected account, or \"*\" for entire wallet. It may be the default account using \"\".\n"
            "2. minconf          (numeric, optional, default=1) Only include transactions confirmed at least this many times.\n"
            "3. includeWatchonly (bool, optional, default=false) Also include balance in watchonly addresses (see 'importaddress')\n"
            "\nResult:\n"
            "amount              (numeric) The total amount in phore received for this account.\n"
            "\nExamples:\n"
            "\nThe total amount in the server across all accounts\n" +
            HelpExampleCli("getbalance", "") +
            "\nThe total amount in the server across all accounts, with at least 5 confirmations\n" + HelpExampleCli("getbalance", "\"*\" 6") +
            "\nThe total amount in the default account with at least 1 confirmation\n" + HelpExampleCli("getbalance", "\"\"") +
            "\nThe total amount in the account named tabby with at least 6 confirmations\n" + HelpExampleCli("getbalance", "\"tabby\" 6") +
            "\nAs a json rpc call\n" + HelpExampleRpc("getbalance", "\"tabby\", 6"));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    if (params.size() == 0)
        return ValueFromAmount(pwalletMain->GetBalance());

    int nMinDepth = 1;
    if (params.size() > 1)
        nMinDepth = params[1].get_int();
    isminefilter filter = ISMINE_SPENDABLE;
    if (params.size() > 2)
        if (params[2].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    if (params[0].get_str() == "*") {
        // Calculate total balance a different way from GetBalance()
        // (GetBalance() sums up all unspent TxOuts)
        // getbalance and "getbalance * 1 true" should return the same number
        CAmount nBalance = 0;
        for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it) {
            const CWalletTx& wtx = (*it).second;
            if (!IsFinalTx(wtx) || wtx.GetBlocksToMaturity() > 0 || wtx.GetDepthInMainChain() < 0)
                continue;

            CAmount allFee;
            string strSentAccount;
            list<COutputEntry> listReceived;
            list<COutputEntry> listSent;
            wtx.GetAmounts(listReceived, listSent, allFee, strSentAccount, filter);
            if (wtx.GetDepthInMainChain() >= nMinDepth) {
                BOOST_FOREACH (const COutputEntry& r, listReceived)
                    nBalance += r.amount;
            }
            BOOST_FOREACH (const COutputEntry& s, listSent)
                nBalance -= s.amount;
            nBalance -= allFee;
        }
        return ValueFromAmount(nBalance);
    }

    string strAccount = AccountFromValue(params[0]);

    CAmount nBalance = GetAccountBalance(strAccount, nMinDepth, filter);

    return ValueFromAmount(nBalance);
}

UniValue getunconfirmedbalance(const UniValue &params, bool fHelp)
{
    if (fHelp || params.size() > 0)
        throw runtime_error(
            "getunconfirmedbalance\n"
            "Returns the server's total unconfirmed balance\n");

    LOCK2(cs_main, pwalletMain->cs_wallet);

    return ValueFromAmount(pwalletMain->GetUnconfirmedBalance());
}


UniValue movecmd(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 3 || params.size() > 5)
        throw std::runtime_error(
            "move \"fromaccount\" \"toaccount\" amount ( minconf \"comment\" )\n"
            "\nMove a specified amount from one account in your wallet to another.\n"
            "\nArguments:\n"
            "1. \"fromaccount\"   (string, required) The name of the account to move funds from. May be the default account using \"\".\n"
            "2. \"toaccount\"     (string, required) The name of the account to move funds to. May be the default account using \"\".\n"
            "3. amount            (numeric, required) Quantity of PIV to move between accounts.\n"
            "4. minconf           (numeric, optional, default=1) Only use funds with at least this many confirmations.\n"
            "5. \"comment\"       (string, optional) An optional comment, stored in the wallet only.\n"

            "\nResult:\n"
            "true|false           (boolean) true if successful.\n"

            "\nExamples:\n"
            "\nMove 0.01 phore from the default account to the account named tabby\n" +
            HelpExampleCli("move", "\"\" \"tabby\" 0.01") +
            "\nMove 0.01 phore from timotei to akiko with a comment\n" +
            HelpExampleCli("move", "\"timotei\" \"akiko\" 0.01 1 \"happy birthday!\"") +
            "\nAs a json rpc call\n" +
            HelpExampleRpc("move", "\"timotei\", \"akiko\", 0.01, 1, \"happy birthday!\""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    std::string strFrom = AccountFromValue(params[0]);
    std::string strTo = AccountFromValue(params[1]);
    CAmount nAmount = AmountFromValue(params[2]);
    if (params.size() > 3)
        // unused parameter, used to be nMinDepth, keep type-checking it though
        (void)params[3].get_int();
    std::string strComment;
    if (params.size() > 4)
        strComment = params[4].get_str();

    CWalletDB walletdb(pwalletMain->strWalletFile);
    if (!walletdb.TxnBegin())
        throw JSONRPCError(RPC_DATABASE_ERROR, "database error");

    int64_t nNow = GetAdjustedTime();

    // Debit
    CAccountingEntry debit;
    debit.nOrderPos = pwalletMain->IncOrderPosNext(&walletdb);
    debit.strAccount = strFrom;
    debit.nCreditDebit = -nAmount;
    debit.nTime = nNow;
    debit.strOtherAccount = strTo;
    debit.strComment = strComment;
    pwalletMain->AddAccountingEntry(debit, walletdb);

    // Credit
    CAccountingEntry credit;
    credit.nOrderPos = pwalletMain->IncOrderPosNext(&walletdb);
    credit.strAccount = strTo;
    credit.nCreditDebit = nAmount;
    credit.nTime = nNow;
    credit.strOtherAccount = strFrom;
    credit.strComment = strComment;
    pwalletMain->AddAccountingEntry(credit, walletdb);

    if (!walletdb.TxnCommit())
        throw JSONRPCError(RPC_DATABASE_ERROR, "database error");

    return true;
}


UniValue sendfrom(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 3 || params.size() > 6)
        throw runtime_error(
            "sendfrom \"fromaccount\" \"tophoreaddress\" amount ( minconf \"comment\" \"comment-to\" )\n"
            "\nSent an amount from an account to a phore address.\n"
            "The amount is a real and is rounded to the nearest 0.00000001." +
            HelpRequiringPassphrase() + "\n"
                                        "\nArguments:\n"
                                        "1. \"fromaccount\"       (string, required) The name of the account to send funds from. May be the default account using \"\".\n"
                                        "2. \"tophoreaddress\"  (string, required) The phore address to send funds to.\n"
                                        "3. amount                (numeric, required) The amount in phore. (transaction fee is added on top).\n"
                                        "4. minconf               (numeric, optional, default=1) Only use funds with at least this many confirmations.\n"
                                        "5. \"comment\"           (string, optional) A comment used to store what the transaction is for. \n"
                                        "                                     This is not part of the transaction, just kept in your wallet.\n"
                                        "6. \"comment-to\"        (string, optional) An optional comment to store the name of the person or organization \n"
                                        "                                     to which you're sending the transaction. This is not part of the transaction, \n"
                                        "                                     it is just kept in your wallet.\n"
                                        "\nResult:\n"
                                        "\"transactionid\"        (string) The transaction id.\n"
                                        "\nExamples:\n"
                                        "\nSend 0.01 phore from the default account to the address, must have at least 1 confirmation\n" +
            HelpExampleCli("sendfrom", "\"\" \"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\" 0.01") +
            "\nSend 0.01 from the tabby account to the given address, funds must have at least 6 confirmations\n" + HelpExampleCli("sendfrom", "\"tabby\" \"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\" 0.01 6 \"donation\" \"seans outpost\"") +
            "\nAs a json rpc call\n" + HelpExampleRpc("sendfrom", "\"tabby\", \"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\", 0.01, 6, \"donation\", \"seans outpost\""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    string strAccount = AccountFromValue(params[0]);
    if (!IsValidDestinationString(params[1].get_str()))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Phore address");

    CTxDestination address = DecodeDestination(params[1].get_str());
    CAmount nAmount = AmountFromValue(params[2]);
    int nMinDepth = 1;
    if (params.size() > 3)
        nMinDepth = params[3].get_int();

    CWalletTx wtx;
    wtx.strFromAccount = strAccount;
    if (params.size() > 4 && !params[4].isNull() && !params[4].get_str().empty())
        wtx.mapValue["comment"] = params[4].get_str();
    if (params.size() > 5 && !params[5].isNull() && !params[5].get_str().empty())
        wtx.mapValue["to"] = params[5].get_str();

    EnsureWalletIsUnlocked();

    // Check funds
    CAmount nBalance = GetAccountBalance(strAccount, nMinDepth, ISMINE_SPENDABLE);
    if (nAmount > nBalance)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Account has insufficient funds");

    SendMoney(address, nAmount, wtx);

    return wtx.GetHash().GetHex();
}


UniValue sendmany(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 4)
        throw runtime_error(
            "sendmany \"fromaccount\" {\"address\":amount,...} ( minconf \"comment\" )\n"
            "\nSend multiple times. Amounts are double-precision floating point numbers." +
            HelpRequiringPassphrase() + "\n"
                                        "\nArguments:\n"
                                        "1. \"fromaccount\"         (string, required) The account to send the funds from, can be \"\" for the default account\n"
                                        "2. \"amounts\"             (string, required) A json object with addresses and amounts\n"
                                        "    {\n"
                                        "      \"address\":amount   (numeric) The phore address is the key, the numeric amount in phore is the value\n"
                                        "      ,...\n"
                                        "    }\n"
                                        "3. minconf                 (numeric, optional, default=1) Only use the balance confirmed at least this many times.\n"
                                        "4. \"comment\"             (string, optional) A comment\n"
                                        "\nResult:\n"
                                        "\"transactionid\"          (string) The transaction id for the send. Only 1 transaction is created regardless of \n"
                                        "                                    the number of addresses.\n"
                                        "\nExamples:\n"
                                        "\nSend two amounts to two different addresses:\n" +
            HelpExampleCli("sendmany", "\"tabby\" \"{\\\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\\\":0.01,\\\"XuQQkwA4FYkq2XERzMY2CiAZhJTEDAbtcg\\\":0.02}\"") +
            "\nSend two amounts to two different addresses setting the confirmation and comment:\n" + HelpExampleCli("sendmany", "\"tabby\" \"{\\\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\\\":0.01,\\\"XuQQkwA4FYkq2XERzMY2CiAZhJTEDAbtcg\\\":0.02}\" 6 \"testing\"") +
            "\nAs a json rpc call\n" + HelpExampleRpc("sendmany", "\"tabby\", \"{\\\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\\\":0.01,\\\"XuQQkwA4FYkq2XERzMY2CiAZhJTEDAbtcg\\\":0.02}\", 6, \"testing\""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    string strAccount = AccountFromValue(params[0]);
    UniValue sendTo = params[1].get_obj();
    int nMinDepth = 1;
    if (params.size() > 2)
        nMinDepth = params[2].get_int();

    CWalletTx wtx;
    wtx.strFromAccount = strAccount;
    if (params.size() > 3 && !params[3].isNull() && !params[3].get_str().empty())
        wtx.mapValue["comment"] = params[3].get_str();

    set<CTxDestination> setAddress;
    vector<pair<CScript, CAmount> > vecSend;

    CAmount totalAmount = 0;
    vector<string> keys = sendTo.getKeys();
    BOOST_FOREACH(const string& name_, keys) {
        if (!IsValidDestinationString(name_))
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, string("Invalid Phore address: ")+name_);
        
        CTxDestination address = DecodeDestination(name_);

        if (setAddress.count(address))
            throw JSONRPCError(RPC_INVALID_PARAMETER, string("Invalid parameter, duplicated address: ")+name_);
        setAddress.insert(address);

        CScript scriptPubKey = GetScriptForDestination(address);
        CAmount nAmount = AmountFromValue(sendTo[name_]);
        totalAmount += nAmount;

        vecSend.push_back(make_pair(scriptPubKey, nAmount));
    }

    EnsureWalletIsUnlocked();

    // Check funds
    CAmount nBalance = GetAccountBalance(strAccount, nMinDepth, ISMINE_SPENDABLE);
    if (totalAmount > nBalance)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Account has insufficient funds");

    // Send
    CReserveKey keyChange(pwalletMain);
    CAmount nFeeRequired = 0;
    string strFailReason;
    bool fCreated = pwalletMain->CreateTransaction(vecSend, wtx, keyChange, nFeeRequired, strFailReason);
    if (!fCreated)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, strFailReason);
    if (!pwalletMain->CommitTransaction(wtx, keyChange))
        throw JSONRPCError(RPC_WALLET_ERROR, "Transaction commit failed");

    return wtx.GetHash().GetHex();
}

// Defined in rpc/misc.cpp
extern CScript _createmultisig_redeemScript(const UniValue& params);

UniValue addmultisigaddress(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 3) {
        string msg = "addmultisigaddress nrequired [\"key\",...] ( \"account\" )\n"
                     "\nAdd a nrequired-to-sign multisignature address to the wallet.\n"
                     "Each key is a Phore address or hex-encoded public key.\n"
                     "If 'account' is specified, assign address to that account.\n"

                     "\nArguments:\n"
                     "1. nrequired        (numeric, required) The number of required signatures out of the n keys or addresses.\n"
                     "2. \"keysobject\"   (string, required) A json array of phore addresses or hex-encoded public keys\n"
                     "     [\n"
                     "       \"address\"  (string) phore address or hex-encoded public key\n"
                     "       ...,\n"
                     "     ]\n"
                     "3. \"account\"      (string, optional) An account to assign the addresses to.\n"

                     "\nResult:\n"
                     "\"phoreaddress\"  (string) A phore address associated with the keys.\n"

                     "\nExamples:\n"
                     "\nAdd a multisig address from 2 addresses\n" +
                     HelpExampleCli("addmultisigaddress", "2 \"[\\\"Xt4qk9uKvQYAonVGSZNXqxeDmtjaEWgfrs\\\",\\\"XoSoWQkpgLpppPoyyzbUFh1fq2RBvW6UK1\\\"]\"") +
                     "\nAs json rpc call\n" + HelpExampleRpc("addmultisigaddress", "2, \"[\\\"Xt4qk9uKvQYAonVGSZNXqxeDmtjaEWgfrs\\\",\\\"XoSoWQkpgLpppPoyyzbUFh1fq2RBvW6UK1\\\"]\"");
        throw runtime_error(msg);
    }

    LOCK2(cs_main, pwalletMain->cs_wallet);

    string strAccount;
    if (params.size() > 2)
        strAccount = AccountFromValue(params[2]);

    // Construct using pay-to-script-hash:
    CScript inner = _createmultisig_redeemScript(params);
    pwalletMain->AddCScript(inner);

    CTxDestination dest = pwalletMain->AddAndGetDestinationForScript(inner, g_address_type);

    pwalletMain->SetAddressBook(dest, strAccount, "send");
    return EncodeDestination(dest);
}

class Witnessifier : public boost::static_visitor<bool>
{
public:
    CTxDestination result;
    bool already_witness;

    explicit Witnessifier() : already_witness(false) {}

    bool operator()(const CKeyID &keyID) {
        CPubKey pubkey;
        if (pwalletMain) {
            CScript basescript = GetScriptForDestination(keyID);
            isminetype typ;
            typ = IsMine(*pwalletMain, basescript, SIGVERSION_WITNESS_V0);
            if (typ != ISMINE_SPENDABLE)
                return false;
            CScript witscript = GetScriptForWitness(basescript);
            return ExtractDestination(witscript, result);
        }
        return false;
    }

    bool operator()(const CScriptID &scriptID) {
        CScript subscript;
        if (pwalletMain && pwalletMain->GetCScript(scriptID, subscript)) {
            int witnessversion;
            std::vector<unsigned char> witprog;
            if (subscript.IsWitnessProgram(witnessversion, witprog)) {
                ExtractDestination(subscript, result);
                already_witness = true;
                return true;
            }
            isminetype typ;
            typ = IsMine(*pwalletMain, subscript, SIGVERSION_WITNESS_V0);
            if (typ != ISMINE_SPENDABLE)
                return false;
            CScript witscript = GetScriptForWitness(subscript);
            return ExtractDestination(witscript, result);
        }
        return false;
    }

    bool operator()(const WitnessV0KeyHash& id) {
        already_witness = true;
        result = id;
        return true;
    }

    bool operator()(const WitnessV0ScriptHash& id) {
        already_witness = true;
        result = id;
        return true;
    }


    template<typename T>
    bool operator()(const T& dest) { return false; }
};

UniValue addwitnessaddress(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
    {
        string msg = "addwitnessaddress \"address\" ( p2sh )\n"
            "\nAdd a witness address for a script (with pubkey or redeemscript known).\n"
            "It returns the witness script.\n"

            "\nArguments:\n"
            "1. \"address\"       (string, required) An address known to the wallet\n"
            "2. p2sh            (bool, optional, default=true) Embed inside P2SH\n"

            "\nResult:\n"
            "\"witnessaddress\",  (string) The value of the new address (P2SH or BIP173).\n"
            "}\n"
        ;
        throw runtime_error(msg);
    }

    if (!IsSporkActive(SPORK_17_SEGWIT_ACTIVATION) && !GetBoolArg("-walletprematurewitness", false)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Segregated witness not enabled on network");
    }

    CTxDestination dest = DecodeDestination(params[0].get_str());
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Bitcoin address");
    }

    bool p2sh = true;
    if (!params[1].isNull()) {
        p2sh = params[1].get_bool();
    }

    Witnessifier w;
    bool ret = boost::apply_visitor(w, dest);
    if (!ret) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Public key or redeemscript not known to wallet, or the key is uncompressed");
    }

    CScript witprogram = GetScriptForDestination(w.result);

    if (p2sh) {
        w.result = CScriptID(witprogram);
    }

    if (w.already_witness) {
        if (!(dest == w.result)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Cannot convert between witness address types");
        }
    } else {
        pwalletMain->AddCScript(witprogram); // Implicit for single-key now, but necessary for multisig and for compatibility with older software
        pwalletMain->SetAddressBook(w.result, "", "receive");
    }

    return EncodeDestination(w.result);
}

struct tallyitem {
    CAmount nAmount;
    int nConf;
    int nBCConf;
    vector<uint256> txids;
    bool fIsWatchonly;
    tallyitem()
    {
        nAmount = 0;
        nConf = std::numeric_limits<int>::max();
        nBCConf = std::numeric_limits<int>::max();
        fIsWatchonly = false;
    }
};

UniValue ListReceived(const UniValue& params, bool fByAccounts)
{
    // Minimum confirmations
    int nMinDepth = 1;
    if (params.size() > 0)
        nMinDepth = params[0].get_int();

    // Whether to include empty accounts
    bool fIncludeEmpty = false;
    if (params.size() > 1)
        fIncludeEmpty = params[1].get_bool();

    isminefilter filter = ISMINE_SPENDABLE;
    if (params.size() > 2)
        if (params[2].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    // Tally
    map<CTxDestination, tallyitem> mapTally;
    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it) {
        const CWalletTx& wtx = (*it).second;

        if (wtx.IsCoinBase() || !IsFinalTx(wtx))
            continue;

        int nDepth = wtx.GetDepthInMainChain();
        int nBCDepth = wtx.GetDepthInMainChain(false);
        if (nDepth < nMinDepth)
            continue;

        BOOST_FOREACH (const CTxOut& txout, wtx.vout) {
            CTxDestination address;
            if (!ExtractDestination(txout.scriptPubKey, address))
                continue;

            isminefilter mine = IsMine(*pwalletMain, address);
            if (!(mine & filter))
                continue;

            tallyitem& item = mapTally[address];
            item.nAmount += txout.nValue;
            item.nConf = min(item.nConf, nDepth);
            item.nBCConf = min(item.nBCConf, nBCDepth);
            item.txids.push_back(wtx.GetHash());
            if (mine & ISMINE_WATCH_ONLY)
                item.fIsWatchonly = true;
        }
    }

    // Reply
    UniValue ret(UniValue::VARR);
    map<string, tallyitem> mapAccountTally;
    BOOST_FOREACH (const PAIRTYPE(CTxDestination, CAddressBookData) & item, pwalletMain->mapAddressBook) {
        const CTxDestination& address = item.first;
        const string& strAccount = item.second.name;
        map<CTxDestination, tallyitem>::iterator it = mapTally.find(address);
        if (it == mapTally.end() && !fIncludeEmpty)
            continue;

        CAmount nAmount = 0;
        int nConf = std::numeric_limits<int>::max();
        int nBCConf = std::numeric_limits<int>::max();
        bool fIsWatchonly = false;
        if (it != mapTally.end()) {
            nAmount = (*it).second.nAmount;
            nConf = (*it).second.nConf;
            nBCConf = (*it).second.nBCConf;
            fIsWatchonly = (*it).second.fIsWatchonly;
        }

        if (fByAccounts) {
            tallyitem& item = mapAccountTally[strAccount];
            item.nAmount += nAmount;
            item.nConf = min(item.nConf, nConf);
            item.nBCConf = min(item.nBCConf, nBCConf);
            item.fIsWatchonly = fIsWatchonly;
        } else {
            UniValue obj(UniValue::VOBJ);
            if (fIsWatchonly)
                obj.push_back(Pair("involvesWatchonly", true));
            obj.push_back(Pair("address", EncodeDestination(address)));
            obj.push_back(Pair("account", strAccount));
            obj.push_back(Pair("amount", ValueFromAmount(nAmount)));
            obj.push_back(Pair("confirmations", (nConf == std::numeric_limits<int>::max() ? 0 : nConf)));
            obj.push_back(Pair("bcconfirmations", (nBCConf == std::numeric_limits<int>::max() ? 0 : nBCConf)));
            UniValue transactions(UniValue::VARR);
            if (it != mapTally.end()) {
                BOOST_FOREACH (const uint256& item, (*it).second.txids) {
                    transactions.push_back(item.GetHex());
                }
            }
            obj.push_back(Pair("txids", transactions));
            ret.push_back(obj);
        }
    }

    if (fByAccounts) {
        for (map<string, tallyitem>::iterator it = mapAccountTally.begin(); it != mapAccountTally.end(); ++it) {
            CAmount nAmount = (*it).second.nAmount;
            int nConf = (*it).second.nConf;
            int nBCConf = (*it).second.nBCConf;
            UniValue obj(UniValue::VOBJ);
            if ((*it).second.fIsWatchonly)
                obj.push_back(Pair("involvesWatchonly", true));
            obj.push_back(Pair("account", (*it).first));
            obj.push_back(Pair("amount", ValueFromAmount(nAmount)));
            obj.push_back(Pair("confirmations", (nConf == std::numeric_limits<int>::max() ? 0 : nConf)));
            obj.push_back(Pair("bcconfirmations", (nBCConf == std::numeric_limits<int>::max() ? 0 : nBCConf)));
            ret.push_back(obj);
        }
    }

    return ret;
}

UniValue listreceivedbyaddress(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 3)
        throw runtime_error(
            "listreceivedbyaddress ( minconf includeempty includeWatchonly)\n"
            "\nList balances by receiving address.\n"
            "\nArguments:\n"
            "1. minconf       (numeric, optional, default=1) The minimum number of confirmations before payments are included.\n"
            "2. includeempty  (numeric, optional, default=false) Whether to include addresses that haven't received any payments.\n"
            "3. includeWatchonly (bool, optional, default=false) Whether to include watchonly addresses (see 'importaddress').\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"involvesWatchonly\" : \"true\",    (bool) Only returned if imported addresses were involved in transaction\n"
            "    \"address\" : \"receivingaddress\",  (string) The receiving address\n"
            "    \"account\" : \"accountname\",       (string) The account of the receiving address. The default account is \"\".\n"
            "    \"amount\" : x.xxx,                  (numeric) The total amount in phore received by the address\n"
            "    \"confirmations\" : n                (numeric) The number of confirmations of the most recent transaction included\n"
            "    \"bcconfirmations\" : n              (numeric) The number of blockchain confirmations of the most recent transaction included\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("listreceivedbyaddress", "") + HelpExampleCli("listreceivedbyaddress", "6 true") + HelpExampleRpc("listreceivedbyaddress", "6, true, true"));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    return ListReceived(params, false);
}

UniValue listreceivedbyaccount(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 3)
        throw runtime_error(
            "listreceivedbyaccount ( minconf includeempty includeWatchonly)\n"
            "\nList balances by account.\n"
            "\nArguments:\n"
            "1. minconf      (numeric, optional, default=1) The minimum number of confirmations before payments are included.\n"
            "2. includeempty (boolean, optional, default=false) Whether to include accounts that haven't received any payments.\n"
            "3. includeWatchonly (bool, optional, default=false) Whether to include watchonly addresses (see 'importaddress').\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"involvesWatchonly\" : \"true\",    (bool) Only returned if imported addresses were involved in transaction\n"
            "    \"account\" : \"accountname\",  (string) The account name of the receiving account\n"
            "    \"amount\" : x.xxx,             (numeric) The total amount received by addresses with this account\n"
            "    \"confirmations\" : n           (numeric) The number of confirmations of the most recent transaction included\n"
            "    \"bcconfirmations\" : n         (numeric) The number of blockchain confirmations of the most recent transaction included\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("listreceivedbyaccount", "") + HelpExampleCli("listreceivedbyaccount", "6 true") + HelpExampleRpc("listreceivedbyaccount", "6, true, true"));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    return ListReceived(params, true);
}

static void MaybePushAddress(UniValue & entry, const CTxDestination &dest)
{
    if (dest.which() != 0)
        entry.push_back(Pair("address", EncodeDestination(dest)));
}

void ListTransactions(const CWalletTx& wtx, const string& strAccount, int nMinDepth, bool fLong, UniValue& ret, const isminefilter& filter)
{
    CAmount nFee;
    string strSentAccount;
    list<COutputEntry> listReceived;
    list<COutputEntry> listSent;

    wtx.GetAmounts(listReceived, listSent, nFee, strSentAccount, filter);

    bool fAllAccounts = (strAccount == string("*"));
    bool involvesWatchonly = wtx.IsFromMe(ISMINE_WATCH_ONLY);

    // Sent
    if ((!listSent.empty() || nFee != 0) && (fAllAccounts || strAccount == strSentAccount)) {
        BOOST_FOREACH (const COutputEntry& s, listSent) {
            UniValue entry(UniValue::VOBJ);
            if (involvesWatchonly || (::IsMine(*pwalletMain, s.destination) & ISMINE_WATCH_ONLY))
                entry.push_back(Pair("involvesWatchonly", true));
            entry.push_back(Pair("account", strSentAccount));
            MaybePushAddress(entry, s.destination);
            std::map<std::string, std::string>::const_iterator it = wtx.mapValue.find("DS");
            entry.push_back(Pair("category", (it != wtx.mapValue.end() && it->second == "1") ? "darksent" : "send"));
            entry.push_back(Pair("amount", ValueFromAmount(-s.amount)));
            entry.push_back(Pair("vout", s.vout));
            entry.push_back(Pair("fee", ValueFromAmount(-nFee)));
            if (fLong)
                WalletTxToJSON(wtx, entry);
            ret.push_back(entry);
        }
    }

    // Received
    if (listReceived.size() > 0 && wtx.GetDepthInMainChain() >= nMinDepth) {
        BOOST_FOREACH (const COutputEntry& r, listReceived) {
            string account;
            if (pwalletMain->mapAddressBook.count(r.destination))
                account = pwalletMain->mapAddressBook[r.destination].name;
            if (fAllAccounts || (account == strAccount)) {
                UniValue entry(UniValue::VOBJ);
                if (involvesWatchonly || (::IsMine(*pwalletMain, r.destination) & ISMINE_WATCH_ONLY))
                    entry.push_back(Pair("involvesWatchonly", true));
                entry.push_back(Pair("account", account));
                MaybePushAddress(entry, r.destination);
                if (wtx.IsCoinBase()) {
                    if (wtx.GetDepthInMainChain() < 1)
                        entry.push_back(Pair("category", "orphan"));
                    else if (wtx.GetBlocksToMaturity() > 0)
                        entry.push_back(Pair("category", "immature"));
                    else
                        entry.push_back(Pair("category", "generate"));
                } else {
                    entry.push_back(Pair("category", "receive"));
                }
                entry.push_back(Pair("amount", ValueFromAmount(r.amount)));
                entry.push_back(Pair("vout", r.vout));
                if (fLong)
                    WalletTxToJSON(wtx, entry);
                ret.push_back(entry);
            }
        }
    }
}

void AcentryToJSON(const CAccountingEntry& acentry, const string& strAccount, UniValue& ret)
{
    bool fAllAccounts = (strAccount == string("*"));

    if (fAllAccounts || acentry.strAccount == strAccount) {
        UniValue entry(UniValue::VOBJ);
        entry.push_back(Pair("account", acentry.strAccount));
        entry.push_back(Pair("category", "move"));
        entry.push_back(Pair("time", acentry.nTime));
        entry.push_back(Pair("amount", ValueFromAmount(acentry.nCreditDebit)));
        entry.push_back(Pair("otheraccount", acentry.strOtherAccount));
        entry.push_back(Pair("comment", acentry.strComment));
        ret.push_back(entry);
    }
}

UniValue listtransactions(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 4)
        throw runtime_error(
            "listtransactions ( \"account\" count from includeWatchonly)\n"
            "\nReturns up to 'count' most recent transactions skipping the first 'from' transactions for account 'account'.\n"
            "\nArguments:\n"
            "1. \"account\"    (string, optional) The account name. If not included, it will list all transactions for all accounts.\n"
            "                                     If \"\" is set, it will list transactions for the default account.\n"
            "2. count          (numeric, optional, default=10) The number of transactions to return\n"
            "3. from           (numeric, optional, default=0) The number of transactions to skip\n"
            "4. includeWatchonly (bool, optional, default=false) Include transactions to watchonly addresses (see 'importaddress')\n"
            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"account\":\"accountname\",       (string) The account name associated with the transaction. \n"
            "                                                It will be \"\" for the default account.\n"
            "    \"address\":\"phoreaddress\",    (string) The phore address of the transaction. Not present for \n"
            "                                                move transactions (category = move).\n"
            "    \"category\":\"send|receive|move\", (string) The transaction category. 'move' is a local (off blockchain)\n"
            "                                                transaction between accounts, and not associated with an address,\n"
            "                                                transaction id or block. 'send' and 'receive' transactions are \n"
            "                                                associated with an address, transaction id and block details\n"
            "    \"amount\": x.xxx,          (numeric) The amount in phore. This is negative for the 'send' category, and for the\n"
            "                                         'move' category for moves outbound. It is positive for the 'receive' category,\n"
            "                                         and for the 'move' category for inbound funds.\n"
            "    \"vout\" : n,               (numeric) the vout value\n"
            "    \"fee\": x.xxx,             (numeric) The amount of the fee in phore. This is negative and only available for the \n"
            "                                         'send' category of transactions.\n"
            "    \"confirmations\": n,       (numeric) The number of confirmations for the transaction. Available for 'send' and \n"
            "                                         'receive' category of transactions.\n"
            "    \"bcconfirmations\": n,     (numeric) The number of blockchain confirmations for the transaction. Available for 'send'\n"
            "                                          and 'receive' category of transactions.\n"
            "    \"blockhash\": \"hashvalue\", (string) The block hash containing the transaction. Available for 'send' and 'receive'\n"
            "                                          category of transactions.\n"
            "    \"blockindex\": n,          (numeric) The block index containing the transaction. Available for 'send' and 'receive'\n"
            "                                          category of transactions.\n"
            "    \"txid\": \"transactionid\", (string) The transaction id. Available for 'send' and 'receive' category of transactions.\n"
            "    \"time\": xxx,              (numeric) The transaction time in seconds since epoch (midnight Jan 1 1970 GMT).\n"
            "    \"timereceived\": xxx,      (numeric) The time received in seconds since epoch (midnight Jan 1 1970 GMT). Available \n"
            "                                          for 'send' and 'receive' category of transactions.\n"
            "    \"comment\": \"...\",       (string) If a comment is associated with the transaction.\n"
            "    \"otheraccount\": \"accountname\",  (string) For the 'move' category of transactions, the account the funds came \n"
            "                                          from (for receiving funds, positive amounts), or went to (for sending funds,\n"
            "                                          negative amounts).\n"
            "  }\n"
            "]\n"

            "\nExamples:\n"
            "\nList the most recent 10 transactions in the systems\n" +
            HelpExampleCli("listtransactions", "") +
            "\nList the most recent 10 transactions for the tabby account\n" + HelpExampleCli("listtransactions", "\"tabby\"") +
            "\nList transactions 100 to 120 from the tabby account\n" + HelpExampleCli("listtransactions", "\"tabby\" 20 100") +
            "\nAs a json rpc call\n" + HelpExampleRpc("listtransactions", "\"tabby\", 20, 100"));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    string strAccount = "*";
    if (params.size() > 0)
        strAccount = params[0].get_str();
    int nCount = 10;
    if (params.size() > 1)
        nCount = params[1].get_int();
    int nFrom = 0;
    if (params.size() > 2)
        nFrom = params[2].get_int();
    isminefilter filter = ISMINE_SPENDABLE;
    if (params.size() > 3)
        if (params[3].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    if (nCount < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative count");
    if (nFrom < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative from");

    UniValue ret(UniValue::VARR);

    const CWallet::TxItems & txOrdered = pwalletMain->wtxOrdered;

    // iterate backwards until we have nCount items to return:
    for (CWallet::TxItems::const_reverse_iterator it = txOrdered.rbegin(); it != txOrdered.rend(); ++it) {
        CWalletTx* const pwtx = (*it).second.first;
        if (pwtx != 0)
            ListTransactions(*pwtx, strAccount, 0, true, ret, filter);
        CAccountingEntry* const pacentry = (*it).second.second;
        if (pacentry != 0)
            AcentryToJSON(*pacentry, strAccount, ret);

        if ((int)ret.size() >= (nCount + nFrom)) break;
    }
    // ret is newest to oldest

    if (nFrom > (int)ret.size())
        nFrom = ret.size();
    if ((nFrom + nCount) > (int)ret.size())
        nCount = ret.size() - nFrom;

    vector<UniValue> arrTmp = ret.getValues();

    vector<UniValue>::iterator first = arrTmp.begin();
    std::advance(first, nFrom);
    vector<UniValue>::iterator last = arrTmp.begin();
    std::advance(last, nFrom+nCount);

    if (last != arrTmp.end()) arrTmp.erase(last, arrTmp.end());
    if (first != arrTmp.begin()) arrTmp.erase(arrTmp.begin(), first);

    std::reverse(arrTmp.begin(), arrTmp.end()); // Return oldest to newest

    ret.clear();
    ret.setArray();
    ret.push_backV(arrTmp);

    return ret;
}

UniValue listaccounts(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw runtime_error(
            "listaccounts ( minconf includeWatchonly)\n"
            "\nReturns Object that has account names as keys, account balances as values.\n"
            "\nArguments:\n"
            "1. minconf          (numeric, optional, default=1) Only include transactions with at least this many confirmations\n"
            "2. includeWatchonly (bool, optional, default=false) Include balances in watchonly addresses (see 'importaddress')\n"
            "\nResult:\n"
            "{                      (json object where keys are account names, and values are numeric balances\n"
            "  \"account\": x.xxx,  (numeric) The property name is the account name, and the value is the total balance for the account.\n"
            "  ...\n"
            "}\n"
            "\nExamples:\n"
            "\nList account balances where there at least 1 confirmation\n" +
            HelpExampleCli("listaccounts", "") +
            "\nList account balances including zero confirmation transactions\n" + HelpExampleCli("listaccounts", "0") +
            "\nList account balances for 6 or more confirmations\n" + HelpExampleCli("listaccounts", "6") +
            "\nAs json rpc call\n" + HelpExampleRpc("listaccounts", "6"));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    int nMinDepth = 1;
    if (params.size() > 0)
        nMinDepth = params[0].get_int();
    isminefilter includeWatchonly = ISMINE_SPENDABLE;
    if (params.size() > 1)
        if (params[1].get_bool())
            includeWatchonly = includeWatchonly | ISMINE_WATCH_ONLY;

    map<string, CAmount> mapAccountBalances;
    BOOST_FOREACH (const PAIRTYPE(CTxDestination, CAddressBookData) & entry, pwalletMain->mapAddressBook) {
        if (IsMine(*pwalletMain, entry.first) & includeWatchonly) // This address belongs to me
            mapAccountBalances[entry.second.name] = 0;
    }

    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it) {
        const CWalletTx& wtx = (*it).second;
        CAmount nFee;
        string strSentAccount;
        list<COutputEntry> listReceived;
        list<COutputEntry> listSent;
        int nDepth = wtx.GetDepthInMainChain();
        if (wtx.GetBlocksToMaturity() > 0 || nDepth < 0)
            continue;
        wtx.GetAmounts(listReceived, listSent, nFee, strSentAccount, includeWatchonly);
        mapAccountBalances[strSentAccount] -= nFee;
        BOOST_FOREACH (const COutputEntry& s, listSent)
            mapAccountBalances[strSentAccount] -= s.amount;
        if (nDepth >= nMinDepth) {
            BOOST_FOREACH (const COutputEntry& r, listReceived)
                if (pwalletMain->mapAddressBook.count(r.destination))
                    mapAccountBalances[pwalletMain->mapAddressBook[r.destination].name] += r.amount;
                else
                    mapAccountBalances[""] += r.amount;
        }
    }

    const list<CAccountingEntry> & acentries = pwalletMain->laccentries;
    BOOST_FOREACH (const CAccountingEntry& entry, acentries)
        mapAccountBalances[entry.strAccount] += entry.nCreditDebit;

    UniValue ret(UniValue::VOBJ);
    BOOST_FOREACH (const PAIRTYPE(string, CAmount) & accountBalance, mapAccountBalances) {
        ret.push_back(Pair(accountBalance.first, ValueFromAmount(accountBalance.second)));
    }
    return ret;
}

UniValue listsinceblock(const UniValue& params, bool fHelp)
{
    if (fHelp)
        throw runtime_error(
            "listsinceblock ( \"blockhash\" target-confirmations includeWatchonly)\n"
            "\nGet all transactions in blocks since block [blockhash], or all transactions if omitted\n"
            "\nArguments:\n"
            "1. \"blockhash\"   (string, optional) The block hash to list transactions since\n"
            "2. target-confirmations:    (numeric, optional) The confirmations required, must be 1 or more\n"
            "3. includeWatchonly:        (bool, optional, default=false) Include transactions to watchonly addresses (see 'importaddress')"
            "\nResult:\n"
            "{\n"
            "  \"transactions\": [\n"
            "    \"account\":\"accountname\",       (string) The account name associated with the transaction. Will be \"\" for the default account.\n"
            "    \"address\":\"phoreaddress\",    (string) The phore address of the transaction. Not present for move transactions (category = move).\n"
            "    \"category\":\"send|receive\",     (string) The transaction category. 'send' has negative amounts, 'receive' has positive amounts.\n"
            "    \"amount\": x.xxx,          (numeric) The amount in phore. This is negative for the 'send' category, and for the 'move' category for moves \n"
            "                                          outbound. It is positive for the 'receive' category, and for the 'move' category for inbound funds.\n"
            "    \"vout\" : n,               (numeric) the vout value\n"
            "    \"fee\": x.xxx,             (numeric) The amount of the fee in phore. This is negative and only available for the 'send' category of transactions.\n"
            "    \"confirmations\": n,       (numeric) The number of confirmations for the transaction. Available for 'send' and 'receive' category of transactions.\n"
            "    \"bcconfirmations\" : n,    (numeric) The number of blockchain confirmations for the transaction. Available for 'send' and 'receive' category of transactions.\n"
            "    \"blockhash\": \"hashvalue\",     (string) The block hash containing the transaction. Available for 'send' and 'receive' category of transactions.\n"
            "    \"blockindex\": n,          (numeric) The block index containing the transaction. Available for 'send' and 'receive' category of transactions.\n"
            "    \"blocktime\": xxx,         (numeric) The block time in seconds since epoch (1 Jan 1970 GMT).\n"
            "    \"txid\": \"transactionid\",  (string) The transaction id. Available for 'send' and 'receive' category of transactions.\n"
            "    \"time\": xxx,              (numeric) The transaction time in seconds since epoch (Jan 1 1970 GMT).\n"
            "    \"timereceived\": xxx,      (numeric) The time received in seconds since epoch (Jan 1 1970 GMT). Available for 'send' and 'receive' category of transactions.\n"
            "    \"comment\": \"...\",       (string) If a comment is associated with the transaction.\n"
            "    \"to\": \"...\",            (string) If a comment to is associated with the transaction.\n"
            "  ],\n"
            "  \"lastblock\": \"lastblockhash\"     (string) The hash of the last block\n"
            "}\n"
            "\nExamples:\n" +
            HelpExampleCli("listsinceblock", "") + HelpExampleCli("listsinceblock", "\"000000000000000bacf66f7497b7dc45ef753ee9a7d38571037cdb1a57f663ad\" 6") + HelpExampleRpc("listsinceblock", "\"000000000000000bacf66f7497b7dc45ef753ee9a7d38571037cdb1a57f663ad\", 6"));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    CBlockIndex* pindex = NULL;
    int target_confirms = 1;
    isminefilter filter = ISMINE_SPENDABLE;

    if (params.size() > 0) {
        uint256 blockId = 0;

        blockId.SetHex(params[0].get_str());
        BlockMap::iterator it = mapBlockIndex.find(blockId);
        if (it != mapBlockIndex.end())
            pindex = it->second;
    }

    if (params.size() > 1) {
        target_confirms = params[1].get_int();

        if (target_confirms < 1)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter");
    }

    if (params.size() > 2)
        if (params[2].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    int depth = pindex ? (1 + chainActive.Height() - pindex->nHeight) : -1;

    UniValue transactions(UniValue::VARR);

    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); it++) {
        CWalletTx tx = (*it).second;

        if (depth == -1 || tx.GetDepthInMainChain(false) < depth)
            ListTransactions(tx, "*", 0, true, transactions, filter);
    }

    CBlockIndex* pblockLast = chainActive[chainActive.Height() + 1 - target_confirms];
    uint256 lastblock = pblockLast ? pblockLast->GetBlockHash() : 0;

    UniValue ret(UniValue::VOBJ);
    ret.push_back(Pair("transactions", transactions));
    ret.push_back(Pair("lastblock", lastblock.GetHex()));

    return ret;
}

UniValue gettransaction(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "gettransaction \"txid\" ( includeWatchonly )\n"
            "\nGet detailed information about in-wallet transaction <txid>\n"
            "\nArguments:\n"
            "1. \"txid\"    (string, required) The transaction id\n"
            "2. \"includeWatchonly\"    (bool, optional, default=false) Whether to include watchonly addresses in balance calculation and details[]\n"
            "\nResult:\n"
            "{\n"
            "  \"amount\" : x.xxx,        (numeric) The transaction amount in phore\n"
            "  \"confirmations\" : n,     (numeric) The number of confirmations\n"
            "  \"bcconfirmations\" : n,   (numeric) The number of blockchain confirmations\n"
            "  \"blockhash\" : \"hash\",  (string) The block hash\n"
            "  \"blockindex\" : xx,       (numeric) The block index\n"
            "  \"blocktime\" : ttt,       (numeric) The time in seconds since epoch (1 Jan 1970 GMT)\n"
            "  \"txid\" : \"transactionid\",   (string) The transaction id.\n"
            "  \"time\" : ttt,            (numeric) The transaction time in seconds since epoch (1 Jan 1970 GMT)\n"
            "  \"timereceived\" : ttt,    (numeric) The time received in seconds since epoch (1 Jan 1970 GMT)\n"
            "  \"details\" : [\n"
            "    {\n"
            "      \"account\" : \"accountname\",  (string) The account name involved in the transaction, can be \"\" for the default account.\n"
            "      \"address\" : \"phoreaddress\",   (string) The phore address involved in the transaction\n"
            "      \"category\" : \"send|receive\",    (string) The category, either 'send' or 'receive'\n"
            "      \"amount\" : x.xxx                  (numeric) The amount in phore\n"
            "      \"vout\" : n,                       (numeric) the vout value\n"
            "    }\n"
            "    ,...\n"
            "  ],\n"
            "  \"hex\" : \"data\"         (string) Raw data for transaction\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"") + HelpExampleCli("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\" true") + HelpExampleRpc("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    uint256 hash;
    hash.SetHex(params[0].get_str());

    isminefilter filter = ISMINE_SPENDABLE;
    if (params.size() > 1)
        if (params[1].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    UniValue entry(UniValue::VOBJ);
    if (!pwalletMain->mapWallet.count(hash))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid or non-wallet transaction id");
    const CWalletTx& wtx = pwalletMain->mapWallet[hash];

    CAmount nCredit = wtx.GetCredit(filter);
    CAmount nDebit = wtx.GetDebit(filter);
    CAmount nNet = nCredit - nDebit;
    CAmount nFee = (wtx.IsFromMe(filter) ? wtx.GetValueOut() - nDebit : 0);

    entry.push_back(Pair("amount", ValueFromAmount(nNet - nFee)));
    if (wtx.IsFromMe(filter))
        entry.push_back(Pair("fee", ValueFromAmount(nFee)));

    WalletTxToJSON(wtx, entry);

    UniValue details(UniValue::VARR);
    ListTransactions(wtx, "*", 0, false, details, filter);
    entry.push_back(Pair("details", details));

    string strHex = EncodeHexTx(static_cast<CTransaction>(wtx), PROTOCOL_VERSION | RPCSerializationFlags());
    entry.push_back(Pair("hex", strHex));

    return entry;
}


UniValue backupwallet(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "backupwallet \"destination\"\n"
            "\nSafely copies wallet.dat to destination, which can be a directory or a path with filename.\n"
            "\nArguments:\n"
            "1. \"destination\"   (string) The destination directory or file\n"
            "\nExamples:\n" +
            HelpExampleCli("backupwallet", "\"backup.dat\"") + HelpExampleRpc("backupwallet", "\"backup.dat\""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    string strDest = params[0].get_str();
    if (!BackupWallet(*pwalletMain, strDest))
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: Wallet backup failed!");

    return NullUniValue;
}


UniValue keypoolrefill(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "keypoolrefill ( newsize )\n"
            "\nFills the keypool." +
            HelpRequiringPassphrase() + "\n"
                                        "\nArguments\n"
                                        "1. newsize     (numeric, optional, default=100) The new keypool size\n"
                                        "\nExamples:\n" +
            HelpExampleCli("keypoolrefill", "") + HelpExampleRpc("keypoolrefill", ""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    // 0 is interpreted by TopUpKeyPool() as the default keypool size given by -keypool
    unsigned int kpSize = 0;
    if (params.size() > 0) {
        if (params[0].get_int() < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected valid size.");
        kpSize = (unsigned int)params[0].get_int();
    }

    EnsureWalletIsUnlocked();
    pwalletMain->TopUpKeyPool(kpSize);

    if (pwalletMain->GetKeyPoolSize() < (pwalletMain->IsHDEnabled() ? kpSize * 2 : kpSize))
        throw JSONRPCError(RPC_WALLET_ERROR, "Error refreshing keypool.");

    return NullUniValue;
}


static void LockWallet(CWallet* pWallet)
{
    LOCK(cs_nWalletUnlockTime);
    nWalletUnlockTime = 0;
    pWallet->fWalletUnlockAnonymizeOnly = false;
    pWallet->Lock();
}

UniValue walletpassphrase(const UniValue& params, bool fHelp)
{
    if (pwalletMain->IsCrypted() && (fHelp || params.size() < 2 || params.size() > 3))
        throw runtime_error(
            "walletpassphrase \"passphrase\" timeout ( anonymizeonly )\n"
            "\nStores the wallet decryption key in memory for 'timeout' seconds.\n"
            "This is needed prior to performing transactions related to private keys such as sending PHRs\n"
            "\nArguments:\n"
            "1. \"passphrase\"     (string, required) The wallet passphrase\n"
            "2. timeout            (numeric, required) The time to keep the decryption key in seconds.\n"
            "3. anonymizeonly      (boolean, optional, default=flase) If is true sending functions are disabled."
            "\nNote:\n"
            "Issuing the walletpassphrase command while the wallet is already unlocked will set a new unlock\n"
            "time that overrides the old one. A timeout of \"0\" unlocks until the wallet is closed.\n"
            "\nExamples:\n"
            "\nUnlock the wallet for 60 seconds\n" +
            HelpExampleCli("walletpassphrase", "\"my pass phrase\" 60") +
            "\nUnlock the wallet for 60 seconds but allow anonymization, automint, and staking only\n" + HelpExampleCli("walletpassphrase", "\"my pass phrase\" 60 true") +
            "\nLock the wallet again (before 60 seconds)\n" + HelpExampleCli("walletlock", "") +
            "\nAs json rpc call\n" + HelpExampleRpc("walletpassphrase", "\"my pass phrase\", 60"));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    if (fHelp)
        return true;
    if (!pwalletMain->IsCrypted())
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletpassphrase was called.");

    // Note that the walletpassphrase is stored in params[0] which is not mlock()ed
    SecureString strWalletPass;
    strWalletPass.reserve(100);
    // TODO: get rid of this .c_str() by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make params[0] mlock()'d to begin with.
    strWalletPass = params[0].get_str().c_str();

    bool anonymizeOnly = false;
    if (params.size() == 3)
        anonymizeOnly = params[2].get_bool();

    if (!pwalletMain->IsLocked() && pwalletMain->fWalletUnlockAnonymizeOnly && anonymizeOnly)
        throw JSONRPCError(RPC_WALLET_ALREADY_UNLOCKED, "Error: Wallet is already unlocked.");

    // Get the timeout
    int64_t nSleepTime = params[1].get_int64();
    // Timeout cannot be negative, otherwise it will relock immediately
    if (nSleepTime < 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Timeout cannot be negative.");
    }
    // Clamp timeout
    constexpr int64_t MAX_SLEEP_TIME = 100000000; // larger values trigger a macos/libevent bug?
    if (nSleepTime > MAX_SLEEP_TIME) {
        nSleepTime = MAX_SLEEP_TIME;
    }

    if (!pwalletMain->Unlock(strWalletPass, anonymizeOnly))
        throw JSONRPCError(RPC_WALLET_PASSPHRASE_INCORRECT, "Error: The wallet passphrase entered was incorrect.");

    pwalletMain->TopUpKeyPool();

    if (nSleepTime > 0) {
        nWalletUnlockTime = GetTime () + nSleepTime;
        RPCRunLater ("lockwallet", boost::bind (LockWallet, pwalletMain), nSleepTime);
    }

    return NullUniValue;
}


UniValue walletpassphrasechange(const UniValue& params, bool fHelp)
{
    if (pwalletMain->IsCrypted() && (fHelp || params.size() != 2))
        throw runtime_error(
            "walletpassphrasechange \"oldpassphrase\" \"newpassphrase\"\n"
            "\nChanges the wallet passphrase from 'oldpassphrase' to 'newpassphrase'.\n"
            "\nArguments:\n"
            "1. \"oldpassphrase\"      (string) The current passphrase\n"
            "2. \"newpassphrase\"      (string) The new passphrase\n"
            "\nExamples:\n" +
            HelpExampleCli("walletpassphrasechange", "\"old one\" \"new one\"") + HelpExampleRpc("walletpassphrasechange", "\"old one\", \"new one\""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    if (fHelp)
        return true;
    if (!pwalletMain->IsCrypted())
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletpassphrasechange was called.");

    // TODO: get rid of these .c_str() calls by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make params[0] mlock()'d to begin with.
    SecureString strOldWalletPass;
    strOldWalletPass.reserve(100);
    strOldWalletPass = params[0].get_str().c_str();

    SecureString strNewWalletPass;
    strNewWalletPass.reserve(100);
    strNewWalletPass = params[1].get_str().c_str();

    if (strOldWalletPass.length() < 1 || strNewWalletPass.length() < 1)
        throw runtime_error(
            "walletpassphrasechange <oldpassphrase> <newpassphrase>\n"
            "Changes the wallet passphrase from <oldpassphrase> to <newpassphrase>.");

    if (!pwalletMain->ChangeWalletPassphrase(strOldWalletPass, strNewWalletPass))
        throw JSONRPCError(RPC_WALLET_PASSPHRASE_INCORRECT, "Error: The wallet passphrase entered was incorrect.");

    return NullUniValue;
}


UniValue walletlock(const UniValue& params, bool fHelp)
{
    if (pwalletMain->IsCrypted() && (fHelp || params.size() != 0))
        throw runtime_error(
            "walletlock\n"
            "\nRemoves the wallet encryption key from memory, locking the wallet.\n"
            "After calling this method, you will need to call walletpassphrase again\n"
            "before being able to call any methods which require the wallet to be unlocked.\n"
            "\nExamples:\n"
            "\nSet the passphrase for 2 minutes to perform a transaction\n" +
            HelpExampleCli("walletpassphrase", "\"my pass phrase\" 120") +
            "\nPerform a send (requires passphrase set)\n" + HelpExampleCli("sendtoaddress", "\"XwnLY9Tf7Zsef8gMGL2fhWA9ZmMjt4KPwg\" 1.0") +
            "\nClear the passphrase since we are done before 2 minutes is up\n" + HelpExampleCli("walletlock", "") +
            "\nAs json rpc call\n" + HelpExampleRpc("walletlock", ""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    if (fHelp)
        return true;
    if (!pwalletMain->IsCrypted())
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletlock was called.");

    {
        LOCK(cs_nWalletUnlockTime);
        pwalletMain->Lock();
        nWalletUnlockTime = 0;
    }

    return NullUniValue;
}


UniValue encryptwallet(const UniValue& params, bool fHelp)
{
    if (!pwalletMain->IsCrypted() && (fHelp || params.size() != 1))
        throw runtime_error(
            "encryptwallet \"passphrase\"\n"
            "\nEncrypts the wallet with 'passphrase'. This is for first time encryption.\n"
            "After this, any calls that interact with private keys such as sending or signing \n"
            "will require the passphrase to be set prior the making these calls.\n"
            "Use the walletpassphrase call for this, and then walletlock call.\n"
            "If the wallet is already encrypted, use the walletpassphrasechange call.\n"
            "Note that this will shutdown the server.\n"
            "\nArguments:\n"
            "1. \"passphrase\"    (string) The pass phrase to encrypt the wallet with. It must be at least 1 character, but should be long.\n"
            "\nExamples:\n"
            "\nEncrypt you wallet\n" +
            HelpExampleCli("encryptwallet", "\"my pass phrase\"") +
            "\nNow set the passphrase to use the wallet, such as for signing or sending PHRs\n" + HelpExampleCli("walletpassphrase", "\"my pass phrase\"") +
            "\nNow we can so something like sign\n" + HelpExampleCli("signmessage", "\"phoreaddress\" \"test message\"") +
            "\nNow lock the wallet again by removing the passphrase\n" + HelpExampleCli("walletlock", "") +
            "\nAs a json rpc call\n" + HelpExampleRpc("encryptwallet", "\"my pass phrase\""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    if (fHelp)
        return true;
    if (pwalletMain->IsCrypted())
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an encrypted wallet, but encryptwallet was called.");

    // TODO: get rid of this .c_str() by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make params[0] mlock()'d to begin with.
    SecureString strWalletPass;
    strWalletPass.reserve(100);
    strWalletPass = params[0].get_str().c_str();

    if (strWalletPass.length() < 1)
        throw runtime_error(
            "encryptwallet <passphrase>\n"
            "Encrypts the wallet with <passphrase>.");

    if (!pwalletMain->EncryptWallet(strWalletPass))
        throw JSONRPCError(RPC_WALLET_ENCRYPTION_FAILED, "Error: Failed to encrypt the wallet.");

    // BDB seems to have a bad habit of writing old data into
    // slack space in .dat files; that is bad if the old data is
    // unencrypted private keys. So:
    StartShutdown();
    return "Wallet encrypted; phore server stopping, restart to run with encrypted wallet. The keypool has been flushed and a new HD seed was generated (if you are using HD). You need to make a new backup.";
}

UniValue lockunspent(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "lockunspent unlock [{\"txid\":\"txid\",\"vout\":n},...]\n"
            "\nUpdates list of temporarily unspendable outputs.\n"
            "Temporarily lock (unlock=false) or unlock (unlock=true) specified transaction outputs.\n"
            "A locked transaction output will not be chosen by automatic coin selection, when spending PHRs.\n"
            "Locks are stored in memory only. Nodes start with zero locked outputs, and the locked output list\n"
            "is always cleared (by virtue of process exit) when a node stops or fails.\n"
            "Also see the listunspent call\n"
            "\nArguments:\n"
            "1. unlock            (boolean, required) Whether to unlock (true) or lock (false) the specified transactions\n"
            "2. \"transactions\"  (string, required) A json array of objects. Each object the txid (string) vout (numeric)\n"
            "     [           (json array of json objects)\n"
            "       {\n"
            "         \"txid\":\"id\",    (string) The transaction id\n"
            "         \"vout\": n         (numeric) The output number\n"
            "       }\n"
            "       ,...\n"
            "     ]\n"

            "\nResult:\n"
            "true|false    (boolean) Whether the command was successful or not\n"

            "\nExamples:\n"
            "\nList the unspent transactions\n" +
            HelpExampleCli("listunspent", "") +
            "\nLock an unspent transaction\n" + HelpExampleCli("lockunspent", "false \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nList the locked transactions\n" + HelpExampleCli("listlockunspent", "") +
            "\nUnlock the transaction again\n" + HelpExampleCli("lockunspent", "true \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nAs a json rpc call\n" + HelpExampleRpc("lockunspent", "false, \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    if (params.size() == 1)
        RPCTypeCheck(params, boost::assign::list_of(UniValue::VBOOL));
    else
        RPCTypeCheck(params, boost::assign::list_of(UniValue::VBOOL)(UniValue::VARR));

    bool fUnlock = params[0].get_bool();

    if (params.size() == 1) {
        if (fUnlock)
            pwalletMain->UnlockAllCoins();
        return true;
    }

    UniValue outputs = params[1].get_array();
    for (unsigned int idx = 0; idx < outputs.size(); idx++) {
        const UniValue& output = outputs[idx];
        if (!output.isObject())
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected object");
        const UniValue& o = output.get_obj();

        RPCTypeCheckObj(o, boost::assign::map_list_of("txid", UniValue::VSTR)("vout", UniValue::VNUM));

        string txid = find_value(o, "txid").get_str();
        if (!IsHex(txid))
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected hex txid");

        int nOutput = find_value(o, "vout").get_int();
        if (nOutput < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout must be positive");

        COutPoint outpt(uint256(txid), nOutput);

        if (fUnlock)
            pwalletMain->UnlockCoin(outpt);
        else
            pwalletMain->LockCoin(outpt);
    }

    return true;
}

UniValue listlockunspent(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 0)
        throw runtime_error(
            "listlockunspent\n"
            "\nReturns list of temporarily unspendable outputs.\n"
            "See the lockunspent call to lock and unlock transactions for spending.\n"
            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"txid\" : \"transactionid\",     (string) The transaction id locked\n"
            "    \"vout\" : n                      (numeric) The vout value\n"
            "  }\n"
            "  ,...\n"
            "]\n"
            "\nExamples:\n"
            "\nList the unspent transactions\n" +
            HelpExampleCli("listunspent", "") +
            "\nLock an unspent transaction\n" + HelpExampleCli("lockunspent", "false \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nList the locked transactions\n" + HelpExampleCli("listlockunspent", "") +
            "\nUnlock the transaction again\n" + HelpExampleCli("lockunspent", "true \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nAs a json rpc call\n" + HelpExampleRpc("listlockunspent", ""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    vector<COutPoint> vOutpts;
    pwalletMain->ListLockedCoins(vOutpts);

    UniValue ret(UniValue::VARR);

    BOOST_FOREACH (COutPoint& outpt, vOutpts) {
        UniValue o(UniValue::VOBJ);

        o.push_back(Pair("txid", outpt.hash.GetHex()));
        o.push_back(Pair("vout", (int)outpt.n));
        ret.push_back(o);
    }

    return ret;
}

UniValue settxfee(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 1)
        throw runtime_error(
            "settxfee amount\n"
            "\nSet the transaction fee per kB.\n"
            "\nArguments:\n"
            "1. amount         (numeric, required) The transaction fee in PHR/kB rounded to the nearest 0.00000001\n"
            "\nResult\n"
            "true|false        (boolean) Returns true if successful\n"
            "\nExamples:\n" +
            HelpExampleCli("settxfee", "0.00001") + HelpExampleRpc("settxfee", "0.00001"));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    // Amount
    CAmount nAmount = 0;
    if (params[0].get_real() != 0.0)
        nAmount = AmountFromValue(params[0]); // rejects 0.0 amounts

    payTxFee = CFeeRate(nAmount, 1000);
    return true;
}

UniValue getwalletinfo(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getwalletinfo\n"
            "Returns an object containing various wallet state info.\n"
            "\nResult:\n"
            "{\n"
            "  \"walletversion\": xxxxx,     (numeric) the wallet version\n"
            "  \"balance\": xxxxxxx,         (numeric) the total PHR balance of the wallet\n"
            "  \"txcount\": xxxxxxx,         (numeric) the total number of transactions in the wallet\n"
            "  \"keypoololdest\": xxxxxx,    (numeric) the timestamp (seconds since GMT epoch) of the oldest pre-generated key in the key pool\n"
            "  \"keypoolsize\": xxxx,        (numeric) how many new keys are pre-generated (only counts external keys)\n"
            "  \"keypoolsize_hd_internal\": xxxx, (numeric) how many new keys are pre-generated for internal use (used for change outputs, only appears if the wallet is using this feature, otherwise external keys are used)\n"
            "  \"unlocked_until\": ttt,      (numeric) the timestamp in seconds since epoch (midnight Jan 1 1970 GMT) that the wallet is unlocked for transfers, or 0 if the wallet is locked\n"
            "  \"hdchainid\": \"<hash>\",      (string) the ID of the HD chain\n"
            "  \"hdaccountcount\": xxx,      (numeric) how many accounts of the HD chain are in this wallet\n"
            "    [\n"
            "      {\n"
            "      \"hdaccountindex\": xxx,         (numeric) the index of the account\n"
            "      \"hdexternalkeyindex\": xxxx,    (numeric) current external childkey index\n"
            "      \"hdinternalkeyindex\": xxxx,    (numeric) current internal childkey index\n"
            "      }\n"
            "      ,...\n"
            "    ]\n"
            "}\n"
            "\nExamples:\n" +
            HelpExampleCli("getwalletinfo", "") + HelpExampleRpc("getwalletinfo", ""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    CHDChain hdChainCurrent;
    bool fHDEnabled = pwalletMain->GetHDChain(hdChainCurrent);
    UniValue obj(UniValue::VOBJ);
    obj.push_back(Pair("walletversion", pwalletMain->GetVersion()));
    obj.push_back(Pair("balance", ValueFromAmount(pwalletMain->GetBalance())));
    obj.push_back(Pair("txcount", (int)pwalletMain->mapWallet.size()));
    obj.push_back(Pair("keypoololdest", pwalletMain->GetOldestKeyPoolTime()));
    obj.push_back(Pair("keypoolsize",   (int64_t)pwalletMain->KeypoolCountExternalKeys()));
    if (fHDEnabled) {
        obj.push_back(Pair("keypoolsize_hd_internal",   (int64_t)(pwalletMain->KeypoolCountInternalKeys())));
    }
    if (pwalletMain->IsCrypted())
        obj.push_back(Pair("unlocked_until", nWalletUnlockTime));
    if (fHDEnabled) {
        obj.push_back(Pair("hdchainid", hdChainCurrent.GetID().GetHex()));
        obj.push_back(Pair("hdaccountcount", (int64_t)hdChainCurrent.CountAccounts()));
        UniValue accounts(UniValue::VARR);
        for (int i = 0; i < hdChainCurrent.CountAccounts(); ++i)
        {
            CHDAccount acc;
            UniValue account(UniValue::VOBJ);
            account.push_back(Pair("hdaccountindex", (int64_t)i));
            if(hdChainCurrent.GetAccount(i, acc)) {
                account.push_back(Pair("hdexternalkeyindex", (int64_t)acc.nExternalChainCounter));
                account.push_back(Pair("hdinternalkeyindex", (int64_t)acc.nInternalChainCounter));
            } else {
                account.push_back(Pair("error", strprintf("account %d is missing", i)));
            }
            accounts.push_back(account);
        }
        obj.push_back(Pair("hdaccounts", accounts));
    }
    return obj;
}

// ppcoin: reserve balance from being staked for network protection
UniValue reservebalance(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw runtime_error(
            "reservebalance ( reserve amount )\n"
            "\nShow or set the reserve amount not participating in network protection\n"
            "If no parameters provided current setting is printed.\n"

            "\nArguments:\n"
            "1. reserve     (boolean, optional) is true or false to turn balance reserve on or off.\n"
            "2. amount      (numeric, optional) is a real and rounded to cent.\n"

            "\nResult:\n"
            "{\n"
            "  \"reserve\": true|false,     (boolean) Status of the reserve balance\n"
            "  \"amount\": x.xxxx       (numeric) Amount reserved\n"
            "\nExamples:\n" +
            HelpExampleCli("reservebalance", "true 5000") + HelpExampleRpc("reservebalance", "true 5000"));

    if (params.size() > 0) {
        bool fReserve = params[0].get_bool();
        if (fReserve) {
            if (params.size() == 1)
                throw runtime_error("must provide amount to reserve balance.\n");
            CAmount nAmount = AmountFromValue(params[1]);
            nAmount = (nAmount / CENT) * CENT; // round to cent
            if (nAmount < 0)
                throw runtime_error("amount cannot be negative.\n");
            nReserveBalance = nAmount;
        } else {
            if (params.size() > 1)
                throw runtime_error("cannot specify amount to turn off reserve.\n");
            nReserveBalance = 0;
        }
    }

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("reserve", (nReserveBalance > 0)));
    result.push_back(Pair("amount", ValueFromAmount(nReserveBalance)));
    return result;
}

// presstab HyperStake
UniValue setstakesplitthreshold(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "setstakesplitthreshold value\n"
            "\nThis will set the output size of your stakes to never be below this number\n"

            "\nArguments:\n"
            "1. value   (numeric, required) Threshold value between 1 and 999999\n"
            "\nResult:\n"
            "{\n"
            "  \"threshold\": n,    (numeric) Threshold value set\n"
            "  \"saved\": true|false    (boolean) 'true' if successfully saved to the wallet file\n"
            "}\n"
            "\nExamples:\n" +
            HelpExampleCli("setstakesplitthreshold", "5000") + HelpExampleRpc("setstakesplitthreshold", "5000"));

    EnsureWalletIsUnlocked();

    uint64_t nStakeSplitThreshold = params[0].get_int();
    if (nStakeSplitThreshold > 999999)
        throw runtime_error("Value out of range, max allowed is 999999");

    CWalletDB walletdb(pwalletMain->strWalletFile);
    LOCK(pwalletMain->cs_wallet);
    {
        bool fFileBacked = pwalletMain->fFileBacked;

        UniValue result(UniValue::VOBJ);
        pwalletMain->nStakeSplitThreshold = nStakeSplitThreshold;
        result.push_back(Pair("threshold", int(pwalletMain->nStakeSplitThreshold)));
        if (fFileBacked) {
            walletdb.WriteStakeSplitThreshold(nStakeSplitThreshold);
            result.push_back(Pair("saved", "true"));
        } else
            result.push_back(Pair("saved", "false"));

        return result;
    }
}

// presstab HyperStake
UniValue getstakesplitthreshold(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getstakesplitthreshold\n"
            "Returns the threshold for stake splitting\n"
            "\nResult:\n"
            "n      (numeric) Threshold value\n"
            "\nExamples:\n" +
            HelpExampleCli("getstakesplitthreshold", "") + HelpExampleRpc("getstakesplitthreshold", ""));

    return int(pwalletMain->nStakeSplitThreshold);
}

UniValue autocombinerewards(const UniValue& params, bool fHelp)
{
    bool fEnable;
    if (params.size() >= 1)
        fEnable = params[0].get_bool();

    if (fHelp || params.size() < 1 || (fEnable && params.size() != 2) || params.size() > 2)
        throw runtime_error(
            "autocombinerewards true|false ( threshold )\n"
            "\nWallet will automatically monitor for any coins with value below the threshold amount, and combine them if they reside with the same Phore address\n"
            "When autocombinerewards runs it will create a transaction, and therefore will be subject to transaction fees.\n"

            "\nArguments:\n"
            "1. true|false      (boolean, required) Enable auto combine (true) or disable (false)\n"
            "2. threshold       (numeric, optional) Threshold amount (default: 0)\n"
            "\nExamples:\n" +
            HelpExampleCli("autocombinerewards", "true 500") + HelpExampleRpc("autocombinerewards", "true 500"));

    CWalletDB walletdb(pwalletMain->strWalletFile);
    CAmount nThreshold = 0;

    if (fEnable)
        nThreshold = params[1].get_int();

    pwalletMain->fCombineDust = fEnable;
    pwalletMain->nAutoCombineThreshold = nThreshold;

    if (!walletdb.WriteAutoCombineSettings(fEnable, nThreshold))
        throw runtime_error("Changed settings in wallet but failed to save to database\n");

    return NullUniValue;
}

UniValue printMultiSend()
{
    UniValue ret(UniValue::VARR);
    UniValue act(UniValue::VOBJ);
    act.push_back(Pair("MultiSendStake Activated?", pwalletMain->fMultiSendStake));
    act.push_back(Pair("MultiSendMasternode Activated?", pwalletMain->fMultiSendMasternodeReward));
    ret.push_back(act);

    if (pwalletMain->vDisabledAddresses.size() >= 1) {
        UniValue disAdd(UniValue::VOBJ);
        for (unsigned int i = 0; i < pwalletMain->vDisabledAddresses.size(); i++) {
            disAdd.push_back(Pair("Disabled From Sending", pwalletMain->vDisabledAddresses[i]));
        }
        ret.push_back(disAdd);
    }

    ret.push_back("MultiSend Addresses to Send To:");

    UniValue vMS(UniValue::VOBJ);
    for (unsigned int i = 0; i < pwalletMain->vMultiSend.size(); i++) {
        vMS.push_back(Pair("Address " + std::to_string(i), pwalletMain->vMultiSend[i].first));
        vMS.push_back(Pair("Percent", pwalletMain->vMultiSend[i].second));
    }

    ret.push_back(vMS);
    return ret;
}

UniValue printAddresses()
{
    std::vector<COutput> vCoins;
    pwalletMain->AvailableCoins(vCoins);
    std::map<std::string, double> mapAddresses;
    BOOST_FOREACH (const COutput& out, vCoins) {
        CTxDestination utxoAddress;
        ExtractDestination(out.tx->vout[out.i].scriptPubKey, utxoAddress);
        std::string strAdd = EncodeDestination(utxoAddress);

        if (mapAddresses.find(strAdd) == mapAddresses.end()) //if strAdd is not already part of the map
            mapAddresses[strAdd] = (double)out.tx->vout[out.i].nValue / (double)COIN;
        else
            mapAddresses[strAdd] += (double)out.tx->vout[out.i].nValue / (double)COIN;
    }

    UniValue ret(UniValue::VARR);
    for (map<std::string, double>::const_iterator it = mapAddresses.begin(); it != mapAddresses.end(); ++it) {
        UniValue obj(UniValue::VOBJ);
        const std::string* strAdd = &(*it).first;
        const double* nBalance = &(*it).second;
        obj.push_back(Pair("Address ", *strAdd));
        obj.push_back(Pair("Balance ", *nBalance));
        ret.push_back(obj);
    }

    return ret;
}

unsigned int sumMultiSend()
{
    unsigned int sum = 0;
    for (unsigned int i = 0; i < pwalletMain->vMultiSend.size(); i++)
        sum += pwalletMain->vMultiSend[i].second;
    return sum;
}

UniValue multisend(const UniValue& params, bool fHelp)
{
    CWalletDB walletdb(pwalletMain->strWalletFile);
    bool fFileBacked;
    //MultiSend Commands
    if (params.size() == 1) {
        string strCommand = params[0].get_str();
        UniValue ret(UniValue::VOBJ);
        if (strCommand == "print") {
            return printMultiSend();
        } else if (strCommand == "printaddress" || strCommand == "printaddresses") {
            return printAddresses();
        } else if (strCommand == "clear") {
            LOCK(pwalletMain->cs_wallet);
            {
                bool erased = false;
                if (pwalletMain->fFileBacked) {
                    if (walletdb.EraseMultiSend(pwalletMain->vMultiSend))
                        erased = true;
                }

                pwalletMain->vMultiSend.clear();
                pwalletMain->setMultiSendDisabled();

                UniValue obj(UniValue::VOBJ);
                obj.push_back(Pair("Erased from database", erased));
                obj.push_back(Pair("Erased from RAM", true));

                return obj;
            }
        } else if (strCommand == "enablestake" || strCommand == "activatestake") {
            if (pwalletMain->vMultiSend.size() < 1)
                throw JSONRPCError(RPC_INVALID_REQUEST, "Unable to activate MultiSend, check MultiSend vector");

            if (IsValidDestinationString(pwalletMain->vMultiSend[0].first)) {
                pwalletMain->fMultiSendStake = true;
                if (!walletdb.WriteMSettings(true, pwalletMain->fMultiSendMasternodeReward, pwalletMain->nLastMultiSendHeight)) {
                    UniValue obj(UniValue::VOBJ);
                    obj.push_back(Pair("error", "MultiSend activated but writing settings to DB failed"));
                    UniValue arr(UniValue::VARR);
                    arr.push_back(obj);
                    arr.push_back(printMultiSend());
                    return arr;
                } else
                    return printMultiSend();
            }

            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unable to activate MultiSend, check MultiSend vector");
        } else if (strCommand == "enablemasternode" || strCommand == "activatemasternode") {
            if (pwalletMain->vMultiSend.size() < 1)
                throw JSONRPCError(RPC_INVALID_REQUEST, "Unable to activate MultiSend, check MultiSend vector");

            if (IsValidDestinationString(pwalletMain->vMultiSend[0].first)) {
                pwalletMain->fMultiSendMasternodeReward = true;

                if (!walletdb.WriteMSettings(pwalletMain->fMultiSendStake, true, pwalletMain->nLastMultiSendHeight)) {
                    UniValue obj(UniValue::VOBJ);
                    obj.push_back(Pair("error", "MultiSend activated but writing settings to DB failed"));
                    UniValue arr(UniValue::VARR);
                    arr.push_back(obj);
                    arr.push_back(printMultiSend());
                    return arr;
                } else
                    return printMultiSend();
            }

            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unable to activate MultiSend, check MultiSend vector");
        } else if (strCommand == "disable" || strCommand == "deactivate") {
            pwalletMain->setMultiSendDisabled();
            if (!walletdb.WriteMSettings(false, false, pwalletMain->nLastMultiSendHeight))
                throw JSONRPCError(RPC_DATABASE_ERROR, "MultiSend deactivated but writing settings to DB failed");

            return printMultiSend();
        } else if (strCommand == "enableall") {
            if (!walletdb.EraseMSDisabledAddresses(pwalletMain->vDisabledAddresses))
                return "failed to clear old vector from walletDB";
            else {
                pwalletMain->vDisabledAddresses.clear();
                return printMultiSend();
            }
        }
    }
    if (params.size() == 2 && params[0].get_str() == "delete") {
        int del = boost::lexical_cast<int>(params[1].get_str());
        if (!walletdb.EraseMultiSend(pwalletMain->vMultiSend))
            throw JSONRPCError(RPC_DATABASE_ERROR, "failed to delete old MultiSend vector from database");

        pwalletMain->vMultiSend.erase(pwalletMain->vMultiSend.begin() + del);
        if (!walletdb.WriteMultiSend(pwalletMain->vMultiSend))
            throw JSONRPCError(RPC_DATABASE_ERROR, "walletdb WriteMultiSend failed!");

        return printMultiSend();
    }
    if (params.size() == 2 && params[0].get_str() == "disable") {
        std::string disAddress = params[1].get_str();
        if (!IsValidDestinationString(disAddress))
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "address you want to disable is not valid");
        else {
            pwalletMain->vDisabledAddresses.push_back(disAddress);
            if (!walletdb.EraseMSDisabledAddresses(pwalletMain->vDisabledAddresses))
                throw JSONRPCError(RPC_DATABASE_ERROR, "disabled address from sending, but failed to clear old vector from walletDB");

            if (!walletdb.WriteMSDisabledAddresses(pwalletMain->vDisabledAddresses))
                throw JSONRPCError(RPC_DATABASE_ERROR, "disabled address from sending, but failed to store it to walletDB");
            else
                return printMultiSend();
        }
    }

    //if no commands are used
    if (fHelp || params.size() != 2)
        throw runtime_error(
            "multisend <command>\n"
            "****************************************************************\n"
            "WHAT IS MULTISEND?\n"
            "MultiSend allows a user to automatically send a percent of their stake reward to as many addresses as you would like\n"
            "The MultiSend transaction is sent when the staked coins mature (100 confirmations)\n"
            "****************************************************************\n"
            "TO CREATE OR ADD TO THE MULTISEND VECTOR:\n"
            "multisend <Phore Address> <percent>\n"
            "This will add a new address to the MultiSend vector\n"
            "Percent is a whole number 1 to 100.\n"
            "****************************************************************\n"
            "MULTISEND COMMANDS (usage: multisend <command>)\n"
            " print - displays the current MultiSend vector \n"
            " clear - deletes the current MultiSend vector \n"
            " enablestake/activatestake - activates the current MultiSend vector to be activated on stake rewards\n"
            " enablemasternode/activatemasternode - activates the current MultiSend vector to be activated on masternode rewards\n"
            " disable/deactivate - disables the current MultiSend vector \n"
            " delete <Address #> - deletes an address from the MultiSend vector \n"
            " disable <address> - prevents a specific address from sending MultiSend transactions\n"
            " enableall - enables all addresses to be eligible to send MultiSend transactions\n"
            "****************************************************************\n");

    //if the user is entering a new MultiSend item
    string strAddress = params[0].get_str();
    if (!IsValidDestinationString(strAddress))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid PHR address");
    if (boost::lexical_cast<int>(params[1].get_str()) < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected valid percentage");
    if (pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");
    unsigned int nPercent = boost::lexical_cast<unsigned int>(params[1].get_str());

    LOCK(pwalletMain->cs_wallet);
    {
        fFileBacked = pwalletMain->fFileBacked;
        //Error if 0 is entered
        if (nPercent == 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Sending 0% of stake is not valid");
        }

        //MultiSend can only send 100% of your stake
        if (nPercent + sumMultiSend() > 100)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Failed to add to MultiSend vector, the sum of your MultiSend is greater than 100%");

        for (unsigned int i = 0; i < pwalletMain->vMultiSend.size(); i++) {
            if (pwalletMain->vMultiSend[i].first == strAddress)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Failed to add to MultiSend vector, cannot use the same address twice");
        }

        if (fFileBacked)
            walletdb.EraseMultiSend(pwalletMain->vMultiSend);

        std::pair<std::string, int> newMultiSend;
        newMultiSend.first = strAddress;
        newMultiSend.second = nPercent;
        pwalletMain->vMultiSend.push_back(newMultiSend);
        if (fFileBacked) {
            if (!walletdb.WriteMultiSend(pwalletMain->vMultiSend))
                throw JSONRPCError(RPC_DATABASE_ERROR, "walletdb WriteMultiSend failed!");
        }
    }
    return printMultiSend();
}
UniValue getzerocoinbalance(const UniValue& params, bool fHelp)
{

    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getzerocoinbalance\n"
            "\nReturn the wallet's total zPHR balance.\n" +
            HelpRequiringPassphrase() + "\n"

            "\nResult:\n"
            "amount         (numeric) Total zPHR balance.\n"

            "\nExamples:\n" +
            HelpExampleCli("getzerocoinbalance", "") + HelpExampleRpc("getzerocoinbalance", ""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    if (pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");

    return ValueFromAmount(pwalletMain->GetZerocoinBalance(true));

}
UniValue listmintedzerocoins(const UniValue& params, bool fHelp)
{

    if (fHelp || params.size() != 0)
        throw runtime_error(
            "listmintedzerocoins\n"
            "\nList all zPHR mints in the wallet.\n" +
            HelpRequiringPassphrase() + "\n"

            "\nResult:\n"
            "[\n"
            "  \"xxx\"      (string) Pubcoin in hex format.\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("listmintedzerocoins", "") + HelpExampleRpc("listmintedzerocoins", ""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    if (pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");

    CWalletDB walletdb(pwalletMain->strWalletFile);
    set<CMintMeta> setMints = pwalletMain->zphrTracker->ListMints(true, true, true);

    UniValue jsonList(UniValue::VARR);
    for (const CMintMeta& meta : setMints) {
        jsonList.push_back(meta.hashPubcoin.GetHex());
    }

    return jsonList;
}

UniValue listzerocoinamounts(const UniValue& params, bool fHelp)
{

    if (fHelp || params.size() != 0)
        throw runtime_error(
            "listzerocoinamounts\n"
            "\nGet information about your zerocoin amounts.\n" +
            HelpRequiringPassphrase() + "\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"denomination\": n,   (numeric) Denomination Value.\n"
            "    \"mints\": n           (numeric) Number of mints.\n"
            "  }\n"
            "  ,..."
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("listzerocoinamounts", "") + HelpExampleRpc("listzerocoinamounts", ""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    EnsureWalletIsUnlocked();

    CWalletDB walletdb(pwalletMain->strWalletFile);
    set<CMintMeta> setMints = pwalletMain->zphrTracker->ListMints(true, true, true);

    std::map<libzerocoin::CoinDenomination, CAmount> spread;
    for (const auto& denom : libzerocoin::zerocoinDenomList)
        spread.insert(std::pair<libzerocoin::CoinDenomination, CAmount>(denom, 0));
    for (auto& meta : setMints) spread.at(meta.denom)++;


    UniValue ret(UniValue::VARR);
    for (const auto& m : libzerocoin::zerocoinDenomList) {
        UniValue val(UniValue::VOBJ);
        val.push_back(Pair("denomination", libzerocoin::ZerocoinDenominationToInt(m)));
        val.push_back(Pair("mints", (int64_t)spread.at(m)));
        ret.push_back(val);
    }
    return ret;
}

UniValue listspentzerocoins(const UniValue& params, bool fHelp)
{

    if (fHelp || params.size() != 0)
        throw runtime_error(
            "listspentzerocoins\n"
            "\nList all the spent zPHR mints in the wallet.\n" +
            HelpRequiringPassphrase() + "\n"

            "\nResult:\n"
            "[\n"
            "  \"xxx\"      (string) Pubcoin in hex format.\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("listspentzerocoins", "") + HelpExampleRpc("listspentzerocoins", ""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    EnsureWalletIsUnlocked(true);

    CWalletDB walletdb(pwalletMain->strWalletFile);
    list<CBigNum> listPubCoin = walletdb.ListSpentCoinsSerial();

    UniValue jsonList(UniValue::VARR);
    for (const CBigNum& pubCoinItem : listPubCoin) {
        jsonList.push_back(pubCoinItem.GetHex());
    }

    return jsonList;
}

UniValue mintzerocoin(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "mintzerocoin amount ( utxos )\n"
            "\nMint the specified zPHR amount\n" +
            HelpRequiringPassphrase() + "\n"

            "\nArguments:\n"
            "1. amount      (numeric, required) Enter an amount of PHR to convert to zPHR\n"
            "2. utxos       (string, optional) A json array of objects.\n"
            "                   Each object needs the txid (string) and vout (numeric)\n"
            "  [\n"
            "    {\n"
            "      \"txid\":\"txid\",    (string) The transaction id\n"
            "      \"vout\": n         (numeric) The output number\n"
            "    }\n"
            "    ,...\n"
            "  ]\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"txid\": \"xxx\",         (string) Transaction ID.\n"
            "    \"value\": amount,       (numeric) Minted amount.\n"
            "    \"pubcoin\": \"xxx\",      (string) Pubcoin in hex format.\n"
            "    \"randomness\": \"xxx\",   (string) Hex encoded randomness.\n"
            "    \"serial\": \"xxx\",       (string) Serial in hex format.\n"
            "    \"time\": nnn            (numeric) Time to mint this transaction.\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n"
            "\nMint 50 from anywhere\n" +
            HelpExampleCli("mintzerocoin", "50") +
            "\nMint 13 from a specific output\n" +
            HelpExampleCli("mintzerocoin", "13 \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nAs a json rpc call\n" +
            HelpExampleRpc("mintzerocoin", "13, \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    if (params.size() == 1)
    {
        RPCTypeCheck(params, boost::assign::list_of(UniValue::VNUM));
    } else
    {
        RPCTypeCheck(params, boost::assign::list_of(UniValue::VNUM)(UniValue::VARR));
    }

    int64_t nTime = GetTimeMillis();
    if(GetAdjustedTime() > GetSporkValue(SPORK_16_ZEROCOIN_MAINTENANCE_MODE))
        throw JSONRPCError(RPC_WALLET_ERROR, "zPHR is currently disabled due to maintenance.");

    EnsureWalletIsUnlocked(true);

    CAmount nAmount = params[0].get_int() * COIN;

    CWalletTx wtx;
    vector<CDeterministicMint> vDMints;
    string strError;
    vector<COutPoint> vOutpts;

    if (params.size() == 2)
    {
        UniValue outputs = params[1].get_array();
        for (unsigned int idx = 0; idx < outputs.size(); idx++) {
            const UniValue& output = outputs[idx];
            if (!output.isObject())
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected object");
            const UniValue& o = output.get_obj();

            RPCTypeCheckObj(o, boost::assign::map_list_of("txid", UniValue::VSTR)("vout", UniValue::VNUM));

            string txid = find_value(o, "txid").get_str();
            if (!IsHex(txid))
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected hex txid");

            int nOutput = find_value(o, "vout").get_int();
            if (nOutput < 0)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout must be positive");

            COutPoint outpt(uint256(txid), nOutput);
            vOutpts.push_back(outpt);
        }
        strError = pwalletMain->MintZerocoinFromOutPoint(nAmount, wtx, vDMints, vOutpts);
    } else
    {
        strError = pwalletMain->MintZerocoin(nAmount, wtx, vDMints);
    }

    if (strError != "")
        throw JSONRPCError(RPC_WALLET_ERROR, strError);

    UniValue arrMints(UniValue::VARR);
    for (CDeterministicMint dMint : vDMints) {
        UniValue m(UniValue::VOBJ);
        m.push_back(Pair("txid", wtx.GetHash().ToString()));
        m.push_back(Pair("value", ValueFromAmount(libzerocoin::ZerocoinDenominationToAmount(dMint.GetDenomination()))));
        m.push_back(Pair("pubcoinhash", dMint.GetPubcoinHash().GetHex()));
        m.push_back(Pair("serialhash", dMint.GetSerialHash().GetHex()));
        m.push_back(Pair("seedhash", dMint.GetSeedHash().GetHex()));
        m.push_back(Pair("count", (int64_t)dMint.GetCount()));
        m.push_back(Pair("time", GetTimeMillis() - nTime));
        arrMints.push_back(m);
    }

    return arrMints;
}

UniValue spendzerocoin(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 5 || params.size() < 4)
        throw runtime_error(
            "spendzerocoin amount mintchange minimizechange securitylevel ( \"address\" )\n"
            "\nSpend zPHR to a PHR address.\n" +
            HelpRequiringPassphrase() + "\n"

            "\nArguments:\n"
            "1. amount          (numeric, required) Amount to spend.\n"
            "2. mintchange      (boolean, required) Re-mint any leftover change.\n"
            "3. minimizechange  (boolean, required) Try to minimize the returning change  [false]\n"
            "4. securitylevel   (numeric, required) Amount of checkpoints to add to the accumulator.\n"
            "                       A checkpoint contains 10 blocks worth of zerocoinmints.\n"
            "                       The more checkpoints that are added, the more untraceable the transaction.\n"
            "                       Use [100] to add the maximum amount of checkpoints available.\n"
            "                       Adding more checkpoints makes the minting process take longer\n"
            "5. \"address\"     (string, optional, default=change) Send to specified address or to a new change address.\n"
            "                       If there is change then an address is required\n"

            "\nResult:\n"
            "{\n"
            "  \"txid\": \"xxx\",             (string) Transaction hash.\n"
            "  \"bytes\": nnn,              (numeric) Transaction size.\n"
            "  \"fee\": amount,             (numeric) Transaction fee (if any).\n"
            "  \"spends\": [                (array) JSON array of input objects.\n"
            "    {\n"
            "      \"denomination\": nnn,   (numeric) Denomination value.\n"
            "      \"pubcoin\": \"xxx\",      (string) Pubcoin in hex format.\n"
            "      \"serial\": \"xxx\",       (string) Serial number in hex format.\n"
            "      \"acc_checksum\": \"xxx\", (string) Accumulator checksum in hex format.\n"
            "    }\n"
            "    ,...\n"
            "  ],\n"
            "  \"outputs\": [                 (array) JSON array of output objects.\n"
            "    {\n"
            "      \"value\": amount,         (numeric) Value in PHR.\n"
            "      \"address\": \"xxx\"         (string) PHR address or \"zerocoinmint\" for reminted change.\n"
            "    }\n"
            "    ,...\n"
            "  ]\n"
            "}\n"

            "\nExamples\n" +
            HelpExampleCli("spendzerocoin", "5000 false true 100 \"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\"") +
            HelpExampleRpc("spendzerocoin", "5000 false true 100 \"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\""));

    LOCK2(cs_main, pwalletMain->cs_wallet);
    
    if(GetAdjustedTime() > GetSporkValue(SPORK_16_ZEROCOIN_MAINTENANCE_MODE))
        throw JSONRPCError(RPC_WALLET_ERROR, "zPHR is currently disabled due to maintenance.");

    int64_t nTimeStart = GetTimeMillis();

    EnsureWalletIsUnlocked();

    CAmount nAmount = AmountFromValue(params[0]);   // Spending amount
    bool fMintChange = params[1].get_bool();        // Mint change to zPHR
    bool fMinimizeChange = params[2].get_bool();    // Minimize change
    int nSecurityLevel = params[3].get_int();       // Security level

    CTxDestination address = CNoDestination(); // Optional sending address. Dummy initialization here.
    if (params.size() == 5) {
        // Destination address was supplied as params[4]. Optional parameters MUST be at the end
        // to avoid type confusion from the JSON interpreter
        if(!IsValidDestinationString(params[4].get_str())) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Phore address");
        } else {
            address = DecodeDestination(params[4].get_str());
        }
    }

    CWalletTx wtx;
    vector<CZerocoinMint> vMintsSelected;
    CZerocoinSpendReceipt receipt;
    bool fSuccess;

    if(params.size() == 5) // Spend to supplied destination address
        fSuccess = pwalletMain->SpendZerocoin(nAmount, nSecurityLevel, wtx, receipt, vMintsSelected, fMintChange, fMinimizeChange, &address);
    else                   // Spend to newly generated local address
        fSuccess = pwalletMain->SpendZerocoin(nAmount, nSecurityLevel, wtx, receipt, vMintsSelected, fMintChange, fMinimizeChange);

    if (!fSuccess)
        throw JSONRPCError(RPC_WALLET_ERROR, receipt.GetStatusMessage());

    CAmount nValueIn = 0;
    UniValue arrSpends(UniValue::VARR);
    for (CZerocoinSpend spend : receipt.GetSpends()) {
        UniValue obj(UniValue::VOBJ);
        obj.push_back(Pair("denomination", spend.GetDenomination()));
        obj.push_back(Pair("pubcoin", spend.GetPubCoin().GetHex()));
        obj.push_back(Pair("serial", spend.GetSerial().GetHex()));
        uint32_t nChecksum = spend.GetAccumulatorChecksum();
        obj.push_back(Pair("acc_checksum", HexStr(BEGIN(nChecksum), END(nChecksum))));
        arrSpends.push_back(obj);
        nValueIn += libzerocoin::ZerocoinDenominationToAmount(spend.GetDenomination());
    }

    CAmount nValueOut = 0;
    UniValue vout(UniValue::VARR);
    for (unsigned int i = 0; i < wtx.vout.size(); i++) {
        const CTxOut& txout = wtx.vout[i];
        UniValue out(UniValue::VOBJ);
        out.push_back(Pair("value", ValueFromAmount(txout.nValue)));
        nValueOut += txout.nValue;

        CTxDestination dest;
        if(txout.scriptPubKey.IsZerocoinMint())
            out.push_back(Pair("address", "zerocoinmint"));
        else if(ExtractDestination(txout.scriptPubKey, dest))
            out.push_back(Pair("address", EncodeDestination(dest)));
        vout.push_back(out);
    }

    //construct JSON to return
    UniValue ret(UniValue::VOBJ);
    ret.push_back(Pair("txid", wtx.GetHash().ToString()));
    ret.push_back(Pair("bytes", (int64_t)wtx.GetSerializeSize(SER_NETWORK, CTransaction::CURRENT_VERSION)));
    ret.push_back(Pair("fee", ValueFromAmount(nValueIn - nValueOut)));
    ret.push_back(Pair("duration_millis", (GetTimeMillis() - nTimeStart)));
    ret.push_back(Pair("spends", arrSpends));
    ret.push_back(Pair("outputs", vout));

    return ret;
}

UniValue resetmintzerocoin(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "resetmintzerocoin ( fullscan )\n"
            "\nScan the blockchain for all of the zerocoins that are held in the wallet.dat.\n"
            "Update any meta-data that is incorrect. Archive any mints that are not able to be found.\n" +
            HelpRequiringPassphrase() + "\n"

            "\nArguments:\n"
            "1. fullscan          (boolean, optional) Rescan each block of the blockchain.\n"
            "                               WARNING - may take 30+ minutes!\n"

            "\nResult:\n"
            "{\n"
            "  \"updated\": [       (array) JSON array of updated mints.\n"
            "    \"xxx\"            (string) Hex encoded mint.\n"
            "    ,...\n"
            "  ],\n"
            "  \"archived\": [      (array) JSON array of archived mints.\n"
            "    \"xxx\"            (string) Hex encoded mint.\n"
            "    ,...\n"
            "  ]\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("resetmintzerocoin", "true") + HelpExampleRpc("resetmintzerocoin", "true"));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    CzPHRTracker* zphrTracker = pwalletMain->zphrTracker.get();
    set<CMintMeta> setMints = zphrTracker->ListMints(false, false, true);
    vector<CMintMeta> vMintsToFind(setMints.begin(), setMints.end());
    vector<CMintMeta> vMintsMissing;
    vector<CMintMeta> vMintsToUpdate;

    // search all of our available data for these mints
    FindMints(vMintsToFind, vMintsToUpdate, vMintsMissing);

    // update the meta data of mints that were marked for updating
    UniValue arrUpdated(UniValue::VARR);
    for (CMintMeta meta : vMintsToUpdate) {
        zphrTracker->UpdateState(meta);
        arrUpdated.push_back(meta.hashPubcoin.GetHex());
    }

    // delete any mints that were unable to be located on the blockchain
    UniValue arrDeleted(UniValue::VARR);
    for (CMintMeta meta : vMintsMissing) {
        zphrTracker->Archive(meta);
        arrDeleted.push_back(meta.hashPubcoin.GetHex()); 
    }

    UniValue obj(UniValue::VOBJ);
    obj.push_back(Pair("updated", arrUpdated));
    obj.push_back(Pair("archived", arrDeleted));
    return obj;
}

UniValue resetspentzerocoin(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "resetspentzerocoin\n"
            "\nScan the blockchain for all of the zerocoins that are held in the wallet.dat.\n"
            "Reset mints that are considered spent that did not make it into the blockchain.\n"

            "\nResult:\n"
            "{\n"
            "  \"restored\": [        (array) JSON array of restored objects.\n"
            "    {\n"
            "      \"serial\": \"xxx\"  (string) Serial in hex format.\n"
            "    }\n"
            "    ,...\n"
            "  ]\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("resetspentzerocoin", "") + HelpExampleRpc("resetspentzerocoin", ""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    CWalletDB walletdb(pwalletMain->strWalletFile);
    CzPHRTracker* zphrTracker = pwalletMain->zphrTracker.get();
    set<CMintMeta> setMints = zphrTracker->ListMints(false, false, false);
    list<CZerocoinSpend> listSpends = walletdb.ListSpentCoins();
    list<CZerocoinSpend> listUnconfirmedSpends;

    for (CZerocoinSpend spend : listSpends) {
        CTransaction tx;
        uint256 hashBlock = 0;
        if (!GetTransaction(spend.GetTxHash(), tx, hashBlock)) {
            listUnconfirmedSpends.push_back(spend);
            continue;
        }

        //no confirmations
        if (hashBlock == 0)
            listUnconfirmedSpends.push_back(spend);
    }

    UniValue objRet(UniValue::VOBJ);
    UniValue arrRestored(UniValue::VARR);
    for (CZerocoinSpend spend : listUnconfirmedSpends) {
        for (auto& meta : setMints) {
            if (meta.hashSerial == GetSerialHash(spend.GetSerial())) {
                zphrTracker->SetPubcoinNotUsed(meta.hashPubcoin);
                walletdb.EraseZerocoinSpendSerialEntry(spend.GetSerial());
                RemoveSerialFromDB(spend.GetSerial());
                UniValue obj(UniValue::VOBJ);
                obj.push_back(Pair("serial", spend.GetSerial().GetHex()));
                arrRestored.push_back(obj);
                continue;
            }
        }
    }

    objRet.push_back(Pair("restored", arrRestored));
    return objRet;
}

UniValue getarchivedzerocoin(const UniValue& params, bool fHelp)
{
    if(fHelp || params.size() != 0)
        throw runtime_error(
            "getarchivedzerocoin\n"
            "\nDisplay zerocoins that were archived because they were believed to be orphans.\n"
            "Provides enough information to recover mint if it was incorrectly archived.\n" +
            HelpRequiringPassphrase() + "\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"txid\": \"xxx\",           (string) Transaction ID for archived mint.\n"
            "    \"denomination\": amount,  (numeric) Denomination value.\n"
            "    \"serial\": \"xxx\",         (string) Serial number in hex format.\n"
            "    \"randomness\": \"xxx\",     (string) Hex encoded randomness.\n"
            "    \"pubcoin\": \"xxx\"         (string) Pubcoin in hex format.\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("getarchivedzerocoin", "") + HelpExampleRpc("getarchivedzerocoin", ""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    EnsureWalletIsUnlocked();

    CWalletDB walletdb(pwalletMain->strWalletFile);
    list<CZerocoinMint> listMints = walletdb.ListArchivedZerocoins();
    list<CDeterministicMint> listDMints = walletdb.ListArchivedDeterministicMints();

    UniValue arrRet(UniValue::VARR);
    for (const CZerocoinMint& mint : listMints) {
        UniValue objMint(UniValue::VOBJ);
        objMint.push_back(Pair("txid", mint.GetTxHash().GetHex()));
        objMint.push_back(Pair("denomination", ValueFromAmount(mint.GetDenominationAsAmount())));
        objMint.push_back(Pair("serial", mint.GetSerialNumber().GetHex()));
        objMint.push_back(Pair("randomness", mint.GetRandomness().GetHex()));
        objMint.push_back(Pair("pubcoin", mint.GetValue().GetHex()));
        arrRet.push_back(objMint);
    }

    for (const CDeterministicMint& dMint : listDMints) {
        UniValue objDMint(UniValue::VOBJ);
        objDMint.push_back(Pair("txid", dMint.GetTxHash().GetHex()));
        objDMint.push_back(Pair("denomination", ValueFromAmount(libzerocoin::ZerocoinDenominationToAmount(dMint.GetDenomination()))));
        objDMint.push_back(Pair("serialhash", dMint.GetSerialHash().GetHex()));
        objDMint.push_back(Pair("pubcoinhash", dMint.GetPubcoinHash().GetHex()));
        objDMint.push_back(Pair("seedhash", dMint.GetSeedHash().GetHex()));
        objDMint.push_back(Pair("count", (int64_t)dMint.GetCount()));
        arrRet.push_back(objDMint);
    }


    return arrRet;
}

UniValue exportzerocoins(const UniValue& params, bool fHelp)
{
    if(fHelp || params.empty() || params.size() > 2)
        throw runtime_error(
            "exportzerocoins include_spent ( denomination )\n"
            "\nExports zerocoin mints that are held by this wallet.dat\n" +
            HelpRequiringPassphrase() + "\n"

            "\nArguments:\n"
            "1. \"include_spent\"        (bool, required) Include mints that have already been spent\n"
            "2. \"denomination\"         (integer, optional) Export a specific denomination of zPHR\n"

            "\nResult:\n"
            "[                   (array of json object)\n"
            "  {\n"
            "    \"d\": n,         (numeric) the mint's zerocoin denomination \n"
            "    \"p\": \"pubcoin\", (string) The public coin\n"
            "    \"s\": \"serial\",  (string) The secret serial number\n"
            "    \"r\": \"random\",  (string) The secret random number\n"
            "    \"t\": \"txid\",    (string) The txid that the coin was minted in\n"
            "    \"h\": n,         (numeric) The height the tx was added to the blockchain\n"
            "    \"u\": used,      (boolean) Whether the mint has been spent\n"
            "    \"v\": version,   (numeric) The version of the zPHR\n"
            "    \"k\": \"privkey\"  (string) The zPHR private key (V2+ zPHR only)\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("exportzerocoins", "false 5") + HelpExampleRpc("exportzerocoins", "false 5"));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    EnsureWalletIsUnlocked();

    CWalletDB walletdb(pwalletMain->strWalletFile);

    bool fIncludeSpent = params[0].get_bool();
    libzerocoin::CoinDenomination denomination = libzerocoin::ZQ_ERROR;
    if (params.size() == 2)
        denomination = libzerocoin::IntToZerocoinDenomination(params[1].get_int());

    CzPHRTracker* zphrTracker = pwalletMain->zphrTracker.get();
    set<CMintMeta> setMints = zphrTracker->ListMints(!fIncludeSpent, false, false);

    UniValue jsonList(UniValue::VARR);
    for (const CMintMeta& meta : setMints) {
        if (denomination != libzerocoin::ZQ_ERROR && denomination != meta.denom)
            continue;

        CZerocoinMint mint;
        if (!pwalletMain->GetMint(meta.hashSerial, mint))
            continue;

        UniValue objMint(UniValue::VOBJ);
        objMint.push_back(Pair("d", mint.GetDenomination()));
        objMint.push_back(Pair("p", mint.GetValue().GetHex()));
        objMint.push_back(Pair("s", mint.GetSerialNumber().GetHex()));
        objMint.push_back(Pair("r", mint.GetRandomness().GetHex()));
        objMint.push_back(Pair("t", mint.GetTxHash().GetHex()));
        objMint.push_back(Pair("h", mint.GetHeight()));
        objMint.push_back(Pair("u", mint.IsUsed()));
        objMint.push_back(Pair("v", mint.GetVersion()));
        if (mint.GetVersion() >= 2) {
            objMint.push_back(Pair("k", HexStr(mint.GetPrivKey())));
        }
        jsonList.push_back(objMint);
    }

    return jsonList;
}

UniValue importzerocoins(const UniValue& params, bool fHelp)
{
    if(fHelp || params.size() == 0)
        throw runtime_error(
            "importzerocoins importdata \n"
            "\n[{\"d\":denomination,\"p\":\"pubcoin_hex\",\"s\":\"serial_hex\",\"r\":\"randomness_hex\",\"t\":\"txid\",\"h\":height, \"u\":used},{\"d\":...}]\n"
            "\nImport zerocoin mints.\n"
            "Adds raw zerocoin mints to the wallet.dat\n"
            "Note it is recommended to use the json export created from the exportzerocoins RPC call\n" +
            HelpRequiringPassphrase() + "\n"

            "\nArguments:\n"
            "1. \"importdata\"    (string, required) A json array of json objects containing zerocoin mints\n"

            "\nResult:\n"
            "{\n"
            "  \"added\": n,        (numeric) The quantity of zerocoin mints that were added\n"
            "  \"value\": amount    (numeric) The total zPHR value of zerocoin mints that were added\n"
            "}\n"

            "\nExamples\n" +
            HelpExampleCli("importzerocoins", "\'[{\"d\":100,\"p\":\"mypubcoin\",\"s\":\"myserial\",\"r\":\"randomness_hex\",\"t\":\"mytxid\",\"h\":104923, \"u\":false},{\"d\":5,...}]\'") +
            HelpExampleRpc("importzerocoins", "[{\"d\":100,\"p\":\"mypubcoin\",\"s\":\"myserial\",\"r\":\"randomness_hex\",\"t\":\"mytxid\",\"h\":104923, \"u\":false},{\"d\":5,...}]"));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    EnsureWalletIsUnlocked();

    RPCTypeCheck(params, list_of(UniValue::VARR)(UniValue::VOBJ));
    UniValue arrMints = params[0].get_array();
    CWalletDB walletdb(pwalletMain->strWalletFile);

    int count = 0;
    CAmount nValue = 0;
    for (unsigned int idx = 0; idx < arrMints.size(); idx++) {
        const UniValue &val = arrMints[idx];
        const UniValue &o = val.get_obj();

        const UniValue& vDenom = find_value(o, "d");
        if (!vDenom.isNum())
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, missing d key");
        int d = vDenom.get_int();
        if (d < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, d must be positive");

        libzerocoin::CoinDenomination denom = libzerocoin::IntToZerocoinDenomination(d);
        CBigNum bnValue = 0;
        bnValue.SetHex(find_value(o, "p").get_str());
        CBigNum bnSerial = 0;
        bnSerial.SetHex(find_value(o, "s").get_str());
        CBigNum bnRandom = 0;
        bnRandom.SetHex(find_value(o, "r").get_str());
        uint256 txid(find_value(o, "t").get_str());

        int nHeight = find_value(o, "h").get_int();
        if (nHeight < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, h must be positive");

        bool fUsed = find_value(o, "u").get_bool();
        //Assume coin is version 1 unless it has the version actually set
        uint8_t nVersion = 1;
        const UniValue& vVersion = find_value(o, "v");
        if (vVersion.isNum())
            nVersion = static_cast<uint8_t>(vVersion.get_int());

        //Set the privkey if applicable
        CPrivKey privkey;
        if (nVersion >= libzerocoin::PrivateCoin::PUBKEY_VERSION) {
            std::string strPrivkey = find_value(o, "k").get_str();
            CKey key;
            uint256 nPrivKey(strPrivkey);
            key.Set(nPrivKey.begin(), nPrivKey.end(), true);
            if (!key.IsValid())
                return JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "privkey is not valid");
            privkey = key.GetPrivKey();
        }

        CZerocoinMint mint(denom, bnValue, bnRandom, bnSerial, fUsed, nVersion, &privkey);
        mint.SetTxHash(txid);
        mint.SetHeight(nHeight);
        pwalletMain->zphrTracker->Add(mint, true);
        count++;
        nValue += libzerocoin::ZerocoinDenominationToAmount(denom);
    }

    UniValue ret(UniValue::VOBJ);
    ret.push_back(Pair("added", count));
    ret.push_back(Pair("value", ValueFromAmount(nValue)));
    return ret;
}

UniValue reconsiderzerocoins(const UniValue& params, bool fHelp)
{
    if(fHelp || !params.empty())
        throw runtime_error(
            "reconsiderzerocoins\n"
            "\nCheck archived zPHR list to see if any mints were added to the blockchain.\n" +
            HelpRequiringPassphrase() + "\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"txid\" : \"xxx\",           (string) the mint's zerocoin denomination \n"
            "    \"denomination\" : amount,  (numeric) the mint's zerocoin denomination\n"
            "    \"pubcoin\" : \"xxx\",        (string) The mint's public identifier\n"
            "    \"height\" : n              (numeric) The height the tx was added to the blockchain\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples\n" +
            HelpExampleCli("reconsiderzerocoins", "") + HelpExampleRpc("reconsiderzerocoins", ""));

    LOCK2(cs_main, pwalletMain->cs_wallet);

    EnsureWalletIsUnlocked();

    list<CZerocoinMint> listMints;
    list<CDeterministicMint> listDMints;
    pwalletMain->ReconsiderZerocoins(listMints, listDMints);

    UniValue arrRet(UniValue::VARR);
    for (const CZerocoinMint& mint : listMints) {
        UniValue objMint(UniValue::VOBJ);
        objMint.push_back(Pair("txid", mint.GetTxHash().GetHex()));
        objMint.push_back(Pair("denomination", ValueFromAmount(mint.GetDenominationAsAmount())));
        objMint.push_back(Pair("pubcoin", mint.GetValue().GetHex()));
        objMint.push_back(Pair("height", mint.GetHeight()));
        arrRet.push_back(objMint);
    }

    for (const CDeterministicMint& dMint : listDMints) {
         UniValue objMint(UniValue::VOBJ);
        objMint.push_back(Pair("txid", dMint.GetTxHash().GetHex()));
        objMint.push_back(Pair("denomination", FormatMoney(libzerocoin::ZerocoinDenominationToAmount(dMint.GetDenomination()))));
        objMint.push_back(Pair("pubcoinhash", dMint.GetPubcoinHash().GetHex()));
        objMint.push_back(Pair("height", dMint.GetHeight()));
        arrRet.push_back(objMint);
    }

    return arrRet;
}

// ppcoin: make a public-private key pair
UniValue makekeypair(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "makekeypair [prefix]\n"
            "Make a public/private key pair.\n"
            "[prefix] is optional preferred prefix for the public key.\n");

    string strPrefix = "";
    if (params.size() > 0)
        strPrefix = params[0].get_str();
 
    CKey key;
    key.MakeNewKey(false);

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("PrivateKey", CBitcoinSecret(key).ToString()));
    result.push_back(Pair("PublicKey", HexStr(key.GetPubKey().Raw())));
    return result;
}

UniValue setzphrseed(const UniValue& params, bool fHelp)
{
    if(fHelp || params.size() != 1)
        throw runtime_error(
            "setzphrseed \"seed\"\n"
            "\nSet the wallet's deterministic zphr seed to a specific value.\n" +
            HelpRequiringPassphrase() + "\n"

            "\nArguments:\n"
            "1. \"seed\"        (string, required) The deterministic zphr seed.\n"

            "\nResult\n"
            "\"success\" : b,  (boolean) Whether the seed was successfully set.\n"

            "\nExamples\n" +
            HelpExampleCli("setzphrseed", "63f793e7895dd30d99187b35fbfb314a5f91af0add9e0a4e5877036d1e392dd5") +
            HelpExampleRpc("setzphrseed", "63f793e7895dd30d99187b35fbfb314a5f91af0add9e0a4e5877036d1e392dd5"));

    EnsureWalletIsUnlocked();

    uint256 seed;
    seed.SetHex(params[0].get_str());

    CzPHRWallet* zwallet = pwalletMain->getZWallet();
    bool fSuccess = zwallet->SetMasterSeed(seed, true);
    if (fSuccess)
        zwallet->SyncWithChain();

    UniValue ret(UniValue::VOBJ);
    ret.push_back(Pair("success", fSuccess));

    return ret;
}

UniValue getzphrseed(const UniValue& params, bool fHelp)
{
    if(fHelp || !params.empty())
        throw runtime_error(
            "getzphrseed\n"
            "\nCheck archived zPHR list to see if any mints were added to the blockchain.\n" +
            HelpRequiringPassphrase() + "\n"

            "\nResult\n"
            "\"seed\" : s,  (string) The deterministic zPHR seed.\n"

            "\nExamples\n" +
            HelpExampleCli("getzphrseed", "") + HelpExampleRpc("getzphrseed", ""));

    EnsureWalletIsUnlocked();

    CzPHRWallet* zwallet = pwalletMain->getZWallet();
    uint256 seed = zwallet->GetMasterSeed();

    UniValue ret(UniValue::VOBJ);
    ret.push_back(Pair("seed", seed.GetHex()));

    return ret;
}

UniValue generatemintlist(const UniValue& params, bool fHelp)
{
    if(fHelp || params.size() != 2)
        throw runtime_error(
            "generatemintlist\n"
            "\nShow mints that are derived from the deterministic zPHR seed.\n" +
            HelpRequiringPassphrase() + "\n"

            "\nArguments\n"
            "1. \"count\"  : n,  (numeric) Which sequential zPHR to start with.\n"
            "2. \"range\"  : n,  (numeric) How many zPHR to generate.\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"count\": n,          (numeric) Deterministic Count.\n"
            "    \"value\": \"xxx\",    (string) Hex encoded pubcoin value.\n"
            "    \"randomness\": \"xxx\",   (string) Hex encoded randomness.\n"
            "    \"serial\": \"xxx\"        (string) Hex encoded Serial.\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples\n" +
            HelpExampleCli("generatemintlist", "1, 100") + HelpExampleRpc("generatemintlist", "1, 100"));

    EnsureWalletIsUnlocked();

    int nCount = params[0].get_int();
    int nRange = params[1].get_int();
    CzPHRWallet* zwallet = pwalletMain->zwalletMain;

    UniValue arrRet(UniValue::VARR);
    for (int i = nCount; i < nCount + nRange; i++) {
        libzerocoin::CoinDenomination denom = libzerocoin::CoinDenomination::ZQ_ONE;
        libzerocoin::PrivateCoin coin(Params().Zerocoin_Params(), denom, false);
        CDeterministicMint dMint;
        zwallet->GenerateMint(i, denom, coin, dMint);
        UniValue obj(UniValue::VOBJ);
        obj.push_back(Pair("count", i));
        obj.push_back(Pair("value", coin.getPublicCoin().getValue().GetHex()));
        obj.push_back(Pair("randomness", coin.getRandomness().GetHex()));
        obj.push_back(Pair("serial", coin.getSerialNumber().GetHex()));
        arrRet.push_back(obj);
    }

    return arrRet;
}

UniValue dzphrstate(const UniValue& params, bool fHelp) {
    if (fHelp || params.size() != 0)
        throw runtime_error(
                "dzphrstate\n"
                        "\nThe current state of the mintpool of the deterministic zPHR wallet.\n" +
                HelpRequiringPassphrase() + "\n"

                        "\nExamples\n" +
                HelpExampleCli("mintpoolstatus", "") + HelpExampleRpc("mintpoolstatus", ""));

    CzPHRWallet* zwallet = pwalletMain->zwalletMain;
    UniValue obj(UniValue::VOBJ);
    int nCount, nCountLastUsed;
    zwallet->GetState(nCount, nCountLastUsed);
    obj.push_back(Pair("dzphr_count", nCount));
    obj.push_back(Pair("mintpool_count", nCountLastUsed));

    return obj;
}


void static SearchThread(CzPHRWallet* zwallet, int nCountStart, int nCountEnd)
{
    LogPrintf("%s: start=%d end=%d\n", __func__, nCountStart, nCountEnd);
    CWalletDB walletDB(pwalletMain->strWalletFile);
    try {
        uint256 seedMaster = zwallet->GetMasterSeed();
        uint256 hashSeed = Hash(seedMaster.begin(), seedMaster.end());
        for(int i = nCountStart; i < nCountEnd; i++) {
            boost::this_thread::interruption_point();
            CDataStream ss(SER_GETHASH, 0);
            ss << seedMaster << i;
            uint512 zerocoinSeed = Hash512(ss.begin(), ss.end());

            CBigNum bnValue;
            CBigNum bnSerial;
            CBigNum bnRandomness;
            CKey key;
            zwallet->SeedToZPHR(zerocoinSeed, bnValue, bnSerial, bnRandomness, key);

            uint256 hashPubcoin = GetPubCoinHash(bnValue);
            zwallet->AddToMintPool(make_pair(hashPubcoin, i), true);
            walletDB.WriteMintPoolPair(hashSeed, hashPubcoin, i);
        }
    } catch (std::exception& e) {
        LogPrintf("SearchThread() exception");
    } catch (...) {
        LogPrintf("SearchThread() exception");
    }
}

UniValue searchdzphr(const UniValue& params, bool fHelp)
{
    if(fHelp || params.size() != 3)
        throw runtime_error(
            "searchdzphr\n"
            "\nMake an extended search for deterministically generated zPHR that have not yet been recognized by the wallet.\n" +
            HelpRequiringPassphrase() + "\n"

            "\nArguments\n"
            "1. \"count\"       (numeric) Which sequential zPHR to start with.\n"
            "2. \"range\"       (numeric) How many zPHR to generate.\n"
            "3. \"threads\"     (numeric) How many threads should this operation consume.\n"

            "\nExamples\n" +
            HelpExampleCli("searchdzphr", "1, 100, 2") + HelpExampleRpc("searchdzphr", "1, 100, 2"));

    EnsureWalletIsUnlocked();

    int nCount = params[0].get_int();
    if (nCount < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Count cannot be less than 0");

    int nRange = params[1].get_int();
    if (nRange < 1)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Range has to be at least 1");

    int nThreads = params[2].get_int();

    CzPHRWallet* zwallet = pwalletMain->zwalletMain;

    boost::thread_group* dzphrThreads = new boost::thread_group();
    int nRangePerThread = nRange / nThreads;

    int nPrevThreadEnd = nCount - 1;
    for (int i = 0; i < nThreads; i++) {
        int nStart = nPrevThreadEnd + 1;;
        int nEnd = nStart + nRangePerThread;
        nPrevThreadEnd = nEnd;
        dzphrThreads->create_thread(boost::bind(&SearchThread, zwallet, nStart, nEnd));
    }

    dzphrThreads->join_all();

    zwallet->RemoveMintsFromPool(pwalletMain->zphrTracker->GetSerialHashes());
    zwallet->SyncWithChain(false);

    //todo: better response
    return "done";
}
