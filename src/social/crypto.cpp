#include "social/crypto.h"

#include <secp256k1.h>
#include <secp256k1_ecdh.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdlib>

namespace social {

namespace {

u32 load32le(const u8* p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }
void store32le(u8* p, u32 v) {
    p[0] = u8(v), p[1] = u8(v >> 8), p[2] = u8(v >> 16), p[3] = u8(v >> 24);
}
u32 rotl(u32 v, int n) { return (v << n) | (v >> (32 - n)); }
u32 rotr(u32 v, int n) { return (v >> n) | (v << (32 - n)); }

// --- SHA-256 (FIPS 180-4) ---
struct Sha256 {
    u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    u8 buf[64];
    size_t used = 0;
    u64 total = 0;

    void block(const u8* p) {
        static const u32 k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
            0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
            0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
            0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
            0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
            0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
            0xc67178f2};
        u32 w[64];
        for (int i = 0; i < 16; ++i) w[i] = u32(p[i * 4]) << 24 | u32(p[i * 4 + 1]) << 16 | u32(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const u32 t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const u32 t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }
    void update(const u8* p, size_t n) {
        total += n;
        while (n > 0) {
            const size_t take = std::min(n, 64 - used);
            memcpy(buf + used, p, take);
            used += take, p += take, n -= take;
            if (used == 64) block(buf), used = 0;
        }
    }
    Key32 finish() {
        const u64 bits = total * 8;
        const u8 one = 0x80, zero = 0;
        update(&one, 1);
        while (used != 56) update(&zero, 1);
        u8 len[8];
        for (int i = 0; i < 8; ++i) len[i] = u8(bits >> (56 - 8 * i));
        update(len, 8);
        Key32 out;
        for (int i = 0; i < 8; ++i)
            out[size_t(i * 4)] = u8(h[i] >> 24), out[size_t(i * 4 + 1)] = u8(h[i] >> 16), out[size_t(i * 4 + 2)] = u8(h[i] >> 8),
                        out[size_t(i * 4 + 3)] = u8(h[i]);
        return out;
    }
};

void chachaBlock(const u32 in[16], u8 out[64]) {
    u32 x[16];
    memcpy(x, in, sizeof(x));
    auto qr = [&](int a, int b, int c, int d) {
        x[a] += x[b], x[d] = rotl(x[d] ^ x[a], 16);
        x[c] += x[d], x[b] = rotl(x[b] ^ x[c], 12);
        x[a] += x[b], x[d] = rotl(x[d] ^ x[a], 8);
        x[c] += x[d], x[b] = rotl(x[b] ^ x[c], 7);
    };
    for (int i = 0; i < 10; ++i) {
        qr(0, 4, 8, 12), qr(1, 5, 9, 13), qr(2, 6, 10, 14), qr(3, 7, 11, 15);
        qr(0, 5, 10, 15), qr(1, 6, 11, 12), qr(2, 7, 8, 13), qr(3, 4, 9, 14);
    }
    for (int i = 0; i < 16; ++i) store32le(out + i * 4, x[i] + in[i]);
}

const secp256k1_context* ctx() {
    static secp256k1_context* c = [] {
        secp256k1_context* n = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
        u8 seed[32];
        randomBytes(seed, sizeof(seed));
        if (!secp256k1_context_randomize(n, seed)) LOG("friends: secp256k1 context randomization failed");
        return n;
    }();
    return c;
}

bool liftX(const Key32& x, secp256k1_pubkey& out) {
    u8 compressed[33];
    compressed[0] = 0x02;
    memcpy(compressed + 1, x.data(), 32);
    return secp256k1_ec_pubkey_parse(ctx(), &out, compressed, sizeof(compressed)) == 1;
}

int copyX(unsigned char* output, const unsigned char* x32, const unsigned char*, void*) {
    memcpy(output, x32, 32);
    return 1;
}

}  // namespace

// --- encodings ----------------------------------------------------------------------------------------------------

std::string toHex(const u8* data, size_t size) {
    static const char* digits = "0123456789abcdef";
    std::string s(size * 2, '0');
    for (size_t i = 0; i < size; ++i) s[i * 2] = digits[data[i] >> 4], s[i * 2 + 1] = digits[data[i] & 15];
    return s;
}

bool fromHex(std::string_view hex, Bytes& out) {
    if (hex.size() % 2) return false;
    out.resize(hex.size() / 2);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < out.size(); ++i) {
        const int hi = nib(hex[i * 2]), lo = nib(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = u8(hi << 4 | lo);
    }
    return true;
}

bool fromHex(std::string_view hex, Key32& out) {
    Bytes b;
    if (hex.size() != 64 || !fromHex(hex, b)) return false;
    std::copy(b.begin(), b.end(), out.begin());
    return true;
}

std::string base64(const u8* data, size_t size) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    s.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        const u32 v = u32(data[i]) << 16 | (i + 1 < size ? u32(data[i + 1]) << 8 : 0) | (i + 2 < size ? data[i + 2] : 0);
        s += t[v >> 18 & 63];
        s += t[v >> 12 & 63];
        s += i + 1 < size ? t[v >> 6 & 63] : '=';
        s += i + 2 < size ? t[v & 63] : '=';
    }
    return s;
}

