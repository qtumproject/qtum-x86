#ifndef NEUTRON_H
#define NEUTRON_H

#include <unordered_map>

#include <univalue.h>
#include <libethcore/Transaction.h>
#include <libethereum/Transaction.h>
#include <coins.h>
#include <script/interpreter.h>
#include <libevm/ExtVMFace.h>
#include <dbwrapper.h>
#include <util.h>
#include <uint256.h>
#include <base58.h>
#include <univalue.h>
#include "qtumstate.h"
#include "neutron-c.h"

std::string parseABIToString(std::string abidata);

class ContractStatus{
    int status;
    std::string statusString;
    std::string extraString;
    ContractStatus(){}
    ContractStatus(int code, std::string str, std::string extra) : status(code), statusString(str), extraString(extra) {}

    public:

    int getCode(){
        return status;
    }
    bool isError(){
        return status != 0;
    }
    std::string toString(){
        if(extraString == ""){
            return statusString;
        }else{
            return statusString + "; Extra info: " + extraString;
        }
    }

    static ContractStatus Success(std::string extra=""){
        return ContractStatus(0, "Success", extra);
    }
    static ContractStatus OutOfGas(std::string extra=""){
        return ContractStatus(1, "Out of gas", extra);
    }
    static ContractStatus CodeError(std::string extra=""){
        return ContractStatus(2, "Unhandled exception triggered in execution", extra);
    }
    static ContractStatus DoesntExist(std::string extra=""){
        return ContractStatus(3, "Contract does not exist", extra);
    }
    static ContractStatus ReturnedError(std::string extra=""){
        return ContractStatus(4, "Contract executed successfully but returned an error code", extra);
    }
    static ContractStatus ErrorWithCommit(std::string extra=""){
        return ContractStatus(5, "Contract chose to commit state, but returned an error code", extra);
    }
    static ContractStatus InternalError(std::string extra=""){
        return ContractStatus(6, "Internal error with contract execution", extra);
    }

};



struct UniversalAddress{
    UniversalAddress(){
        version = AddressVersion::UNKNOWN;
        convertData();
    }
    UniversalAddress(AddressVersion v, const std::vector<uint8_t> &d)
    : version(v), data(d.begin(), d.end()) {
        convertData();
    }
    UniversalAddress(AddressVersion v, const unsigned char* begin, const unsigned char* end)
    : version(v), data(begin, end) {
        convertData();
    }
    /*
    UniversalAddress(BitcoinAddress &address){
        fromBitcoinAddress(address);
    }*/
    UniversalAddress(const UniversalAddressABI &abi)
    : version((AddressVersion)abi.version), data(&abi.data[0], &abi.data[sizeof(abi.data)])
    {
        convertData();
    }

    bool operator<(const UniversalAddress& a) const{
        return version < a.version || data < a.data;
    }
    bool operator==(const UniversalAddress& a) const{
        return version == a.version && data == a.data;
    }
    bool operator!=(const UniversalAddress& a) const{
        return !(a == *this);
    }
    UniversalAddressABI toAbi() const{
        UniversalAddressABI abi;
        toAbi(abi);
        return abi;
    }
    void toAbi(UniversalAddressABI &abi) const{
        abi.version = (uint32_t) version;
        memset(&abi.data[0], 0, ADDRESS_DATA_SIZE);
        memcpy(&abi.data[0], data.data(), data.size());
    }
    //converts to flat data suitable for passing to contract code
    std::vector<uint8_t> toFlatData() const{
        std::vector<uint8_t> tmp;
        tmp.resize(sizeof(UniversalAddressABI));
        UniversalAddressABI abi = toAbi();
        memcpy(&tmp.front(), &abi, tmp.size());
        return tmp;
    }

    //converts to flat data suitable for placing in blockchain
    std::vector<uint8_t> toChainData() const{
        std::vector<uint8_t> tmp;
        size_t sz = getRealAddressSize(version);
        tmp.resize(sz);
        memcpy(&tmp.front(), data.data(), sz);
        return tmp;
    }
    //hasAAL means this type of address should have an AAL record in DeltaDB
    bool hasAAL() const{
        return version == AddressVersion::EVM ||
               version == AddressVersion::X86;
    }
    bool isContract() const{
        return hasAAL();
    }

