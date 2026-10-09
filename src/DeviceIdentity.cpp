#include "DeviceIdentity.h"

#include <Preferences.h>
#include <esp_system.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>

namespace {
constexpr char PREFERENCES_NAMESPACE[] = "device";
constexpr size_t DEVICE_ID_LENGTH = 36;
constexpr size_t PRIVATE_KEY_LENGTH = 32;
constexpr size_t PUBLIC_KEY_LENGTH = 65;
constexpr size_t SHA256_LENGTH = 32;
constexpr size_t P256_MAX_DER_SIGNATURE_LENGTH = 72;

char deviceId[DEVICE_ID_LENGTH + 1] = {};
uint8_t privateKey[PRIVATE_KEY_LENGTH] = {};
uint8_t publicKey[PUBLIC_KEY_LENGTH] = {};
bool identityReady = false;

int randomBytes(void*, unsigned char* output, size_t length) {
    esp_fill_random(output, length);
    return 0;
}

void generateUuid(char* output) {
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    snprintf(output, DEVICE_ID_LENGTH + 1,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11],
             bytes[12], bytes[13], bytes[14], bytes[15]);
}

bool generateKeyPair(uint8_t* privateOutput, uint8_t* publicOutput) {
    mbedtls_ecp_group group;
    mbedtls_mpi privateScalar;
    mbedtls_ecp_point publicPoint;
    mbedtls_ecp_group_init(&group);
    mbedtls_mpi_init(&privateScalar);
    mbedtls_ecp_point_init(&publicPoint);

    int result = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
    if (result == 0) {
        result = mbedtls_ecp_gen_keypair(
            &group, &privateScalar, &publicPoint, randomBytes, nullptr);
    }

    size_t publicLength = 0;
    if (result == 0) {
        result = mbedtls_mpi_write_binary(
            &privateScalar, privateOutput, PRIVATE_KEY_LENGTH);
    }
    if (result == 0) {
        result = mbedtls_ecp_point_write_binary(
            &group, &publicPoint, MBEDTLS_ECP_PF_UNCOMPRESSED,
            &publicLength, publicOutput, PUBLIC_KEY_LENGTH);
    }

    mbedtls_ecp_point_free(&publicPoint);
    mbedtls_mpi_free(&privateScalar);
    mbedtls_ecp_group_free(&group);
    return result == 0 && publicLength == PUBLIC_KEY_LENGTH;
}

bool validateKeyPair(const uint8_t* privateInput,
                     const uint8_t* publicInput) {
    mbedtls_ecp_group group;
    mbedtls_mpi privateScalar;
    mbedtls_ecp_point publicPoint;
    mbedtls_ecp_group_init(&group);
    mbedtls_mpi_init(&privateScalar);
    mbedtls_ecp_point_init(&publicPoint);

    int result = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
    if (result == 0) {
        result = mbedtls_mpi_read_binary(
            &privateScalar, privateInput, PRIVATE_KEY_LENGTH);
    }
    if (result == 0) {
        result = mbedtls_ecp_mul(
            &group, &publicPoint, &privateScalar, &group.G,
            randomBytes, nullptr);
    }

    uint8_t derivedPublicKey[PUBLIC_KEY_LENGTH] = {};
    size_t publicLength = 0;
    if (result == 0) {
        result = mbedtls_ecp_point_write_binary(
            &group, &publicPoint, MBEDTLS_ECP_PF_UNCOMPRESSED,
            &publicLength, derivedPublicKey, sizeof(derivedPublicKey));
    }

    bool valid = result == 0 &&
                 publicLength == PUBLIC_KEY_LENGTH &&
                 memcmp(derivedPublicKey, publicInput, PUBLIC_KEY_LENGTH) == 0;
    mbedtls_ecp_point_free(&publicPoint);
    mbedtls_mpi_free(&privateScalar);
    mbedtls_ecp_group_free(&group);
    return valid;
}

