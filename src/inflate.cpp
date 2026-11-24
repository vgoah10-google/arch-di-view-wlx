// inflate.cpp — Minimal raw-DEFLATE (RFC 1951) decompressor.
//
// Supports stored, fixed-Huffman and dynamic-Huffman blocks. Enough to inflate
// the gzip payload of .gz / .tgz members so their contents can be listed.
#include "archive.h"

#include <vector>
#include <cstring>

namespace archdiview {
namespace {

// Canonical Huffman decoding table (limit-check style, as in zlib's "puff").
struct HuffTable {
    std::vector<uint16_t> count;  // number of symbols per bit length
    std::vector<uint16_t> symbol; // symbols ordered by (length, symbol)

    bool build(const uint8_t* lengths, size_t n) {
        count.assign(16, 0);
        for (size_t i = 0; i < n; ++i) count[lengths[i]]++;
        if (count[0] == n) { symbol.clear(); return true; } // no codes
        // Check for an over-subscribed or incomplete set of lengths.
        int left = 1;
        for (int len = 1; len <= 15; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false; // over-subscribed
        }
        // Offsets into symbol[] for each length.
        std::vector<uint16_t> offs(16, 0);
        for (int len = 1; len < 15; ++len)
            offs[len + 1] = offs[len] + count[len];
        symbol.assign(n, 0);
        for (size_t i = 0; i < n; ++i)
            if (lengths[i]) symbol[offs[lengths[i]]++] = (uint16_t)i;
        return true;
    }
};

class BitReader {
public:
    BitReader(const uint8_t* data, size_t len) : d_(data), n_(len), pos_(0), bit_(0), acc_(0) {}

