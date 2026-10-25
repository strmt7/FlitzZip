// FlitzZip development tool. The repository AGPL-3.0 license applies.
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/ProfileData/InstrProfReader.h"
#include "llvm/ProfileData/Coverage/CoverageMapping.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/DataExtractor.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "InstrProfiling.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {
using namespace llvm;
cl::opt<std::string> HostPath("host", cl::desc("Same-compilation HIP host bitcode"));
cl::opt<std::string> DevicePath("device", cl::desc("Its extracted AMDGPU ELF"));
cl::opt<bool> Identify("identify", cl::init(false),
                       cl::desc("Report the compiled profiling ABI without reading inputs"));
cl::opt<bool> Complete("complete", cl::init(false), cl::desc("Complete missing normal profiling registrations"));
cl::opt<std::string> OutputPath("o", cl::desc("New output bitcode; existing files are never replaced"));
cl::opt<std::string> ProfilePath("profile", cl::desc("Native raw profile to associate with exactly one device image"));
cl::list<std::string> MatchDevices("match-device", cl::desc("Candidate device ELF for native profile association"));
cl::opt<std::string> CoverageProfile("coverage-profile",
                                     cl::desc("Same-device indexed profile for native line mapping"));
constexpr std::uint64_t MaxInputBytes = 64ULL * 1024 * 1024;

struct DeviceSymbol {
    std::string name;
    std::uint64_t bytes = 0;
};

struct DeviceProfile {
    std::map<std::string, DeviceSymbol> symbols;
    std::set<std::string> data;
    std::string names;
};

struct HostProfileGroup {
    std::array<std::string, 4> names;
    // Borrowed module-owned calls; the group never outlives its LLVM module.
    std::array<CallBase*, 4> notifications{};
};

struct HostRegistration {
    Function* owner = nullptr;
    GlobalVariable* table = nullptr;
    std::map<std::string, GlobalVariable*> symbols;
    std::map<GlobalVariable*, CallBase*> nativeCalls;
    std::vector<HostProfileGroup> groups;
};

struct ProfileRegion {
    std::uint64_t begin = 0;
    std::uint64_t bytes = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
};

// Purpose: Return an actionable bounded-input diagnostic without printing IR contents.
// Inputs: Public context text. Outputs: An owned LLVM error; no process or device operation.
Error reject(const Twine& message) {
    return createStringError(inconvertibleErrorCode(), message);
}

// Purpose: Require dense, nonoverlapping symbol coverage of every profile section before completing any registration.
// Inputs: Actual ELF regions and symbols. Outputs: Success only when no hidden or omitted profiling bytes remain.
Error validateProfileRegions(const object::ObjectFile& image) {
    const std::set<std::string> required{
        INSTR_PROF_QUOTE(INSTR_PROF_DATA_COMMON), INSTR_PROF_QUOTE(INSTR_PROF_CNTS_COMMON),
        INSTR_PROF_QUOTE(INSTR_PROF_UCNTS_COMMON), INSTR_PROF_QUOTE(INSTR_PROF_NAME_COMMON)};
    std::map<std::string, ProfileRegion> regions;
    std::uint64_t sectionCount = 0;
    for (const auto& section : image.sections()) {
        if (++sectionCount > 100000)
            return reject("Device ELF section inventory exceeds the analysis budget");
        auto name = section.getName();
        if (!name)
            return name.takeError();
        if (name->size() > 64)
            continue;
        if (!required.contains(name->str()))
            continue;
        if (!section.getSize() || section.getSize() > MaxInputBytes ||
            !regions.emplace(name->str(), ProfileRegion{section.getAddress(), section.getSize(), {}}).second)
            return reject("Invalid or repeated device profile section");
    }
    if (regions.size() != required.size())
        return reject("Device lacks a complete supported profile section set");
    std::uint64_t symbolCount = 0;
    for (const auto& symbol : image.symbols()) {
        if (++symbolCount > 1000000)
            return reject("Device ELF symbol inventory exceeds the analysis budget");
        auto section = symbol.getSection();
        if (!section)
            return section.takeError();
        if (*section == image.section_end())
            continue;
        auto sectionName = (**section).getName();
        auto name = symbol.getName();
        if (!sectionName)
            return sectionName.takeError();
        if (!name)
            return name.takeError();
        if (sectionName->size() > 64)
            continue;
        auto found = regions.find(sectionName->str());
        if (found == regions.end() ||
            (!name->starts_with("__profd_") && !name->starts_with("__profc_") &&
             !name->starts_with("__llvm_prf_unifcnt_") && !name->starts_with("__llvm_prf_nm_")))
            continue;
        auto address = symbol.getAddress();
        if (!address)
            return address.takeError();
        const auto bytes = object::ELFSymbolRef(symbol).getSize();
        auto& region = found->second;
        if (!bytes || *address < region.begin || bytes > region.bytes || *address - region.begin > region.bytes - bytes)
            return reject("Profile symbol falls outside its device section");
        region.ranges.emplace_back(*address - region.begin, bytes);
    }
    for (auto& [name, region] : regions) {
        std::sort(region.ranges.begin(), region.ranges.end());
        std::uint64_t cursor = 0;
        for (const auto& [offset, bytes] : region.ranges) {
            if (offset != cursor)
                return reject("Profile symbols omit or overlap device section bytes");
            cursor += bytes;
        }
        if (cursor != region.bytes)
            return reject("Profile symbols do not cover the complete device section");
    }
    return Error::success();
}

