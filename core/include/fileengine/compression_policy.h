// Copyright (C) 2026 James Hickman
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

// Should this payload be compressed at all?
//
// design_documents/storage_pipeline.md §3, SR-5 / SR-6 / SR-6a / SR-6b.
//
// The core compresses every stored payload when compression is enabled for the
// deployment, and decompresses on every read. For a large share of what is
// actually stored — video, images, Office documents (which are ZIP
// containers), archives, and the conversion pipeline's own renditions — that
// work returns nothing: the payload is already entropy-coded. It is CPU spent
// once on write and again on EVERY read to save a percent or two.
//
// Two mechanisms, in this order:
//
//   1. A SIGNATURE FAST PATH (SR-6a). The payload's leading bytes are matched
//      against formats that are compressed *by construction*. A match skips
//      compression AND skips the trial below.
//   2. A TRIAL DEFLATE (SR-6). Everything unmatched is measured: compress a
//      sample at a low level and keep the compressed form only if it saved
//      enough to pay for the inflate on every future read.
//
// Deciding from the BYTES rather than from a content type is deliberate
// (SR-5). `FileInfo` carries no MIME type and the core should not gain one —
// it is trusted-upstream and leaves interpretation to the doors. A filename is
// not a format, and a type table needs maintaining and is wrong by default for
// every format nobody has thought of yet. Signature matching reads the payload
// itself, so it depends on no filename, no caller-supplied type and no
// database field; SR-5 stands.
//
// The trial is what keeps the signature table from needing to be complete,
// which is why the table contains ONLY formats compressed by construction
// (SR-6b) — see kSkipSignatures in the .cpp for what is deliberately absent
// (PDF, TIFF, legacy OLE2 Office) and why.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fileengine {

//: Defaults from storage_pipeline.md §11-D3, overridable per deployment.
constexpr size_t kCompressionSampleBytes = 128 * 1024;
constexpr double kCompressionMinGain = 0.10;   // keep only if >= 10% saved

//: How the decision was reached. Recorded for metrics and for tests that must
//: assert the fast path actually fired rather than inferring it from the
//: outcome (a signature skip and a measured skip look identical in the result).
enum class CompressionDecisionReason {
    NoData,           // empty payload
    Disabled,         // compression is off for this deployment
    KnownCompressed,  // SR-6a: matched a signature in the skip table
    MeasuredPoor,     // SR-6: the trial saved less than the minimum gain
    MeasuredGood,     // SR-6: the trial paid for itself
    TrialFailed,      // the trial could not run; SR-8 says compress
};

struct CompressionDecision {
    bool compress = true;
    CompressionDecisionReason reason = CompressionDecisionReason::MeasuredGood;
    //: Ratio observed by the trial (compressed/original), or -1 when no trial
    //: ran. Diagnostics only; nothing branches on it after the fact.
    double sample_ratio = -1.0;

    const char* reason_name() const;
};

struct CompressionPolicyConfig {
    bool enabled = true;                              // deployment flag (D5)
    size_t sample_bytes = kCompressionSampleBytes;
    double min_gain = kCompressionMinGain;
};

//: True when `head` begins with a format that is compressed by construction
//: (SR-6a). `head` may be a prefix of the payload; a short buffer simply fails
//: to match rather than reading out of range.
bool looks_already_compressed(const uint8_t* head, size_t n);
bool looks_already_compressed(const std::vector<uint8_t>& head);

//: The signature name that matched, or "" — for logging and for tests that
//: assert WHICH rule fired rather than only that one did.
std::string matched_signature(const uint8_t* head, size_t n);

//: SR-5 – SR-6b. `head` is the payload, or its leading bytes when the payload
//: is arriving as a stream (SR-7: the decision is made from the first buffer
//: and applied unchanged to the remainder of that write).
CompressionDecision decide_compression(const uint8_t* head, size_t n,
                                       const CompressionPolicyConfig& cfg = {});
CompressionDecision decide_compression(const std::vector<uint8_t>& head,
                                       const CompressionPolicyConfig& cfg = {});

} // namespace fileengine
