#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace flitzzip::integrity_detail {

#if defined(_WIN32)
// Purpose: Isolate the native Windows CNG operations used by an owning SHA-256 context.
// Inputs: Calls are synchronous; successful acquisitions must be real CNG handles and borrowed bytes remain stable.
// Outputs: Native methods report STATUS_SUCCESS as true. Tests may refuse calls per instance, without global hooks.
class CngSha256Provider {
  public:
    // Purpose: Release interface storage; inputs: no owned CNG handles; outputs: nonthrowing destruction.
    virtual ~CngSha256Provider() = default;

    // Purpose: Acquire the SHA-256 algorithm; inputs: null output; outputs: a real owned provider or failure.
    virtual bool open_algorithm(void*& algorithm) const noexcept;

    // Purpose: Query object storage; inputs: live algorithm; outputs: exact required byte count or failure.
    virtual bool object_length(void* algorithm, std::uint32_t& bytes) const noexcept;

    // Purpose: Create a hash; inputs: algorithm and retained object storage fitting uint32_t; outputs: owned hash.
    virtual bool create_hash(void* algorithm, void*& hash, std::span<std::byte> object) const noexcept;

    // Purpose: Consume one bounded chunk; inputs: live hash and at most uint32_t bytes; outputs: no input mutation.
    virtual bool update_hash(void* hash, std::span<const std::byte> bytes) const noexcept;

    // Purpose: Finalize SHA-256; inputs: live hash and exact digest storage; outputs: 32 digest bytes or failure.
    virtual bool finish_hash(void* hash, std::span<std::byte, 32> digest) const noexcept;

    // Purpose: Destroy a hash before its object buffer/provider; inputs: real owned hash; outputs: release status.
    virtual bool destroy_hash(void* hash) const noexcept;

    // Purpose: Close an algorithm after its hashes; inputs: real owned provider; outputs: release status.
    virtual bool close_algorithm(void* algorithm) const noexcept;
};

// Purpose: Hash a borrowed snapshot through one explicit synchronous platform boundary.
// Inputs: Immutable bytes and a provider that outlives this call and returns only real successful CNG acquisitions.
// Outputs: The normal digest or ArchiveError; resources are released before an error propagates.
std::string sha256_bytes_with_provider(std::span<const std::byte> bytes, const CngSha256Provider& provider);
#endif

}  // namespace flitzzip::integrity_detail
