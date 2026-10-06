#include "test_util.hpp"
#include "Ppmd7.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>

extern "C" {
#include "7z.h"
}

namespace {

constexpr std::array<Byte, 6> kEmptyPpmdInput{};
// Encoded independently with pyppmd 1.3.1: order 2, 64 KiB model, no end marker.
constexpr std::array<Byte, 51> kTextPpmdInput{
    0x00, 0x50, 0x01, 0xE2, 0xFB, 0xF5, 0x17, 0x7F, 0x77, 0xE2, 0xC5, 0x86, 0x11, 0xD9, 0x2A, 0x85, 0x9B,
    0x6E, 0x54, 0xC4, 0x83, 0x47, 0x02, 0x2F, 0xBB, 0x0E, 0x5A, 0x47, 0x96, 0x9D, 0xD9, 0x28, 0x1E, 0xF6,
    0xD0, 0x05, 0x1A, 0xDD, 0xD3, 0xF6, 0x97, 0x13, 0x68, 0xEA, 0xF4, 0x29, 0x3B, 0x12, 0x2F, 0xB6, 0xFC};
constexpr std::string_view kPpmdText = "PPMd streaming regression.\n";

struct PpmdAllocationControl {
    ISzAlloc vt{};
    std::array<void*, 2> live{};
    std::size_t owner_bytes = 0;
    std::size_t calls = 0;
    std::size_t fail_call = 0;
    std::size_t bridge_checks = 0;
    bool bridge_is_owned = true;
    bool invalid_free = false;
};

struct PpmdTestStream {
    ILookInStream vt{};
    std::span<const Byte> input = kEmptyPpmdInput;
    std::size_t input_size = 5;
    std::size_t position = 0;
    std::size_t chunk_size = input.size();
    std::size_t look_calls = 0;
    std::size_t max_requested = 0;
    std::size_t fail_look_call = 0;
    bool fail_skip = false;
    bool leave_failed_outputs_unchanged = false;
    bool return_null_buffer = false;
    bool return_oversized_buffer = false;
    PpmdAllocationControl* allocation_control = nullptr;
};

// Purpose: Independently prove that the public decoder's live callback table belongs to its allocation.
// Inputs: The allocator's first recorded owner and exact requested byte extent during an input callback.
// Outputs: True only when the complete IByteIn table lies within that owner, excluding stack bridges.
bool ppmd_callback_is_owned(const PpmdAllocationControl& control) {
    if (control.live[0] == nullptr || control.owner_bytes < sizeof(CPpmd7)) {
        return false;
    }
    const auto* decoder = static_cast<const CPpmd7*>(control.live[0]);
    const auto owner = reinterpret_cast<std::uintptr_t>(control.live[0]);
    const auto bridge = reinterpret_cast<std::uintptr_t>(decoder->rc.dec.Stream);
    return bridge >= owner && bridge - owner <= control.owner_bytes - sizeof(IByteIn);
}

// Purpose: Recover the test stream whose first member is the SDK callback table.
// Inputs: `stream` points to the callback table of a live PpmdTestStream.
// Outputs: Returns the owning test state without retaining the pointer.
PpmdTestStream& ppmd_test_stream(ILookInStreamPtr stream) {
    return *reinterpret_cast<PpmdTestStream*>(const_cast<ILookInStream*>(stream));
}

// Purpose: Supply bounded input chunks and independently inject a read failure.
// Inputs: `stream` owns stable bytes; `buffer` and `size` are SDK look outputs.
// Outputs: Returns bytes without consuming them, or an error with configured output behavior.
SRes ppmd_test_look(ILookInStreamPtr stream, const void** buffer, std::size_t* size) {
    auto& self = ppmd_test_stream(stream);
    if (self.allocation_control != nullptr) {
        ++self.allocation_control->bridge_checks;
        self.allocation_control->bridge_is_owned =
            self.allocation_control->bridge_is_owned && ppmd_callback_is_owned(*self.allocation_control);
    }
    ++self.look_calls;
    self.max_requested = std::max(self.max_requested, *size);
    const bool failed = self.look_calls == self.fail_look_call;
    if (failed && self.leave_failed_outputs_unchanged) {
        return SZ_ERROR_READ;
    }
    *size = std::min({*size, self.chunk_size, self.input_size - self.position});
    *buffer = self.return_null_buffer ? nullptr : self.input.data() + self.position;
    if (self.return_oversized_buffer) {
        ++*size;
    }
    return failed ? SZ_ERROR_READ : SZ_OK;
}

// Purpose: Consume only previously exposed input, optionally failing before advancement.
// Inputs: `stream` owns the cursor and `offset` is the decoder's consumed byte count.
// Outputs: Advances the cursor on success; preserves it and returns an error on failure.
SRes ppmd_test_skip(ILookInStreamPtr stream, std::size_t offset) {
    auto& self = ppmd_test_stream(stream);
    if (self.fail_skip && offset != 0U) {
        return SZ_ERROR_READ;
    }
    if (offset > self.input_size - self.position || offset > self.chunk_size) {
        return SZ_ERROR_PARAM;
    }
    self.position += offset;
    return SZ_OK;
}

// Purpose: Reject unexpected direct reads in this look-only decoder regression.
// Inputs: Unused SDK direct-read arguments.
// Outputs: Returns an explicit unsupported-operation status.
SRes ppmd_test_read(ILookInStreamPtr, void*, std::size_t*) {
    return SZ_ERROR_UNSUPPORTED;
}

// Purpose: Accept only the decoder's initial seek to this in-memory packed stream.
// Inputs: `stream` owns the cursor, `position` is the absolute offset, and `origin` selects seek mode.
// Outputs: Resets the cursor for offset zero; rejects other seeks.
SRes ppmd_test_seek(ILookInStreamPtr stream, Int64* position, ESzSeek origin) {
    if (*position != 0 || origin != SZ_SEEK_SET) {
        return SZ_ERROR_PARAM;
    }
    ppmd_test_stream(stream).position = 0;
    return SZ_OK;
}

// Purpose: Allocate bounded storage for the public SDK decoder regression.
// Inputs: `size` is an SDK request; no allocation may exceed 1 MiB.
// Outputs: Returns zero-initialized C storage or null, matching the SDK allocator contract.
void* ppmd_test_allocate(ISzAllocPtr, std::size_t size) {
    return size <= 1024U * 1024U ? std::calloc(1U, size) : nullptr;
}

// Purpose: Release storage obtained by the matching SDK test allocator.
// Inputs: `address` is null or an allocation from ppmd_test_allocate.
// Outputs: Frees that allocation without throwing.
void ppmd_test_free(ISzAllocPtr, void* address) {
    std::free(address);
}

// Purpose: Fail either production PPMd allocation independently while recording successful ownership.
// Inputs: The allocator table belongs to a live control; size retains the existing fixture's bounded allocation.
// Outputs: Returns owned storage or null, preserving the production allocator's failure convention.
void* ppmd_control_allocate(ISzAllocPtr allocator, std::size_t size) {
    auto& control = *reinterpret_cast<PpmdAllocationControl*>(const_cast<ISzAlloc*>(allocator));
    ++control.calls;
    if (control.calls == control.fail_call || control.calls > control.live.size()) {
        return nullptr;
    }
    auto* result = ppmd_test_allocate(nullptr, size);
    control.live[control.calls - 1] = result;
    if (control.calls == 1) {
        control.owner_bytes = size;
    }
    return result;
}

// Purpose: Verify matching deallocation on success and failure without throwing through the SDK C boundary.
// Inputs: Null or one address returned by this control's allocator.
// Outputs: Releases a recorded owner or records an invalid/double free for the caller to assert.
void ppmd_control_free(ISzAllocPtr allocator, void* address) {
    if (address == nullptr) {
        return;
    }
    auto& control = *reinterpret_cast<PpmdAllocationControl*>(const_cast<ISzAlloc*>(allocator));
    for (auto& live : control.live) {
        if (live == address) {
            ppmd_test_free(nullptr, address);
            live = nullptr;
            return;
        }
    }
    control.invalid_free = true;
}

// Purpose: Exercise the production PPMd reader through the public folder decoder, not a copied implementation.
// Inputs: `stream` injects I/O behavior; `packed_size` and `output` declare exact packed and decoded extents.
// Outputs: Returns the real decoder status and writes any decoded bytes into caller-owned output.
SRes decode_ppmd(PpmdTestStream& stream, UInt64 packed_size = 5, std::span<Byte> output = {},
                 ISzAllocPtr controlled_allocator = nullptr) {
    stream.vt = {ppmd_test_look, ppmd_test_skip, ppmd_test_read, ppmd_test_seek};
    std::array<Byte, 11> coders{1, 0x23, 0x03, 0x04, 0x01, 5, 2, 0, 0, 1, 0};
    std::array<std::size_t, 2> coder_offsets{0, coders.size()};
    std::array<UInt32, 2> indices{0, 1};
    std::array<Byte, 1> main_indices{0};
    std::array<UInt64, 1> unpack_sizes{output.size()};
    std::array<UInt64, 2> pack_positions{0, packed_size};
    CSzAr archive{};
    archive.NumPackStreams = 1;
    archive.NumFolders = 1;
    archive.PackPositions = pack_positions.data();
    archive.FoCodersOffsets = coder_offsets.data();
    archive.FoStartPackStreamIndex = indices.data();
    archive.FoToCoderUnpackSizes = indices.data();
    archive.FoToMainUnpackSizeIndex = main_indices.data();
    archive.CoderUnpackSizes = unpack_sizes.data();
    archive.CodersData = coders.data();
    const ISzAlloc allocator{ppmd_test_allocate, ppmd_test_free};
    return SzAr_DecodeFolder(&archive, 0, &stream.vt, 0, output.data(), output.size(),
                             controlled_allocator != nullptr ? controlled_allocator : &allocator);
}

}  // namespace

