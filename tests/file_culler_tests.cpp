#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <memory>
#include <limits>
#include <filesystem>
#include <fstream>
#include <functional>

#include "fileengine/file_culler.h"
#include "fileengine/types.h"
#include "fileengine/IStorage.h"
#include "fileengine/IObjectStore.h"
#include "fileengine/storage_tracker.h"

using namespace fileengine;

// ---------------------------------------------------------------------------
// Test doubles. Only the methods the culler exercises carry behavior; the rest
// are inert stubs implementing the interface contracts.
// ---------------------------------------------------------------------------

// Records delete_file calls so a test can assert whether a local payload was
// (or, critically, was NOT) culled.
class MockStorage : public IStorage {
public:
    int delete_calls = 0;
    std::vector<std::string> deleted;

    Result<void> delete_file(const std::string& storage_path, const std::string& = "") override {
        ++delete_calls;
        deleted.push_back(storage_path);
        return Result<void>::ok();
    }

    // --- inert stubs ---
    Result<std::string> store_file(const std::string&, const std::string&,
                                   const std::vector<uint8_t>&, const std::string& = "") override {
        return Result<std::string>::ok("");
    }
    Result<std::vector<uint8_t>> read_file(const std::string&, const std::string& = "") override {
        return Result<std::vector<uint8_t>>::ok({});
    }
    Result<bool> file_exists(const std::string&, const std::string& = "") override {
        return Result<bool>::ok(true);
    }
    std::string get_storage_path(const std::string& uid, const std::string& vts,
                                 const std::string& = "") const override {
        return uid + "/" + vts;
    }
    bool is_encryption_enabled() const override { return false; }
    bool is_compression_enabled() const override { return false; }
    Result<void> create_tenant_directory(const std::string&) override { return Result<void>::ok(); }
    Result<bool> tenant_directory_exists(const std::string&) override { return Result<bool>::ok(true); }
    Result<void> cleanup_tenant_directory(const std::string&) override { return Result<void>::ok(); }
    Result<void> sync_to_object_store(
        std::function<void(const std::string&, const std::string&, int)> = nullptr) override {
        return Result<void>::ok();
    }
    Result<std::vector<std::string>> get_local_file_paths(const std::string& = "") const override {
        return Result<std::vector<std::string>>::ok({});
    }
    Result<void> clear_storage(const std::string& = "") override { return Result<void>::ok(); }
    void set_object_store(IObjectStore*) override {}
    IObjectStore* get_object_store() const override { return nullptr; }
};

// Object store whose file_exists answer is scriptable: either a fixed boolean or
// a simulated backend error.
class MockObjectStore : public IObjectStore {
public:
    bool exists_result = false;   // does the payload exist in the store?
    bool exists_error = false;    // simulate a store error on the existence check
    mutable int exists_calls = 0; // how many times verification queried the store

    Result<bool> file_exists(const std::string&, const std::string& = "") override {
        ++exists_calls;
        if (exists_error) return Result<bool>::err("simulated object-store error");
        return Result<bool>::ok(exists_result);
    }
    std::string get_storage_path(const std::string& vp, const std::string& vts,
                                 const std::string& tenant = "") const override {
        return (tenant.empty() ? "" : tenant + "/") + vp + "/" + vts;
    }

    // --- inert stubs ---
    bool is_initialized() const override { return true; }
    Result<void> initialize() override { return Result<void>::ok(); }
    Result<std::string> store_file(const std::string&, const std::string&,
                                   const std::vector<uint8_t>&, const std::string& = "") override {
        return Result<std::string>::ok("");
    }
    Result<std::vector<uint8_t>> read_file(const std::string&, const std::string& = "") override {
        return Result<std::vector<uint8_t>>::ok({});
    }
    Result<void> delete_file(const std::string&, const std::string& = "") override {
        return Result<void>::ok();
    }
    Result<void> create_bucket_if_not_exists(const std::string& = "") override { return Result<void>::ok(); }
    Result<bool> bucket_exists(const std::string& = "") override { return Result<bool>::ok(true); }
    bool is_encryption_enabled() const override { return false; }
    Result<void> create_tenant_bucket(const std::string&) override { return Result<void>::ok(); }
    Result<bool> tenant_bucket_exists(const std::string&) override { return Result<bool>::ok(true); }
    Result<void> cleanup_tenant_bucket(const std::string&) override { return Result<void>::ok(); }
    Result<void> clear_storage(const std::string& = "") override { return Result<void>::ok(); }
};

namespace {
// Build a real on-disk payload whose path matches Storage::get_storage_path's
// layout: <base>/<tenant>/<l1>/<l2>/<l3>/<uid>/<version_timestamp>. The culler
// calls std::filesystem::file_size on candidates, so the file must exist.
std::filesystem::path make_payload(const std::filesystem::path& base) {
    std::filesystem::path file =
        base / "tenant_test" / "ab" / "cd" / "ef" / "abcdef0123456789" / "20260101_120000.000";
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << "payload-bytes";
    return file;
}

// Construct a culler over the given doubles. Its default config is enabled with
// the LRU strategy, so registering a candidate with the tracker and then calling
// perform_culling_for_space(SIZE_MAX) — a demand the early-out can never satisfy
// — drives the candidate through verify_file_in_object_store.
FileCuller make_culler(MockStorage& storage, MockObjectStore* os, StorageTracker& tracker) {
    return FileCuller(&storage, os, &tracker);
}
}  // namespace

