#include "fum/core/hash.hpp"

#include <cstring>

namespace fum::hash {
namespace {

constexpr std::array<std::uint32_t, 64> kK = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr std::uint32_t rotr(std::uint32_t value, unsigned bits) noexcept {
  return (value >> bits) | (value << (32u - bits));
}

}  // namespace

void Sha256Stream::compress(const std::uint8_t* block) noexcept {
  std::uint32_t w[64];
  for (std::size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
  std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + s1 + ch + kK[i] + w[i];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

Sha256Stream::Sha256Stream() noexcept {
  state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
}

void Sha256Stream::update(std::string_view data) noexcept {
  if (finished_) {
    return;
  }
  total_bytes_ += static_cast<std::uint64_t>(data.size());
  std::size_t offset = 0;
  if (buffered_ > 0) {
    const std::size_t need = 64 - buffered_;
    const std::size_t take = data.size() < need ? data.size() : need;
    std::memcpy(buffer_.data() + buffered_, data.data(), take);
    buffered_ += take;
    offset += take;
    if (buffered_ == 64) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }
  while (offset + 64 <= data.size()) {
    compress(reinterpret_cast<const std::uint8_t*>(data.data() + offset));
    offset += 64;
  }
  if (offset < data.size()) {
    const std::size_t remaining = data.size() - offset;
    std::memcpy(buffer_.data(), data.data() + offset, remaining);
    buffered_ = remaining;
  }
}

Sha256Digest Sha256Stream::finish() noexcept {
  if (!finished_) {
    const std::uint64_t bit_length = total_bytes_ * 8u;
    std::uint8_t pad = 0x80;
    update(std::string_view(reinterpret_cast<const char*>(&pad), 1));
    const std::uint8_t zero = 0x00;
    while (buffered_ != 56) {
      update(std::string_view(reinterpret_cast<const char*>(&zero), 1));
    }
    std::uint8_t length_bytes[8];
    for (std::size_t i = 0; i < 8; ++i) {
      length_bytes[i] = static_cast<std::uint8_t>((bit_length >> ((7 - i) * 8)) & 0xFFu);
    }
    update(std::string_view(reinterpret_cast<const char*>(length_bytes), 8));
    finished_ = true;
  }
  Sha256Digest out{};
  for (std::size_t i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xFFu);
    out[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xFFu);
    out[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xFFu);
    out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xFFu);
  }
  return out;
}

Sha256Digest sha256(std::string_view data) noexcept {
  Sha256Stream stream;
  stream.update(data);
  return stream.finish();
}

Sha256Digest sha256(std::string_view a, std::string_view b) noexcept {
  Sha256Stream stream;
  stream.update(a);
  stream.update(b);
  return stream.finish();
}

std::string to_hex(const Sha256Digest& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(kSha256Bytes * 2);
  for (const std::uint8_t byte : digest) {
    out.push_back(kHex[(byte >> 4) & 0x0Fu]);
    out.push_back(kHex[byte & 0x0Fu]);
  }
  return out;
}

Result<Sha256Digest> from_hex(std::string_view hex) {
  if (hex.size() != kSha256Bytes * 2) {
    return make_error(ErrorCode::invalid_argument, "digest hex length must be 64",
                      std::to_string(hex.size()));
  }
  Sha256Digest out{};
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < kSha256Bytes; ++i) {
    const int hi = nibble(hex[i * 2]);
    const int lo = nibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return make_error(ErrorCode::invalid_argument, "digest hex contains a non-hex character");
    }
    out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return out;
}

std::string sha256_hex(std::string_view data) { return to_hex(sha256(data)); }

bool constant_time_equal(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff = static_cast<unsigned char>(diff | static_cast<unsigned char>(a[i] ^ b[i]));
  }
  return diff == 0;
}

}  // namespace fum::hash