// Purpose: Decode an empty folder without requiring a dummy output allocation.
// Inputs: A null empty output span and valid or truncated packed range state.
// Outputs: Accepts the valid stream and still rejects invalid packed data before returning success.
TEST_CASE(sevenzip_ppmd_accepts_null_empty_output_without_skipping_validation) {
    const std::span<Byte> output;
    REQUIRE_TRUE(output.data() == nullptr);
    PpmdTestStream valid;
    REQUIRE_EQ(decode_ppmd(valid, 5, output), SZ_OK);
    PpmdTestStream truncated;
    REQUIRE_EQ(decode_ppmd(truncated, 4, output), SZ_ERROR_DATA);
}

// Purpose: Retain both PPMd owners through decoding and release them on allocation or callback failure.
// Inputs: Independent allocation failures, a real I/O failure and known valid empty output.
// Outputs: Exact SDK statuses, no leaked owner, no invalid free, and no I/O before admitted allocations.
TEST_CASE(sevenzip_ppmd_releases_common_owner_on_every_exit) {
    for (std::size_t fail_call = 0; fail_call <= 2; ++fail_call) {
        PpmdAllocationControl control{{ppmd_control_allocate, ppmd_control_free}};
        control.fail_call = fail_call;
        PpmdTestStream stream;
        stream.allocation_control = &control;
        REQUIRE_EQ(decode_ppmd(stream, 5, {}, &control.vt), fail_call == 0 ? SZ_OK : SZ_ERROR_MEM);
        REQUIRE_EQ(control.calls, fail_call == 1 ? 1U : 2U);
        REQUIRE_TRUE(!control.invalid_free);
        REQUIRE_TRUE(control.live[0] == nullptr && control.live[1] == nullptr);
        if (fail_call != 0) {
            REQUIRE_EQ(stream.look_calls, 0U);
        } else {
            REQUIRE_TRUE(control.bridge_checks > 0U && control.bridge_is_owned);
        }
    }
    PpmdAllocationControl control{{ppmd_control_allocate, ppmd_control_free}};
    PpmdTestStream stream;
    stream.allocation_control = &control;
    stream.fail_look_call = 1;
    REQUIRE_EQ(decode_ppmd(stream, 5, {}, &control.vt), SZ_ERROR_READ);
    REQUIRE_TRUE(!control.invalid_free);
    REQUIRE_TRUE(control.live[0] == nullptr && control.live[1] == nullptr);
    REQUIRE_TRUE(control.bridge_checks > 0U && control.bridge_is_owned);
}