// Purpose: Admit only bounded regular analysis artifacts before asking LLVM to parse them.
// Inputs: A user-selected filename. Outputs: Success or a filesystem/extent error; no writes.
Error admitInput(StringRef path) {
    sys::fs::file_status status;
    if (auto error = sys::fs::status(path, status))
        return errorCodeToError(error);
    if (!sys::fs::is_regular_file(status) || status.getSize() == 0 || status.getSize() > MaxInputBytes)
        return reject("Analysis input must be a nonempty regular file of at most 64 MiB");
    return Error::success();
}

// Purpose: Validate complete per-function data/counter pairs after bounded symbol loading.
// Inputs: Owned actual ELF inventory. Outputs: Success only for the matching runtime ABI and no orphan counters.
Error validateDeviceRecords(const DeviceProfile& profile) {
    if (profile.data.empty() || profile.names.empty())
        return reject("Device image lacks complete profiling records and names");
    for (const auto& data : profile.data) {
        const auto suffix = StringRef(data).drop_front(StringRef("__profd_").size());
        if (suffix.empty())
            return reject("Profile data has an empty function identity");
        if (profile.symbols.at(data).bytes != sizeof(__llvm_profile_data))
            return reject("Device profiling data differs from the matching runtime ABI");
        for (StringRef prefix : {StringRef("__profc_"), StringRef("__llvm_prf_unifcnt_")}) {
            const auto found = profile.symbols.find((prefix + suffix).str());
            if (found == profile.symbols.end() || found->second.bytes % sizeof(std::uint64_t) != 0)
                return reject("Device record lacks its bounded counter pair");
        }
        if (profile.symbols.at((Twine("__profc_") + suffix).str()).bytes !=
            profile.symbols.at((Twine("__llvm_prf_unifcnt_") + suffix).str()).bytes)
            return reject("Uniform and ordinary counter extents differ from the supported ABI");
    }
    for (const auto& [name, symbol] : profile.symbols) {
        const StringRef text(name);
        const auto prefix = text.starts_with("__profc_")              ? StringRef("__profc_")
                            : text.starts_with("__llvm_prf_unifcnt_") ? StringRef("__llvm_prf_unifcnt_")
                                                                      : StringRef();
        if (!prefix.empty() && !profile.data.contains((Twine("__profd_") + text.drop_front(prefix.size())).str()))
            return reject("Device counters lack a matching data record");
    }
    return Error::success();
}

// Purpose: Collect actual profiling symbols from a device image rather than infer helpers from host AST names.
// Inputs: One admitted AMDGPU ELF. Outputs: Owned symbol names/extents; malformed or ambiguous records fail closed.
Expected<DeviceProfile> readDeviceProfile(StringRef path) {
    auto binary = object::ObjectFile::createObjectFile(path);
    if (!binary)
        return binary.takeError();
    auto& image = *binary->getBinary();
    if (!isa<object::ELFObjectFileBase>(image) || image.getArch() != Triple::amdgpu || image.getBytesInAddress() != 8 ||
        !image.isLittleEndian())
        return reject("Expected a little-endian 64-bit AMDGPU ELF");
    if (auto error = validateProfileRegions(image))
        return error;
    DeviceProfile profile;
    std::uint64_t copiedNameBytes = 0;
    for (const auto& symbol : image.symbols()) {
        auto flags = symbol.getFlags();
        auto name = symbol.getName();
        if (!flags)
            return flags.takeError();
        if (!name)
            return name.takeError();
        if (*flags & object::SymbolRef::SF_Undefined)
            continue;
        if (!name->starts_with("__profd_") && !name->starts_with("__profc_") &&
            !name->starts_with("__llvm_prf_unifcnt_") && !name->starts_with("__llvm_prf_nm_"))
            continue;
        if (!json::isUTF8(*name) || name->size() > 65536 || name->size() > MaxInputBytes - copiedNameBytes)
            return reject("Profiling symbol names exceed the per-name or aggregate byte budget");
        copiedNameBytes += name->size();
        auto section = symbol.getSection();
        if (!section)
            return section.takeError();
        if (*section == image.section_end())
            return reject("Profile symbol has no ordinary device section");
        auto sectionName = (**section).getName();
        if (!sectionName)
            return sectionName.takeError();
        const StringRef expectedSection = name->starts_with("__profd_")   ? INSTR_PROF_QUOTE(INSTR_PROF_DATA_COMMON)
                                          : name->starts_with("__profc_") ? INSTR_PROF_QUOTE(INSTR_PROF_CNTS_COMMON)
                                          : name->starts_with("__llvm_prf_unifcnt_")
                                              ? INSTR_PROF_QUOTE(INSTR_PROF_UCNTS_COMMON)
                                              : INSTR_PROF_QUOTE(INSTR_PROF_NAME_COMMON);
        if (*sectionName != expectedSection)
            return reject("Profile symbol is outside its expected ABI section");
        const auto bytes = object::ELFSymbolRef(symbol).getSize();
        if (bytes == 0 || bytes > MaxInputBytes || profile.symbols.size() >= 100000)
            return reject("Profiling symbol extent or inventory is invalid");
        if (!profile.symbols.emplace(name->str(), DeviceSymbol{name->str(), bytes}).second)
            return reject("Duplicate profiling symbol");
        if (name->starts_with("__profd_"))
            profile.data.insert(name->str());
        if (name->starts_with("__llvm_prf_nm_")) {
            if (!profile.names.empty())
                return reject("This analysis requires one non-RDC translation unit");
            profile.names = name->str();
        }
    }
    if (auto error = validateDeviceRecords(profile))
        return error;
    return profile;
}

