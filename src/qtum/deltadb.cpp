#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <util.h>
#include <tinyformat.h>
#include <script/standard.h>
#include <chainparams.h>
#include <validation.h>
#include <pubkey.h>
#include "qtumtransaction.h"
#include <qtum/deltadb.h>
#include <qtum/aalv2.h>
#include <vector>
#include <streams.h>
#include <serialize.h>
#include <uint256.h>
#include <string>

#include <univalue.h>

//key prefixes for each table section in deltadb
static const std::string DELTADB_PREFIX_STATE = "state_";

//prefixes for "state" entries in deltadb
static const char DELTADB_STATE_BYTECODE = 'c';
static const char DELTADB_STATE_KEY = '_';
static const char DELTADB_STATE_AAL = 'a';


bool DeltaDBWrapper::Write(valtype K, valtype V){
    std::string k(K.begin(), K.end());
    current->deltas[k] = V;
    return true;
    //return db->Write(K, V);
}
bool DeltaDBWrapper::Read(valtype K, valtype& V){
    std::string k(K.begin(), K.end());
    //check from the latest checkpoint to the oldest before giving up and going to database
    for(int i = checkpoints.size() - 1; i >= 0; i--){
        auto *check = &checkpoints[i];
        if(current->deltas.find(k) != current->deltas.end()){
            V = current->deltas[k];
            return true;
        }
    }
    if(db == nullptr){
        return false;
    }
    return db->Read(K, V);
}
bool DeltaDBWrapper::Write(valtype K, uint64_t V){
    std::vector<uint8_t> v(sizeof(uint64_t));
    memcpy(v.data(), (void*) &V, sizeof(V));
    return Write(K, v);
}
bool DeltaDBWrapper::Read(valtype K, uint64_t& V){
    std::vector<uint8_t> v(sizeof(uint64_t));
    bool status = Read(K, v);
    memcpy(&V, v.data(), sizeof(V));
    return status;
}

void DeltaDBWrapper::commit() {
    if(db == nullptr){
        //only possible in unit tests
        throw new std::exception();
    }
    CDBBatch b(*db);
    condenseAllCheckpoints(); //make sure we only have one checkpoint to deal with
    for(auto kv : current->deltas){
        if(kv.second.size() == 0){
            b.Erase(kv.first);
        }else{
            b.Write(kv.first, kv.second);
        }
    }

    db->WriteBatch(b, true); //need fSync?

    //clear data stored and reinit
    checkpoints.clear();
    checkpoints.push_back(DeltaCheckpoint());
    current = &checkpoints[0];
    hasNoAAL.clear();
}
int DeltaDBWrapper::checkpoint() {
    checkpoints.push_back(DeltaCheckpoint());
    current = &checkpoints[checkpoints.size() - 1];
    return checkpoints.size() - 1;
}
int DeltaDBWrapper::revertCheckpoint() {
    if(checkpoints.size() == 1){
        return 0;
    }
    checkpoints.pop_back();
    current = &checkpoints[checkpoints.size() - 1];
    return checkpoints.size() - 1;
}

uint64_t DeltaDBWrapper::getBalance(UniversalAddress a) {
    for(int i=checkpoints.size() - 1; i >= 0; i--){
        if(checkpoints[i].balances.find(a) !=checkpoints[i].balances.end()){
            return checkpoints[i].balances[a];
        }
    }
    //not found in modified balances, so go to database
    uint256 txid;
    unsigned int vout;
    uint64_t balance = 0;
    if(readAalData(a, txid, vout, balance)){
        return balance;
    }
    return 0;
}

