#pragma once
// Cryptography for the friends system (social/):
//  - SHA-256, HMAC-SHA256, HKDF (RFC 5869), ChaCha20 and ChaCha20-Poly1305 (RFC 8439), written here;
//  - secp256k1 from the vendored libsecp256k1: BIP340 Schnorr keys and signatures (the Nostr identity) and ECDH;
//  - NIP-44 v2, the Nostr standard for encrypting a message to one recipient (ECDH conversation key, ChaCha20,
//    HMAC-SHA256, padded lengths).
#include "core/common.h"

#include <array>
#include <optional>

namespace social {

using Bytes = std::vector<u8>;
using Key32 = std::array<u8, 32>;

// --- encodings ----------------------------------------------------------------------------------------------------
std::string toHex(const u8* data, size_t size);
inline std::string toHex(const Bytes& b) { return toHex(b.data(), b.size()); }
inline std::string toHex(const Key32& k) { return toHex(k.data(), k.size()); }
bool fromHex(std::string_view hex, Bytes& out);
bool fromHex(std::string_view hex, Key32& out);  // exactly 64 hex digits
std::string base64(const u8* data, size_t size);
bool unbase64(std::string_view text, Bytes& out);  // strict (padding required, no whitespace)

// --- hashes, MACs, KDF, ciphers -----------------------------------------------------------------------------------
Key32 sha256(const u8* data, size_t size);
inline Key32 sha256(std::string_view s) { return sha256(reinterpret_cast<const u8*>(s.data()), s.size()); }
Key32 hmacSha256(const u8* key, size_t keySize, const u8* data, size_t size);
Key32 hkdfExtract(const u8* salt, size_t saltSize, const u8* ikm, size_t ikmSize);
Bytes hkdfExpand(const Key32& prk, const u8* info, size_t infoSize, size_t length);
// ChaCha20 (RFC 8439): data ^= keystream(key, nonce, counter...).
void chacha20(const u8 key[32], const u8 nonce[12], u32 counter, u8* data, size_t size);
void poly1305(const u8 key[32], const u8* data, size_t size, u8 tag[16]);
// ChaCha20-Poly1305 AEAD: out = ciphertext || 16-byte tag. open: false when the tag does not match.
void aeadSeal(const u8 key[32], const u8 nonce[12], const u8* aad, size_t aadSize, const u8* plain, size_t size, u8* out);
bool aeadOpen(const u8 key[32], const u8 nonce[12], const u8* aad, size_t aadSize, const u8* sealed, size_t size, u8* out);
bool constantTimeEqual(const u8* a, const u8* b, size_t size);

void randomBytes(u8* out, size_t size);  // the system's CSPRNG
template <size_t N>
std::array<u8, N> randomArray() {
    std::array<u8, N> a;
    randomBytes(a.data(), N);
    return a;
}

// --- secp256k1 (BIP340 x-only keys) -------------------------------------------------------------------------------
struct KeyPair {
    Key32 secret{}, pub{};  // pub: x-only (32 bytes), as Nostr uses it
};
KeyPair generateKeyPair();
bool keyPairFromSecret(const Key32& secret, KeyPair& out);  // false: not a valid secret key
bool validPublicKey(const Key32& pub);
// BIP340: aux is the auxiliary randomness (random when not given).
std::array<u8, 64> schnorrSign(const KeyPair& kp, const u8* msg, size_t msgSize, const Key32* aux = nullptr);
bool schnorrVerify(const Key32& pub, const u8* msg, size_t msgSize, const u8 sig[64]);
// ECDH: the x coordinate of secret * pub (pub lifted to even y, like BIP340 / NIP-44), unhashed.
bool sharedX(const Key32& secret, const Key32& pub, Key32& out);

// --- NIP-44 v2 ----------------------------------------------------------------------------------------------------
bool nip44ConversationKey(const Key32& secret, const Key32& pub, Key32& out);
size_t nip44PaddedLength(size_t length);
// Base64 payload. nonce: 32 random bytes unless given (tests).
std::string nip44Encrypt(const Key32& conversationKey, std::string_view plaintext, const Key32* nonce = nullptr);
std::optional<std::string> nip44Decrypt(const Key32& conversationKey, std::string_view payload);

}  // namespace social
