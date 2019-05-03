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

};

BOOST_AUTO_TEST_CASE(x86_hypervisor_SCCS){
    //in theory it's ok to not have a backing database for the wrapper
    //as long as we don't access a non-existent key or try to commit to database
    FakeVMContainer fake;

    uint32_t testValue = 0x12345678;
    fake.cpu.WriteMemory(0x1000, sizeof(testValue), &testValue);
    
    fake.cpu.SetReg32(EAX, QSC_SCCSPush); //syscall number
    fake.cpu.SetReg32(EBX, 0x1000); //buffer location
    fake.cpu.SetReg32(ECX, sizeof(testValue)); //buffer size

    fake.hv.HandleInt(QtumSystem, fake.cpu);

    BOOST_CHECK(fake.cpu.Reg32(EAX) == 0); //should return success
    BOOST_CHECK(fake.hv.sizeofSCCS() == 1); //one item on stack
    BOOST_CHECK(*((uint32_t*)fake.hv.popSCCS().data()) == 0x12345678);
    BOOST_CHECK(fake.hv.sizeofSCCS() == 0); //zero items on stack after popping

    //push another item on the stack to test poppping
    {
        uint32_t temp = 0x87654321;
        std::vector<uint8_t> t((uint8_t*)&temp, ((uint8_t*)&temp) + sizeof(uint32_t));
        fake.hv.pushSCCS(t);
    }
    fake.cpu.SetReg32(EAX, QSC_SCCSPop); //syscall number
    fake.cpu.SetReg32(EBX, 0x1100); //buffer location
    fake.cpu.SetReg32(ECX, sizeof(uint32_t)); //buffer size 
    fake.hv.HandleInt(QtumSystem, fake.cpu);

    BOOST_CHECK(fake.cpu.Reg32(EAX) == sizeof(uint32_t));
    BOOST_CHECK(fake.hv.sizeofSCCS() == 0);
    {
        uint32_t val = 0;
        fake.cpu.ReadMemory(0x1100, sizeof(uint32_t), &val);
        BOOST_CHECK(val == 0x87654321);
    }
}

BOOST_AUTO_TEST_CASE(x86_hypervisor_storage){
    FakeVMContainer fake;
    std::vector<uint8_t> key1;
    key1.push_back(0x82);
    std::vector<uint8_t> val1;
    val1.push_back(0x12);
    val1.push_back(0x34);
    fake.wrapper.writeState(fake.address, key1, val1);

    fake.cpu.WriteMemory(0x1000, 1, key1.data());

    fake.cpu.SetReg32(EAX, QSC_ReadStorage);
    fake.cpu.SetReg32(EBX, 0x1000); //key pointer
    fake.cpu.SetReg32(ECX, 1); //key size
    fake.cpu.SetReg32(EDX, 0x1100); //value pointer to be written to
    fake.cpu.SetReg32(ESI, 100); //max value size
    fake.hv.HandleInt(QtumSystem, fake.cpu);

    BOOST_CHECK(fake.cpu.Reg32(EAX) == 2);
    {
        uint8_t t[4];
        fake.cpu.ReadMemory(0x1100, 4, t);
        BOOST_CHECK(t[0] == 0x12);
        BOOST_CHECK(t[1] == 0x34);
        BOOST_CHECK(t[2] == 0);
        BOOST_CHECK(t[3] == 0);
    }

}

BOOST_AUTO_TEST_CASE(x86_hypervisor_sha256) {
    //in theory it's ok to not have a backing database for the wrapper
    //as long as we don't access a non-existent key or try to commit to database
    FakeVMContainer fake;

    unsigned char testValue[] = "hello world";
    fake.cpu.WriteMemory(0x1000, sizeof(testValue), &testValue);

    fake.cpu.SetReg32(EAX, QSC_SHA256);      //syscall number
    fake.cpu.SetReg32(EBX, 0x1000);            //input location
    fake.cpu.SetReg32(ECX, sizeof(testValue)); //input size
    fake.cpu.SetReg32(EDX, 0x1200); // output location

    fake.hv.HandleInt(QtumSystem, fake.cpu);
    std::string expectedHashVal = "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9";
    unsigned char *gHashVal = new unsigned char[32];
    fake.cpu.ReadMemory(0x1200, 256, gHashVal);
    // convert gHashVal to hex string
    std::stringstream ss;
    ss << std::hex;
    for (int i = 0; i < 32; ++i) 
        ss << std::setw(2) << std::setfill('0') << (int)gHashVal[i];
    std::string gotHashVal  = ss.str();
    BOOST_CHECK(expectedHashVal == gotHashVal);
}

BOOST_AUTO_TEST_SUITE_END()