bool DeltaDBWrapper::transfer(UniversalAddress from, UniversalAddress to, uint64_t value) {
    /*Operation:
     * Look up from and to balances
     * If they are in any checkpoint balance map, then simply use that and put the new balances in the latest checkpoint
     * If either are not located in the checkpoints, then lookup the utxo info from disk
     *      After lookup, add previous utxo to spentVin list and then put new balances into latest checkpoint
     * In this way, spentVin only needs to be touched if the address balance was previously unmodified
     * If the address balance was modified, then the utxo will already be placed into spentVin
     * This works independently of if the outputs are contracts, pubkeyhash, or anything else
     */

    if(value == 0) { return true; }

    uint64_t fromOldBalance = 0;
    bool foundFromBalance = false;
    for (int i = checkpoints.size() - 1; i >= 0; i--) {
        if (checkpoints[i].balances.find(from) != checkpoints[i].balances.end()) {
            fromOldBalance = checkpoints[i].balances[from];
            foundFromBalance = true;
            break;
        }
    }
    uint256 txid;
    unsigned int vout;
    uint64_t balance;
    if(!foundFromBalance){
        //hasn't been touched in this execution, so lookup from database
        if(readAalData(from, txid, vout, balance)){
            fromOldBalance = balance;
            //since there is a vout being used, we should spend it
            current->spentVins.insert(COutPoint(txid, vout));
        }
    }
    if (value > fromOldBalance) {
        //not enough balance to cover transfer
        return false;
    }
    current->balances[from] = fromOldBalance - value;

    if(initialCoinsReceiver == from){
        //if initial coins receiver, then just spend that vin
        //result is either initialCoins is already in currentVins and there is no oldvout
        //OR that both initialCoins and oldvout is already in currentVins
        //OR that initialCoins is not in currentvins and there is no oldvout
        //either way, we don't need to go to database and we must spend the initialCoins vout
        current->spentVins.insert(initialCoins);
    }else{
        //coins are normal, not from initial coins receiver
        if(readAalData(from, txid, vout, balance)){
            current->spentVins.insert(COutPoint(txid, vout));
        }
        //if readAalData is false, then no previous vout to spend
        //So it must be "virtual" transfers without an associated UTXO
        //This can happen when transfering coins from A -> B -> C where B had no UTXO before A's execution
    }
    uint64_t toOldBalance = 0;
    bool foundToBalance = false;
    //now spend the 'to' utxo if it has one so that both from and to UTXOs are spent for condensing
    for (int i = checkpoints.size() - 1; i >= 0; i--) {
        if (checkpoints[i].balances.find(to) != checkpoints[i].balances.end()) {
            toOldBalance = checkpoints[i].balances[to];
            foundToBalance = true;
            break;
        }
    }
    if(!foundToBalance){
        //hasn't been touched in this execution, so lookup from database
        if(readAalData(to, txid, vout, balance)){
            toOldBalance = balance;
            //this vout will need to be spent and condensed into a new single vout
            current->spentVins.insert(COutPoint(txid, vout));
        }
    }
    current->balances[to] = toOldBalance + value;
    return true;
}

void DeltaDBWrapper::setInitialCoins(UniversalAddress a, COutPoint vout, uint64_t value) {
    if(value == 0){
        return;
    }
    if(checkpoints.size() != 1){
        return; //this shouldn't be called other than at the very beginning
    }
    uint256 oldtxid;
    unsigned int oldvout;
    uint64_t oldbalance;
    if(readAalData(a, oldtxid, oldvout, oldbalance)){
        //need to spend old vout and sum balance+value
        current->balances[a] = oldbalance + value;
        //need to spend both old vout and new vout to condense into a single vout
        current->spentVins.insert(COutPoint(oldtxid, oldvout));
        current->spentVins.insert(vout);
    }else{
        //no previous record, so just set balance, no need to spend vin
        current->balances[a] = value;
        //if the contract exec causes a spend, this AAL record will be overwritten
        writeAalData(a, vout.hash, vout.n, value);
    }
    initialCoins = vout;
    initialCoinsReceiver = a;
}

void DeltaDBWrapper::condenseAllCheckpoints() {
    if(checkpoints.size() == 1){
        return;
    }
    //can't refactor this to just do multiple condenseSingle
    //without data being touched several times unnecessarily
    current = &checkpoints[0];
    //apply from 1 to latest
    for(int i=1;i<checkpoints.size();i++){
        DeltaCheckpoint* check = &checkpoints[i];
        for(auto &kv : check->deltas){
            current->deltas[kv.first] = kv.second;
        }
        for(auto &kv : check->balances){
            current->balances[kv.first] = check->balances[kv.first];
        }
        for(auto &v : check->spentVins){
            current->spentVins.insert(v);
        }
    }
    for(int i=1;i<checkpoints.size();i++){
        checkpoints.pop_back();
    }
}

