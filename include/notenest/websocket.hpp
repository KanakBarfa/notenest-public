#ifndef WEBSOCKET_HPP
#define WEBSOCKET_HPP

#include <cstdint>
#include <cstring>
#include <string>

namespace WebSocket {

// Decoded WebSocket frame structure.
struct Frame {
    bool fin = false;
    uint8_t opcode = 0;
    std::string payload;
    size_t consumed_bytes = 0;
};

// Encodes a text or binary payload into an unmasked server-to-client RFC 6455 WebSocket frame.
inline std::string encodeFrame(const std::string& payload, uint8_t opcode = 0x01) {
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | (opcode & 0x0F)));
    size_t len = payload.size();
    if (len <= 125) {
        frame.push_back(static_cast<char>(len & 0x7F));
    } else if (len <= 65535) {
        frame.push_back(static_cast<char>(126));
        frame.push_back(static_cast<char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<char>(len & 0xFF));
    } else {
        frame.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
        }
    }
    frame.append(payload);
    return frame;
}

// Parses an incoming RFC 6455 WebSocket frame from raw buffer data.
inline bool parseFrame(const std::string& buf, Frame& frame) {
    if (buf.size() < 2) {
        return false;
    }

    uint8_t byte0 = static_cast<uint8_t>(buf[0]);
    uint8_t byte1 = static_cast<uint8_t>(buf[1]);

    frame.fin = (byte0 & 0x80) != 0;
    frame.opcode = byte0 & 0x0F;
    bool masked = (byte1 & 0x80) != 0;
    uint64_t payload_len = byte1 & 0x7F;

    size_t header_size = 2;
    if (payload_len == 126) {
        if (buf.size() < 4) {
            return false;
        }
        payload_len = (static_cast<uint8_t>(buf[2]) << 8) | static_cast<uint8_t>(buf[3]);
        header_size += 2;
    } else if (payload_len == 127) {
        if (buf.size() < 10) {
            return false;
        }
        payload_len = 0;
        for (int i = 0; i < 8; ++i) {
            payload_len = (payload_len << 8) | static_cast<uint8_t>(buf[4 + i]);
        }
        header_size += 8;
    }

    uint8_t mask_key[4] = {0};
    if (masked) {
        if (buf.size() < header_size + 4) {
            return false;
        }
        std::memcpy(mask_key, buf.data() + header_size, 4);
        header_size += 4;
    }

    if (buf.size() < header_size + payload_len) {
        return false;
    }

    frame.payload.resize(payload_len);
    const char* src = buf.data() + header_size;
    if (masked) {
        for (size_t i = 0; i < payload_len; ++i) {
            frame.payload[i] = src[i] ^ mask_key[i % 4];
        }
    } else {
        std::memcpy(frame.payload.data(), src, payload_len);
    }

    frame.consumed_bytes = header_size + payload_len;
    return true;
}

}  // namespace WebSocket

#endif