    static UniversalAddress FromScript(const CScript& script);
    static UniversalAddress FromOutput(AddressVersion v, uint256 txid, uint32_t vout);
/*
    CBitcoinAddress asBitcoinAddress() const{
        CBitcoinAddress a(convertUniversalVersion(version), data.data(), getRealAddressSize(version));
        return a;
    }
    
    void fromBitcoinAddress(CBitcoinAddress &address) {
        version = convertBitcoinVersion(address.getVersion());
        data = address.getData();
        convertData();
    }
*/
    void convertData(){
        data.resize(ADDRESS_DATA_SIZE);
    }

    bool isNull(){
        //todo, later check data for 0
        return version == AddressVersion::UNKNOWN;
    }

    static AddressVersion convertBitcoinVersion(std::vector<unsigned char> version){
        if(version.size() == 0){
            return AddressVersion::UNKNOWN;
        }
        unsigned char v = version[0];
        if(Params().Base58Prefix(CChainParams::PUBKEY_ADDRESS)[0] == v){
            return AddressVersion::PUBKEYHASH;
        }else if(Params().Base58Prefix(CChainParams::SCRIPT_ADDRESS)[0] == v){
            return AddressVersion::SCRIPTHASH;
        }else if(Params().Base58Prefix(CChainParams::EVM_ADDRESS)[0] == v) {
            return AddressVersion::EVM;
        }else if(Params().Base58Prefix(CChainParams::NEUTRON_ADDRESS)[0] == v) {
            return AddressVersion::X86;
        }else {
            return AddressVersion::UNKNOWN;
        }
    }
    static std::vector<unsigned char> convertUniversalVersion(AddressVersion v){
        if(v == AddressVersion::EVM){
            return Params().Base58Prefix(CChainParams::EVM_ADDRESS);
        }else if(v == AddressVersion::X86){
            return Params().Base58Prefix(CChainParams::NEUTRON_ADDRESS);
        }else if(v == AddressVersion::PUBKEYHASH){
            return Params().Base58Prefix(CChainParams::PUBKEY_ADDRESS);
        }else if(v == AddressVersion::SCRIPTHASH){
            return Params().Base58Prefix(CChainParams::SCRIPT_ADDRESS);
        }else{
            return std::vector<unsigned char>();
        }
    }
    static size_t getRealAddressSize(AddressVersion v){
        switch(v){
            case AddressVersion::EVM:
            case AddressVersion::X86:
            case AddressVersion::PUBKEYHASH:
            case AddressVersion::LEGACYEVM:
            case AddressVersion::SCRIPTHASH:
            return 20;
            default:
            return ADDRESS_DATA_SIZE; //TODO this will change later
        }
    }

    

    AddressVersion version;
    std::vector<uint8_t> data;
};

namespace std
{
  template<>
    struct hash<UniversalAddress>
    {
      size_t
      operator()(const UniversalAddress& a) const
      {
          auto tmp = a.toFlatData();
          std::string s(tmp.begin(), tmp.end());
        return hash<string>()(s);
      }
    };
}

struct ContractOutput{
    VersionVM version;
    uint64_t value, gasPrice, gasLimit;
    UniversalAddress address;
    std::vector<uint8_t> data;
    UniversalAddress sender;
    COutPoint vout;
    bool OpCreate;
};

class ContractOutputParser{
public:

    ContractOutputParser(const CTransaction &tx, uint32_t vout, const CCoinsViewCache* v = NULL, const std::vector<CTransactionRef>* blockTxs = NULL)
            : tx(tx), nvout(vout), view(v), blockTransactions(blockTxs) {}
    bool parseOutput(ContractOutput& output);
    UniversalAddress getSenderAddress();

private:
    bool receiveStack(const CScript& scriptPubKey);
    const CTransaction &tx;
    const uint32_t nvout;
    const CCoinsViewCache* view;
    const std::vector<CTransactionRef> *blockTransactions;
    std::vector<valtype> stack;
    opcodetype opcode;

};

struct ContractEnvironment{
    uint32_t blockNumber;
    uint64_t blockTime;
    uint64_t difficulty;
    uint64_t gasLimit;
    UniversalAddress blockCreator;
    std::vector<uint256> blockHashes;

    //todo for x86: tx info
};




#endif