void DeltaDBWrapper::condenseSingleCheckpoint() {
    if(checkpoints.size() == 1){
        return;
    }
    current = &checkpoints[checkpoints.size() - 2]; //set to previous checkpoint
    DeltaCheckpoint* check = &checkpoints[checkpoints.size() - 1]; // set to latest checkpoiint
    for(auto &kv : check->deltas){
        current->deltas[kv.first] = kv.second;
    }
    for(auto &kv : check->balances){
        current->balances[kv.first] = check->balances[kv.first];
    }
    for(auto &v : check->spentVins){
        current->spentVins.insert(v);
    }
    checkpoints.pop_back(); //remove latest
}

CTransaction DeltaDBWrapper::createCondensingTx() {
    //note: this is the new AAL support
    //see qtumstate.cpp for legacy EVM support for the AAL
    condenseAllCheckpoints();
    if(current->spentVins.size() == 0){
        return CTransaction();
    }


    //sort vouts and vins so that the consensus critical order is easy to verify and implementation details can be changed easily
    //vouts are sorted by address
    //vins are sorted by txid + vout number

    std::vector<COutPoint> sortedVins(current->spentVins.begin(), current->spentVins.end());
    std::sort(sortedVins.begin(), sortedVins.end());

    std::vector<UniversalAddress> sortedVoutTargets;
    for(auto& t : current->balances){
        sortedVoutTargets.push_back(t.first);
    }
    std::sort(sortedVoutTargets.begin(), sortedVoutTargets.end());


    CMutableTransaction tx;
    //first, spend all vins
    for(auto& v : sortedVins){
        //op_spend, AAL version 2
        tx.vin.push_back(CTxIn(v.hash, v.n, CScript() << valtype{2} << OP_SPEND));
    }

    //now set vouts to modified balances
    int n=0;
    for(auto &dest : sortedVoutTargets) {
        if (current->balances[dest] == 0) {
            //no need for 0 coin outputs
            continue;
        }
        CScript script;
        CScriptBase c;
        if (dest.version == AddressVersion::PUBKEYHASH) {
            script = CScript() << OP_DUP << OP_HASH160 << dest.data << OP_EQUALVERIFY << OP_CHECKSIG;
        } else if (dest.version == AddressVersion::SCRIPTHASH) {
            //TODO
        } else {
            //TODO
            //create a no-exec contract output
            //CBitcoinAddress btc = dest.asBitcoinAddress();
            //script = CScript() << VersionVM::GetNoExecVersion2().toRaw() << valtype{0} << valtype{0} << valtype{0} << btc.getData() << OP_CALL;
        }
        tx.vout.push_back(CTxOut(current->balances[dest], script));
        if (n + 1 > MAX_CONTRACT_VOUTS) {
            LogPrintf("AAL Transaction has exceeded MAX_CONTRACT_VOUTS!");
            return CTransaction();
        }
        n++;
    }
    if(!tx.vin.size() && tx.vout.size()>0){
        LogPrintf("AAL Transaction has a vout, but no vins");
        return CTransaction();
    }
    if(!tx.vout.size() && tx.vin.size()>0){
        LogPrintf("AAL Transaction has a vin, but no vouts");
        return CTransaction();
    }
    auto txid = tx.GetHash();
    n = 0;
    for(auto &dest : sortedVoutTargets){
        if (current->balances[dest] == 0) {
            removeAalData(dest);
            continue;
        }
        writeAalData(dest, txid, n, current->balances[dest]);
        n++;
    }

    return CTransaction(tx);
}

std::vector<uint8_t> getBytecodeKey(UniversalAddress address){
    std::vector<uint8_t> K;
    K.insert(K.end(), DELTADB_PREFIX_STATE.begin(), DELTADB_PREFIX_STATE.end());
	K.insert(K.end(), address.version);
	K.insert(K.end(), address.data.begin(), address.data.end());	
	K.insert(K.end(), DELTADB_STATE_BYTECODE);	
    return K;
}