using ProfileSignature = std::set<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>>;

// Purpose: Read exact function-name references, CFG hashes and counter extents through LLVM's ELF APIs.
// Inputs: An admitted device image. Outputs: Unique physical profile-record identities, never inferred source hits.
Expected<ProfileSignature> deviceSignature(StringRef path) {
    auto profile = readDeviceProfile(path);
    if (!profile)
        return profile.takeError();
    auto binary = object::ObjectFile::createObjectFile(path);
    if (!binary)
        return binary.takeError();
    for (const auto& section : binary->getBinary()->sections()) {
        auto name = section.getName();
        if (!name)
            return name.takeError();
        if (*name != INSTR_PROF_QUOTE(INSTR_PROF_DATA_COMMON))
            continue;
        auto contents = section.getContents();
        if (!contents)
            return contents.takeError();
        if (contents->size() != profile->data.size() * sizeof(__llvm_profile_data))
            return reject("Device profile records are not a dense supported ABI inventory");
        DataExtractor data(*contents, true);
        ProfileSignature signature;
        for (std::uint64_t offset = 0; offset < contents->size(); offset += sizeof(__llvm_profile_data)) {
            DataExtractor::Cursor identity(offset + offsetof(__llvm_profile_data, NameRef));
            const auto reference = data.getU64(identity);
            const auto hash = data.getU64(identity);
            DataExtractor::Cursor extent(offset + offsetof(__llvm_profile_data, NumCounters));
            const auto counters = data.getU32(extent);
            if (auto error = identity.takeError())
                return error;
            if (auto error = extent.takeError())
                return error;
            if (!counters || counters > MaxInputBytes / sizeof(std::uint64_t) ||
                !signature.emplace(reference, hash, counters).second)
                return reject("Device profile identity is repeated or has an unsupported counter extent");
        }
        return signature;
    }
    return reject("Device lacks its physical profile data records");
}

// Purpose: Read actual raw profile identities with LLVM's versioned native reader.
// Inputs: One bounded profiling artifact. Outputs: Exact name-reference/CFG-hash/counter-count inventory.
Expected<ProfileSignature> rawSignature(StringRef path) {
    auto reader = InstrProfReader::create(path, *vfs::getRealFileSystem());
    if (!reader)
        return reader.takeError();
    ProfileSignature signature;
    for (const auto& record : **reader) {
        if (signature.size() >= 100000 || record.Counts.empty() || record.Counts.size() > MaxInputBytes / 8 ||
            !signature.emplace(IndexedInstrProf::ComputeHash(record.Name), record.Hash, record.Counts.size()).second)
            return reject("Native profile has an invalid or repeated function inventory");
    }
    if (auto error = (*reader)->getError())
        return error;
    if (signature.empty())
        return reject("Native profile has no physical function records");
    return signature;
}

// Purpose: Associate each profile with its real translation unit before any cross-file coverage aggregation.
// Inputs: One native raw profile and bounded candidate device files. Outputs: Exactly one matching image or refusal.
Expected<json::Object> matchProfile() {
    if (ProfilePath.empty() || MatchDevices.empty() || MatchDevices.size() > 64 || !HostPath.empty() ||
        !DevicePath.empty() || !CoverageProfile.empty() || Complete || !OutputPath.empty())
        return reject("--profile requires one to 64 --match-device inputs and no other analysis mode");
    if (auto error = admitInput(ProfilePath))
        return error;
    auto actual = rawSignature(ProfilePath);
    if (!actual)
        return actual.takeError();
    std::vector<std::string> matched;
    for (const auto& path : MatchDevices) {
        if (auto error = admitInput(path))
            return error;
        auto expected = deviceSignature(path);
        if (!expected)
            return expected.takeError();
        if (*expected == *actual)
            matched.push_back(path);
    }
    if (matched.size() != 1)
        return reject("Native profile does not match exactly one device inventory");
    return json::Object{{"matched_device", matched.front()},
                        {"records", static_cast<int64_t>(actual->size())},
                        {"scope", "Native profile/ELF inventory association; no execution"}};
}

// Purpose: Obtain exact mapped line hits from LLVM's own coverage iterator for one correctly paired image/profile.
// Inputs: Device ELF and same-device indexed profile. Outputs: Native line hits; every CFG mismatch remains blocking.
Expected<json::Object> coverageLines() {
    if (DevicePath.empty() || CoverageProfile.empty() || !HostPath.empty() || !ProfilePath.empty() ||
        !MatchDevices.empty() || Complete || !OutputPath.empty())
        return reject("--coverage-profile requires one --device and no other analysis mode");
    if (auto error = admitInput(DevicePath))
        return error;
    if (auto error = admitInput(CoverageProfile))
        return error;
    auto inventory = deviceSignature(DevicePath);
    if (!inventory)
        return inventory.takeError();
    const std::array<StringRef, 1> objects{DevicePath.getValue()};
    auto mapping =
        coverage::CoverageMapping::load(objects, StringRef(CoverageProfile.getValue()), *vfs::getRealFileSystem());
    if (!mapping)
        return mapping.takeError();
    if ((*mapping)->getMismatchedCount())
        return reject("Device source mapping has mismatched native profile hashes");
    json::Array files;
    std::uint64_t visitedLines = 0;
    for (const auto filename : (*mapping)->getUniqueSourceFiles()) {
        auto data = (*mapping)->getCoverageForFile(filename);
        if (data.begin() == data.end())
            continue;
        json::Array lines;
        coverage::LineCoverageIterator line(data);
        for (const auto end = line.getEnd(); line != end; ++line) {
            if (++visitedLines > 1000000)
                return reject("Native line traversal exceeds the analysis budget");
            if (!line->isMapped())
                continue;
            lines.emplace_back(json::Array{line->getLine(), line->getExecutionCount() != 0});
        }
        files.emplace_back(json::Object{{"filename", filename.str()}, {"lines", std::move(lines)}});
    }
    return json::Object{{"scope", "LLVM per-image mapped line hits; no execution"}, {"files", std::move(files)}};
}

