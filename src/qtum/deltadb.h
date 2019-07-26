#ifndef DELTADB_H
#define DELTADB_H

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
#include "qtumstate.h"
#include "neutron-c.h"
#include "neutron.h"
#include <chainparams.h>


struct DeltaCheckpoint{
    //all state changes in current checkpoint
    std::unordered_map<std::string, std::vector<uint8_t>> deltas;
    //all vins spent in transfers within the current checkpoint
    std::set<COutPoint> spentVins;
    //all addresses with modified balances in the current checkpoint
    //note: do not use this as a cache. It should only be used to track modified balances
    std::map<UniversalAddress, uint64_t> balances;

    UniValue toJSON();
};


struct ContractExecutionResult{
    uint256 blockHash;
    uint32_t blockHeight;
    COutPoint tx;
    uint64_t usedGas;
    CAmount refundSender = 0;
    ContractStatus status = ContractStatus::CodeError();
    CMutableTransaction transferTx;
    bool commitState;
    DeltaCheckpoint modifiedData;
    std::map<std::string, std::string> events;
    std::vector<ContractExecutionResult> callResults;
    UniversalAddress address;

    UniValue toJSON(){
        UniValue result(UniValue::VOBJ);
        result.pushKV("block-hash", blockHash.GetHex());
        result.pushKV("blockh-height", (uint64_t) blockHeight);
        result.pushKV("tx-hash", tx.hash.GetHex());
        result.pushKV("tx-n", (uint64_t) tx.n);
        //result.push_back(Pair("address", address.asBitcoinAddress().ToString()));
        result.pushKV("used-gas", usedGas);
        result.pushKV("sender-refund", refundSender);
        result.pushKV("status", status.toString());
        result.pushKV("status-code", status.getCode());
        result.pushKV("transfer-txid", transferTx.GetHash().ToString());
        result.pushKV("commit-state", commitState);
        result.pushKV("modified-state", modifiedData.toJSON());
        UniValue returnjson(UniValue::VOBJ);
        for(auto& kvp : events){
            returnjson.pushKV(parseABIToString(kvp.first), parseABIToString(kvp.second));
        }
        result.pushKV("events", returnjson);
        UniValue calls(UniValue::VARR);
        for(auto& res : callResults){
            //calls.pushKV(res.toJSON());
        }
        result.pushKV("calls", calls);
        return result;
    }
};


//EventDB is an optional database which stores all contract execution result
//and indexes them into a searchable leveldb database
class EventDB : public CDBWrapper
{
    std::vector<ContractExecutionResult> results;

    //goes through results and builds a map of all addresses and the coutpoints that mention them
    std::map<UniversalAddress, std::vector<COutPoint>> buildAddressMap();
public:
	EventDB(size_t nCacheSize, bool fMemory, bool fWipe) : CDBWrapper(GetDataDir() / "eventDB", nCacheSize, fMemory, fWipe) { }	
	EventDB() : CDBWrapper(GetDataDir() / "eventDB", 4, false, false) { }
	~EventDB() {    }
    //adds a result to the buffer
    //used during block validation after each contract execution
    bool addResult(const ContractExecutionResult& result);
    //commits all the buffers to the database as the block height specified
    //used when the block is fully validated
    bool commit(uint32_t height);
    //reverts all in progress data in the buffers
    //only used in case of block validation failure
    bool revert(); 
    //erases a block's contract results and all associated indexes
    //used when disconnecting a block
    bool eraseBlock(uint32_t height);

    //result functions:

    //returns list of ContractExecutionResults that touch address at specified block height
    //address can be set to unknown version in order to not apply an address filter
    //note, for now this returns ContractExecutionResult as a JSON string
    std::vector<std::string> getResults(UniversalAddress address, int minheight, int maxheight, int maxresults);

    //returns results in descending order, ie, results are ordered from maxheight to minheight
    std::vector<std::string> getDescendingResults(UniversalAddress address, int minheight, int maxheight, int maxresults);

    //returns true and sets result if a result is found for the specified vout
    //bool getResult(COutPoint vout, ContractExecutionResult &result);
};

class DeltaDB : public CDBWrapper
{

public:
	DeltaDB(size_t nCacheSize, bool fMemory, bool fWipe) : CDBWrapper(GetDataDir() / "deltaDB", nCacheSize, fMemory, fWipe) { }	
	DeltaDB() : CDBWrapper(GetDataDir() / "deltaDB", 4, false, false) { }
	~DeltaDB() {    }
};