bool unbase64(std::string_view text, Bytes& out) {
    if (text.size() % 4) return false;
    out.clear();
    out.reserve(text.size() / 4 * 3);
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    for (size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        int v[4];
        int pad = 0;
        for (int j = 0; j < 4; ++j) {
            const char c = text[i + size_t(j)];
            if (c == '=' && last && j >= 2) {
                v[j] = 0, ++pad;
                continue;
            }
            if (pad) return false;  // data after padding
            v[j] = val(c);
            if (v[j] < 0) return false;
        }
        const u32 n = u32(v[0]) << 18 | u32(v[1]) << 12 | u32(v[2]) << 6 | u32(v[3]);
        out.push_back(u8(n >> 16));
        if (pad < 2) out.push_back(u8(n >> 8));
        if (pad < 1) out.push_back(u8(n));
    }
    return true;
}

// --- hashes, MACs, KDF, ciphers -----------------------------------------------------------------------------------

Key32 sha256(const u8* data, size_t size) {
    Sha256 h;
    h.update(data, size);
    return h.finish();
}

Key32 hmacSha256(const u8* key, size_t keySize, const u8* data, size_t size) {
    u8 k[64] = {};
    if (keySize > 64) {
        const Key32 hk = sha256(key, keySize);
        memcpy(k, hk.data(), 32);
    } else {
        memcpy(k, key, keySize);
    }
    u8 ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) ipad[i] = k[i] ^ 0x36, opad[i] = k[i] ^ 0x5c;
    Sha256 inner;
    inner.update(ipad, 64);
    inner.update(data, size);
    const Key32 ih = inner.finish();
    Sha256 outer;
    outer.update(opad, 64);
    outer.update(ih.data(), 32);
    return outer.finish();
}

Key32 hkdfExtract(const u8* salt, size_t saltSize, const u8* ikm, size_t ikmSize) {
    return hmacSha256(salt, saltSize, ikm, ikmSize);
}

Bytes hkdfExpand(const Key32& prk, const u8* info, size_t infoSize, size_t length) {
    Bytes out, t;
    for (u8 counter = 1; out.size() < length; ++counter) {
        Bytes msg = t;
        msg.insert(msg.end(), info, info + infoSize);
        msg.push_back(counter);
        const Key32 block = hmacSha256(prk.data(), prk.size(), msg.data(), msg.size());
        t.assign(block.begin(), block.end());
        out.insert(out.end(), block.begin(), block.end());
    }
    out.resize(length);
    return out;
}

void chacha20(const u8 key[32], const u8 nonce[12], u32 counter, u8* data, size_t size) {
    u32 st[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; ++i) st[4 + i] = load32le(key + i * 4);
    st[12] = counter;
    for (int i = 0; i < 3; ++i) st[13 + i] = load32le(nonce + i * 4);
    u8 block[64];
    for (size_t off = 0; off < size; off += 64) {
        chachaBlock(st, block);
        ++st[12];
        const size_t n = std::min<size_t>(64, size - off);
        for (size_t i = 0; i < n; ++i) data[off + i] ^= block[i];
    }
}