// Purpose: Resolve compiler-generated constant symbol names without accepting computed or mutable strings.
// Inputs: A borrowed IR pointer. Outputs: Exact owned symbol text, or a structural error.
Expected<std::string> constantName(Value* pointer) {
    const auto* global = dyn_cast<GlobalVariable>(pointer->stripPointerCasts());
    if (!global || !global->isConstant() || !global->hasInitializer())
        return reject("Registration name is not an immutable global");
    const auto* bytes = dyn_cast<ConstantDataArray>(global->getInitializer());
    if (!bytes || !bytes->isCString() || bytes->getNumElements() > 65536 || !json::isUTF8(bytes->getAsCString()))
        return reject("Registration name is not a bounded C string");
    return bytes->getAsCString().str();
}

// Purpose: Locate the actual static-HIP registration function and its per-TU profiling table.
// Inputs: A borrowed verified host module. Outputs: One supported unconditional registration owner, or failure.
Expected<HostRegistration> findRegistration(Module& module) {
    HostRegistration result;
    for (auto& function : module) {
        for (auto& block : function) {
            for (auto& instruction : block) {
                auto* call = dyn_cast<CallBase>(&instruction);
                if (!call || !call->getCalledFunction() ||
                    call->getCalledFunction()->getName() != "__llvm_profile_offload_register_shadow_variable")
                    continue;
                if (result.owner || !isa<CallInst>(call) || !call->getType()->isVoidTy() ||
                    call->getCallingConv() != CallingConv::C || call->getNumOperandBundles() ||
                    call->getFunctionType()->isVarArg() || call->arg_size() != 1 || function.size() != 1 ||
                    function.arg_size() != 1 || !function.getArg(0)->getType()->isPointerTy())
                    return reject("Expected one unconditional profiling registration owner");
                result.owner = &function;
                result.table = dyn_cast<GlobalVariable>(call->getArgOperand(0)->stripPointerCasts());
            }
        }
    }
    if (!result.owner || !result.table || !result.table->getName().starts_with("__llvm_profile_sections_"))
        return reject("Host lacks the compiler-generated profile table registration");
    return result;
}

// Purpose: Require the exact static profiling registration ABI before cloning any native call.
// Inputs: One profile-bound call and its single-context owner. Outputs: Success or a structural/constant error.
Error validateVariableCall(const CallBase& call, const Function& owner) {
    if (!isa<CallInst>(call) || !call.getType()->isVoidTy() || call.getNumOperandBundles() || call.arg_size() != 8 ||
        call.getFunctionType()->isVarArg() || call.getCallingConv() != CallingConv::C ||
        call.getArgOperand(0) != owner.getArg(0))
        return reject("Unsupported profiling variable registration ABI");
    for (const unsigned index : {4U, 6U, 7U}) {
        const auto* flag = dyn_cast<ConstantInt>(call.getArgOperand(index));
        if (!flag || !flag->getType()->isIntegerTy(32) || !flag->isZero())
            return reject("Unexpected profiling variable registration flags");
    }
    const auto* size = dyn_cast<ConstantInt>(call.getArgOperand(5));
    if (!size || !size->getType()->isIntegerTy(64) || size->getZExtValue() != sizeof(void*))
        return reject("Unexpected profiling shadow extent");
    return Error::success();
}

// Purpose: Admit only ordinary zero-initialized host pointer shadows used by the pinned runtime ABI.
// Inputs: A module-owned global. Outputs: Success or rejection before any new registration or output is emitted.
Error validateShadow(const GlobalVariable& shadow) {
    if (!shadow.getValueType()->isPointerTy() || shadow.getValueType()->getPointerAddressSpace() != 0 ||
        shadow.getAddressSpace() != 0 || shadow.isConstant() || shadow.isThreadLocal() || !shadow.hasInitializer() ||
        !isa<ConstantPointerNull>(shadow.getInitializer()))
        return reject("Unsupported profiling shadow storage ABI");
    return Error::success();
}

