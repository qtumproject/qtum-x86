#include <boost/test/unit_test.hpp>
#include <test/test_bitcoin.h>
#include <qtumtests/test_utils.h>
#include <x86lib.h>
#include <qtum/qtumx86.h>
#include <qtum/qtumtransaction.h>

using namespace x86Lib;



ExecDataABI fakeExecData(UniversalAddress self, UniversalAddress sender = UniversalAddress(), UniversalAddress origin =  UniversalAddress()){
    ExecDataABI e;
    e.size = sizeof(e);
    e.isCreate = 0;
    e.sender = sender.toAbi();
    e.gasLimit = 10000000;
    e.valueSent = 0;
    e.origin = origin.toAbi();
    e.self = self.toAbi();
    e.nestLevel = 0;
    return e;
}

ContractEnvironment fakeContractEnv(UniversalAddress blockCreator = UniversalAddress()){
    ContractEnvironment env;
    env.blockNumber = 1;
    env.blockTime = 1;
    env.difficulty = 1;
    env.gasLimit = 10000000;
    env.blockCreator = blockCreator;
    env.blockHashes = std::vector<uint256>(); //ok to leave empty for now
    return env;
}

x86CPU fakeCPU(RAMemory &memory){
    MemorySystem *sys = new MemorySystem();
    sys->Add(0x1000, 0x1000 + memory.getSize(), &memory);
    x86CPU cpu;
    cpu.Memory = sys;
    return cpu;
}

void destroyFakeCPU(x86CPU &cpu){
    delete cpu.Memory;
}

    //x86ContractVM(DeltaDBWrapper &db, const ContractEnvironment &env, uint64_t remainingGasLimit)


BOOST_FIXTURE_TEST_SUITE(x86_tests, TestingSetup)
const uint32_t addressGen = 0x19fa12de;
struct FakeVMContainer{
    UniversalAddress address;
    DeltaDBWrapper wrapper;
    x86ContractVM vm;
    ExecDataABI execdata;
    QtumHypervisor hv;
    RAMemory mem;
    x86CPU cpu;
    FakeVMContainer() : 
        address(X86, (uint8_t*)&addressGen, ((uint8_t*)&addressGen) + sizeof(addressGen)),
        wrapper(nullptr),
        vm(wrapper, fakeContractEnv(), 1000000),
        execdata(fakeExecData(address)),
        hv(vm, wrapper, execdata),
        mem(1000, "testmem"),
        cpu(fakeCPU(mem))
    {
        
    }
    ~FakeVMContainer(){
        destroyFakeCPU(cpu);
    }
};

BOOST_AUTO_TEST_CASE(x86_hypervisor_SCCS){
    //in theory it's ok to not have a backing database for the wrapper
    //as long as we don't access a non-existent key or try to commit to database
    FakeVMContainer *fake = new FakeVMContainer();

    uint32_t testValue = 0x12345678;
    fake->cpu.WriteMemory(0x1000, sizeof(testValue), &testValue);
    
    fake->cpu.SetReg32(EAX, QSC_SCCSPush); //syscall number
    fake->cpu.SetReg32(EBX, 0x1000); //buffer location
    fake->cpu.SetReg32(ECX, sizeof(testValue)); //buffer size

    fake->hv.HandleInt(QtumSystem, fake->cpu);

    BOOST_CHECK(fake->cpu.Reg32(EAX) == 0); //should return success
    BOOST_CHECK(fake->hv.sizeofSCCS() == 1); //one item on stack
    BOOST_CHECK(*((uint32_t*)fake->hv.popSCCS().data()) == 0x12345678);
    BOOST_CHECK(fake->hv.sizeofSCCS() == 0); //zero items on stack after popping

    //push another item on the stack to test poppping
    {
        uint32_t temp = 0x87654321;
        std::vector<uint8_t> t((uint8_t*)&temp, ((uint8_t*)&temp) + sizeof(uint32_t));
        fake->hv.pushSCCS(t);
    }
    fake->cpu.SetReg32(EAX, QSC_SCCSPop); //syscall number
    fake->cpu.SetReg32(EBX, 0x1100); //buffer location
    fake->cpu.SetReg32(ECX, sizeof(uint32_t)); //buffer size 
    fake->hv.HandleInt(QtumSystem, fake->cpu);

    BOOST_CHECK(fake->cpu.Reg32(EAX) == sizeof(uint32_t));
    BOOST_CHECK(fake->hv.sizeofSCCS() == 0);
    {
        uint32_t val = 0;
        fake->cpu.ReadMemory(0x1100, sizeof(uint32_t), &val);
        BOOST_CHECK(val == 0x87654321);
    }
    delete fake;
}