void poly1305(const u8 key[32], const u8* m, size_t size, u8 tag[16]) {
    // poly1305-donna, 26-bit limbs.
    const u32 r0 = load32le(key) & 0x3ffffff, r1 = (load32le(key + 3) >> 2) & 0x3ffff03,
              r2 = (load32le(key + 6) >> 4) & 0x3ffc0ff, r3 = (load32le(key + 9) >> 6) & 0x3f03fff,
              r4 = (load32le(key + 12) >> 8) & 0x00fffff;
    const u32 s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    u32 h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;
    while (size > 0) {
        u8 blk[16] = {};
        u32 hibit = 1u << 24;
        const size_t n = std::min<size_t>(16, size);
        memcpy(blk, m, n);
        if (n < 16) blk[n] = 1, hibit = 0;
        m += n, size -= n;
        h0 += load32le(blk) & 0x3ffffff;
        h1 += (load32le(blk + 3) >> 2) & 0x3ffffff;
        h2 += (load32le(blk + 6) >> 4) & 0x3ffffff;
        h3 += (load32le(blk + 9) >> 6) & 0x3ffffff;
        h4 += (load32le(blk + 12) >> 8) | hibit;
        const u64 d0 = u64(h0) * r0 + u64(h1) * s4 + u64(h2) * s3 + u64(h3) * s2 + u64(h4) * s1;
        u64 d1 = u64(h0) * r1 + u64(h1) * r0 + u64(h2) * s4 + u64(h3) * s3 + u64(h4) * s2;
        u64 d2 = u64(h0) * r2 + u64(h1) * r1 + u64(h2) * r0 + u64(h3) * s4 + u64(h4) * s3;
        u64 d3 = u64(h0) * r3 + u64(h1) * r2 + u64(h2) * r1 + u64(h3) * r0 + u64(h4) * s4;
        u64 d4 = u64(h0) * r4 + u64(h1) * r3 + u64(h2) * r2 + u64(h3) * r1 + u64(h4) * r0;
        u32 c = u32(d0 >> 26);
        h0 = u32(d0) & 0x3ffffff;
        d1 += c, c = u32(d1 >> 26), h1 = u32(d1) & 0x3ffffff;
        d2 += c, c = u32(d2 >> 26), h2 = u32(d2) & 0x3ffffff;
        d3 += c, c = u32(d3 >> 26), h3 = u32(d3) & 0x3ffffff;
        d4 += c, c = u32(d4 >> 26), h4 = u32(d4) & 0x3ffffff;
        h0 += c * 5, c = h0 >> 26, h0 &= 0x3ffffff;
        h1 += c;
    }
    u32 c = h1 >> 26;
    h1 &= 0x3ffffff;
    h2 += c, c = h2 >> 26, h2 &= 0x3ffffff;
    h3 += c, c = h3 >> 26, h3 &= 0x3ffffff;
    h4 += c, c = h4 >> 26, h4 &= 0x3ffffff;
    h0 += c * 5, c = h0 >> 26, h0 &= 0x3ffffff;
    h1 += c;
    u32 g0 = h0 + 5;
    c = g0 >> 26, g0 &= 0x3ffffff;
    u32 g1 = h1 + c;
    c = g1 >> 26, g1 &= 0x3ffffff;
    u32 g2 = h2 + c;
    c = g2 >> 26, g2 &= 0x3ffffff;
    u32 g3 = h3 + c;
    c = g3 >> 26, g3 &= 0x3ffffff;
    u32 g4 = h4 + c - (1u << 26);
    u32 mask = (g4 >> 31) - 1;  // all ones when h >= p
    g0 &= mask, g1 &= mask, g2 &= mask, g3 &= mask, g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0, h1 = (h1 & mask) | g1, h2 = (h2 & mask) | g2, h3 = (h3 & mask) | g3, h4 = (h4 & mask) | g4;
    h0 = h0 | (h1 << 26);
    h1 = (h1 >> 6) | (h2 << 20);
    h2 = (h2 >> 12) | (h3 << 14);
    h3 = (h3 >> 18) | (h4 << 8);
    u64 f = u64(h0) + load32le(key + 16);
    store32le(tag, u32(f));
    f = u64(h1) + load32le(key + 20) + (f >> 32);
    store32le(tag + 4, u32(f));
    f = u64(h2) + load32le(key + 24) + (f >> 32);
    store32le(tag + 8, u32(f));
    f = u64(h3) + load32le(key + 28) + (f >> 32);
    store32le(tag + 12, u32(f));
}

