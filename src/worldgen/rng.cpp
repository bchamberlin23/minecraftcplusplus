#include "worldgen/rng.h"

namespace wg {

i32 javaStringHash(std::string_view s) {
    i32 h = 0;
    for (unsigned char c : s) h = jmul32(h, 31) + i32(c);  // identifiers are ASCII
    return h;
}

i64 mixStafford13(i64 z) {
    z = jmul(z ^ lshr(z, 30), -4658895280553007687LL);
    z = jmul(z ^ lshr(z, 27), -7723592293110705685LL);
    return z ^ lshr(z, 31);
}

Seed128 upgradeSeedTo128bit(i64 legacySeed) {
    const i64 lo = legacySeed ^ 7640891576956012809LL;
    const i64 hi = jadd(lo, -7046029254386353131LL);
    return {mixStafford13(lo), mixStafford13(hi)};
}

namespace {
// Compact MD5 (RFC 1321).
void md5(const u8* msg, size_t len, u8 out[16]) {
    static const u32 K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                              14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    u32 h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476;
    std::vector<u8> m(msg, msg + len);
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    const u64 bits = u64(len) * 8;
    for (int i = 0; i < 8; ++i) m.push_back(u8(bits >> (8 * i)));
    for (size_t off = 0; off < m.size(); off += 64) {
        u32 w[16];
        for (int i = 0; i < 16; ++i)
            w[i] = u32(m[off + i * 4]) | (u32(m[off + i * 4 + 1]) << 8) | (u32(m[off + i * 4 + 2]) << 16) |
                   (u32(m[off + i * 4 + 3]) << 24);
        u32 a = h0, b = h1, c = h2, d = h3;
        for (int i = 0; i < 64; ++i) {
            u32 f;
            int g;
            if (i < 16)
                f = (b & c) | (~b & d), g = i;
            else if (i < 32)
                f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
            else if (i < 48)
                f = b ^ c ^ d, g = (3 * i + 5) % 16;
            else
                f = c ^ (b | ~d), g = (7 * i) % 16;
            const u32 tmp = d;
            d = c;
            c = b;
            const u32 x = a + f + K[i] + w[g];
            b = b + ((x << R[i]) | (x >> (32 - R[i])));
            a = tmp;
        }
        h0 += a, h1 += b, h2 += c, h3 += d;
    }
    const u32 hs[4] = {h0, h1, h2, h3};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = u8(hs[i] >> (8 * j));
}
}  // namespace

Seed128 seedFromHashOf(std::string_view input) {
    u8 h[16];
    md5(reinterpret_cast<const u8*>(input.data()), input.size(), h);
    i64 lo = 0, hi = 0;
    for (int i = 0; i < 8; ++i) lo = i64((u64(lo) << 8) | h[i]);  // Longs.fromBytes: big-endian
    for (int i = 8; i < 16; ++i) hi = i64((u64(hi) << 8) | h[i]);
    return {lo, hi};
}

double RandomSource::nextGaussian() {
    if (haveNextGaussian_) {
        haveNextGaussian_ = false;
        return nextNextGaussian_;
    }
    double x, y, r;
    do {
        x = 2.0 * nextDouble() - 1.0;
        y = 2.0 * nextDouble() - 1.0;
        r = x * x + y * y;
    } while (r >= 1.0 || r == 0.0);
    const double m = std::sqrt(-2.0 * std::log(r) / r);
    nextNextGaussian_ = y * m;
    haveNextGaussian_ = true;
    return x * m;
}

XoroshiroRandom::XoroshiroRandom(i64 seed) : rng_(0, 0) {
    const Seed128 s = upgradeSeedTo128bit(seed);
    rng_ = Xoroshiro128PlusPlus(s.lo, s.hi);
}

void XoroshiroRandom::setSeed(i64 seed) {
    const Seed128 s = upgradeSeedTo128bit(seed);
    rng_ = Xoroshiro128PlusPlus(s.lo, s.hi);
    resetGaussian();
}

std::unique_ptr<RandomSource> XoroshiroRandom::fork() {
    const i64 lo = rng_.nextLong(), hi = rng_.nextLong();
    return std::make_unique<XoroshiroRandom>(lo, hi);
}

std::unique_ptr<PositionalRandomFactory> XoroshiroRandom::forkPositional() {
    const i64 lo = rng_.nextLong(), hi = rng_.nextLong();
    return std::make_unique<XoroshiroPositional>(lo, hi);
}

i32 XoroshiroRandom::nextInt(i32 bound) {
    u64 randomBits = u32(nextInt());
    u64 product = randomBits * u64(bound);
    u64 fractional = product & 0xFFFFFFFFull;
    if (fractional < u64(bound)) {
        const u32 threshold = u32(~bound + 1) % u32(bound);  // Integer.remainderUnsigned(-bound, bound)
        while (fractional < threshold) {
            randomBits = u32(nextInt());
            product = randomBits * u64(bound);
            fractional = product & 0xFFFFFFFFull;
        }
    }
    return i32(product >> 32);
}

std::unique_ptr<PositionalRandomFactory> LegacyRandom::forkPositional() {
    return std::make_unique<LegacyPositional>(nextLong());
}

i32 LegacyRandom::nextInt(i32 bound) {
    if ((bound & (bound - 1)) == 0) return jint((i64(bound) * i64(next(31))) >> 31);
    i32 sample, modulo;
    do {
        sample = next(31);
        modulo = sample % bound;
    } while (i32(u32(sample) - u32(modulo) + u32(bound - 1)) < 0);
    return modulo;
}

}  // namespace wg