class DeltaDBWrapper{
    DeltaDB* db;
    //0 is 0th checkpoint, 1 is 1st checkpoint etc
    std::vector<DeltaCheckpoint> checkpoints;
    DeltaCheckpoint *current;

    std::set<UniversalAddress> hasNoAAL; //a cache to keep track of which addresses have no AAL data in the disk-database
    COutPoint initialCoins; //initial coins sent by origin tx
    UniversalAddress initialCoinsReceiver;
public:
    DeltaDBWrapper(DeltaDB* db_) : db(db_){
        checkpoint(); //this will add the initial "0" checkpoint and set all pointers
    }

    void commit(); //commits everything to disk
    int checkpoint(); //advanced to next checkpoint; returns new checkpoint number
    int revertCheckpoint(); //Discard latest checkpoint and revert to previous checkpoint; returns new checkpoint number
    void condenseAllCheckpoints(); //condences all outstanding checkpoints to 0th
    void condenseSingleCheckpoint(); //condenses only the latest checkpoint into the previous
    void setInitialCoins(UniversalAddress a, COutPoint vout, uint64_t value); //initial coins sent with origin tx
    //AAL access
    uint64_t getBalance(UniversalAddress a);
    bool transfer(UniversalAddress from, UniversalAddress to, uint64_t value);

    DeltaCheckpoint getLatestModifiedState(){
        return *current;
    }

    CTransaction createCondensingTx();

    /*************** Live data *****************/
    /* newest data associated with the contract. */
    bool writeState(UniversalAddress address, valtype key, valtype value);
    bool readState(UniversalAddress address, valtype key, valtype& value);

    /* bytecode of the contract. */
    bool writeByteCode(UniversalAddress address,valtype byteCode);
    bool readByteCode(UniversalAddress address,valtype& byteCode);


    /* data updated point of the keys in a contract. */
    bool writeUpdatedKey(UniversalAddress address, valtype key, unsigned int blk_num, uint256 blk_hash);
    bool readUpdatedKey(UniversalAddress address, valtype key, unsigned int &blk_num, uint256 &blk_hash);


    /*************** Change log data *****************/
    /* raw name of the keys in a contract. */
    bool writeRawKey(UniversalAddress address,      valtype key, valtype rawkey);
    bool readRawKey(UniversalAddress address,     valtype key, valtype &rawkey);

    /* current iterator of the keys in a contract. */
    bool writeCurrentIterator(UniversalAddress address,      valtype key, uint64_t iterator);
    bool readCurrentIterator(UniversalAddress address,      valtype key, uint64_t &iterator);

    /* data of the keys in a contract, indexed by iterator. */
    bool writeStateWithIterator(UniversalAddress address,        valtype key, uint64_t iterator, valtype value);
    bool readStateWithIterator(UniversalAddress address,        valtype key, uint64_t iterator, valtype &value);

    /* info of the keys in a contract, indexed by iterator. */
    bool writeInfoWithIterator(UniversalAddress address,        valtype key, uint64_t iterator, unsigned int blk_num, uint256 blk_hash, uint256 txid, unsigned int vout);
    bool readInfoWithIterator(UniversalAddress address,        valtype key, uint64_t iterator, unsigned int &blk_num, uint256 &blk_hash, uint256 &txid, unsigned int &vout);

    /* Oldest iterator and the respect block info that exists in the changelog database. */
    bool writeOldestIterator(UniversalAddress address,        valtype key, uint64_t iterator, unsigned int blk_num, uint256 blk_hash);
    bool readOldestIterator(UniversalAddress address,        valtype key, uint64_t &iterator, unsigned int &blk_num, uint256 &blk_hash);


private:
    //AAL is more complicated, so don't allow direct access
    bool writeAalData(UniversalAddress address, uint256 txid, unsigned int vout, uint64_t balance);
    bool readAalData(UniversalAddress address, uint256 &txid, unsigned int &vout, uint64_t &balance);
    bool removeAalData(UniversalAddress address);
    bool Write(valtype K, valtype V);
    bool Read(valtype K, valtype& V);
    bool Write(valtype K, uint64_t V);
    bool Read(valtype K, uint64_t& V);
};

//TODO: don't use a global for this so we can execute things in parallel
extern DeltaDB* pdeltaDB;
extern EventDB *peventdb;



#endif