std::vector<uint8_t> getStateKey(UniversalAddress address, std::vector<uint8_t> key){
	std::vector<uint8_t> K;
    std::vector<unsigned char> keyHash(32);

    K.insert(K.end(), DELTADB_PREFIX_STATE.begin(), DELTADB_PREFIX_STATE.end());
	K.insert(K.end(), address.version);
	K.insert(K.end(), address.data.begin(), address.data.end());	
	K.insert(K.end(), DELTADB_STATE_KEY);
	if(key.size() > 31){
		CSHA256().Write(key.data(), key.size()).Finalize(keyHash.data());
		K.insert(K.end(), keyHash.begin(),keyHash.end());	
	}else{
		K.insert(K.end(), '_'); 
		K.insert(K.end(), key.begin(),key.end());	
	}

    return K;
}

//live state key format: state_%address%_%key%

//live bytecode: state_%address%c
bool DeltaDBWrapper:: writeByteCode(UniversalAddress address,valtype byteCode){
	return Write(getBytecodeKey(address), byteCode);
}

bool DeltaDBWrapper:: readByteCode(UniversalAddress address,valtype& byteCode){
    return Read(getBytecodeKey(address), byteCode);   
}

bool DeltaDBWrapper:: writeAalData(UniversalAddress address, uint256 txid, unsigned int vout, uint64_t balance){
	std::vector<uint8_t> K;
	std::vector<uint8_t> V;
    K.insert(K.end(), DELTADB_PREFIX_STATE.begin(), DELTADB_PREFIX_STATE.end());
	K.insert(K.end(), address.version);
	K.insert(K.end(), address.data.begin(), address.data.end());	
	K.insert(K.end(), DELTADB_STATE_AAL);	

	CDataStream dsValue(SER_DISK,0);
	dsValue<<txid;	
	dsValue<<vout;
	dsValue<<balance;	
	V.insert(V.end(),dsValue.begin(),dsValue.end());
	return Write(K, V);
}

bool DeltaDBWrapper::removeAalData(UniversalAddress address){
	std::vector<uint8_t> K;
	std::vector<uint8_t> V;
    K.insert(K.end(), DELTADB_PREFIX_STATE.begin(), DELTADB_PREFIX_STATE.end());
	K.insert(K.end(), address.version);
	K.insert(K.end(), address.data.begin(), address.data.end());	
	K.insert(K.end(), DELTADB_STATE_AAL);	
    return Write(K, V);
}

bool DeltaDBWrapper:: readAalData(UniversalAddress address, uint256 &txid, unsigned int &vout, uint64_t &balance){
    if(hasNoAAL.find(address) != hasNoAAL.end()){
        return false;
    }
	std::vector<uint8_t> K;
	std::vector<uint8_t> V;
    K.insert(K.end(), DELTADB_PREFIX_STATE.begin(), DELTADB_PREFIX_STATE.end());
	K.insert(K.end(), address.version);
	K.insert(K.end(), address.data.begin(), address.data.end());
	K.insert(K.end(), DELTADB_STATE_AAL);	
	if(Read(K, V)){
		CDataStream dsValue(V,SER_DISK,0);
		dsValue>>txid;	
		dsValue>>vout;
		dsValue>>balance;
		return true;
	}else{
        hasNoAAL.insert(address);
        return false;
	}
}

bool DeltaDBWrapper:: writeState(UniversalAddress address, valtype key, valtype value){
	return Write(getStateKey(address, key), value);
}

bool DeltaDBWrapper:: readState(UniversalAddress address, valtype key, valtype& value){
    return Read(getStateKey(address, key), value);
}



/* Live data of contract updated:  %address%_updated_%key%_ */
bool DeltaDBWrapper:: writeUpdatedKey(UniversalAddress address, valtype key, unsigned int blk_num, uint256 blk_hash){
	std::vector<uint8_t> K;
	std::vector<uint8_t> V;
	K.insert(K.end(), address.version);
	K.insert(K.end(), address.data.begin(), address.data.end());	
	K.insert(K.end(), '_');
	//K.insert(K.end(), updatePre, updatePre + sizeof(updatePre)/sizeof(uint8_t));
	K.insert(K.end(), key.begin(),key.end());	
	
	CDataStream dsValue(SER_DISK,0);
	dsValue<<blk_num;
	dsValue<<blk_hash;
	V.insert(V.end(),dsValue.begin(),dsValue.end());
	return Write(K, V);

}

