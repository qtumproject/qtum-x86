#include <boost/test/unit_test.hpp>
#include <test/test_bitcoin.h>
#include <qtumtests/test_utils.h>
#include <x86lib.h>
#include <qtum/qtumx86.h>
#include <qtum/qtumtransaction.h>

using namespace x86Lib;

ExecDataABI fakeExecData(){
    ExecDataABI e;
    e.size = sizeof(e);
    e.isCreate = 0;
    e.sender = UniversalAddressABI();
    e.gasLimit = 10000000;
    e.valueSent = 0;
    e.origin = UniversalAddressABI();
    e.nestLevel = 0;
    return e;
}

ContractEnvironment fakeContractEnv(){
    ContractEnvironment env;
    env.blockNumber = 1;
    env.blockTime = 1;
    env.difficulty = 1;
    env.gasLimit = 10000000;
    env.blockCreator = UniversalAddress();
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


BOOST_AUTO_TEST_CASE(x86_hypervisor_sccs){
    //in theory it's ok to not have a backing database for the wrapper
    //as long as we don't access a non-existent key or try to commit to database
    DeltaDBWrapper wrapper(nullptr);
    x86ContractVM vm(wrapper, fakeContractEnv(), 1000000);
    //QtumHypervisor(x86ContractVM &vm, DeltaDBWrapper& db_, const ExecDataABI& execdata)
    ExecDataABI execdata = fakeExecData();
    QtumHypervisor hv(vm, wrapper, execdata);
    RAMemory mem(1000, "testmem");
    x86CPU cpu = fakeCPU(mem);

    uint32_t testValue = 0x12345678;
    //note "address" for this write is relative to the memory space we're writing into, NOT where it's located in the overall memory map of the CPU
    mem.Write(0, sizeof(testValue), &testValue);
    
    cpu.SetReg32(EAX, QSC_SCCSPush); //syscall number
    cpu.SetReg32(EBX, 0x1000); //buffer location
    cpu.SetReg32(ECX, sizeof(testValue)); //buffer size

    hv.HandleInt(QtumSystem, cpu);

    BOOST_CHECK(cpu.Reg32(EAX) == 0); //should return success
    BOOST_CHECK(hv.sizeofSCCS() == 1); //one item on stack
    BOOST_CHECK(hv.popSCCS()[0] == 0x78); //and top byte should be 78
}

BOOST_AUTO_TEST_SUITE_END()