    // Peek `need` bits (LSB-first) without consuming; returns false on EOF.
    bool bits(uint32_t need, uint32_t& out) {
        while (bit_ < need) {
            if (pos_ >= n_) return false;
            acc_ |= (uint32_t)d_[pos_++] << bit_;
            bit_ += 8;
        }
        out = acc_ & ((1u << need) - 1);
        return true;
    }
    void consume(uint32_t n) { acc_ >>= n; bit_ -= n; }
    // Discard bits to align to the next byte boundary.
    void alignToByte() { uint32_t r = bit_ & 7; if (r) consume(r); }
    // Read `need` bits, consuming them.
    bool read(uint32_t need, uint32_t& out) {
        if (!bits(need, out)) return false;
        consume(need);
        return true;
    }
    // Decode one symbol using a canonical Huffman table. Returns -1 on error.
    int decode(const HuffTable& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            uint32_t b;
            if (!read(1, b)) return -1;
            code |= (int)b;
            int cnt = h.count[len];
            if (code - cnt < first) {
                if (index + (code - first) >= (int)h.symbol.size()) return -1;
                return h.symbol[index + (code - first)];
            }
            index += cnt;
            first = (first + cnt) << 1;
            code <<= 1;
        }
        return -1;
    }

private:
    const uint8_t* d_;
    size_t n_, pos_;
    uint32_t bit_, acc_;
};

// Length/distance tables per RFC 1951.
struct LenDist {
    uint16_t base, extra;
};
const LenDist kLengthTable[29] = {
    {3,0},{4,0},{5,0},{6,0},{7,0},{8,0},{9,0},{10,0},{11,1},{13,1},
    {15,1},{17,1},{19,2},{23,2},{27,2},{31,2},{35,3},{43,3},{51,3},{59,3},
    {67,4},{83,4},{99,4},{115,4},{131,5},{163,5},{195,5},{227,5},{258,0}
};
const LenDist kDistTable[30] = {
    {1,0},{2,0},{3,0},{4,0},{5,1},{7,1},{9,2},{13,2},{17,3},{25,3},
    {33,4},{49,4},{65,5},{97,5},{129,6},{193,6},{257,7},{385,7},{513,8},{769,8},
    {1025,9},{1537,9},{2049,10},{3073,10},{4097,11},{6145,11},{8193,12},{12289,12},{16385,13},{24577,13}
};

bool inflateBlock(BitReader& br, std::vector<uint8_t>& out,
                  const HuffTable& lit, const HuffTable& dist) {
    while (true) {
        int sym = br.decode(lit);
        if (sym < 0) return false;
        if (sym < 256) {
            out.push_back((uint8_t)sym);
            continue;
        }
        if (sym == 256) return true; // end of block
        sym -= 257;
        if (sym >= 29) return false;
        uint32_t extra = 0;
        if (kLengthTable[sym].extra && !br.read(kLengthTable[sym].extra, extra))
            return false;
        uint32_t length = kLengthTable[sym].base + extra;

        int dsym = br.decode(dist);
        if (dsym < 0 || dsym >= 30) return false;
        extra = 0;
        if (kDistTable[dsym].extra && !br.read(kDistTable[dsym].extra, extra))
            return false;
        uint32_t distance = kDistTable[dsym].base + extra;
        if (distance == 0 || distance > out.size()) return false;

        size_t start = out.size() - distance;
        for (uint32_t i = 0; i < length; ++i)
            out.push_back(out[start + i]);
    }
}

bool inflateFixed(BitReader& br, std::vector<uint8_t>& out) {
    uint8_t litLen[288], distLen[30];
    for (int i = 0; i < 288; ++i) litLen[i] = (i < 144) ? 8 : (i < 256) ? 9 : (i < 280) ? 7 : 8;
    for (int i = 0; i < 30; ++i) distLen[i] = 5;
    HuffTable lit, dist;
    if (!lit.build(litLen, 288) || !dist.build(distLen, 30)) return false;
    return inflateBlock(br, out, lit, dist);
}

bool inflateDynamic(BitReader& br, std::vector<uint8_t>& out) {
    uint32_t hlit, hdist, hclen;
    if (!br.read(5, hlit) || !br.read(5, hdist) || !br.read(4, hclen))
        return false;
    size_t nlit = hlit + 257, ndist = hdist + 1;
    if (nlit > 286 || ndist > 30) return false;

    // Code-length alphabet (order per RFC 1951 section 3.2.7).
    static const uint8_t kOrder[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
    };
    uint8_t clLen[19] = {0};
    for (int i = 0; i < (int)hclen + 4; ++i) {
        uint32_t v;
        if (!br.read(3, v)) return false;
        clLen[kOrder[i]] = (uint8_t)v;
    }
    HuffTable cl;
    if (!cl.build(clLen, 19)) return false;

    // Read the literal/length + distance code lengths.
    std::vector<uint8_t> lengths(nlit + ndist, 0);
    size_t i = 0;
    while (i < nlit + ndist) {
        int sym = br.decode(cl);
        if (sym < 0) return false;
        if (sym < 16) {
            lengths[i++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (i == 0) return false;
            uint32_t rep;
            if (!br.read(2, rep)) return false;
            rep += 3;
            uint8_t prev = lengths[i - 1];
            while (rep-- && i < nlit + ndist) lengths[i++] = prev;
        } else if (sym == 17) {
            uint32_t rep;
            if (!br.read(3, rep)) return false;
            rep += 3;
            while (rep-- && i < nlit + ndist) lengths[i++] = 0;
        } else { // 18
            uint32_t rep;
            if (!br.read(7, rep)) return false;
            rep += 11;
            while (rep-- && i < nlit + ndist) lengths[i++] = 0;
        }
    }
    if (lengths[256] == 0) return false; // no end-of-block code

    HuffTable lit, dist;
    if (!lit.build(lengths.data(), nlit)) return false;
    if (!dist.build(lengths.data() + nlit, ndist)) return false;
    return inflateBlock(br, out, lit, dist);
}

} // namespace

bool Inflate(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out) {
    BitReader br(src, srcLen);
    out.clear();
    bool final = false;
    while (!final) {
        uint32_t bfinal, btype;
        if (!br.read(1, bfinal) || !br.read(2, btype)) return false;
        final = (bfinal == 1);
        switch (btype) {
            case 0: { // stored
                br.alignToByte();
                uint32_t len, nlen;
                if (!br.read(16, len) || !br.read(16, nlen)) return false;
                if ((len ^ 0xFFFF) != nlen) return false;
                // Read the raw bytes through the bit reader (byte-aligned here).
                for (uint32_t i = 0; i < len; ++i) {
                    uint32_t b;
                    if (!br.read(8, b)) return false;
                    out.push_back((uint8_t)b);
                }
                break;
            }
            case 1: if (!inflateFixed(br, out)) return false; break;
            case 2: if (!inflateDynamic(br, out)) return false; break;
            default: return false; // reserved
        }
    }
    return true;
}

} // namespace archdiview