namespace {
void aeadTag(const u8 key[32], const u8 nonce[12], const u8* aad, size_t aadSize, const u8* ct, size_t size, u8 tag[16]) {
    u8 otk[64] = {};
    chacha20(key, nonce, 0, otk, 64);
    Bytes mac;
    mac.reserve(aadSize + size + 48);
    mac.insert(mac.end(), aad, aad + aadSize);
    mac.resize((mac.size() + 15) / 16 * 16, 0);
    mac.insert(mac.end(), ct, ct + size);
    mac.resize((mac.size() + 15) / 16 * 16, 0);
    for (u64 v : {u64(aadSize), u64(size)})
        for (int i = 0; i < 8; ++i) mac.push_back(u8(v >> (8 * i)));
    poly1305(otk, mac.data(), mac.size(), tag);
}
}  // namespace

void aeadSeal(const u8 key[32], const u8 nonce[12], const u8* aad, size_t aadSize, const u8* plain, size_t size, u8* out) {
    if (out != plain) memmove(out, plain, size);
    chacha20(key, nonce, 1, out, size);
    aeadTag(key, nonce, aad, aadSize, out, size, out + size);
}

bool aeadOpen(const u8 key[32], const u8 nonce[12], const u8* aad, size_t aadSize, const u8* sealed, size_t size, u8* out) {
    if (size < 16) return false;
    const size_t n = size - 16;
    u8 tag[16];
    aeadTag(key, nonce, aad, aadSize, sealed, n, tag);
    if (!constantTimeEqual(tag, sealed + n, 16)) return false;
    if (out != sealed) memmove(out, sealed, n);
    chacha20(key, nonce, 1, out, n);
    return true;
}

bool constantTimeEqual(const u8* a, const u8* b, size_t size) {
    u8 d = 0;
    for (size_t i = 0; i < size; ++i) d |= a[i] ^ b[i];
    return d == 0;
}