// Purpose: Decode registered symbol groups in the exact data/counter/uniform/names order consumed by the runtime.
// Inputs: A located owner and immutable symbol strings. Outputs: Complete groups; unsupported/duplicate forms reject.
Error readHostGroups(HostRegistration& result) {
    std::map<GlobalVariable*, std::string> names;
    for (auto& instruction : result.owner->front()) {
        auto* call = dyn_cast<CallBase>(&instruction);
        if (!call || !call->getCalledFunction() || call->getCalledFunction()->getName() != "__hipRegisterVar")
            continue;
        if (call->arg_size() != 8)
            return reject("Unexpected HIP variable-registration ABI");
        auto* shadow = dyn_cast<GlobalVariable>(call->getArgOperand(1)->stripPointerCasts());
        auto name = constantName(call->getArgOperand(2));
        auto repeated = constantName(call->getArgOperand(3));
        if (!name)
            return name.takeError();
        if (!repeated)
            return repeated.takeError();
        if (!shadow || *name != *repeated || !names.emplace(shadow, *name).second)
            return reject("Ambiguous HIP shadow registration");
        result.nativeCalls.emplace(shadow, call);
    }
    const auto tableCall = result.nativeCalls.find(result.table);
    if (tableCall == result.nativeCalls.end() || names.at(result.table) != result.table->getName())
        return reject("Profiling table lacks its exact native registration");
    if (auto error = validateVariableCall(*tableCall->second, *result.owner))
        return error;
    if (auto error = validateShadow(*result.table))
        return error;
    HostProfileGroup group;
    std::size_t count = 0;
    for (auto& instruction : result.owner->front()) {
        auto* call = dyn_cast<CallBase>(&instruction);
        if (!call || !call->getCalledFunction() ||
            call->getCalledFunction()->getName() != "__llvm_profile_offload_register_section_shadow_variable")
            continue;
        if (!isa<CallInst>(call) || !call->getType()->isVoidTy() || call->getCallingConv() != CallingConv::C ||
            call->getFunctionType()->isVarArg() || call->getNumOperandBundles() || call->arg_size() != 1)
            return reject("Unexpected profile section-registration ABI");
        auto* shadow = dyn_cast<GlobalVariable>(call->getArgOperand(0)->stripPointerCasts());
        const auto found = names.find(shadow);
        if (found == names.end())
            return reject("Section shadow lacks a native symbol registration");
        if (auto error = validateVariableCall(*result.nativeCalls.at(shadow), *result.owner))
            return error;
        if (auto error = validateShadow(*shadow))
            return error;
        if (!result.symbols.emplace(found->second, shadow).second &&
            !StringRef(found->second).starts_with("__llvm_prf_nm_"))
            return reject("Repeated non-name profile section registration");
        group.names[count] = found->second;
        group.notifications[count++] = call;
        if (count == group.names.size()) {
            if (result.groups.size() >= 100000)
                return reject("Host profile group inventory exceeds the analysis budget");
            result.groups.push_back(group);
            count = 0;
        }
    }
    if (count != 0 || result.groups.empty())
        return reject("Incomplete profile shadow group");
    return Error::success();
}

// Purpose: Identify full printed IR independently of its input-path comment. Inputs: Live module. Outputs: SHA-256.
std::string moduleDigest(const Module& module);

// Purpose: Compare the actual emitted and registered inventories without changing code or executing a device.
// Inputs: One same-compilation host module and ELF profile inventory. Outputs: A bounded JSON omission report.
Expected<json::Object> makeReport(Module& module, const DeviceProfile& device) {
    auto host = findRegistration(module);
    if (!host)
        return host.takeError();
    if (auto error = readHostGroups(*host))
        return error;
    const auto cuid = StringRef(device.names).drop_front(StringRef("__llvm_prf_nm_").size());
    if (cuid.empty() || host->table->getName() != (Twine("__llvm_profile_sections_") + cuid).str())
        return reject("Host and device profiling translation-unit identities differ");
    std::set<std::string> registered;
    std::size_t absent = 0;
    for (const auto& group : host->groups) {
        if (!StringRef(group.names[0]).starts_with("__profd_"))
            return reject("Host profile group does not begin with a data record");
        const auto suffix = StringRef(group.names[0]).drop_front(StringRef("__profd_").size());
        if (group.names[1] != (Twine("__profc_") + suffix).str() ||
            group.names[2] != (Twine("__llvm_prf_unifcnt_") + suffix).str() || group.names[3] != device.names)
            return reject("Host profile group does not match the actual ELF inventory");
        if (device.data.contains(group.names[0])) {
            if (!registered.insert(group.names[0]).second)
                return reject("Repeated device profile record registration");
        } else {
            if (device.symbols.contains(group.names[1]) || device.symbols.contains(group.names[2]))
                return reject("Host profile group has only partially present device records");
            ++absent;
        }
    }
    json::Array missing;
    for (const auto& data : device.data)
        if (!registered.contains(data))
            missing.emplace_back(data);
    return json::Object{{"scope", "LLVM IR/ELF registration inventory; no execution"},
                        {"module_ir_sha256", moduleDigest(module)},
                        {"device_data_records", static_cast<int64_t>(device.data.size())},
                        {"registered_data_records", static_cast<int64_t>(registered.size())},
                        {"host_profile_groups", static_cast<int64_t>(host->groups.size())},
                        {"absent_host_profile_groups", static_cast<int64_t>(absent)},
                        {"profile_record_bytes", static_cast<int64_t>(sizeof(__llvm_profile_data))},
                        {"table", host->table->getName().str()},
                        {"missing", std::move(missing)}};
}

class HashStream final : public raw_ostream {
    SHA256 hash;
    std::uint64_t position = 0;
    bool skipIdentifierComment;

    // Purpose: Hash LLVM's serialization without retaining its potentially large text.
    // Inputs: Borrowed printer bytes. Outputs: Updates the owned digest and byte position without external I/O.
    void write_impl(const char* data, std::size_t size) override {
        position += size;
        StringRef text(data, size);
        if (skipIdentifierComment) {
            const auto end = text.find('\n');
            if (end == StringRef::npos)
                return;
            text = text.drop_front(end + 1);
            skipIdentifierComment = false;
        }
        hash.update(text);
    }
    // Purpose: Expose the printer's current position. Inputs: None. Outputs: Number of bytes hashed.
    std::uint64_t current_pos() const override {
        return position;
    }