bool DeltaDBWrapper:: readUpdatedKey(UniversalAddress address, valtype key, unsigned int &blk_num, uint256 &blk_hash){
	std::vector<uint8_t> K;
	std::vector<uint8_t> V;
	K.insert(K.end(), address.version);
	K.insert(K.end(), address.data.begin(), address.data.end());	
	K.insert(K.end(), '_');
	//K.insert(K.end(), updatePre, updatePre + sizeof(updatePre)/sizeof(uint8_t));
	K.insert(K.end(), key.begin(),key.end());	
	if(Read(K, V)){
		CDataStream dsValue(V,SER_DISK,0);
		dsValue>>blk_num;
		dsValue>>blk_hash;
		return true;
	}else{
        return false;
	}
}

/* the raw unhashed key to be looked up by hash: %address%_keys_%key% */
bool DeltaDBWrapper:: writeRawKey(UniversalAddress address,	valtype key, valtype rawkey){
	std::vector<uint8_t> K;
	K.insert(K.end(), address.version);
	K.insert(K.end(), address.data.begin(), address.data.end());	
	K.insert(K.end(), '_');
	//K.insert(K.end(), keysPre, keysPre + sizeof(keysPre)/sizeof(uint8_t));
	K.insert(K.end(), key.begin(),key.end());	
	return Write(K, rawkey);
}

bool DeltaDBWrapper:: readRawKey(UniversalAddress address, valtype key, valtype &rawkey){
	std::vector<uint8_t> K;
	K.insert(K.end(), address.version);
	K.insert(K.end(), address.data.begin(), address.data.end());	
	K.insert(K.end(), '_');
	//K.insert(K.end(), keysPre, keysPre + sizeof(keysPre)/sizeof(uint8_t));
	K.insert(K.end(), key.begin(),key.end());	
	return Read(K, rawkey);
}

/* current iterator of a key: %address%_iterator_%key% */
bool DeltaDBWrapper:: writeCurrentIterator(UniversalAddress address, valtype key, uint64_t iterator){
	std::vector<uint8_t> K;
    char prefix = 'I';
	K.insert(K.end(), address.version);
	K.insert(K.end(), address.data.begin(), address.data.end());	
	K.insert(K.end(), '_');
	K.insert(K.end(), &prefix, &prefix + sizeof(prefix));
	K.insert(K.end(), '_');
	K.insert(K.end(), key.begin(),key.end());	
	return Write(K, iterator);
}

bool DeltaDBWrapper:: readCurrentIterator(UniversalAddress address,		valtype key, uint64_t &iterator){
	std::vector<uint8_t> K;
    char prefix = 'I';
	K.insert(K.end(), address.version);
	K.insert(K.end(), address.data.begin(), address.data.end());	
	K.insert(K.end(), '_');
	K.insert(K.end(), &prefix, &prefix + sizeof(prefix));
	K.insert(K.end(), '_');
	K.insert(K.end(), key.begin(),key.end());	
	return Read(K, iterator);
}

/*  key's data at point of iterator: %address%_data_%key%_%iterator% */
bool DeltaDBWrapper:: writeStateWithIterator(UniversalAddress address,		 valtype key, uint64_t iterator, valtype value){
	std::vector<uint8_t> K;
	CDataStream dsKey(SER_DISK,0);
	dsKey<<iterator;
	K.insert(K.end(), address.version);
	K.insert(K.end(), address.data.begin(), address.data.end());	
	//K.insert(K.end(), dataPre, dataPre + sizeof(dataPre)/sizeof(uint8_t));
	K.insert(K.end(), key.begin(),key.end());
	K.insert(K.end(), dsKey.begin(),dsKey.end());	
	return Write(K, value);
}