void test_culling_config_structure() {
    std::cout << "Testing CullingConfig structure..." << std::endl;
    fileengine::CullingConfig config;
    config.enabled = true;
    config.threshold_percentage = 0.85;
    config.min_age_days = 30;
    config.keep_count = 2;
    config.strategy = "lru";
    assert(config.enabled == true);
    assert(config.threshold_percentage == 0.85);
    assert(config.keep_count == 2);
    assert(config.strategy == "lru");
    std::cout << "CullingConfig structure test passed!" << std::endl;
}

// Data-loss regression: a payload that is NOT confirmed present in the object
// store must never be culled from local storage (that would destroy the sole
// copy). verify_file_in_object_store must fail closed.
void test_cull_skips_when_object_missing() {
    std::cout << "Testing cull is skipped when object store lacks the payload..." << std::endl;
    auto base = std::filesystem::temp_directory_path() / "fe_culler_missing";
    std::filesystem::remove_all(base);
    auto file = make_payload(base);

    StorageTracker tracker(base.string());
    tracker.record_file_creation(file.string(), 13, "");

    MockStorage storage;
    MockObjectStore os;
    os.exists_result = false;  // payload is NOT backed up

    FileCuller culler = make_culler(storage, &os, tracker);
    culler.perform_culling_for_space(std::numeric_limits<size_t>::max());

    assert(os.exists_calls >= 1);              // verification actually consulted the store
    assert(storage.delete_calls == 0);         // fail closed: nothing culled
    assert(std::filesystem::exists(file));     // local payload preserved
    std::filesystem::remove_all(base);
    std::cout << "Skip-when-missing test passed!" << std::endl;
}

// The counterpart: when the object store confirms the payload, culling proceeds.
void test_cull_proceeds_when_object_present() {
    std::cout << "Testing cull proceeds when the payload is safely in the object store..." << std::endl;
    auto base = std::filesystem::temp_directory_path() / "fe_culler_present";
    std::filesystem::remove_all(base);
    auto file = make_payload(base);

    StorageTracker tracker(base.string());
    tracker.record_file_creation(file.string(), 13, "");

    MockStorage storage;
    MockObjectStore os;
    os.exists_result = true;  // payload IS backed up

    FileCuller culler = make_culler(storage, &os, tracker);
    culler.perform_culling_for_space(std::numeric_limits<size_t>::max());

    assert(os.exists_calls >= 1);
    assert(storage.delete_calls == 1);              // culled
    assert(storage.deleted.front() == file.string());
    std::filesystem::remove_all(base);
    std::cout << "Proceed-when-present test passed!" << std::endl;
}

// A transient store error must also fail closed — an unverifiable payload is
// treated as "not safe to cull".
void test_cull_skips_on_verify_error() {
    std::cout << "Testing cull is skipped when the existence check errors..." << std::endl;
    auto base = std::filesystem::temp_directory_path() / "fe_culler_error";
    std::filesystem::remove_all(base);
    auto file = make_payload(base);

    StorageTracker tracker(base.string());
    tracker.record_file_creation(file.string(), 13, "");

    MockStorage storage;
    MockObjectStore os;
    os.exists_error = true;  // store cannot answer

    FileCuller culler = make_culler(storage, &os, tracker);
    culler.perform_culling_for_space(std::numeric_limits<size_t>::max());

    assert(storage.delete_calls == 0);         // fail closed on error
    assert(std::filesystem::exists(file));
    std::filesystem::remove_all(base);
    std::cout << "Skip-on-error test passed!" << std::endl;
}

// With no object store at all there is nowhere durable to fall back to, so
// culling must never delete local data.
void test_cull_skips_without_object_store() {
    std::cout << "Testing cull is skipped when there is no object store..." << std::endl;
    auto base = std::filesystem::temp_directory_path() / "fe_culler_noos";
    std::filesystem::remove_all(base);
    auto file = make_payload(base);

    StorageTracker tracker(base.string());
    tracker.record_file_creation(file.string(), 13, "");

    MockStorage storage;
    FileCuller culler = make_culler(storage, nullptr, tracker);
    culler.perform_culling_for_space(std::numeric_limits<size_t>::max());

    assert(storage.delete_calls == 0);
    assert(std::filesystem::exists(file));
    std::filesystem::remove_all(base);
    std::cout << "Skip-without-object-store test passed!" << std::endl;
}

int main() {
    std::cout << "Running FileEngine Core File Culler Unit Tests..." << std::endl;

    test_culling_config_structure();
    test_cull_skips_when_object_missing();
    test_cull_proceeds_when_object_present();
    test_cull_skips_on_verify_error();
    test_cull_skips_without_object_store();

    std::cout << "All FileCuller unit tests passed!" << std::endl;
    return 0;
}