void randomBytes(u8* out, size_t size) {
    if (BCryptGenRandom(nullptr, out, ULONG(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        LOG("error: friends: the system random number generator failed");
        std::abort();  // never continue with predictable keys
    }
}

// --- secp256k1 ----------------------------------------------------------------------------------------------------

bool keyPairFromSecret(const Key32& secret, KeyPair& out) {
    secp256k1_keypair kp;
    if (!secp256k1_keypair_create(ctx(), &kp, secret.data())) return false;
    secp256k1_xonly_pubkey xpk;
    if (!secp256k1_keypair_xonly_pub(ctx(), &xpk, nullptr, &kp)) return false;
    out.secret = secret;
    return secp256k1_xonly_pubkey_serialize(ctx(), out.pub.data(), &xpk) == 1;
}

KeyPair generateKeyPair() {
    KeyPair kp;
    for (;;) {
        const Key32 sk = randomArray<32>();
        if (keyPairFromSecret(sk, kp)) return kp;
    }
}

bool validPublicKey(const Key32& pub) {
    secp256k1_xonly_pubkey xpk;
    return secp256k1_xonly_pubkey_parse(ctx(), &xpk, pub.data()) == 1;
}

std::array<u8, 64> schnorrSign(const KeyPair& k, const u8* msg, size_t msgSize, const Key32* aux) {
    std::array<u8, 64> sig{};
    secp256k1_keypair kp;
    if (!secp256k1_keypair_create(ctx(), &kp, k.secret.data())) return sig;
    const Key32 rnd = aux ? *aux : randomArray<32>();
    secp256k1_schnorrsig_extraparams params = SECP256K1_SCHNORRSIG_EXTRAPARAMS_INIT;
    params.ndata = const_cast<u8*>(rnd.data());
    if (!secp256k1_schnorrsig_sign_custom(ctx(), sig.data(), msg, msgSize, &kp, &params)) sig.fill(0);
    return sig;
}

bool schnorrVerify(const Key32& pub, const u8* msg, size_t msgSize, const u8 sig[64]) {
    secp256k1_xonly_pubkey xpk;
    if (!secp256k1_xonly_pubkey_parse(ctx(), &xpk, pub.data())) return false;
    return secp256k1_schnorrsig_verify(ctx(), sig, msg, msgSize, &xpk) == 1;
}

bool sharedX(const Key32& secret, const Key32& pub, Key32& out) {
    secp256k1_pubkey p;
    if (!liftX(pub, p)) return false;
    return secp256k1_ecdh(ctx(), out.data(), &p, secret.data(), copyX, nullptr) == 1;
}

// --- NIP-44 v2 ----------------------------------------------------------------------------------------------------

bool nip44ConversationKey(const Key32& secret, const Key32& pub, Key32& out) {
    Key32 x;
    if (!sharedX(secret, pub, x)) return false;
    static const char salt[] = "nip44-v2";
    out = hkdfExtract(reinterpret_cast<const u8*>(salt), sizeof(salt) - 1, x.data(), x.size());
    return true;
}

size_t nip44PaddedLength(size_t len) {
    if (len <= 32) return 32;
    size_t nextPower = 1;
    while (nextPower < len) nextPower <<= 1;  // 1 << (floor(log2(len - 1)) + 1)
    const size_t chunk = nextPower <= 256 ? 32 : nextPower / 8;
    return chunk * ((len - 1) / chunk + 1);
}

namespace {
struct MessageKeys {
    u8 chachaKey[32], chachaNonce[12], hmacKey[32];
};
MessageKeys messageKeys(const Key32& conversationKey, const u8 nonce[32]) {
    const Bytes k = hkdfExpand(conversationKey, nonce, 32, 76);
    MessageKeys m;
    memcpy(m.chachaKey, k.data(), 32);
    memcpy(m.chachaNonce, k.data() + 32, 12);
    memcpy(m.hmacKey, k.data() + 44, 32);
    return m;
}
}  // namespace

std::string nip44Encrypt(const Key32& conversationKey, std::string_view plaintext, const Key32* nonceIn) {
    if (plaintext.empty() || plaintext.size() > 65535) return {};
    const Key32 nonce = nonceIn ? *nonceIn : randomArray<32>();
    const MessageKeys mk = messageKeys(conversationKey, nonce.data());
    Bytes padded(2 + nip44PaddedLength(plaintext.size()), 0);
    padded[0] = u8(plaintext.size() >> 8), padded[1] = u8(plaintext.size());
    memcpy(padded.data() + 2, plaintext.data(), plaintext.size());
    chacha20(mk.chachaKey, mk.chachaNonce, 0, padded.data(), padded.size());
    Bytes out;
    out.reserve(1 + 32 + padded.size() + 32);
    out.push_back(2);
    out.insert(out.end(), nonce.begin(), nonce.end());
    out.insert(out.end(), padded.begin(), padded.end());
    const Key32 mac = hmacSha256(mk.hmacKey, 32, out.data() + 1, out.size() - 1);  // nonce || ciphertext
    out.insert(out.end(), mac.begin(), mac.end());
    return base64(out.data(), out.size());
}

std::optional<std::string> nip44Decrypt(const Key32& conversationKey, std::string_view payload) {
    if (payload.empty() || payload[0] == '#') return std::nullopt;  // unknown version
    if (payload.size() < 132 || payload.size() > 87472) return std::nullopt;
    Bytes d;
    if (!unbase64(payload, d) || d.size() < 99 || d.size() > 65603 || d[0] != 2) return std::nullopt;
    const u8* nonce = d.data() + 1;
    const u8* ct = d.data() + 33;
    const size_t ctSize = d.size() - 33 - 32;
    const MessageKeys mk = messageKeys(conversationKey, nonce);
    const Key32 mac = hmacSha256(mk.hmacKey, 32, d.data() + 1, 32 + ctSize);
    if (!constantTimeEqual(mac.data(), d.data() + d.size() - 32, 32)) return std::nullopt;
    Bytes padded(ct, ct + ctSize);
    chacha20(mk.chachaKey, mk.chachaNonce, 0, padded.data(), padded.size());
    const size_t len = size_t(padded[0]) << 8 | padded[1];
    if (len == 0 || len > padded.size() - 2 || padded.size() != 2 + nip44PaddedLength(len)) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(padded.data()) + 2, len);
}

}  // namespace social