// Purpose: Preserve successful PPMd initialization across arbitrary input chunk boundaries.
// Inputs: The same valid empty range stream, supplied one to five bytes at a time.
// Outputs: Requires successful decoding for every fragmentation pattern.
TEST_CASE(sevenzip_ppmd_accepts_fragmented_input) {
    for (std::size_t chunk = 1; chunk <= 5; ++chunk) {
        PpmdTestStream stream;
        stream.chunk_size = chunk;
        REQUIRE_EQ(decode_ppmd(stream), SZ_OK);
    }
}

// Purpose: Never accept a packed stream when its input provider reports a read failure.
// Inputs: Valid range bytes returned together with an injected error, at initial and later refills.
// Outputs: Requires the exact I/O error instead of success, with no callback after the failed read.
TEST_CASE(sevenzip_ppmd_preserves_read_errors_with_bytes) {
    for (const std::size_t failed_call : {1U, 2U}) {
        PpmdTestStream stream;
        stream.chunk_size = failed_call == 1U ? 5U : 2U;
        stream.fail_look_call = failed_call;
        REQUIRE_EQ(decode_ppmd(stream), SZ_ERROR_READ);
        REQUIRE_EQ(stream.look_calls, failed_call);
    }
}

// Purpose: Stop before another look when advancing the prior packed span fails.
// Inputs: A valid fragmented stream whose nonzero skip operation reports an I/O error.
// Outputs: Requires the original skip error and exactly one successful look.
TEST_CASE(sevenzip_ppmd_preserves_skip_errors) {
    PpmdTestStream stream;
    stream.chunk_size = 1;
    stream.fail_skip = true;
    REQUIRE_EQ(decode_ppmd(stream), SZ_ERROR_READ);
    REQUIRE_EQ(stream.look_calls, 1U);
}

