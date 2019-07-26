#ifndef NEUTRON_C_H
#define NEUTRON_C_H
#include <stdint.h>

//structs
#define ADDRESS_DATA_SIZE 20

typedef struct
{
    //Do not modify this struct's fields
    //This is consensus critical!
    uint32_t version;
    uint8_t data[ADDRESS_DATA_SIZE];
} __attribute__((__packed__)) UniversalAddressABI;

//constants below this line should exactly match libqtum's qtum.h!

//Note: we don't use Params().Base58Prefixes for these version numbers because otherwise
//contracts would need to use two different SDKs since the the base58 version for pubkeyhash etc changes
//between regtest, testnet, and mainnet
enum AddressVersion
{
    UNKNOWN = 0,
    //legacy is either pubkeyhash or EVM, depending on if the address already exists
    LEGACYEVM = 1,
    PUBKEYHASH = 2,
    EVM = 3,
    X86 = 4,
    SCRIPTHASH = 5,
    P2WSH = 6,
    P2WPKH = 7,
};

#endif
