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

#include "fileengine/compression_policy.h"

#include <algorithm>
#include <cstring>

#include <zlib.h>

namespace fileengine {

namespace {

struct Signature {
    const char* name;
    size_t offset;              // where the pattern starts
    const uint8_t* pattern;
    size_t len;
    //: Optional second pattern at a second offset (RIFF/WEBP needs it: "RIFF"
    //: alone is also WAV and AVI, which are NOT compressed).
    size_t offset2;
    const uint8_t* pattern2;
    size_t len2;
};

#define SIG_BYTES(name, ...) static const uint8_t name[] = {__VA_ARGS__}

SIG_BYTES(kZipLocal,   0x50, 0x4B, 0x03, 0x04);
SIG_BYTES(kZipEmpty,   0x50, 0x4B, 0x05, 0x06);
SIG_BYTES(kZipSpan,    0x50, 0x4B, 0x07, 0x08);
SIG_BYTES(kGzip,       0x1F, 0x8B);
SIG_BYTES(kBzip2,      0x42, 0x5A, 0x68);                          // "BZh"
SIG_BYTES(kXz,         0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00);
SIG_BYTES(kZstd,       0x28, 0xB5, 0x2F, 0xFD);
SIG_BYTES(kSevenZip,   0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C);
SIG_BYTES(kRar4,       0x52, 0x61, 0x72, 0x21, 0x1A, 0x07);        // "Rar!\x1a\x07"
SIG_BYTES(kJpeg,       0xFF, 0xD8, 0xFF);
SIG_BYTES(kPng,        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A);
SIG_BYTES(kGif87,      0x47, 0x49, 0x46, 0x38, 0x37, 0x61);        // "GIF87a"
SIG_BYTES(kGif89,      0x47, 0x49, 0x46, 0x38, 0x39, 0x61);        // "GIF89a"
SIG_BYTES(kRiff,       0x52, 0x49, 0x46, 0x46);                    // "RIFF"
SIG_BYTES(kWebp,       0x57, 0x45, 0x42, 0x50);                    // "WEBP" @8
SIG_BYTES(kFtyp,       0x66, 0x74, 0x79, 0x70);                    // "ftyp" @4
SIG_BYTES(kMatroska,   0x1A, 0x45, 0xDF, 0xA3);                    // mkv / webm
SIG_BYTES(kOgg,        0x4F, 0x67, 0x67, 0x53);                    // "OggS"
SIG_BYTES(kId3,        0x49, 0x44, 0x33);                          // "ID3"
SIG_BYTES(kFlac,       0x66, 0x4C, 0x61, 0x43);                    // "fLaC"

#undef SIG_BYTES

// storage_pipeline.md §3.1. ONLY formats compressed by construction (SR-6b).
//
// Deliberately absent, each for a reason — these are the entries a
// well-meaning extension of this table would add and must not:
//
//   PDF  (%PDF)          streams are compressed, but the xref table, object
//                        headers and metadata are not; a PDF commonly still
//                        yields 5-15%. Left to the trial.
//   TIFF (II*\0 / MM\0*) may be LZW- or JPEG-compressed, or entirely raw. The
//                        signature does not say which.
//   OLE2 (D0 CF 11 E0)   legacy .doc/.xls/.ppt are not compressed at all and
//                        compress very well.
//   RIFF alone           WAV and AVI are RIFF and are typically uncompressed;
//                        only RIFF....WEBP qualifies, hence the second pattern.
//
// There is deliberately NO known-compressible table (IFC, JSON, XML...). It
// would save one 128 KiB trial against a full-payload compress that is
// happening anyway, where the skip list avoids a whole-payload deflate on
// write and an inflate on every read.
const Signature kSkipSignatures[] = {
    {"zip",      0, kZipLocal, sizeof(kZipLocal), 0, nullptr, 0},
    {"zip",      0, kZipEmpty, sizeof(kZipEmpty), 0, nullptr, 0},
    {"zip",      0, kZipSpan,  sizeof(kZipSpan),  0, nullptr, 0},
    {"gzip",     0, kGzip,     sizeof(kGzip),     0, nullptr, 0},
    {"bzip2",    0, kBzip2,    sizeof(kBzip2),    0, nullptr, 0},
    {"xz",       0, kXz,       sizeof(kXz),       0, nullptr, 0},
    {"zstd",     0, kZstd,     sizeof(kZstd),     0, nullptr, 0},
    {"7z",       0, kSevenZip, sizeof(kSevenZip), 0, nullptr, 0},
    {"rar",      0, kRar4,     sizeof(kRar4),     0, nullptr, 0},
    {"jpeg",     0, kJpeg,     sizeof(kJpeg),     0, nullptr, 0},
    {"png",      0, kPng,      sizeof(kPng),      0, nullptr, 0},
    {"gif",      0, kGif87,    sizeof(kGif87),    0, nullptr, 0},
    {"gif",      0, kGif89,    sizeof(kGif89),    0, nullptr, 0},
    {"webp",     0, kRiff,     sizeof(kRiff),     8, kWebp, sizeof(kWebp)},
    {"iso-bmff", 4, kFtyp,     sizeof(kFtyp),     0, nullptr, 0},  // mp4/mov/heic/avif/3gp
    {"matroska", 0, kMatroska, sizeof(kMatroska), 0, nullptr, 0},
    {"ogg",      0, kOgg,      sizeof(kOgg),      0, nullptr, 0},
    {"mp3-id3",  0, kId3,      sizeof(kId3),      0, nullptr, 0},
    {"flac",     0, kFlac,     sizeof(kFlac),     0, nullptr, 0},
};

bool matches_at(const uint8_t* head, size_t n, size_t off,
                const uint8_t* pat, size_t len) {
    if (pat == nullptr || len == 0) return true;   // "no second pattern"
    if (off > n || n - off < len) return false;
    return std::memcmp(head + off, pat, len) == 0;
}

// An MPEG audio frame sync: 11 set bits, then a layer/version that is not the
// reserved encoding. Checked separately from the table because it is a bit
// pattern rather than a byte string, and because a bare two-byte 0xFF Ex test
// is loose enough to match arbitrary binary — the extra validation below is
// what keeps it from doing so.
bool looks_like_mp3_frame(const uint8_t* h, size_t n) {
    if (n < 3) return false;
    if (h[0] != 0xFF) return false;
    if ((h[1] & 0xE0) != 0xE0) return false;        // sync bits
    if ((h[1] & 0x18) == 0x08) return false;        // reserved MPEG version
    if ((h[1] & 0x06) == 0x00) return false;        // reserved layer
    if ((h[2] & 0xF0) == 0xF0) return false;        // bad bitrate index
    if ((h[2] & 0x0C) == 0x0C) return false;        // reserved sample rate
    return true;
}

} // namespace

const char* CompressionDecision::reason_name() const {
    switch (reason) {
        case CompressionDecisionReason::NoData:          return "no-data";
        case CompressionDecisionReason::Disabled:        return "disabled";
        case CompressionDecisionReason::KnownCompressed: return "known-compressed";
        case CompressionDecisionReason::MeasuredPoor:    return "measured-poor";
        case CompressionDecisionReason::MeasuredGood:    return "measured-good";
        case CompressionDecisionReason::TrialFailed:     return "trial-failed";
    }
    return "unknown";
}

std::string matched_signature(const uint8_t* head, size_t n) {
    if (head == nullptr || n == 0) return "";
    for (const auto& sig : kSkipSignatures) {
        if (!matches_at(head, n, sig.offset, sig.pattern, sig.len)) continue;
        if (!matches_at(head, n, sig.offset2, sig.pattern2, sig.len2)) continue;
        return sig.name;
    }
    if (looks_like_mp3_frame(head, n)) return "mp3-frame";
    return "";
}

bool looks_already_compressed(const uint8_t* head, size_t n) {
    return !matched_signature(head, n).empty();
}

bool looks_already_compressed(const std::vector<uint8_t>& head) {
    return looks_already_compressed(head.data(), head.size());
}

CompressionDecision decide_compression(const uint8_t* head, size_t n,
                                       const CompressionPolicyConfig& cfg) {
    CompressionDecision d;

    if (!cfg.enabled) {
        d.compress = false;
        d.reason = CompressionDecisionReason::Disabled;
        return d;
    }
    if (head == nullptr || n == 0) {
        // Nothing to compress and nothing to measure. Reported as its own
        // reason rather than folded into "poor" so the caller can tell an empty
        // payload from a measured refusal.
        d.compress = false;
        d.reason = CompressionDecisionReason::NoData;
        return d;
    }

    // SR-6a: the signature fast path, ahead of the trial.
    if (looks_already_compressed(head, n)) {
        d.compress = false;
        d.reason = CompressionDecisionReason::KnownCompressed;
        return d;
    }

    // SR-6: measure. Level 1 — this is a prediction, not the final compression,
    // and level 1 predicts compressibility well at a fraction of the cost.
    const size_t sample = std::min(n, cfg.sample_bytes);
    uLongf bound = compressBound(static_cast<uLong>(sample));
    std::vector<uint8_t> out(bound);
    const int rc = compress2(out.data(), &bound, head, static_cast<uLong>(sample), 1);
    if (rc != Z_OK) {
        // SR-8: any error resolves to compress. A wrong "compress" costs CPU; a
        // wrong "skip" costs storage. Neither can cost correctness, because the
        // outcome is recorded per version (SR-1).
        d.compress = true;
        d.reason = CompressionDecisionReason::TrialFailed;
        return d;
    }

    d.sample_ratio = static_cast<double>(bound) / static_cast<double>(sample);
    const double saved = 1.0 - d.sample_ratio;
    if (saved >= cfg.min_gain) {
        d.compress = true;
        d.reason = CompressionDecisionReason::MeasuredGood;
    } else {
        d.compress = false;
        d.reason = CompressionDecisionReason::MeasuredPoor;
    }
    return d;
}

CompressionDecision decide_compression(const std::vector<uint8_t>& head,
                                       const CompressionPolicyConfig& cfg) {
    return decide_compression(head.data(), head.size(), cfg);
}

} // namespace fileengine