// Purpose: Ignore unspecified output parameters when an input callback reports an error.
// Inputs: Initial and later read failures that leave buffer and requested size unchanged.
// Outputs: Requires the original error without dereferencing uninitialized or stale input.
TEST_CASE(sevenzip_ppmd_does_not_consume_failed_look_outputs) {
    for (const std::size_t failed_call : {1U, 2U}) {
        PpmdTestStream stream;
        stream.chunk_size = 2;
        stream.fail_look_call = failed_call;
        stream.leave_failed_outputs_unchanged = true;
        REQUIRE_EQ(decode_ppmd(stream), SZ_ERROR_READ);
        REQUIRE_EQ(stream.look_calls, failed_call);
    }
}

// Purpose: Restrict look requests to this folder's packed span, even if more source bytes are available.
// Inputs: A complete range stream followed by an unrelated byte, and shorter declared packed extents.
// Outputs: Accepts only the complete extent and never asks the provider to expose bytes beyond it.
TEST_CASE(sevenzip_ppmd_bounds_declared_packed_input) {
    for (UInt64 packed_size = 0; packed_size <= 5; ++packed_size) {
        PpmdTestStream stream;
        stream.input_size = stream.input.size();
        const auto result = decode_ppmd(stream, packed_size);
        REQUIRE_EQ(result, packed_size == 5U ? SZ_OK : SZ_ERROR_DATA);
        REQUIRE_TRUE(stream.max_requested <= packed_size);
    }
}

// Purpose: Reject truncated data and inconsistent size declarations while retaining the success control.
// Inputs: Every shorter physical range stream, plus a complete stream with an oversized declaration.
// Outputs: Requires a data error for every incomplete or mismatched input.
TEST_CASE(sevenzip_ppmd_rejects_truncated_and_mismatched_input) {
    for (std::size_t input_size = 0; input_size < 5; ++input_size) {
        PpmdTestStream stream;
        stream.input_size = input_size;
        REQUIRE_EQ(decode_ppmd(stream), SZ_ERROR_DATA);
        if (input_size == 0U) {
            REQUIRE_EQ(stream.look_calls, 1U);
        }
    }
    PpmdTestStream stream;
    REQUIRE_EQ(decode_ppmd(stream, 6), SZ_ERROR_DATA);
}

// Purpose: Preserve real PPMd payload decoding through every single-byte and multi-byte refill path.
// Inputs: An independently encoded 432-byte repeated-text fixture and four input fragmentation sizes.
// Outputs: Requires exact decoded text and a look budget no larger than the declared packed extent.
TEST_CASE(sevenzip_ppmd_decodes_independent_text_fixture) {
    for (const std::size_t chunk : {1U, 2U, 7U, 51U}) {
        PpmdTestStream stream;
        stream.input = kTextPpmdInput;
        stream.input_size = stream.input.size();
        stream.chunk_size = chunk;
        std::array<Byte, kPpmdText.size() * 16U> output{};
        REQUIRE_EQ(decode_ppmd(stream, stream.input_size, output), SZ_OK);
        for (std::size_t offset = 0; offset < output.size(); offset += kPpmdText.size()) {
            REQUIRE_TRUE(std::equal(kPpmdText.begin(), kPpmdText.end(), output.begin() + offset));
        }
        REQUIRE_TRUE(stream.max_requested <= stream.input_size);
    }
}

// Purpose: Stop on all premature packed boundaries rather than using neighboring archive bytes to finish.
// Inputs: Every truncated declared extent of the independently encoded nonempty text fixture.
// Outputs: Requires rejection even when the source provider still holds the full valid stream.
TEST_CASE(sevenzip_ppmd_rejects_truncated_text_fixture) {
    for (std::size_t extent = 0; extent < kTextPpmdInput.size(); ++extent) {
        PpmdTestStream stream;
        stream.input = kTextPpmdInput;
        stream.input_size = stream.input.size();
        std::array<Byte, kPpmdText.size() * 16U> output{};
        REQUIRE_EQ(decode_ppmd(stream, extent, output), SZ_ERROR_DATA);
        REQUIRE_TRUE(stream.max_requested <= extent);
    }
}

// Purpose: Reject an inconsistent provider response before indexing its returned pointer.
// Inputs: Successful look responses with a null nonempty buffer or an oversized returned extent.
// Outputs: Requires an SDK contract failure without any subsequent look operation.
TEST_CASE(sevenzip_ppmd_rejects_invalid_look_buffers) {
    PpmdTestStream null_stream;
    null_stream.return_null_buffer = true;
    REQUIRE_EQ(decode_ppmd(null_stream), SZ_ERROR_FAIL);
    REQUIRE_EQ(null_stream.look_calls, 1U);
    PpmdTestStream oversized_stream;
    oversized_stream.return_oversized_buffer = true;
    REQUIRE_EQ(decode_ppmd(oversized_stream), SZ_ERROR_FAIL);
    REQUIRE_EQ(oversized_stream.look_calls, 1U);
}