bool DeltaDBWrapper:: readStateWithIterator(UniversalAddress address,		valtype key, uint64_t iterator, valtype &value){
	std::vector<uint8_t> K;
	CDataStream dsKey(SER_DISK,0);
	dsKey<<iterator;
	K.insert(K.end(), address.version);
	K.insert(K.end(), address.data.begin(), address.data.end());	
	//K.insert(K.end(), dataPre, dataPre + sizeof(dataPre)/sizeof(uint8_t));
	K.insert(K.end(), key.begin(),key.end());	
	K.insert(K.end(), dsKey.begin(),dsKey.end());	
	return Read(K, value);
}

/* block number + block hash + txid/vout at point of iterator: %address%_info_%key%_%iterator%  */
bool DeltaDBWrapper:: writeInfoWithIterator(UniversalAddress address,		valtype key, uint64_t iterator, unsigned int blk_num, uint256 blk_hash, uint256 txid, unsigned int vout){
	std::vector<uint8_t> K;
	std::vector<uint8_t> V;
	CDataStream dsKey(SER_DISK,0);
	dsKey<<iterator;
	K.insert(K.end(), address.version);
	K.insert(K.end(), address.data.begin(), address.data.end());	
	K.insert(K.end(), '_');
	//K.insert(K.end(), infoPre, infoPre + sizeof(infoPre)/sizeof(uint8_t));
	K.insert(K.end(), key.begin(),key.end());	
	K.insert(K.end(), dsKey.begin(),dsKey.end());

	CDataStream dsValue(SER_DISK,0);
	dsValue<<blk_num;	
	dsValue<<blk_hash;	
	dsValue<<txid;	
	dsValue<<vout;	
	V.insert(V.end(),dsValue.begin(),dsValue.end());
	return Write(K, V);
}

bool DeltaDBWrapper:: readInfoWithIterator(UniversalAddress address, 	   valtype key, uint64_t iterator, unsigned int &blk_num, uint256 &blk_hash, uint256 &txid, unsigned int &vout){
	std::vector<uint8_t> K;
	std::vector<uint8_t> V;
	CDataStream dsKey(SER_DISK,0);
	dsKey<<iterator;
	K.insert(K.end(), address.version);
	K.insert(K.end(), address.data.begin(), address.data.end());	
	K.insert(K.end(), '_');
	//K.insert(K.end(), infoPre, infoPre + sizeof(infoPre)/sizeof(uint8_t));
	K.insert(K.end(), key.begin(),key.end());
	K.insert(K.end(), dsKey.begin(),dsKey.end());
	if(Read(K, V)){
		CDataStream dsValue(V,SER_DISK,0);
		dsValue>>blk_num;
		dsValue>>blk_hash;
		dsValue>>txid;	
		dsValue>>vout;	
		return true;
	}else{
        return false;
	}
}

/* Oldest iterator that exists in the changelog database: %address%_old_%key% */
bool DeltaDBWrapper:: writeOldestIterator(UniversalAddress address,		  valtype key, uint64_t iterator, unsigned int blk_num, uint256 blk_hash){
	std::vector<uint8_t> K;
	std::vector<uint8_t> V;
	K.insert(K.end(), address.version);
	K.insert(K.end(), address.data.begin(), address.data.end());	
	K.insert(K.end(), '_');
	//K.insert(K.end(), oldPre, oldPre + sizeof(oldPre)/sizeof(uint8_t));
	K.insert(K.end(), key.begin(),key.end());	

	CDataStream dsValue(SER_DISK,0);
	dsValue<<iterator;	
	dsValue<<blk_num;	
	dsValue<<blk_hash;	
	V.insert(V.end(),dsValue.begin(),dsValue.end());
	return Write(K, V);
}

bool DeltaDBWrapper:: readOldestIterator(UniversalAddress address,		 valtype key, uint64_t &iterator, unsigned int &blk_num, uint256 &blk_hash){
	std::vector<uint8_t> K;
	std::vector<uint8_t> V;
	K.insert(K.end(), address.version);
	K.insert(K.end(), address.data.begin(), address.data.end());	
	K.insert(K.end(), '_');
	//K.insert(K.end(), oldPre, oldPre + sizeof(oldPre)/sizeof(uint8_t));
	K.insert(K.end(), key.begin(),key.end());
	if(Read(K, V)){
		CDataStream dsValue(V,SER_DISK,0);
		dsValue>>iterator;	
		dsValue>>blk_num;
		dsValue>>blk_hash;
		return true;
	}else{
        return false;
	}
}

