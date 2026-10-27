#include "ryty_gpu/ryty_camera.h"
#include "ryty_gpu/ryty_camera_validation.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <cstdio>
#include <sstream>
#include <vector>

namespace Ryty::Camera {
namespace {
#include "ryty_camera_car_catalog.h"
#include "camera_math/orbit_ground_apply.inc"
#include "camera_math/roof_apply.inc"
using Clock = std::chrono::steady_clock;
std::uint64_t Milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}
std::atomic<std::uint32_t> current_car{};
std::atomic<std::uint64_t> car_updated{};
std::atomic<std::uint64_t> camera_frames{};
std::atomic<std::uint32_t> observed_car_id{};
std::atomic<std::uint64_t> native_samples{};
std::atomic<View> current_view{View::Count};
std::atomic<std::uint64_t> view_updated{};
// Set only on the game's camera thread, from the native prepare-view context.
thread_local void* active_control{};
thread_local void* active_camera{};
thread_local std::uint64_t active_updated{};
std::mutex identity_mutex;
std::uint32_t candidate_car{};
unsigned candidate_count{};
std::mutex input_mutex;
struct Stick { float x{}, y{}; bool focused{}; std::uint64_t updated{}; } stick;
std::mutex runtime_mutex;
std::string status = "Camera integration is starting.";
std::uintptr_t image{};
std::atomic<bool> runtime_ready{};
std::mutex request_mutex;
std::atomic<bool> request_pending{};
struct ViewRequest { int view = -1; std::uint32_t car{}; std::uint64_t timestamp{}; } request;
struct Hook {
    std::uintptr_t slot, original;
    std::atomic<float> native_fov{}, minimum{-5};
    std::mutex mutex;
    alignas(16) std::array<unsigned char, 256> data{};
    std::uint32_t car{};
    std::uint64_t last_frame{};
};
std::array<Hook, 4> hooks{{{0x2878ab8, 0x86b030}, {0x2878918, 0x8688e0},
                         {0x28789e8, 0x869620}, {0x2878848, 0x8683f0}}};
// These final-view methods use five integer/pointer arguments (SysV RDI,RSI,RDX,RCX,R8).
// The original common method at RVA 0x8a6320 reads the fifth argument before building the view.
using ViewFunction = std::uintptr_t(__attribute__((sysv_abi)) *)(void*, void*, void*, float*, std::uintptr_t);
bool Readable(const void* address, std::size_t bytes) {
    MEMORY_BASIC_INFORMATION info{};
    if (!address || !VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    return bytes <= info.RegionSize && at >= reinterpret_cast<std::uintptr_t>(info.BaseAddress) &&
        at - reinterpret_cast<std::uintptr_t>(info.BaseAddress) <= info.RegionSize - bytes;
}
unsigned char* Pointer(const void* p) {
    return Readable(p, 8) ? *reinterpret_cast<unsigned char* const*>(p) : nullptr;
}
void PublishIdentity(std::uint32_t id) {
    const bool known = std::any_of(std::begin(CarCatalog), std::end(CarCatalog),
                                  [id](const auto& entry) { return entry.id == id; });
    std::lock_guard lock(identity_mutex);
    if (!known) { current_car.store(0); candidate_car = 0; candidate_count = 0; return; }
    if (id != candidate_car) { candidate_car = id; candidate_count = 1; current_car.store(0); }
    else if (candidate_count < 3) ++candidate_count;
    if (candidate_count >= 3) { current_car.store(id); car_updated.store(Milliseconds()); }
}
std::uintptr_t NativeMethod(void* object, unsigned offset) {
    auto* table = Pointer(object);
    if (!Readable(table, offset + 8)) return 0;
    const auto fn = *reinterpret_cast<const std::uintptr_t*>(table + offset);
    return fn >= image + 0x10000 && fn < image + 0x1d00000 ? fn : 0;
}
void ObserveContext(void* camera, float* view, void* context) {
    // BaseCamera::prepare (8a5560) uses context+8 as IRaceStatus and
    // view+bc as the viewed car index. The context is a stack structure:
    // 85d75d/85d765 writes Impl+48 to context+28. Impl+98 is the owner
    // back-pointer (85ca30). Neither address comes from a script global.
    auto* ctx = static_cast<unsigned char*>(context);
    if (!Readable(ctx, 0x30) || !Readable(view, 0xe0)) return;
    const auto member = *reinterpret_cast<const std::uintptr_t*>(ctx + 0x28);
    auto* impl = member >= 0x48 ? reinterpret_cast<unsigned char*>(member - 0x48) : nullptr;
    auto* control = Readable(impl, 0xa0) ? *reinterpret_cast<unsigned char**>(impl + 0x98) : nullptr;
    if (!Readable(control, 16) || *reinterpret_cast<std::uintptr_t*>(control) != image + 0x2877ae8 ||
        *reinterpret_cast<unsigned char**>(control + 8) != impl) return;
    if (*reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(view) + 0xc4) != 0) return;
    active_control = control; active_camera = camera; active_updated = Milliseconds();
    auto* race = *reinterpret_cast<unsigned char**>(ctx + 8);
    const int index = *reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(view) + 0xbc);
    auto* race_table = Pointer(race);
    if (!Readable(race_table, 0x60)) { PublishIdentity(0); return; }
    const auto get_status = *reinterpret_cast<std::uintptr_t*>(race_table + 0x58);
    const auto get_count = *reinterpret_cast<std::uintptr_t*>(race_table + 0x28);
    using Count = int(__attribute__((sysv_abi)) *)(void*);
    using Status = void*(__attribute__((sysv_abi)) *)(void*, int);
    using Code = std::uint32_t(__attribute__((sysv_abi)) *)(void*);
    if (get_status < image + 0x10000 || get_status >= image + 0x1d00000 ||
        get_count < image + 0x10000 || get_count >= image + 0x1d00000 ||
        index < 0 || index >= 128) { PublishIdentity(0); return; }
    const auto count = reinterpret_cast<Count>(get_count)(race);
    if (count <= index || count > 128) { PublishIdentity(0); return; }
    auto* car = reinterpret_cast<Status>(get_status)(race, index);
    const auto get_code = NativeMethod(car, 0xd8);
    if (!get_code) { PublishIdentity(0); return; }
    const auto id = reinterpret_cast<Code>(get_code)(car);
    ++native_samples; observed_car_id.store(id);
    PublishIdentity(id);
}
using PrepareFunction = std::uintptr_t(__attribute__((sysv_abi)) *)(void*, float*, void*, void*, float);
std::uintptr_t __attribute__((sysv_abi)) PrepareView(void* camera, float* view, void* context, void* input, float dt) {
    const auto result = reinterpret_cast<PrepareFunction>(image + 0x8a5560)(camera, view, context, input, dt);
    if (result) ObserveContext(camera, view, context);
    return result;
}
bool TerrainAvailable() {
    auto global = reinterpret_cast<void***>(image + 0x2bf5e48);
    if (!Readable(global, 8) || !Readable(*global, 8) || !Readable(**global, 0x25b8)) return false;
    auto* root = reinterpret_cast<unsigned char*>(**global);
    if (*reinterpret_cast<std::uintptr_t*>(root) != image + 0x2854348) return false;
    auto* course = *reinterpret_cast<unsigned char**>(root + 0x25b0);
    return Readable(course, 0x670) && *reinterpret_cast<std::uintptr_t*>(course + 0x660) == image + 0x2896278;
}
void ProcessViewRequest() {
    if (!request_pending.load(std::memory_order_acquire)) return;
    ViewRequest pending;
    { std::lock_guard lock(request_mutex); pending = request; request.view = -1; request_pending.store(false, std::memory_order_relaxed); }
    if (pending.view < 0 || pending.view >= 4 || !runtime_ready ||
        (pending.car && CurrentCar() != pending.car) || Milliseconds() - pending.timestamp > 2000) return;
    // Use the owner observed in this camera thread, not the general script reference.
    auto ptr = Pointer;
    auto* control = static_cast<unsigned char*>(active_control);
    if (Milliseconds() - active_updated > 250) return;
    if (!Readable(control, 16) || *reinterpret_cast<std::uintptr_t*>(control) != image + 0x2877ae8) return;
    auto* implementation = ptr(control + 8);
    auto* data = Readable(implementation, 0xe8) ? ptr(implementation + 0xe0) : nullptr;
    if (!Readable(data, 16) || reinterpret_cast<std::uintptr_t>(ptr(data)) != image + 0x28785f8) return;
    auto* data_impl = ptr(data + 8);
    auto* switcher = Readable(data_impl, 0x48) ? ptr(data_impl + 0x40) : nullptr;
    // +40 points to plain per-window state records (stride 0x88), not a polymorphic object.
    if (!Readable(switcher, 0x88)) return;
    // Scenario names and view types are defined in camera_util.ad's onboard_list.
    constexpr const char* scenarios[] = {"incar", "chase", "driver", "bonnet"};
    using NameCode = std::uint64_t(__attribute__((sysv_abi)) *)(const char*);
    using Select = void(__attribute__((sysv_abi)) *)(void*, int, std::uint64_t, bool);
    const auto code = reinterpret_cast<NameCode>(image + 0x866320)(scenarios[pending.view]);
    // Validate a matching scenario before calling the native selector; no guessed indices.
    auto* list = ptr(switcher + 0x30);
    auto* begin = Readable(list, 0x18) ? ptr(list + 8) : nullptr;
    auto* end = Readable(list, 0x18) ? ptr(list + 0x10) : nullptr;
    const auto first = reinterpret_cast<std::uintptr_t>(begin), last = reinterpret_cast<std::uintptr_t>(end);
    if (!first || last < first || last - first > 0x10000 || (last - first) % 32 || !Readable(begin, last - first)) return;
    bool found = false;
    for (auto at = first; at < last; at += 32) if (*reinterpret_cast<std::uint64_t*>(at) == code) found = true;
    if (found) reinterpret_cast<Select>(image + 0x8657b0)(switcher, 0, code, true);
}
void ResetData(Hook& hook) {
    hook.data.fill(0);
    auto* b = hook.data.data(); auto* p = reinterpret_cast<float*>(b);
    p[11] = p[13] = p[17] = p[19] = 1; p[49] = 0.45f;
    *reinterpret_cast<std::uintptr_t*>(b + 200) = image + 0x2bf5e48;
    *reinterpret_cast<std::uintptr_t*>(b + 208) = image + 0x2854348;
    *reinterpret_cast<std::uintptr_t*>(b + 216) = image + 0x2896278;
    *reinterpret_cast<std::uintptr_t*>(b + 224) = image + 0xa91990;
    *reinterpret_cast<QueryClock*>(b + 128) = reinterpret_cast<QueryClock>(&QueryPerformanceCounter);
    QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(b + 136));
    hook.minimum.store(-5, std::memory_order_relaxed);
}
void Offset(float* view, float height, float forward) {
    const float x = view[4], y = view[5], z = view[6], w = view[7], backward = -forward;
    view[0] += 2 * (x*z + w*y) * backward;
    view[1] += 2 * (y*z - w*x) * backward + height;
    view[2] += (1 - 2 * (x*x + y*y)) * backward;
}
template<View camera>
std::uintptr_t __attribute__((sysv_abi)) CameraView(void* a, void* b, void* c, float* view, std::uintptr_t flags) {
    constexpr unsigned index = static_cast<unsigned>(camera);
    auto& hook = hooks[index];
    const auto car = CurrentCar();
    const auto settings = Presets().Effective(car, camera);
    const bool valid = Readable(view, 0xe0);
    std::array<float, 4> anchor{};
    std::array<unsigned char, 8> look_back{};
    if (valid) std::memcpy(anchor.data(), view, 16);
    if constexpr (camera == View::Chase) {
        if (valid && settings.orbit) {
            // Suppress only this chase invocation's native look-back, then restore its flags.
            std::memcpy(look_back.data(), reinterpret_cast<unsigned char*>(view) + 0xd8, 8);
            std::memset(reinterpret_cast<unsigned char*>(view) + 0xd8, 0, 8);
        }
    }
    const auto result = reinterpret_cast<ViewFunction>(image + hook.original)(a, b, c, view, flags);
    if constexpr (camera == View::Chase) {
        if (valid && settings.orbit) std::memcpy(reinterpret_cast<unsigned char*>(view) + 0xd8, look_back.data(), 8);
    }
    if (!valid) return result;
    if (a == active_camera && Milliseconds() - active_updated <= 250) {
        current_view.store(camera, std::memory_order_relaxed);
        view_updated.store(Milliseconds(), std::memory_order_release);
    }
    ++camera_frames;
    // The native onboard callback establishes that there is an active camera. Identity is
    // needed only for per-car presets, not for global edits or camera selection. Delay the
    // selector until this callback has finished and released the camera state lock.
    struct DispatchRequest { void* object; ~DispatchRequest() { if (object == active_camera) ProcessViewRequest(); } } dispatch{a};
    if (std::isfinite(view[22]) && view[22] > 0 && view[22] < 180) hook.native_fov.store(view[22]);
    if (settings.fov != 0) view[22] = settings.fov;
    if constexpr (camera == View::Cockpit) {
        Offset(view, settings.height, settings.distance);
    } else {
        std::lock_guard lock(hook.mutex);
        if constexpr (camera == View::Bumper || camera == View::Roof) {
            // Default views pass through without terrain work while the UI is closed.
            // Opening the UI still refreshes the live minimum-height slider limit.
            if (settings.height == 0 && settings.distance == 0 && !OverlayOpen()) return result;
        }
        if constexpr (camera == View::Chase) {
            // With no position/orbit override the original chase view must pass through
            // unchanged. Avoid a terrain query or correction of the native camera.
            if (!settings.orbit && settings.height == 0 && settings.distance == 0) {
                if (hook.data[100]) ResetData(hook);
                return result;
            }
        }
        const auto now = Milliseconds();
        if (hook.car != car || now - hook.last_frame > 250) { ResetData(hook); hook.car = car; }
        hook.last_frame = now;
        auto* data = hook.data.data(); auto* p = reinterpret_cast<float*>(data);
        data[0] = 0; // FOV is applied separately; keep native FOV observation independent.
        data[12] = settings.height != 0 || settings.distance != 0;
        p[4] = settings.height; p[5] = camera == View::Bumper ? 0 : settings.distance;
        if constexpr (camera == View::Chase) {
            float native[8]; std::memcpy(native, view, sizeof(native));
            data[192] = 1;
            if (!TerrainAvailable()) return result;
            Offset(view, settings.height, settings.distance);
            data[40] = data[65] = data[100] = settings.orbit;
            Stick input;
            { std::lock_guard input_lock(input_mutex); input = stick; }
            p[26] = input.x; p[27] = input.y; p[28] = OrbitDegreesPerSecond(settings.sensitivity);
            p[29] = settings.smoothing_ms; p[30] = settings.return_seconds;
            data[124] = settings.auto_return; data[125] = settings.invert_x; data[126] = settings.invert_y;
            data[127] = input.focused && now - input.updated < 200;
            QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(data + 176));
            // Disabling orbit also resets previous angles, so enabling it again starts behind the car.
            if (!settings.orbit) { p[38] = p[39] = p[40] = p[41] = p[42] = 0; *reinterpret_cast<i64*>(data + 144) = 0; }
            orbit_apply(view, data, anchor.data(), native);
        } else {
            if (!TerrainAvailable()) return result;
            roof_apply(view, data);
            if (data[193] >= 2 && std::isfinite(p[8])) hook.minimum.store(p[8]);
        }
    }
    return result;
}
std::filesystem::path GamePath() {
    std::wstring file(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, file.data(), static_cast<DWORD>(file.size()));
    if (!length || length >= file.size()) throw std::runtime_error("Cannot locate the game executable.");
    file.resize(length); return std::filesystem::path(file);
}
bool InstallCameraHooks(Validation::Inspect inspect = Validation::InspectImage, const Validation::Report& report = {}) {
    constexpr ViewFunction functions[] = {&CameraView<View::Cockpit>, &CameraView<View::Chase>, &CameraView<View::Bumper>, &CameraView<View::Roof>};
    struct Patch { std::uintptr_t slot, original; void* replacement; };
    std::array<Patch, 8> patches{};
    std::array<Validation::HookSite, 8> sites{};
    for (unsigned i = 0; i < hooks.size(); ++i) {
        patches[i] = {hooks[i].slot, hooks[i].original, reinterpret_cast<void*>(functions[i])};
        patches[i + 4] = {hooks[i].slot - 0x50, 0x8a5560, reinterpret_cast<void*>(&PrepareView)};
    }
    for (unsigned i = 0; i < patches.size(); ++i) sites[i] = {patches[i].slot, patches[i].original};
    const auto validation = Validation::Validate(image, sites, inspect, report);
    if (!validation.empty()) {
        status = validation;
        if (report) report("VALIDATION FAILED: " + validation + " No camera hooks were installed.");
        return false;
    }
    if (report) report("VALIDATION SUCCESS: all 8/8 hook slots passed; installing hooks.");
    for (auto& hook : hooks) ResetData(hook);
    unsigned installed = 0;
    std::string failure;
    for (const auto& hook : patches) {
        auto* slot = reinterpret_cast<void**>(image + hook.slot); DWORD protection{};
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) {
            failure = Validation::At("Cannot make camera hook writable", hook.slot) +
                " (Windows error " + std::to_string(GetLastError()) + ")."; break;
        }
        const auto expected = reinterpret_cast<void*>(image + hook.original);
        const auto observed = InterlockedCompareExchangePointer(slot, hook.replacement, expected);
        DWORD ignored{};
        const bool restored = VirtualProtect(slot, sizeof(void*), protection, &ignored) != 0;
        const DWORD restore_error = restored ? 0 : GetLastError();
        if (observed != expected) {
            failure = Validation::At("Camera hook changed during installation", hook.slot); break;
        }
        ++installed;
        if (!restored) {
            failure = Validation::At("Cannot restore camera hook protection", hook.slot) +
                " (Windows error " + std::to_string(restore_error) + ")."; break;
        }
        if (report) report(Validation::At(("HOOK INSTALLED " + std::to_string(installed) + "/8").c_str(), hook.slot));
    }
    if (failure.empty()) {
        if (report) report("INSTALLATION SUCCESS: all 8/8 camera hooks installed.");
        return true;
    }
    unsigned incomplete = 0;
    while (installed) {
        const auto& hook = patches[--installed];
        auto* slot = reinterpret_cast<void**>(image + hook.slot); DWORD protection{};
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection)) { ++incomplete; continue; }
        const auto original = reinterpret_cast<void*>(image + hook.original);
        const auto observed = InterlockedCompareExchangePointer(slot, original, hook.replacement);
        DWORD ignored{};
        if (!VirtualProtect(slot, sizeof(void*), protection, &ignored) ||
            (observed != hook.replacement && observed != original)) ++incomplete;
    }
    status = failure + (incomplete ? " Rollback incomplete for " + std::to_string(incomplete) + " slot(s)." : " Installed camera hooks rolled back.");
    if (report) report("INSTALLATION FAILED: " + status);
    return false;
}
}
Store& Presets() { static Store store(GamePath().parent_path() / L"user" / L"camera_presets.json"); return store; }
void RequestView(View view) {
    std::lock_guard lock(request_mutex); request = {static_cast<int>(view), CurrentCar(), Milliseconds()};
    request_pending.store(true, std::memory_order_release);
}
std::uint32_t CurrentCar() { return Milliseconds() - car_updated.load() <= 1500 ? current_car.load() : 0; }
View CurrentView() {
    return Milliseconds() - view_updated.load(std::memory_order_acquire) <= 1500
        ? current_view.load(std::memory_order_relaxed) : View::Count;
}
std::string CarName(std::uint32_t id) {
    for (const auto& entry : CarCatalog) if (entry.id == id) return std::string(entry.name) + " (ID " + std::to_string(id) + ")";
    return "Unknown vehicle";
}
void PublishStick(float x, float y, bool focused) {
    std::lock_guard lock(input_mutex); stick = {x, y, focused, Milliseconds()};
}
float NativeFov(View view) { return hooks[static_cast<unsigned>(view)].native_fov.load(); }
float MinimumHeight(View view) { return hooks[static_cast<unsigned>(view)].minimum.load(); }
std::string RuntimeStatus() { std::lock_guard lock(runtime_mutex); return status; }
std::string RuntimeDiagnostics() {
    return "Camera callbacks: " + std::to_string(camera_frames.load()) +
        " | Native vehicle samples: " + std::to_string(native_samples.load()) +
        " | Raw ID: " + std::to_string(observed_car_id.load());
}
void InitializeRuntime() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::lock_guard lock(runtime_mutex);
        std::ofstream log;
        try {
            const auto directory = GamePath().parent_path() / L"logs";
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            log.open(directory / L"camera-validation.log", std::ios::app);
        } catch (const std::exception&) {}
        const Validation::Report report = [&log](const std::string& message) {
            if (!log.is_open()) return;
            SYSTEMTIME now{}; GetLocalTime(&now);
            char timestamp[64]{};
            std::snprintf(timestamp, sizeof(timestamp), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                unsigned(now.wYear), unsigned(now.wMonth), unsigned(now.wDay), unsigned(now.wHour),
                unsigned(now.wMinute), unsigned(now.wSecond), unsigned(now.wMilliseconds));
            log << '[' << timestamp << "] " << message << '\n';
            log.flush();
        };
        report("SESSION START: Camera executable validation; process " + std::to_string(GetCurrentProcessId()));
        try {
            std::string error;
            const bool loaded = Presets().Load(error);
            image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            if (!InstallCameraHooks(Validation::InspectImage, report)) return;
            status = loaded ? "Camera controls ready." : "Camera controls ready. Presets could not be loaded: " + error;
            runtime_ready.store(true);
            report("RUNTIME READY: " + status);
            if (!log) status += " Validation log could not be written to logs/camera-validation.log.";
        } catch (const std::exception& e) { status = e.what(); report("INITIALIZATION FAILED: " + status); }
    });
}
} // namespace Ryty::Camera
