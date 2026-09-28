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

// The compression decision against the REAL local corpus, not synthetic bytes
// — storage_pipeline.md §10 (S2), SR-9.
//
// The synthetic suite in compression_policy_tests.cpp proves the mechanism. It
// cannot prove the mechanism is right about this platform's actual content,
// because the payloads were written to make it right. This one runs the
// decision over the checked-in sample files — real IFC models, real PDFs, real
// glTF/GLB, real STEP — and asserts the outcome against what each format
// genuinely is.
//
// SR-9 is the requirement at stake: IFC, CityJSON, OBJ, STEP and the rest of
// this platform's enormous ASCII must keep being compressed. A regression there
// is quiet and expensive, and no synthetic fixture would catch it.
//
// Skips (77) when the corpus is not present, so the suite is portable.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <zlib.h>

#include "fileengine/compression_policy.h"

using namespace fileengine;
namespace fs = std::filesystem;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::cerr << "  FAIL: " << what << std::endl;
    }
}

static std::vector<uint8_t> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

// The ratio a FULL deflate actually achieves — the ground truth the sampled
// decision is being judged against.
static double actual_ratio(const std::vector<uint8_t>& data) {
    if (data.empty()) return 1.0;
    uLongf bound = compressBound(static_cast<uLong>(data.size()));
    std::vector<uint8_t> out(bound);
    if (compress2(out.data(), &bound, data.data(),
                  static_cast<uLong>(data.size()), Z_DEFAULT_COMPRESSION) != Z_OK) {
        return 1.0;
    }
    return static_cast<double>(bound) / static_cast<double>(data.size());
}

static std::vector<fs::path> corpus_roots() {
    // Walk up from the working directory looking for the sibling repos. Which
    // directory ctest runs a test from is a build-layout detail, and hard-coded
    // "../.." paths break the moment it changes — which is exactly how this
    // test silently skipped the first time it was wired up.
    static const char* kRelative[] = {
        "difference_service/samples/v1",
        "convert_search_ai/tests/fixtures/3d",
        "frontend/html",                      // a real PNG, gzip and SVG
    };

    std::vector<fs::path> found;
    std::error_code ec;
    fs::path base = fs::current_path(ec);
    if (ec) return found;

    for (int up = 0; up < 6 && !base.empty(); ++up) {
        for (const char* rel : kRelative) {
            fs::path candidate = base / rel;
            if (fs::is_directory(candidate, ec)) {
                if (std::find(found.begin(), found.end(), candidate) == found.end()) {
                    found.push_back(candidate);
                }
            }
        }
        if (!found.empty()) break;            // first level that has any wins
        if (!base.has_parent_path() || base.parent_path() == base) break;
        base = base.parent_path();
    }
    return found;
}

// What each extension IS, as a matter of format — not as a matter of what the
// decision happens to return.
enum class Expect { Compressible, AlreadyCompressed, Unknown };

static Expect expected_for(const std::string& ext) {
    // Text-shaped engineering formats: this platform's biggest files, and the
    // ones SR-9 names.
    if (ext == ".ifc" || ext == ".step" || ext == ".stp" || ext == ".obj" ||
        ext == ".gltf" || ext == ".json" || ext == ".wrl" || ext == ".iges" ||
        ext == ".txt" || ext == ".md" || ext == ".xml" || ext == ".svg" ||
        ext == ".html" || ext == ".ply" || ext == ".stl" || ext == ".las") {
        return Expect::Compressible;
    }
    // Binary containers that are compressed by construction.
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".zip" ||
        ext == ".docx" || ext == ".xlsx" || ext == ".pptx" || ext == ".gz" ||
        ext == ".webm" || ext == ".mp4" || ext == ".mp3") {
        return Expect::AlreadyCompressed;
    }
    // .glb and .brep and .pdf are genuinely mixed — glTF-binary may or may not
    // embed compressed buffers, and a PDF's streams are compressed while its
    // xref and metadata are not. The decision must be RIGHT about them, which
    // is judged against the measured ratio rather than against a guess here.
    return Expect::Unknown;
}

int main() {
    std::cout << "=== compression_corpus_tests ===\n";

    const auto roots = corpus_roots();
    if (roots.empty()) {
        std::cout << "SKIP: no local corpus found (difference_service/samples,"
                     " convert_search_ai fixtures)\n";
        return 77;
    }

    int files = 0;
    int agreed = 0;
    for (const auto& root : roots) {
        std::cout << "corpus: " << root << "\n";
        for (const auto& entry : fs::directory_iterator(root)) {
            if (!entry.is_regular_file()) continue;
            const auto data = read_file(entry.path());
            if (data.empty()) continue;
            ++files;

            const std::string ext = entry.path().extension().string();
            const auto decision = decide_compression(data);
            const double truth = actual_ratio(data);
            const std::string name = entry.path().filename().string();

            // THE CORE ASSERTION: the sampled decision agrees with what a full
            // deflate would actually have achieved. A decision to compress must
            // be justified by the real ratio; a decision to skip must not be
            // leaving a large saving on the table.
            const bool really_worth_it = (1.0 - truth) >= kCompressionMinGain;
            if (decision.compress == really_worth_it) {
                ++agreed;
            } else if (decision.reason == CompressionDecisionReason::KnownCompressed) {
                // A signature skip on a file that would in fact have compressed
                // is the accepted cost of SR-6a — but only for formats that ARE
                // compressed by construction. If this fires for something the
                // table should not contain, SR-6b has been violated.
                check(expected_for(ext) != Expect::Compressible,
                      name + ": a signature skipped a compressible format (SR-6b)");
                ++agreed;
            } else {
                // Sampling error on a file whose head is unrepresentative. Only
                // acceptable when the real saving is marginal.
                const double missed = std::abs((1.0 - truth) - kCompressionMinGain);
                check(missed < 0.15,
                      name + ": sampled decision (" + std::string(decision.reason_name()) +
                          ") disagrees with a full deflate ratio of " +
                          std::to_string(truth));
            }

            // SR-9, per file: the text-shaped engineering formats must be
            // compressed, and must really be earning it.
            if (expected_for(ext) == Expect::Compressible) {
                check(decision.compress, name + ": SR-9 — " + ext + " must be compressed");
                check(truth < 0.9,
                      name + ": SR-9 — " + ext + " really does compress (ratio " +
                          std::to_string(truth) + ")");
            }
            if (expected_for(ext) == Expect::AlreadyCompressed) {
                check(!decision.compress,
                      name + ": " + ext + " must not be compressed again");
            }

            std::cout << "  " << name << "  " << data.size() << "B  ratio "
                      << truth << "  -> " << (decision.compress ? "compress" : "skip")
                      << " (" << decision.reason_name() << ")\n";
        }
    }

    check(files > 0, "the corpus contained files");
    // The decision must be right about the overwhelming majority of real
    // content, not merely about the cases someone thought of.
    check(files == 0 || (agreed * 100 / files) >= 90,
          "the decision agreed with a full deflate on " + std::to_string(agreed) +
              "/" + std::to_string(files) + " real files");

    std::cout << "\n=== " << (g_checks - g_failures) << "/" << g_checks
              << " checks passed over " << files << " real files ===\n";
    return g_failures == 0 ? 0 : 1;
}