static bool containsOnlyASCII(const std::string& path) {
  for (auto c: path) {
    if (static_cast<unsigned char>(c) > 127 || static_cast<unsigned char>(c) < 32) {
      return false;
    }
  }
  return true;
}

UniValue DeltaCheckpoint::toJSON(){
    UniValue result(UniValue::VOBJ); //root
    UniValue deltasJson(UniValue::VOBJ);
    UniValue deltasRawJson(UniValue::VOBJ);
    for(auto &p : deltas){
        deltasRawJson.pushKV(HexStr(p.first), HexStr(p.second));
        std::string value(p.second.begin(), p.second.end());
        if(containsOnlyASCII(p.first)){
            if(containsOnlyASCII(value)){
                deltasJson.pushKV(p.first, value);
            }else{
                deltasJson.pushKV(p.first, HexStr(p.second));
            }
        }else{
            if(containsOnlyASCII(value)){
                deltasJson.pushKV(HexStr(p.first), value);
            }else{
                deltasJson.pushKV(HexStr(p.first), HexStr(p.second));
            }
        }
    }
    result.pushKV("deltas", deltasJson);
    result.pushKV("deltas-raw", deltasRawJson);
    
    UniValue balancesJson(UniValue::VOBJ);
    for(auto &p : balances){
        //balancesJson.push_back(Pair(p.first.asBitcoinAddress().ToString(), p.second));
    }
    result.pushKV("modified-balances", balancesJson);

    UniValue vinsJson(UniValue::VARR);
    for(auto &v : spentVins){
        vinsJson.push_back(v.ToString());
    }
    result.pushKV("spent-vins", vinsJson);

    return result;
}


//EventDB implementation

/*Internal database consists of two sections

Height index: h_%blockheight%_%address% -> [vout1, vout2, ...]

Result index: r_%blockheight%_%vout% -> ContractExecutionResult

Terrible efficiency, but ContractExecutionResult is for now stored as JSON

vout is stored {hash, n}

blockheight must be stored big-endian so that leveldb can iterate bytewise 
*/
static const std::string EVENTDB_PREFIX_HEIGHT = "h_";
static const std::string EVENTDB_PREFIX_RESULT = "r_";

std::vector<uint8_t> createHeightKey(uint32_t blockheight, UniversalAddress address){
    blockheight = htobe32(blockheight);
    std::vector<uint8_t> k;
    k.insert(k.end(), EVENTDB_PREFIX_HEIGHT.begin(), EVENTDB_PREFIX_HEIGHT.end());
    for(int i = 0; i < sizeof(uint32_t) ; i++){
        k.push_back(((uint8_t*)&blockheight)[i]);
    }
	k.insert(k.end(), address.version);
	k.insert(k.end(), address.data.begin(), address.data.end());	
    return k;
}
std::vector<uint8_t> createResultKey(uint32_t blockheight, COutPoint vout = COutPoint()){
    blockheight = htobe32(blockheight);
    std::vector<uint8_t> k;
    k.insert(k.end(), EVENTDB_PREFIX_RESULT.begin(), EVENTDB_PREFIX_RESULT.end());
    for(int i = 0; i < sizeof(uint32_t) ; i++){
        k.push_back(((uint8_t*)&blockheight)[i]);
    }
    if(!vout.IsNull()){
        k.insert(k.end(), vout.hash.begin(), vout.hash.end());
        k.insert(k.end(), vout.n);
    }
    return k;
}

bool EventDB::commit(uint32_t height){
    auto map = buildAddressMap();
    CDBBatch b(*this);
    //build height index first
    for(auto &pair : map){
        std::vector<uint8_t> v;
        v.reserve (pair.second.size() * (32 + 4)); //size * (sizeof(txid) + sizeof(n))
        for(auto vout : pair.second){
            v.insert(v.end(), vout.hash.begin(), vout.hash.end());
            v.insert(v.end(), vout.n);
        }
        b.Write(createHeightKey(height, pair.first), v);
    }
    //build result index
    for(auto res : results){
        b.Write(createResultKey(height, res.tx), res.toJSON().write(1, 2));
    }

    return WriteBatch(b);
}