bool loadExistingIdentity(Preferences& preferences) {
    size_t idLength = preferences.getBytesLength("id");
    size_t privateLength = preferences.getBytesLength("priv");
    size_t publicLength = preferences.getBytesLength("pub");
    if (idLength != sizeof(deviceId) ||
        privateLength != sizeof(privateKey) ||
        publicLength != sizeof(publicKey)) {
        Serial.println("Device identity data is incomplete or has invalid lengths");
        return false;
    }

    if (preferences.getBytes("id", deviceId, sizeof(deviceId)) != sizeof(deviceId) ||
        preferences.getBytes("priv", privateKey, sizeof(privateKey)) != sizeof(privateKey) ||
        preferences.getBytes("pub", publicKey, sizeof(publicKey)) != sizeof(publicKey) ||
        deviceId[DEVICE_ID_LENGTH] != '\0' ||
        !validateKeyPair(privateKey, publicKey)) {
        Serial.println("Device identity data failed validation");
        return false;
    }
    return true;
}

bool createIdentity(Preferences& preferences) {
    generateUuid(deviceId);
    if (!generateKeyPair(privateKey, publicKey)) {
        Serial.println("Failed to generate device ECDSA key pair");
        return false;
    }

    bool saved =
        preferences.putBytes("id", deviceId, sizeof(deviceId)) == sizeof(deviceId) &&
        preferences.putBytes("priv", privateKey, sizeof(privateKey)) == sizeof(privateKey) &&
        preferences.putBytes("pub", publicKey, sizeof(publicKey)) == sizeof(publicKey);
    if (!saved) {
        Serial.println("Failed to persist device identity in NVS");
        return false;
    }
    return true;
}
}

bool initializeDeviceIdentity() {
    if (identityReady) {
        return true;
    }

    Preferences preferences;
    if (!preferences.begin(PREFERENCES_NAMESPACE, false)) {
        Serial.println("Failed to open device identity NVS namespace");
        return false;
    }

    bool hasIdentity = preferences.isKey("id") ||
                       preferences.isKey("priv") ||
                       preferences.isKey("pub");
    bool success = hasIdentity
                       ? loadExistingIdentity(preferences)
                       : createIdentity(preferences);
    preferences.end();

    if (success) {
        identityReady = true;
        Serial.printf("Device identity ready: %s\n", deviceId);
    }
    return success;
}

const char* getDeviceId() {
    return identityReady ? deviceId : "";
}

const uint8_t* getDevicePublicKey(size_t& length) {
    length = identityReady ? sizeof(publicKey) : 0;
    return identityReady ? publicKey : nullptr;
}

bool signDeviceMessage(const uint8_t* message,
                       size_t messageLength,
                       uint8_t* signature,
                       size_t signatureCapacity,
                       size_t& signatureLength) {
    signatureLength = 0;
    if (!identityReady || !message || !signature ||
        signatureCapacity < P256_MAX_DER_SIGNATURE_LENGTH) {
        return false;
    }

    uint8_t hash[SHA256_LENGTH];
    if (mbedtls_sha256_ret(message, messageLength, hash, 0) != 0) {
        Serial.println("Failed to hash device message");
        return false;
    }

    mbedtls_ecdsa_context context;
    mbedtls_ecdsa_init(&context);

    int result = mbedtls_ecp_group_load(
        &context.grp, MBEDTLS_ECP_DP_SECP256R1);
    if (result == 0) {
        result = mbedtls_mpi_read_binary(
            &context.d, privateKey, sizeof(privateKey));
    }
    if (result == 0) {
        result = mbedtls_ecdsa_write_signature(
            &context, MBEDTLS_MD_SHA256, hash, sizeof(hash),
            signature, &signatureLength,
            randomBytes, nullptr);
    }

    mbedtls_ecdsa_free(&context);
    if (result != 0) {
        Serial.printf("Failed to sign device message: %d\n", result);
        return false;
    }
    return true;
}