BOOST_AUTO_TEST_CASE(x86_hypervisor_storage){
    FakeVMContainer *fake = new FakeVMContainer();
    std::vector<uint8_t> key1;
    key1.push_back(0x82);
    std::vector<uint8_t> val1;
    val1.push_back(0x12);
    val1.push_back(0x34);
    fake->wrapper.writeState(fake->address, key1, val1);

    fake->cpu.WriteMemory(0x1000, 1, key1.data());

    fake->cpu.SetReg32(EAX, QSC_ReadStorage);
    fake->cpu.SetReg32(EBX, 0x1000); //key pointer
    fake->cpu.SetReg32(ECX, 1); //key size
    fake->cpu.SetReg32(EDX, 0x1100); //value pointer to be written to
    fake->cpu.SetReg32(ESI, 100); //max value size
    fake->hv.HandleInt(QtumSystem, fake->cpu);

    BOOST_CHECK(fake->cpu.Reg32(EAX) == 2);
    {
        uint8_t t[4];
        fake->cpu.ReadMemory(0x1100, 4, t);
        BOOST_CHECK(t[0] == 0x12);
        BOOST_CHECK(t[1] == 0x34);
        BOOST_CHECK(t[2] == 0);
        BOOST_CHECK(t[3] == 0);
    }

    delete fake;
}

BOOST_AUTO_TEST_CASE(x86_hypervisor_sha256) {
    //in theory it's ok to not have a backing database for the wrapper
    //as long as we don't access a non-existent key or try to commit to database
    FakeVMContainer *fake = new FakeVMContainer();

    unsigned char testValue[] = "hello world";
    fake->cpu.WriteMemory(0x1000, sizeof(testValue)-1, &testValue);

    fake->cpu.SetReg32(EAX, QSC_SHA256);      //syscall number
    fake->cpu.SetReg32(EBX, 0x1000);            //input location
    fake->cpu.SetReg32(ECX, sizeof(testValue)-1); //input size
    fake->cpu.SetReg32(EDX, 0x1100); // output location

    fake->hv.HandleInt(QtumSystem, fake->cpu);
    std::string expectedHashVal = "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9";
    unsigned char gHashVal[32];
    fake->cpu.ReadMemory(0x1100, 32, gHashVal);
    // convert gHashVal to hex string
    std::string gotHashVal  = bytesToHexString(gHashVal, 32);
    BOOST_CHECK(expectedHashVal == gotHashVal);

    // test case 2
    unsigned char testValue2[] = "I need £ to exchange ¥ \\0so I can buy this in € on this Coinbase© Exchange® Called Toshi™";
    fake->cpu.WriteMemory(0x1000, sizeof(testValue2)-1, &testValue2);

    fake->cpu.SetReg32(EAX, QSC_SHA256);        //syscall number
    fake->cpu.SetReg32(ECX, sizeof(testValue2)-1); //input size

    fake->hv.HandleInt(QtumSystem, fake->cpu);
    std::string expectedHashVal2 = "7253746a31fda040de5306f564b5f04af5ccdad97f892820c2bb917fe030cb4a";
    unsigned char gHashVal2[32];
    fake->cpu.ReadMemory(0x1100, 32, gHashVal2);
    // convert gHashVal to hex string
    std::string gotHashVal2 = bytesToHexString(gHashVal2, 32);
    BOOST_CHECK(expectedHashVal2 == gotHashVal2);

    unsigned char testValue3[] = "s0mething\ts0mething\td4rk51d3!!!";
    fake->cpu.WriteMemory(0x1000, sizeof(testValue3)-1, &testValue3);

    fake->cpu.SetReg32(EAX, QSC_SHA256);         //syscall number
    fake->cpu.SetReg32(ECX, sizeof(testValue3)-1); //input size

    fake->hv.HandleInt(QtumSystem, fake->cpu);
    std::string expectedHashVal3 = "5573373555e3ce30a71cace9f2797e2204008399b11cb83ef4599684a36b7ebb";
    unsigned char gHashVal3[32];
    fake->cpu.ReadMemory(0x1100, 32, gHashVal3);
    // convert gHashVal to hex string
    std::string gotHashVal3 = bytesToHexString(gHashVal3, 32);
    BOOST_CHECK(expectedHashVal3 == gotHashVal3);

    std::string testValue4 = "hello \x00 \xff world";
    fake->cpu.WriteMemory(0x1000, sizeof(testValue4)-1, (void*)testValue4.c_str());

    fake->cpu.SetReg32(EAX, QSC_SHA256);         //syscall number
    fake->cpu.SetReg32(ECX, sizeof(testValue4)-1); //input size

    fake->hv.HandleInt(QtumSystem, fake->cpu);
    std::string expectedHashVal4 = "859bb38de457c7ce1cf7619ce57a3f6c001545770d44dfaf7140011c335a142b";
    unsigned char gHashVal4[32];
    fake->cpu.ReadMemory(0x1100, 32, gHashVal4);
    // convert gHashVal to hex string
    std::string gotHashVal4 = bytesToHexString(gHashVal4, 32);
    BOOST_CHECK(expectedHashVal4 == gotHashVal4);
    delete fake;
}

BOOST_AUTO_TEST_SUITE_END()