  public:
    // Purpose: Start an unbuffered digest. Inputs: Whether LLVM emits an artifact-name comment. Outputs: Owned stream.
    explicit HashStream(bool skipComment = false) : raw_ostream(true), skipIdentifierComment(skipComment) {}
    // Purpose: Finalize the printer digest. Inputs: No further writes are permitted. Outputs: Exact SHA-256 bytes.
    std::array<std::uint8_t, 32> finish() {
        flush();
        return hash.final();
    }
};

// Purpose: Fingerprint complete IR independently of bitcode encoding and its input-path comment.
// Inputs: A live module. Outputs: SHA-256 retaining all program, target, source and profiling metadata.
std::string moduleDigest(const Module& module) {
    const auto& identifier = module.getModuleIdentifier();
    HashStream stream(!identifier.empty() && identifier.find('\n') == std::string::npos);
    module.print(stream, nullptr);
    return toHex(stream.finish(), true);
}

// Purpose: Fingerprint one original IR value without copying or executing its code.
// Inputs: A live module-owned value. Outputs: SHA-256 of LLVM's own exact printed representation.
std::array<std::uint8_t, 32> valueDigest(const Value& value) {
    HashStream stream;
    value.print(stream);
    return stream.finish();
}

struct PreservedCode {
    std::map<const GlobalValue*, std::array<std::uint8_t, 32>> values;
    std::map<const Instruction*, std::array<std::uint8_t, 32>> registrationInstructions;
};

// Purpose: Freeze all program values and instructions outside explicitly validated profiling-metadata retirements.
// Inputs: Module, registration owner and exact metadata calls to retire. Outputs: Borrowed identities and digests.
PreservedCode preserveCode(Module& module, Function* registration, const std::set<Instruction*>& retiredMetadata) {
    PreservedCode result;
    for (auto& value : module.global_values())
        if (&value != registration && value.getName() != "llvm.compiler.used")
            result.values.emplace(&value, valueDigest(value));
    for (auto& instruction : registration->front())
        if (!retiredMetadata.contains(&instruction))
            result.registrationInstructions.emplace(&instruction, valueDigest(instruction));
    return result;
}

// Purpose: Refuse changes to program code, embedded device bytes or registrations outside the planned metadata repair.
// Inputs: The same module/owner and its pre-transform fingerprints. Outputs: Success only for preserved old values.
Error requirePreservedCode(Module& module, Function* registration, const PreservedCode& before) {
    std::set<const GlobalValue*> values;
    for (const auto& value : module.global_values())
        values.insert(&value);
    for (const auto& [value, digest] : before.values)
        if (!values.contains(value) || valueDigest(*value) != digest)
            return reject("Registration completion changed an original program value");
    std::set<const Instruction*> instructions;
    for (const auto& instruction : registration->front())
        instructions.insert(&instruction);
    for (const auto& [instruction, digest] : before.registrationInstructions)
        if (!instructions.contains(instruction) || valueDigest(*instruction) != digest)
            return reject("Registration completion changed an existing instruction");
    return Error::success();
}

// Purpose: Give each added symbol a deterministic collision-checked host name without embedding source paths.
// Inputs: Exact device symbol and validated translation-unit table identity. Outputs: Stable namespaced SHA-256 suffix.
std::string symbolDigest(StringRef symbol, StringRef translationUnit) {
    SHA256 hash;
    hash.update(translationUnit);
    hash.update(StringRef("\0", 1));
    hash.update(symbol);
    return toHex(ArrayRef<std::uint8_t>(hash.final()), true);
}

// Purpose: Emit one ordinary HIP registration and its runtime section-shadow notification.
// Inputs: Verified call templates, one model shadow, and an actual ELF symbol; builder points before the owned return.
// Outputs: A new owned shadow plus only the normal registration calls, or rejection before output publication.
Expected<GlobalVariable*> registerSection(Module& module, IRBuilder<>& builder, CallBase& variableCall,
                                          CallBase& sectionCall, GlobalVariable& model, StringRef symbol,
                                          StringRef translationUnit) {
    const auto suffix = symbolDigest(symbol, translationUnit);
    const auto shadowName = "__flitzzip_hip_profile_shadow_" + suffix;
    const auto stringName = "__flitzzip_hip_profile_name_" + suffix;
    if (module.getNamedValue(shadowName) || module.getNamedValue(stringName))
        return reject("Additional profiling symbol collides with an existing host value");
    auto* shadow = new GlobalVariable(module, model.getValueType(), false, model.getLinkage(),
                                      Constant::getNullValue(model.getValueType()), shadowName, nullptr,
                                      model.getThreadLocalMode(), model.getAddressSpace());
    shadow->copyAttributesFrom(&model);
    auto* name = builder.CreateGlobalString(symbol, stringName, 0, &module);
    SmallVector<Value*, 8> arguments;
    for (const Use& argument : variableCall.args())
        arguments.push_back(argument.get());
    arguments[1] = shadow;
    arguments[2] = name;
    arguments[3] = name;
    auto* native = builder.CreateCall(variableCall.getFunctionType(), variableCall.getCalledOperand(), arguments);
    native->setCallingConv(variableCall.getCallingConv());
    native->setAttributes(variableCall.getAttributes());
    auto* section = builder.CreateCall(sectionCall.getFunctionType(), sectionCall.getCalledOperand(), {shadow});
    section->setCallingConv(sectionCall.getCallingConv());
    section->setAttributes(sectionCall.getAttributes());
    return shadow;
}