void getResultTouches(const ContractExecutionResult &result, std::unordered_set<UniversalAddress>& touches){
    touches.insert(result.address);
    for(auto &sub : result.callResults){
        getResultTouches(sub, touches);
    }
}

std::map<UniversalAddress, std::vector<COutPoint>> EventDB::buildAddressMap(){
    std::map<UniversalAddress, std::vector<COutPoint>> map;
    
    for(auto &res : results){
        std::unordered_set<UniversalAddress> touches;
        getResultTouches(res, touches);
        for(auto &a : touches){
            if(map.find(a) == map.end()){
                map[a] = std::vector<COutPoint>();
            }
            map[a].push_back(res.tx);
        }
    }
    return map;
}

    //adds a result to the buffer
    //used during block validation after each contract execution
bool EventDB::addResult(const ContractExecutionResult &result){
    results.push_back(result);
    return true;
}
bool EventDB::revert(){
    results.clear();
    return true;
}
bool EventDB::eraseBlock(uint32_t height){
    //todo. search for h_%blockheight% and r_%blockheight% and delete
    return false;
}


std::vector<std::string> EventDB::getResults(UniversalAddress address, int minheight, int maxheight, int maxresults){
    std::vector<std::string> results;
    CDBIterator* it = NewIterator();
    std::vector<uint8_t> tmp;
    tmp = createResultKey(minheight);
    std::string start(tmp.begin(), tmp.end());
    tmp = createResultKey(maxheight+1);
    std::string end(tmp.begin(), tmp.end());
    std::string k;
    it->Seek(start);
    if(!it->Valid()){
        return results;
    }
    
    while(it->GetKey(k)){
        if(k >= end || k.size() == 0 ||  k[0] != 'r'){
            break;
        }
        std::string v;
        if(!this->Read(k, v)){
            break; //needed? 
        }
        results.push_back(v);
        if(results.size() >= maxresults){
            break;
        }
        it->Next();
    }
    delete it;
    return results;
}


ContractExecutor::ContractExecutor(const CBlock &_block, ContractOutput _output, uint64_t _blockGasLimit)
: block(_block), output(_output), blockGasLimit(_blockGasLimit)
{

}


ContractEnvironment ContractExecutor::buildEnv() {
    ContractEnvironment env;
    CBlockIndex* tip = chainActive.Tip();
    //assert(*tip->phashBlock == block.hashPrevBlock); //TODO, currently blockNumber and hashes will be wrong
    env.blockNumber = tip-> nHeight + 1;
    env.blockTime = block.nTime;
    env.difficulty = block.nBits;
    env.gasLimit = blockGasLimit;
    env.blockHashes.resize(256);
    for(int i = 0 ; i < 256; i++){
        if(!tip)
            break;
        env.blockHashes[i] = *tip->phashBlock;
        tip = tip->pprev;
    }

    if(block.IsProofOfStake()){
        env.blockCreator = UniversalAddress::FromScript(block.vtx[1]->vout[1].scriptPubKey);
    }else {
        env.blockCreator = UniversalAddress::FromScript(block.vtx[0]->vout[0].scriptPubKey);
    }
    return env;
}

bool ContractExecutor::execute(ContractExecutionResult &result, bool commit)
{
    /*
    DeltaDBWrapper wrapper(pdeltaDB);
    ContractEnvironment env=buildEnv();
    if(result.blockHash == uint256()){
        result.blockHash = block.GetHash();
    }
    if(output.version.rootVM == ROOT_VM_EVM){
        EVMContractVM evm(wrapper, env, blockGasLimit);
        evm.execute(output, result, commit);
    }else if(output.version.rootVM == ROOT_VM_X86){
        wrapper.setInitialCoins(output.address, output.vout, output.value);
        x86ContractVM x86(wrapper, env, blockGasLimit);
        x86.execute(output, result, commit);
        result.transferTx = CMutableTransaction(wrapper.createCondensingTx());
    }else{
        return false;
    }
    if(commit && result.commitState){
        wrapper.commit();
    }
    //no need to revert if not committing
    */
    return true;
}


