#include "test_util.hpp"

#include "core/com_task_string.hpp"

#include <atomic>
#include <memory>
#include <new>
#include <string_view>
#include <type_traits>
#include <objidl.h>

namespace {

// Observes real task-allocator releases without changing allocator behavior or
// allocating memory from inside callbacks. Registration is local to this process.
class TaskMemorySpy final : public IMallocSpy {
  public:
    std::atomic<void*> watched{nullptr};
    std::atomic<unsigned> frees{0U};

    // Purpose: Expose only supported COM interfaces. Inputs: IID/output slot. Outputs: Retained interface or failure.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (result == nullptr) {
            return E_POINTER;
        }
        *result = nullptr;
        if (iid != IID_IUnknown && iid != IID_IMallocSpy) {
            return E_NOINTERFACE;
        }
        *result = static_cast<IMallocSpy*>(this);
        AddRef();
        return S_OK;
    }
    // Purpose: Retain the observer. Inputs: Current reference count. Outputs: Incremented count.
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
    }
    // Purpose: Release the observer. Inputs: Current reference count. Outputs: Deletes after the final reference.
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --references_;
        if (remaining == 0U) {
            delete this;
        }
        return remaining;
    }
    // Purpose: Preserve allocation size. Inputs: Requested bytes. Outputs: Unchanged bytes.
    SIZE_T STDMETHODCALLTYPE PreAlloc(SIZE_T bytes) override {
        return bytes;
    }
    // Purpose: Preserve allocation result. Inputs: Actual pointer. Outputs: Unchanged pointer.
    void* STDMETHODCALLTYPE PostAlloc(void* value) override {
        return value;
    }
    // Purpose: Count a watched real release exactly once. Inputs: Pointer and allocator provenance. Outputs: Same
    // pointer.
    void* STDMETHODCALLTYPE PreFree(void* value, BOOL) override {
        auto expected = value;
        if (value != nullptr && watched.compare_exchange_strong(expected, nullptr)) {
            ++frees;
        }
        return value;
    }
    // Purpose: Preserve free completion. Inputs: Allocator provenance. Outputs: No side effects.
    void STDMETHODCALLTYPE PostFree(BOOL) override {}
    // Purpose: Preserve resizing. Inputs: Old pointer, requested bytes and output slot. Outputs: Same pointer and size.
    SIZE_T STDMETHODCALLTYPE PreRealloc(void* value, SIZE_T bytes, void** replacement, BOOL) override {
        *replacement = value;
        return bytes;
    }
    // Purpose: Preserve resized allocation. Inputs: Actual result and provenance. Outputs: Unchanged result.
    void* STDMETHODCALLTYPE PostRealloc(void* value, BOOL) override {
        return value;
    }
    // Purpose: Preserve size inspection. Inputs: Pointer and provenance. Outputs: Unchanged pointer.
    void* STDMETHODCALLTYPE PreGetSize(void* value, BOOL) override {
        return value;
    }
    // Purpose: Preserve size result. Inputs: Size and provenance. Outputs: Unchanged size.
    SIZE_T STDMETHODCALLTYPE PostGetSize(SIZE_T bytes, BOOL) override {
        return bytes;
    }
    // Purpose: Preserve ownership inspection. Inputs: Pointer and provenance. Outputs: Unchanged pointer.
    void* STDMETHODCALLTYPE PreDidAlloc(void* value, BOOL) override {
        return value;
    }
    // Purpose: Preserve ownership result. Inputs: Pointer, provenance and result. Outputs: Unchanged result.
    int STDMETHODCALLTYPE PostDidAlloc(void*, BOOL, int result) override {
        return result;
    }
    // Purpose: Preserve heap maintenance. Inputs: None. Outputs: No side effects.
    void STDMETHODCALLTYPE PreHeapMinimize() override {}
    // Purpose: Preserve heap maintenance completion. Inputs: None. Outputs: No side effects.
    void STDMETHODCALLTYPE PostHeapMinimize() override {}

  private:
    std::atomic<ULONG> references_{1U};
};

class TaskMemoryObservation final {
  public:
    // Purpose: Register a process-local observer with COM-owned lifetime safety.
    // Inputs: None. Outputs: Active observer, or failure without retaining a partial registration.
    TaskMemoryObservation() {
        const auto result = CoRegisterMallocSpy(spy);
        if (FAILED(result)) {
            spy->Release();
            throw std::runtime_error("could not register task allocator observer");
        }
    }
    // Purpose: Revoke this test's observer before releasing its own reference.
    // Inputs: Active registration. Outputs: No observer remains registered after complete allocation cleanup.
    ~TaskMemoryObservation() {
        if (active_) {
            (void)CoRevokeMallocSpy();
        }
        spy->Release();
    }
    // Purpose: Verify revocation while assertions can still report a failure.
    // Inputs: Active registration. Outputs: COM result; successful revocation prevents another destructor attempt.
    HRESULT finish() {
        const auto result = CoRevokeMallocSpy();
        if (SUCCEEDED(result)) {
            active_ = false;
        }
        return result;
    }
    TaskMemoryObservation(const TaskMemoryObservation&) = delete;
    TaskMemoryObservation& operator=(const TaskMemoryObservation&) = delete;
    TaskMemorySpy* const spy = new TaskMemorySpy();