struct MetadataRetirement {
    std::set<Instruction*> calls;
    std::size_t groups = 0;
};

struct RegistrationChanges {
    std::size_t added = 0;
    std::size_t retired = 0;
};

// Purpose: Retire only validated profiling calls referencing globals absent from the selected device image.
// Inputs: Verified complete groups and actual ELF records. Outputs: Borrowed calls to erase; all program calls remain.
MetadataRetirement planMetadataRetirement(const HostRegistration& host, const DeviceProfile& device) {
    MetadataRetirement result;
    std::set<GlobalVariable*> retained;
    for (const auto& group : host.groups)
        if (device.data.contains(group.names[0]))
            for (const auto* call : group.notifications)
                retained.insert(cast<GlobalVariable>(call->getArgOperand(0)->stripPointerCasts()));
    if (std::any_of(device.data.begin(), device.data.end(),
                    [&](const auto& name) { return !host.symbols.contains(name); }))
        retained.insert(host.symbols.at(device.names));
    for (const auto& group : host.groups) {
        if (device.data.contains(group.names[0]))
            continue;
        ++result.groups;
        for (auto* call : group.notifications) {
            result.calls.insert(call);
            auto* shadow = cast<GlobalVariable>(call->getArgOperand(0)->stripPointerCasts());
            if (!retained.contains(shadow))
                result.calls.insert(host.nativeCalls.at(shadow));
        }
    }
    return result;
}

// Purpose: Reconcile normal profiling registrations with actual ELF records without changing device or program code.
// Inputs: Verified same-TU inventories. Outputs: Added records and retired invalid profiling groups; no execution.
Expected<RegistrationChanges> completeRegistrations(Module& module, const DeviceProfile& device) {
    auto host = findRegistration(module);
    if (!host)
        return host.takeError();
    if (auto error = readHostGroups(*host))
        return error;
    auto* model = host->symbols.at(host->groups.front().names[0]);
    CallBase* variableCall = nullptr;
    CallBase* sectionCall = nullptr;
    for (auto& instruction : host->owner->front()) {
        auto* call = dyn_cast<CallBase>(&instruction);
        if (!call || !call->getCalledFunction())
            continue;
        if (call->getCalledFunction()->getName() == "__hipRegisterVar" &&
            call->getArgOperand(1)->stripPointerCasts() == model)
            variableCall = call;
        if (call->getCalledFunction()->getName() == "__llvm_profile_offload_register_section_shadow_variable")
            sectionCall = call;
    }
    auto* terminator = dyn_cast<ReturnInst>(host->owner->front().getTerminator());
    if (!variableCall || !sectionCall || !terminator || terminator->getReturnValue() ||
        variableCall->getNumOperandBundles() || sectionCall->getNumOperandBundles())
        return reject("Unsupported profiling registration control flow");
    const auto retirement = planMetadataRetirement(*host, device);
    const auto before = preserveCode(module, host->owner, retirement.calls);
    IRBuilder<> builder(terminator);
    SmallVector<GlobalValue*, 16> added;
    std::size_t completed = 0;
    for (const auto& data : device.data) {
        if (host->symbols.contains(data))
            continue;
        const auto suffix = StringRef(data).drop_front(StringRef("__profd_").size());
        const std::array<std::string, 3> symbols{data, (Twine("__profc_") + suffix).str(),
                                                 (Twine("__llvm_prf_unifcnt_") + suffix).str()};
        for (const auto& symbol : symbols) {
            auto shadow =
                registerSection(module, builder, *variableCall, *sectionCall, *model, symbol, host->table->getName());
            if (!shadow)
                return shadow.takeError();
            added.push_back(*shadow);
        }
        auto* names = host->symbols.at(device.names);
        auto* repeated = builder.CreateCall(sectionCall->getFunctionType(), sectionCall->getCalledOperand(), {names});
        repeated->setCallingConv(sectionCall->getCallingConv());
        repeated->setAttributes(sectionCall->getAttributes());
        ++completed;
    }
    if (!added.empty())
        appendToCompilerUsed(module, added);
    for (auto iterator = host->owner->front().begin(); iterator != host->owner->front().end();) {
        auto& instruction = *iterator++;
        if (retirement.calls.contains(&instruction))
            instruction.eraseFromParent();
    }
    if (auto error = requirePreservedCode(module, host->owner, before))
        return error;
    if (verifyModule(module, &errs()))
        return reject("Completed host module failed LLVM verification");
    return RegistrationChanges{completed, retirement.groups};
}

// Purpose: Write verified bitcode to a new artifact without replacing an existing file or claiming execution.
// Inputs: A verified module and explicit unused output path. Outputs: Complete bitcode or a retained write failure.
Error writeCompletedModule(const Module& module, StringRef path) {
    int descriptor = -1;
    if (auto error = sys::fs::openFileForWrite(path, descriptor, sys::fs::CD_CreateNew, sys::fs::OF_None))
        return errorCodeToError(error);
    raw_fd_ostream output(descriptor, true);
    WriteBitcodeToFile(module, output);
    output.close();
    if (output.has_error())
        return errorCodeToError(output.error());
    return Error::success();
}

