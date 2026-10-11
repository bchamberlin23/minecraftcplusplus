#pragma once
// TCP connection to a Minecraft server: VarInt length framing and zlib packet compression
// (threshold from set_compression). One thread reads; any thread may send.
#include "core/common.h"

#include <atomic>
#include <memory>
#include <mutex>

struct libdeflate_compressor;
struct libdeflate_decompressor;

namespace net {

// Stream cipher applied to every byte after login encryption starts (online mode: AES/CFB8 with
// the shared secret). Not implemented yet; Connection is ready to apply one.
struct StreamCipher {
    virtual ~StreamCipher() = default;
    virtual void encrypt(u8* data, size_t size) = 0;
    virtual void decrypt(u8* data, size_t size) = 0;
};

class Connection {
public:
    Connection();
    ~Connection();

    bool open(const std::string& host, u16 port, std::string& error);
    void close();  // also unblocks a pending read
    // Blocks for the next packet (id + body, decompressed). False when the connection ends.
    bool read(std::vector<u8>& packet);
    // Frames, compresses and sends one packet (id + body). Thread-safe.
    bool send(const std::vector<u8>& packet);
    void setCompression(int threshold) { threshold_ = threshold; }
    // Takes effect for bytes after the current packet in both directions.
    void setCipher(std::unique_ptr<StreamCipher> cipher);
    u64 bytesReceived() const { return received_; }

private:
    bool fill();  // reads more bytes into the receive buffer

    uintptr_t socket_;
    std::mutex sendMutex_;
    std::atomic<int> threshold_{-1};
    std::unique_ptr<StreamCipher> cipher_;
    libdeflate_compressor* compressor_ = nullptr;
    libdeflate_decompressor* decompressor_ = nullptr;
    std::vector<u8> rbuf_;
    size_t rpos_ = 0, rend_ = 0;
    std::vector<u8> frame_;
    std::atomic<u64> received_{0};
};

}  // namespace net