  private:
    bool active_ = true;
};

// Purpose: Simulate an API that supplies allocated output before reporting failure.
// Inputs: Empty caller-owned output slot and independent release observer. Outputs: A tiny real COM allocation and
// E_FAIL.
HRESULT allocate_failed_shell_output(PWSTR* output, TaskMemorySpy& spy) {
    *output = static_cast<PWSTR>(CoTaskMemAlloc(32U));
    REQUIRE_TRUE(*output != nullptr);
    (*output)[0] = L'x';
    (*output)[1] = L'\0';
    spy.watched = *output;
    return E_FAIL;
}

// Purpose: Inspect the embedded manifest without executing the inspected application's code.
// Inputs: An already-built sibling executable under the current test binary's directory.
// Outputs: Returns a copy of its bounded manifest bytes; throws on missing/invalid resource metadata.
std::string embedded_manifest(const std::filesystem::path& executable) {
    const auto unload = [](HMODULE module) { (void)FreeLibrary(module); };
    const std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(unload)> module(
        LoadLibraryExW(executable.c_str(), nullptr,
                       LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE),
        unload);
    REQUIRE_TRUE(module != nullptr);
    const auto resource = FindResourceW(module.get(), MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(24));
    REQUIRE_TRUE(resource != nullptr);
    const auto length = SizeofResource(module.get(), resource);
    REQUIRE_TRUE(length > 0U && length <= 64U * 1024U);
    const auto loaded = LoadResource(module.get(), resource);
    REQUIRE_TRUE(loaded != nullptr);
    const auto* data = static_cast<const char*>(LockResource(loaded));
    REQUIRE_TRUE(data != nullptr);
    return {data, length};
}

}  // namespace

// Purpose: Prove real task-allocator ownership across replacement, failed API output and stack unwinding.
// Inputs: Bounded COM allocations and an injected bad_alloc after acquisition, without exhausting host memory.
// Outputs: The allocator observes one release per owned pointer and none while borrowed text remains in use.
TEST_CASE(windows_task_string_owns_failed_outputs_and_unwinds) {
    static_assert(!std::is_copy_constructible_v<flitzzip::CoTaskMemWideString>);
    static_assert(std::is_nothrow_destructible_v<flitzzip::CoTaskMemWideString>);
    TaskMemoryObservation observation;
    auto& spy = *observation.spy;
    {
        flitzzip::CoTaskMemWideString value;
        REQUIRE_TRUE(value.get() == nullptr);
        REQUIRE_EQ(allocate_failed_shell_output(value.put(), spy), E_FAIL);
        int unrelated = 0;
        REQUIRE_TRUE(spy.PreFree(&unrelated, FALSE) == &unrelated);
        REQUIRE_EQ(std::filesystem::path(value.get()), std::filesystem::path(L"x"));
        REQUIRE_EQ(spy.frees.load(), 0U);
        REQUIRE_TRUE(*value.put() == nullptr);
        REQUIRE_EQ(spy.frees.load(), 1U);
        REQUIRE_EQ(allocate_failed_shell_output(value.put(), spy), E_FAIL);
    }
    REQUIRE_EQ(spy.frees.load(), 2U);
    bool caught = false;
    try {
        flitzzip::CoTaskMemWideString value;
        (void)allocate_failed_shell_output(value.put(), spy);
        throw std::bad_alloc();
    } catch (const std::bad_alloc&) {
        caught = true;
    }
    REQUIRE_TRUE(caught);
    REQUIRE_EQ(spy.frees.load(), 3U);
    REQUIRE_TRUE(spy.watched.load() == nullptr);
    REQUIRE_EQ(observation.finish(), S_OK);
}

// Purpose: Prevent CLI/test compatibility shims from diverging from the shipped graphical application.
// Inputs: The configured executable resources, inspected without launching the optional GUI.
// Outputs: Requires one Windows 10/11 identity per enabled component and preserved GUI DPI metadata when built.
TEST_CASE(windows_embedded_compatibility_manifest_parity) {
    std::wstring executable(32768U, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    REQUIRE_TRUE(length > 0U && length < executable.size());
    executable.resize(length);
    const auto directory = std::filesystem::path(executable).parent_path();
    constexpr std::string_view identity = "{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}";
    constexpr const wchar_t* names[] = {
        L"flitzzip_tests.exe",
        L"flitzzip_cli.exe",
#if FLITZZIP_TEST_GUI_BUILT
        L"FlitzZip.exe",
#endif
    };
    for (const auto* name : names) {
        const auto manifest = embedded_manifest(directory / name);
        const auto position = manifest.find(identity);
        REQUIRE_TRUE(position != std::string::npos);
        REQUIRE_EQ(manifest.find(identity, position + identity.size()), std::string::npos);
        if (std::wstring_view(name) == L"FlitzZip.exe") {
            REQUIRE_TRUE(manifest.find("PerMonitorV2") != std::string::npos);
        }
    }
}