// Purpose: Report the completed artifact that subsequent compiler invocations will actually read.
// Inputs: Newly written bitcode, its device inventory and the validated in-memory registration inventory.
// Outputs: Persisted IR digest and unchanged registration fields, or refusal; no device code executes.
Expected<json::Object> readCompletedReport(StringRef path, const DeviceProfile& device, json::Object expected) {
    if (auto error = admitInput(path))
        return error;
    LLVMContext context;
    SMDiagnostic diagnostic;
    auto persisted = parseIRFile(path, diagnostic, context);
    if (!persisted || verifyModule(*persisted, &errs()))
        return reject("Completed host bitcode failed independent readback");
    auto report = makeReport(*persisted, device);
    if (!report)
        return report.takeError();
    // Bind the output digest to the parsed artifact and require every registration
    // inventory field to persist unchanged across serialization and readback.
    expected["module_ir_sha256"] = report->getString("module_ir_sha256")->str();
    if (expected != *report)
        return reject("Completed host bitcode changed its registration inventory on readback");
    return report;
}

// Purpose: Run bounded static analysis through LLVM's own IR and object parsers.
// Inputs: Explicit host/device artifacts. Outputs: JSON on success; diagnostic and failure without output mutation.
Error analyze() {
    if (Identify) {
        if (!HostPath.empty() || !DevicePath.empty() || Complete || !OutputPath.empty() || !ProfilePath.empty() ||
            !MatchDevices.empty() || !CoverageProfile.empty())
            return reject("--identify accepts no input or transform options");
        outs() << formatv("{0:2}\n", json::Value(json::Object{
                                         {"schema_version", 1},
                                         {"llvm_revision", FLITZZIP_COVERAGE_LLVM_REVISION},
                                         {"source_sha256", FLITZZIP_COVERAGE_TOOL_SOURCE_SHA256},
                                         {"cmake_sha256", FLITZZIP_COVERAGE_TOOL_CMAKE_SHA256},
                                         {"toolchain_sha256", FLITZZIP_COVERAGE_TOOLCHAIN_SHA256},
                                         {"toolchain_parser_sha256", FLITZZIP_COVERAGE_TOOLCHAIN_PARSER_SHA256},
                                         {"profile_record_bytes", static_cast<int64_t>(sizeof(__llvm_profile_data))},
                                         {"host", "windows-x64"},
                                         {"device", "amdgpu-elf64le"}}));
        return Error::success();
    }
    if (!ProfilePath.empty() || !MatchDevices.empty() || !CoverageProfile.empty()) {
        auto report = CoverageProfile.empty() ? matchProfile() : coverageLines();
        if (!report)
            return report.takeError();
        outs() << formatv("{0:2}\n", json::Value(std::move(*report)));
        return Error::success();
    }
    if (HostPath.empty() || DevicePath.empty())
        return reject("Both --host and --device inputs are required");
    if (Complete != !OutputPath.empty())
        return reject("--complete requires a new -o artifact; read-only mode accepts no output path");
    if (auto error = admitInput(HostPath))
        return error;
    if (auto error = admitInput(DevicePath))
        return error;
    LLVMContext context;
    SMDiagnostic diagnostic;
    auto module = parseIRFile(HostPath, diagnostic, context);
    if (!module)
        return reject("Host IR parse failed: " + diagnostic.getMessage());
    const Triple hostTriple(module->getTargetTriple());
    if (hostTriple.getArch() != Triple::x86_64 || !hostTriple.isOSWindows() ||
        module->getDataLayout().getPointerSize() != 8 || !module->getDataLayout().isLittleEndian() ||
        verifyModule(*module, &errs()))
        return reject("Host IR is invalid or not little-endian Windows x64");
    auto device = readDeviceProfile(DevicePath);
    if (!device)
        return device.takeError();
    auto report = makeReport(*module, *device);
    if (!report)
        return report.takeError();
    if (Complete) {
        auto completed = completeRegistrations(*module, *device);
        if (!completed)
            return completed.takeError();
        auto after = makeReport(*module, *device);
        if (!after)
            return after.takeError();
        if (!after->getArray("missing")->empty() || after->getInteger("absent_host_profile_groups") != 0)
            return reject("Completed module still omits device profile records");
        if (auto error = writeCompletedModule(*module, OutputPath))
            return error;
        after = readCompletedReport(OutputPath, *device, std::move(*after));
        if (!after)
            return after.takeError();
        outs() << formatv(
            "{0:2}\n",
            json::Value(json::Object{{"scope", "Registration completion only; no execution or counter qualification"},
                                     {"completed_records", static_cast<int64_t>(completed->added)},
                                     {"retired_profile_groups", static_cast<int64_t>(completed->retired)},
                                     {"original_program_values_preserved", true},
                                     {"before", std::move(*report)},
                                     {"after", std::move(*after)}}));
    } else {
        outs() << formatv("{0:2}\n", json::Value(std::move(*report)));
    }
    return Error::success();
}
}  // namespace

// Purpose: Expose static registration analysis/completion and contain all LLVM error ownership.
// Inputs: Explicit command-line paths. Outputs: Zero only after structural analysis, never a device-coverage pass.
int main(int argc, char** argv) {
    llvm::InitLLVM initialization(argc, argv);
    llvm::cl::ParseCommandLineOptions(argc, argv);
    if (auto error = analyze()) {
        llvm::errs() << "profile registration analysis: " << llvm::toString(std::move(error)) << '\n';
        return 1;
    }
    return 0;
}
