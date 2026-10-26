/*
 * Virtual Toy Pad console overlay - renderer injected into SceShellUI.
 *
 * Derived from the PS5 HUD and DualSense Controller Overlay renderer, itself derived from
 * Common FPS for PS5:
 *   Copyright (C) 2026 porhe911 (Common FPS for PS5)
 *   Modifications Copyright (C) 2026 khalifa007 (SimpleFPS: FW 11.xx-13.xx hook backend)
 *   Modifications Copyright (C) 2026 erickdavestech (ImageBox sprites, FW 13.60 validation)
 *   Modifications Copyright (C) 2026 SpiderNic96 (Virtual Toy Pad: retained-mode engine,
 *     texture upload, TCP protocol; the HUD/pad/perf drawing code is removed)
 *
 * The Mono/PUI runtime setup, the Application.Update hook and the legacy main-thread guard
 * below are kept from that renderer unchanged except for log/diagnostic paths.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "common_fps/shellui_hook_protocol.hpp"
#include "common_fps/shellui_stage.hpp"
#include "../common/ovl_proto.h"

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace common_fps::ps5::shellui {
void record_stage(const char* stage) noexcept;
}

namespace common_fps::ps5::shellui {

namespace {
static_assert(SOL_SOCKET == 0xffff);
static_assert(SO_RCVTIMEO == 0x1006);
static_assert(sizeof(timeval) == 16);

struct MonoDomain;
struct MonoThread;
struct MonoAssembly;
struct MonoImage;
struct MonoClass;
struct MonoMethod;
struct MonoProperty;
struct MonoObject;
struct MonoString;

using mono_get_root_domain_t = MonoDomain* (*)();
using mono_domain_get_t = MonoDomain* (*)();
using mono_thread_attach_t = MonoThread* (*)(MonoDomain*);
using mono_domain_assembly_open_t = MonoAssembly* (*)(MonoDomain*, const char*);
using mono_assembly_get_image_t = MonoImage* (*)(MonoAssembly*);
using mono_class_from_name_t = MonoClass* (*)(MonoImage*, const char*, const char*);
using mono_class_get_method_from_name_t = MonoMethod* (*)(MonoClass*, const char*, int);
using mono_class_get_property_from_name_t = MonoProperty* (*)(MonoClass*, const char*);
using mono_property_get_get_method_t = MonoMethod* (*)(MonoProperty*);
using mono_property_get_set_method_t = MonoMethod* (*)(MonoProperty*);
using mono_runtime_invoke_t = MonoObject* (*)(MonoMethod*, void*, void**, MonoObject**);
using mono_string_new_t = MonoString* (*)(MonoDomain*, const char*);
using mono_object_new_t = MonoObject* (*)(MonoDomain*, MonoClass*);
using mono_runtime_object_init_t = void (*)(MonoObject*);
using mono_object_unbox_t = void* (*)(MonoObject*);
using mono_compile_method_t = void* (*)(MonoMethod*);
using mono_gchandle_new_t = std::uint32_t (*)(MonoObject*, int);
using mono_gchandle_get_target_t = MonoObject* (*)(std::uint32_t);
using mono_gchandle_free_t = void (*)(std::uint32_t);

extern "C" {
MonoDomain* mono_get_root_domain();
MonoDomain* mono_domain_get();
MonoThread* mono_thread_attach(MonoDomain*);
MonoAssembly* mono_domain_assembly_open(MonoDomain*, const char*);
MonoImage* mono_assembly_get_image(MonoAssembly*);
MonoClass* mono_class_from_name(MonoImage*, const char*, const char*);
MonoMethod* mono_class_get_method_from_name(MonoClass*, const char*, int);
MonoProperty* mono_class_get_property_from_name(MonoClass*, const char*);
MonoMethod* mono_property_get_get_method(MonoProperty*);
MonoMethod* mono_property_get_set_method(MonoProperty*);
MonoObject* mono_runtime_invoke(MonoMethod*, void*, void**, MonoObject**);
MonoString* mono_string_new(MonoDomain*, const char*);
MonoObject* mono_object_new(MonoDomain*, MonoClass*);
void mono_runtime_object_init(MonoObject*);
void* mono_object_unbox(MonoObject*);
void* mono_compile_method(MonoMethod*);
std::uint32_t mono_gchandle_new(MonoObject*, int);
MonoObject* mono_gchandle_get_target(std::uint32_t);
void mono_gchandle_free(std::uint32_t);
}

mono_get_root_domain_t mono_get_root_domain_ = mono_get_root_domain;
mono_domain_get_t mono_domain_get_ = mono_domain_get;
mono_thread_attach_t mono_thread_attach_ = mono_thread_attach;
mono_domain_assembly_open_t mono_domain_assembly_open_ =
    mono_domain_assembly_open;
mono_assembly_get_image_t mono_assembly_get_image_ = mono_assembly_get_image;
mono_class_from_name_t mono_class_from_name_ = mono_class_from_name;
mono_class_get_method_from_name_t mono_class_get_method_from_name_ =
    mono_class_get_method_from_name;
mono_class_get_property_from_name_t mono_class_get_property_from_name_ =
    mono_class_get_property_from_name;
mono_property_get_get_method_t mono_property_get_get_method_ =
    mono_property_get_get_method;
mono_property_get_set_method_t mono_property_get_set_method_ =
    mono_property_get_set_method;
mono_runtime_invoke_t mono_runtime_invoke_ = mono_runtime_invoke;
mono_string_new_t mono_string_new_ = mono_string_new;
mono_object_new_t mono_object_new_ = mono_object_new;
mono_runtime_object_init_t mono_runtime_object_init_ =
    mono_runtime_object_init;
mono_object_unbox_t mono_object_unbox_ = mono_object_unbox;
mono_compile_method_t mono_compile_method_ = mono_compile_method;
mono_gchandle_new_t mono_gchandle_new_ = mono_gchandle_new;
mono_gchandle_get_target_t mono_gchandle_get_target_ =
    mono_gchandle_get_target;
mono_gchandle_free_t mono_gchandle_free_ = mono_gchandle_free;

MonoDomain* g_domain{};
MonoImage* g_pui_image{};

std::uint32_t g_game_scene_handle{};
MonoMethod* g_find_scene_method{};
bool g_background_render_mode = false;

using application_update_t = void (*)(MonoObject*);
application_update_t g_application_update_original{};
bool g_hook_install_confirmed = false;
bool g_hook_attempted = false;

std::atomic_bool g_runtime_ready{false};

pthread_mutex_t g_log_mx = PTHREAD_MUTEX_INITIALIZER;
struct LogLine { char text[200]; };
LogLine g_log_out[32];   /* forwarded to the injector log (OVL_STATUS) */
unsigned g_log_n = 0;
constexpr const char* kPuiDll =
    "/system_ex/common_ex/lib/Sce.PlayStation.PUI.dll";
constexpr const char* kAppSystemDll =
    "/system_ex/common_ex/lib/Sce.Vsh.ShellUI.AppSystem.dll";
constexpr const char* kCoreDll =
    "/system_ex/common_ex/lib/Sce.PlayStation.Core.dll";

extern "C" __attribute__((naked, noinline, used, aligned(16)))
void commonfps_update_trampoline() {
    __asm__ volatile(
        ".rept 128\n"
        "nop\n"
        ".endr\n"
        "ret\n");
}

void application_update_hook(MonoObject* instance);

constexpr std::size_t kAbsoluteJumpSize = 14;
constexpr std::size_t kAtomicPatchSize = 16;
constexpr std::size_t kTrampolineCapacity = 128;

void encode_absolute_jump(
    std::uint8_t* output,
    const void* destination) noexcept {

    static constexpr std::uint8_t kPrefix[6] = {
        0xff, 0x25, 0x00, 0x00, 0x00, 0x00,
    };
    std::memcpy(output, kPrefix, sizeof(kPrefix));
    const std::uint64_t target =
        reinterpret_cast<std::uint64_t>(destination);
    std::memcpy(output + sizeof(kPrefix), &target, sizeof(target));
}

bool prepare_trampoline(
    const std::uint8_t* original,
    std::size_t original_size,
    const void* continuation) noexcept {

    if (!original || original_size == 0 ||
        original_size + (continuation ? kAbsoluteJumpSize : 0) >
            kTrampolineCapacity) {
        return false;
    }

    auto* trampoline = reinterpret_cast<std::uint8_t*>(
        &commonfps_update_trampoline);
    std::memcpy(trampoline, original, original_size);
    std::size_t written = original_size;
    if (continuation) {
        encode_absolute_jump(trampoline + written, continuation);
        written += kAbsoluteJumpSize;
    }

    __builtin___clear_cache(
        reinterpret_cast<char*>(trampoline),
        reinterpret_cast<char*>(trampoline + written));
    return true;
}

std::size_t decode_relocatable_instruction(
    const std::uint8_t* code,
    std::size_t available) noexcept {

    if (!code || available == 0)
        return 0;

    std::size_t cursor = 0;
    bool rex_w = false;
    bool operand16 = false;

    while (cursor < available) {
        const std::uint8_t byte = code[cursor];
        if (byte == 0x66) {
            operand16 = true;
            ++cursor;
            continue;
        }
        if (byte == 0x67) {

            return 0;
        }
        if (byte == 0xf0 || byte == 0xf2 || byte == 0xf3 ||
            byte == 0x2e || byte == 0x36 || byte == 0x3e ||
            byte == 0x26 || byte == 0x64 || byte == 0x65) {
            ++cursor;
            continue;
        }
        if (byte >= 0x40 && byte <= 0x4f) {
            rex_w = (byte & 0x08U) != 0;
            ++cursor;
            continue;
        }
        break;
    }

    if (cursor >= available)
        return 0;

    const std::uint8_t opcode = code[cursor++];

    if ((opcode >= 0x50 && opcode <= 0x5f) || opcode == 0x90)
        return cursor;

    if (opcode == 0x68) {
        const std::size_t immediate = operand16 ? 2U : 4U;
        return cursor + immediate <= available
            ? cursor + immediate
            : 0;
    }
    if (opcode == 0x6a)
        return cursor + 1U <= available ? cursor + 1U : 0;

    if (opcode >= 0xb8 && opcode <= 0xbf) {
        const std::size_t immediate = rex_w ? 8U : (operand16 ? 2U : 4U);
        return cursor + immediate <= available
            ? cursor + immediate
            : 0;
    }

    if (opcode == 0xe8 || opcode == 0xe9 || opcode == 0xeb ||
        (opcode >= 0x70 && opcode <= 0x7f) ||
        (opcode >= 0xe0 && opcode <= 0xe3)) {
        return 0;
    }

    std::size_t immediate = 0;
    bool has_modrm = false;

    switch (opcode) {
    case 0x01: case 0x03: case 0x09: case 0x0b:
    case 0x21: case 0x23: case 0x29: case 0x2b:
    case 0x31: case 0x33: case 0x39: case 0x3b:
    case 0x63: case 0x85: case 0x87: case 0x89:
    case 0x8b: case 0x8d:
        has_modrm = true;
        break;
    case 0x80: case 0x82: case 0x83: case 0xc6:
        has_modrm = true;
        immediate = 1;
        break;
    case 0x81: case 0xc7:
        has_modrm = true;
        immediate = operand16 ? 2U : 4U;
        break;
    case 0x0f: {
        if (cursor >= available)
            return 0;
        const std::uint8_t second = code[cursor++];
        if (second >= 0x80 && second <= 0x8f)
            return 0;
        if (second == 0x1e || second == 0x1f ||
            second == 0xb6 || second == 0xb7 ||
            second == 0xbe || second == 0xbf) {
            has_modrm = true;
            break;
        }
        return 0;
    }
    default:
        return 0;
    }

    if (!has_modrm || cursor >= available)
        return 0;

    const std::uint8_t modrm = code[cursor++];
    const std::uint8_t mod = static_cast<std::uint8_t>(modrm >> 6);
    const std::uint8_t rm = static_cast<std::uint8_t>(modrm & 7U);

    if (mod != 3 && rm == 4) {
        if (cursor >= available)
            return 0;
        const std::uint8_t sib = code[cursor++];
        const std::uint8_t base = static_cast<std::uint8_t>(sib & 7U);
        if (mod == 0 && base == 5) {
            if (cursor + 4U > available)
                return 0;
            cursor += 4U;
        }
    } else if (mod == 0 && rm == 5) {

        return 0;
    }

    if (mod == 1) {
        if (cursor + 1U > available)
            return 0;
        cursor += 1U;
    } else if (mod == 2) {
        if (cursor + 4U > available)
            return 0;
        cursor += 4U;
    }

    if (cursor + immediate > available)
        return 0;
    return cursor + immediate;
}

std::size_t native_patch_length(
    const std::uint8_t* code) noexcept {

    std::size_t length = 0;
    while (length < kAbsoluteJumpSize) {
        const std::size_t decoded = decode_relocatable_instruction(
            code + length,
            kAtomicPatchSize - length);
        if (decoded == 0)
            return 0;
        length += decoded;
    }

    return length <= kAtomicPatchSize ? length : 0;
}

template <typename T>
bool write_exact_file(
    const char* path,
    const T& value) noexcept {

    FILE* fp = std::fopen(path, "wb");
    if (!fp)
        return false;

    const bool complete =
        std::fwrite(&value, 1, sizeof(value), fp) == sizeof(value) &&
        std::fflush(fp) == 0;
    const bool closed = std::fclose(fp) == 0;
    return complete && closed;
}

template <typename T>
bool read_exact_file(const char* path, T& value) noexcept {
    FILE* fp = std::fopen(path, "rb");
    if (!fp)
        return false;

    const std::size_t count = std::fread(&value, 1, sizeof(value), fp);
    std::fclose(fp);
    return count == sizeof(value);
}

bool publish_hook_request(
    std::uint8_t* method,
    const std::array<std::uint8_t, kAtomicPatchSize>& expected,
    const std::array<std::uint8_t, kAtomicPatchSize>& desired,
    std::size_t displaced_size,
    application_update_t original_call,
    std::uint64_t& nonce) noexcept {

    ShellUiHookRequest request{};
    request.pid = getpid();
    request.method_address =
        reinterpret_cast<std::uint64_t>(method);
    request.hook_address =
        reinterpret_cast<std::uint64_t>(&application_update_hook);
    request.original_call_address =
        reinterpret_cast<std::uint64_t>(original_call);
    request.displaced_size = static_cast<std::uint32_t>(displaced_size);
    std::memcpy(request.expected, expected.data(), expected.size());
    std::memcpy(request.desired, desired.data(), desired.size());
    request.nonce = request.method_address ^ request.hook_address ^
        request.original_call_address ^
        (static_cast<std::uint64_t>(request.pid) << 32U) ^
        0x8d4f23a76c19e502ULL;
    if (request.nonce == 0)
        request.nonce = 1;
    request.checksum = shellui_hook_request_checksum(request);
    nonce = request.nonce;

    return write_exact_file(
        kShellUiHookRequestPath,
        request);
}

bool wait_for_native_hook_ack(
    pid_t pid,
    std::uint64_t nonce,
    ShellUiHookAck& ack) noexcept;

void log_line(const char* fmt, ...);
MonoImage* open_image(const char* path);

bool probe_hook_bytes(
    std::uint8_t* method,
    std::array<std::uint8_t, kAtomicPatchSize>& expected) noexcept {

    const std::array<std::uint8_t, kAtomicPatchSize> empty{};
    std::uint64_t nonce = 0;
    record_stage("hook_probe_request");
    if (!publish_hook_request(
            method,
            empty,
            empty,
            0,
            nullptr,
            nonce)) {
        log_line(
            "Application.Update probe publish failed method=%p",
            static_cast<void*>(method));
        return false;
    }

    ShellUiHookAck ack{};
    record_stage("hook_probe_ack_wait");
    if (!wait_for_native_hook_ack(getpid(), nonce, ack)) {
        log_line(
            "Application.Update probe timeout method=%p nonce=0x%llx",
            static_cast<void*>(method),
            static_cast<unsigned long long>(nonce));
        return false;
    }

    if (ack.phase != static_cast<std::uint8_t>(ShellUiHookAckPhase::Probe) ||
        ack.status != static_cast<std::int32_t>(ShellUiHookStatus::Success) ||
        ack.verified == 0 || ack.detached == 0 || ack.auth_restored == 0) {
        log_line(
            "Application.Update probe failed method=%p status=%d "
            "read_rc=%d verified=%u detached=%u auth_restored=%u phase=%u",
            static_cast<void*>(method),
            ack.status,
            ack.read_rc,
            static_cast<unsigned>(ack.verified),
            static_cast<unsigned>(ack.detached),
            static_cast<unsigned>(ack.auth_restored),
            static_cast<unsigned>(ack.phase));
        return false;
    }

    std::memcpy(expected.data(), ack.observed, expected.size());
    return true;
}

bool wait_for_native_hook_ack(
    pid_t pid,
    std::uint64_t nonce,
    ShellUiHookAck& ack) noexcept {

    for (int attempt = 0; attempt < 1000; ++attempt) {
        ShellUiHookAck candidate{};
        if (read_exact_file(kShellUiHookAckPath, candidate) &&
            candidate.magic == kShellUiHookAckMagic &&
            candidate.version == kShellUiHookProtocolVersion &&
            candidate.pid == pid &&
            candidate.nonce == nonce &&
            candidate.checksum == shellui_hook_ack_checksum(candidate)) {
            ack = candidate;
            return true;
        }
        usleep(20000);
    }
    return false;
}

enum class MainThreadGuardResult {
    Installed,
    Unsupported,
    Failed,
};

bool publish_main_thread_guard_request(
    std::uint8_t* method,
    const std::array<std::uint8_t, kAtomicPatchSize>& expected,
    std::uint64_t& nonce) noexcept {

    ShellUiHookRequest request{};
    request.pid = getpid();
    request.method_address = reinterpret_cast<std::uint64_t>(method);
    request.hook_address = request.method_address;
    request.original_call_address = 0;
    request.displaced_size = 1;
    std::memcpy(request.expected, expected.data(), expected.size());
    std::memcpy(request.desired, expected.data(), expected.size());
    request.desired[0] = 0xc3;
    request.nonce = request.method_address ^
        (static_cast<std::uint64_t>(request.pid) << 32U) ^
        0x6d9bc2f1a54837e0ULL;
    if (request.nonce == 0)
        request.nonce = 1;
    request.checksum = shellui_hook_request_checksum(request);
    nonce = request.nonce;
    return write_exact_file(kShellUiHookRequestPath, request);
}

MainThreadGuardResult install_main_thread_guard() {
    record_stage("legacy_guard_lookup");

    MonoImage* core = open_image(kCoreDll);
    if (!core) {
        log_line("legacy guard: core image unavailable");
        return MainThreadGuardResult::Failed;
    }

    MonoClass* diagnostics = mono_class_from_name_(
        core,
        "Sce.PlayStation.Core.Runtime",
        "Diagnostics");
    MonoMethod* check = diagnostics
        ? mono_class_get_method_from_name_(
              diagnostics,
              "CheckRunningOnMainThread",
              0)
        : nullptr;
    if (!check) {
        log_line("legacy guard: CheckRunningOnMainThread unavailable");
        return MainThreadGuardResult::Failed;
    }

    record_stage("legacy_guard_compile");
    auto* address = static_cast<std::uint8_t*>(mono_compile_method_(check));
    if (!address) {
        log_line("legacy guard: compile failed");
        return MainThreadGuardResult::Failed;
    }

    std::array<std::uint8_t, kAtomicPatchSize> expected{};
    record_stage("legacy_guard_probe");
    if (!probe_hook_bytes(address, expected)) {
        log_line("legacy guard: probe failed method=%p",
                 static_cast<void*>(address));
        return MainThreadGuardResult::Failed;
    }

    if (expected[0] == 0xc3) {
        log_line("legacy guard already disabled method=%p",
                 static_cast<void*>(address));
        record_stage("legacy_guard_ready");
        return MainThreadGuardResult::Installed;
    }

    std::uint64_t nonce = 0;
    record_stage("legacy_guard_request");
    if (!publish_main_thread_guard_request(address, expected, nonce)) {
        log_line("legacy guard: request publish failed method=%p",
                 static_cast<void*>(address));
        return MainThreadGuardResult::Failed;
    }

    ShellUiHookAck ack{};
    record_stage("legacy_guard_ack_wait");
    if (!wait_for_native_hook_ack(getpid(), nonce, ack)) {
        log_line("legacy guard: request timeout method=%p",
                 static_cast<void*>(address));
        return MainThreadGuardResult::Failed;
    }

    if (ack.status ==
        static_cast<std::int32_t>(ShellUiHookStatus::UnsupportedFirmware)) {
        log_line("legacy guard unsupported by controller method=%p",
                 static_cast<void*>(address));
        return MainThreadGuardResult::Unsupported;
    }

    if (ack.phase != static_cast<std::uint8_t>(
            ShellUiHookAckPhase::MainThreadGuard) ||
        ack.status != static_cast<std::int32_t>(ShellUiHookStatus::Success) ||
        ack.verified == 0 || ack.detached == 0 || ack.auth_restored == 0) {
        log_line(
            "legacy guard failed method=%p status=%d read_rc=%d write_rc=%d "
            "verified=%u restored=%u detached=%u auth_restored=%u phase=%u",
            static_cast<void*>(address),
            ack.status,
            ack.read_rc,
            ack.write_rc,
            static_cast<unsigned>(ack.verified),
            static_cast<unsigned>(ack.restored),
            static_cast<unsigned>(ack.detached),
            static_cast<unsigned>(ack.auth_restored),
            static_cast<unsigned>(ack.phase));
        return MainThreadGuardResult::Failed;
    }

    log_line(
        "legacy main-thread guard online method=%p stopped_patch=1 "
        "bytes_changed=1 verified=1",
        static_cast<void*>(address));
    record_stage("legacy_guard_ready");
    return MainThreadGuardResult::Installed;
}

constexpr const char* kRendererLog = "/system_tmp/vtpovl_shellui.log";
constexpr long kRendererLogCap = 12 * 1024;

void log_line(const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_log_mx);
    if (g_log_n < 32) std::snprintf(g_log_out[g_log_n++].text, sizeof g_log_out[0].text, "%s", line);
    pthread_mutex_unlock(&g_log_mx);
    FILE* fp = std::fopen(kRendererLog, "a");
    if (!fp)
        return;
    if (std::fseek(fp, 0, SEEK_END) == 0 && std::ftell(fp) > kRendererLogCap) {
        fp = std::freopen(kRendererLog, "w", fp);
        if (!fp)
            return;
    }
    std::fputs(line, fp);
    std::fputc('\n', fp);
    std::fclose(fp);
}

MonoImage* open_image(const char* path) {
    if (!g_domain || !mono_domain_assembly_open_ || !mono_assembly_get_image_)
        return nullptr;

    MonoAssembly* assembly = mono_domain_assembly_open_(g_domain, path);
    return assembly ? mono_assembly_get_image_(assembly) : nullptr;
}

MonoObject* invoke(
    MonoMethod* method,
    MonoObject* instance,
    void** args = nullptr) {

    if (!method || !mono_runtime_invoke_)
        return nullptr;

    MonoObject* exception = nullptr;
    MonoObject* result = mono_runtime_invoke_(
        method,
        instance,
        args,
        &exception);

    return exception ? nullptr : result;
}

MonoMethod* property_getter(MonoClass* klass, const char* name) {
    if (!klass)
        return nullptr;
    MonoProperty* property =
        mono_class_get_property_from_name_(klass, name);
    return property ? mono_property_get_get_method_(property) : nullptr;
}

MonoObject* managed_target(std::uint32_t handle) {
    return handle != 0 && mono_gchandle_get_target_
        ? mono_gchandle_get_target_(handle)
        : nullptr;
}

bool replace_managed_handle(
    std::uint32_t& handle,
    MonoObject* object) {

    if (!object || !mono_gchandle_new_ || !mono_gchandle_get_target_)
        return false;

    if (handle != 0 && managed_target(handle) == object)
        return true;

    if (handle != 0 && mono_gchandle_free_)
        mono_gchandle_free_(handle);

    handle = mono_gchandle_new_(object, 1);
    return handle != 0 && managed_target(handle) == object;
}

MonoObject* get_root_widget() {
    MonoObject* game_scene = managed_target(g_game_scene_handle);
    if (!game_scene)
        return nullptr;

    static MonoMethod* getter = nullptr;
    if (!getter) {
        MonoClass* scene = mono_class_from_name_(
            g_pui_image,
            "Sce.PlayStation.PUI.UI2",
            "Scene");
        getter = property_getter(scene, "RootWidget");
    }
    return getter ? invoke(getter, game_scene) : nullptr;
}

bool refresh_game_scene() {
    if (!g_domain || !g_find_scene_method || !mono_string_new_)
        return false;

    static std::uint32_t game_path_handle = 0;
    MonoString* game_path = reinterpret_cast<MonoString*>(managed_target(game_path_handle));
    if (!game_path) {
        MonoDomain* active_domain = mono_domain_get_();
        game_path = mono_string_new_(active_domain ? active_domain : g_domain, "Game");
        if (game_path && mono_gchandle_new_)
            game_path_handle = mono_gchandle_new_(reinterpret_cast<MonoObject*>(game_path), 1);
    }
    if (!game_path)
        return false;
    void* find_args[1] = {game_path};
    MonoObject* game_scene = invoke(
        g_find_scene_method, nullptr, find_args);
    if (!game_scene)
        return false;

    if (managed_target(g_game_scene_handle) != game_scene) {
        if (!replace_managed_handle(g_game_scene_handle, game_scene))
            return false;
        log_line(
            "Game ContainerScene refreshed scene=%p handle=%u mode=%s",
            static_cast<void*>(game_scene),
            g_game_scene_handle,
            g_background_render_mode ? "legacy-background" : "update-hook");
    }
    return true;
}

/* ======================================================================================
 * Virtual Toy Pad overlay engine (SpiderNic96, 2026; GPL-3.0-or-later).
 *
 * A retained-mode compositor driven by the injector over TCP (see ovl_proto.h):
 * widgets (ImageBox / Panel / Label) are created once from a DEFINE list, textures are
 * uploaded once, and FRAME deltas only touch the properties that changed. Everything that
 * touches PUI runs on the UI thread (Application.Update hook) - or on our own render thread
 * when only the legacy main-thread guard could be installed, the same split the HUD overlay
 * uses. The receiver thread only parses messages into the desired state under g_state_mx.
 * ====================================================================================== */

struct MonoMethodSignature;
struct MonoType;
struct MonoArray;
extern "C" {
MonoClass* mono_class_get_parent(MonoClass*);
MonoMethod* mono_class_get_methods(MonoClass*, void**);
const char* mono_method_get_name(MonoMethod*);
MonoMethodSignature* mono_method_signature(MonoMethod*);
std::uint32_t mono_signature_get_param_count(MonoMethodSignature*);
MonoType* mono_signature_get_params(MonoMethodSignature*, void**);
char* mono_type_get_name(MonoType*);
void mono_free(void*);
MonoClass* mono_get_byte_class();
MonoArray* mono_array_new(MonoDomain*, MonoClass*, std::uintptr_t);
char* mono_array_addr_with_size(MonoArray*, int, std::uintptr_t);
MonoClass* mono_object_get_class(MonoObject*);
}

constexpr float kCanvasW = 1920.0f;

struct WDesired {
    std::uint8_t type = 0;
    float x = 0, y = 0, w = 0, h = 0;
    std::uint32_t vis = 0;
    float alpha = 1.0f;
    std::uint8_t c[4] = {255, 255, 255, 255};
    std::uint32_t tex = 0;
    char text[160] = {0};
    std::uint16_t font = 24;
    std::uint8_t align = 0, lines = 1;
    std::uint32_t dirty = 0;
};

struct WLive {
    std::uint8_t type = 0;
    std::uint32_t handle = 0;
    std::uint32_t font_handle = 0;  /* boxed UIFont kept alive */
    std::uint32_t tex = 0;          /* texture currently shown */
    std::uint16_t font = 0;
};

struct Tex {
    std::vector<std::uint8_t> data;   /* pending upload (RGBA or PNG) */
    std::uint16_t w = 0, h = 0;
    bool png = false;
    bool pending = false;
    std::uint32_t texture = 0;        /* Texture2D handle (pixels mode) */
    std::uint32_t asset = 0;          /* ImageAsset handle */
    char uri[96] = {0};               /* file mode: file:///Temp/vtpN.png */
    int failures = 0;
};

pthread_mutex_t g_state_mx = PTHREAD_MUTEX_INITIALIZER;
std::vector<WDesired> g_desired;           /* guarded by g_state_mx */
std::uint32_t g_define_gen = 0;            /* guarded */
/* function-local statics: the loader runs no global constructors inside SceShellUI */
std::map<std::uint32_t, Tex>& tex_map() { static std::map<std::uint32_t, Tex> m; return m; }
#define g_tex tex_map()   /* map guarded; handles only touched on the UI thread */
std::vector<std::uint32_t> g_tex_free;     /* guarded */
std::uint32_t g_tex_mode_pref = 0;         /* 0 auto, 1 pixels, 2 files */
std::atomic<int> g_client_connected{0};

std::vector<WLive> g_live;                 /* UI thread */
std::uint32_t g_live_gen = 0;
std::uint32_t g_container = 0;             /* Panel holding our widgets */
MonoObject* g_live_root = nullptr;
float g_scale = 1.0f;
std::atomic<float> g_scene_w{0.0f}, g_scene_h{0.0f};
std::atomic<int> g_scene_up{0};

/* ------------------------------------------------------------- bindings */

MonoClass *g_widget_class{}, *g_panel_class{}, *g_image_class{}, *g_label_class{}, *g_font_class{},
    *g_color_class{}, *g_asset_class{}, *g_texture_class{};
MonoMethod *g_append{}, *g_remove{}, *g_font_ctor{}, *g_asset_uri_ctor{}, *g_asset_tex_ctor{},
    *g_texture_ctor{}, *g_set_pixels{}, *g_load_async{}, *g_dispose_tex{}, *g_dispose_asset{};
std::uint32_t g_caps = 0;
const char* g_temp_dir = nullptr;

MonoMethod* method_exact(MonoClass* type, const char* name, std::initializer_list<const char*> params) {
    for (MonoClass* c = type; c; c = mono_class_get_parent(c)) {
        void* iter = nullptr;
        while (MonoMethod* m = mono_class_get_methods(c, &iter)) {
            if (std::strcmp(mono_method_get_name(m), name)) continue;
            MonoMethodSignature* sig = mono_method_signature(m);
            if (!sig || mono_signature_get_param_count(sig) != params.size()) continue;
            void* pi = nullptr;
            bool match = true;
            for (const char* expected : params) {
                char* actual = mono_type_get_name(mono_signature_get_params(sig, &pi));
                match = match && actual && !std::strcmp(actual, expected);
                if (actual) mono_free(actual);
            }
            if (match) return m;
        }
    }
    return nullptr;
}

MonoMethod* setter_of(MonoClass* c, const char* name) {
    for (; c; c = mono_class_get_parent(c)) {
        MonoProperty* p = mono_class_get_property_from_name_(c, name);
        if (p) return mono_property_get_set_method_(p);
    }
    return nullptr;
}

MonoMethod* getter_of(MonoClass* c, const char* name) {
    for (; c; c = mono_class_get_parent(c)) {
        MonoProperty* p = mono_class_get_property_from_name_(c, name);
        if (p) return mono_property_get_get_method_(p);
    }
    return nullptr;
}

/* invoke, logging (rate limited) any managed exception */
Tex* tex_get(std::uint32_t id) {
    pthread_mutex_lock(&g_state_mx);
    auto it = g_tex.find(id);
    Tex* t = it == g_tex.end() ? nullptr : &it->second;
    pthread_mutex_unlock(&g_state_mx);
    return t;
}

bool call(MonoMethod* m, void* obj, void** args, MonoObject** out = nullptr) {
    if (!m) return false;
    MonoObject* exc = nullptr;
    MonoObject* r = mono_runtime_invoke_(m, obj, args, &exc);
    if (exc) {
        static int logged = 0;
        if (logged++ < 20) log_line("managed exception in %s", mono_method_get_name(m));
        return false;
    }
    if (out) *out = r;
    return true;
}

/* property cache: (class, name) -> setter */
struct PropKey { MonoClass* c; const char* n; bool operator<(const PropKey& o) const { return c != o.c ? c < o.c : std::strcmp(n, o.n) < 0; } };
std::map<PropKey, MonoMethod*>& setter_map() { static std::map<PropKey, MonoMethod*> m; return m; }
#define g_setters setter_map()

MonoMethod* cached_setter(MonoClass* c, const char* name) {
    auto it = g_setters.find({c, name});
    if (it != g_setters.end()) return it->second;
    MonoMethod* m = setter_of(c, name);
    g_setters[{c, name}] = m;
    if (!m) log_line("no setter %s", name);
    return m;
}

bool set_f(MonoClass* c, MonoObject* o, const char* n, float v) { void* a[] = {&v}; return call(cached_setter(c, n), o, a); }
bool set_i(MonoClass* c, MonoObject* o, const char* n, int v) { void* a[] = {&v}; return call(cached_setter(c, n), o, a); }
bool set_b(MonoClass* c, MonoObject* o, const char* n, bool v) { void* a[] = {&v}; return call(cached_setter(c, n), o, a); }
bool set_o(MonoClass* c, MonoObject* o, const char* n, void* v) { void* a[] = {v}; return call(cached_setter(c, n), o, a); }
bool set_color(MonoClass* c, MonoObject* o, const char* n, const std::uint8_t rgba[4]) {
    float col[4] = {rgba[0] / 255.0f, rgba[1] / 255.0f, rgba[2] / 255.0f, rgba[3] / 255.0f};
    void* a[] = {col};   /* UIColor is a struct of 4 floats, passed by pointer */
    return call(cached_setter(c, n), o, a);
}

MonoObject* target(std::uint32_t h) { return h ? mono_gchandle_get_target_(h) : nullptr; }
void release(std::uint32_t& h) { if (h) mono_gchandle_free_(h); h = 0; }

void bind_engine() {
    g_widget_class = mono_class_from_name_(g_pui_image, "Sce.PlayStation.PUI.UI2", "Widget");
    g_panel_class = mono_class_from_name_(g_pui_image, "Sce.PlayStation.PUI.UI2", "Panel");
    g_image_class = mono_class_from_name_(g_pui_image, "Sce.PlayStation.PUI.UI2", "ImageBox");
    g_label_class = mono_class_from_name_(g_pui_image, "Sce.PlayStation.PUI.UI2", "Label");
    g_font_class = mono_class_from_name_(g_pui_image, "Sce.PlayStation.PUI.UI2", "UIFont");
    g_color_class = mono_class_from_name_(g_pui_image, "Sce.PlayStation.PUI", "UIColor");
    g_asset_class = mono_class_from_name_(g_pui_image, "Sce.PlayStation.PUI", "ImageAsset");
    MonoImage* core = open_image(kCoreDll);
    g_texture_class = core ? mono_class_from_name_(core, "Sce.PlayStation.Core.Graphics", "Texture2D") : nullptr;

    g_append = g_widget_class ? mono_class_get_method_from_name_(g_widget_class, "AppendChild", 1) : nullptr;
    g_remove = g_widget_class ? mono_class_get_method_from_name_(g_widget_class, "RemoveChild", 1) : nullptr;
    g_font_ctor = g_font_class ? mono_class_get_method_from_name_(g_font_class, ".ctor", 3) : nullptr;
    if (g_asset_class) {
        g_asset_uri_ctor = method_exact(g_asset_class, ".ctor", {"System.String"});
        g_asset_tex_ctor = method_exact(g_asset_class, ".ctor", {"Sce.PlayStation.Core.Graphics.Texture"});
        g_dispose_asset = mono_class_get_method_from_name_(g_asset_class, "Dispose", 0);
    }
    if (g_texture_class) {
        g_texture_ctor = method_exact(g_texture_class, ".ctor",
            {"System.Int32", "System.Int32", "System.Boolean", "Sce.PlayStation.Core.Graphics.PixelFormat"});
        g_set_pixels = method_exact(g_texture_class, "SetPixels", {"System.Int32", "System.Array"});
        g_dispose_tex = mono_class_get_method_from_name_(g_texture_class, "Dispose", 0);
    }
    g_load_async = g_image_class ? mono_class_get_method_from_name_(g_image_class, "LoadAsync", 1) : nullptr;

    g_caps = 0;
    if (g_texture_ctor && g_set_pixels && g_asset_tex_ctor && setter_of(g_image_class, "Image")) g_caps |= OVL_CAP_PIXELS;
    if (g_load_async || (g_asset_uri_ctor && setter_of(g_image_class, "Image"))) g_caps |= OVL_CAP_FILES;
    if (setter_of(g_image_class, "FilterColor")) g_caps |= OVL_CAP_FILTER;
    if (g_panel_class && setter_of(g_panel_class, "BackgroundColor")) g_caps |= OVL_CAP_PANEL;
    if (setter_of(g_label_class, "MaxLineCount") && setter_of(g_label_class, "TextTrimming")) g_caps |= OVL_CAP_TRIM;
    log_line("engine bindings widget=%d panel=%d image=%d label=%d font=%d asset=%d texture=%d caps=0x%x",
             g_widget_class != nullptr, g_panel_class != nullptr, g_image_class != nullptr,
             g_label_class != nullptr, g_font_ctor != nullptr, g_asset_class != nullptr,
             g_texture_class != nullptr, g_caps);
}

/* ------------------------------------------------------------- textures (UI thread) */

bool ensure_temp_dir() {
    if (g_temp_dir) return true;
    static const char* cands[] = {"/Temp", "/download", "/system_tmp", nullptr};
    for (int i = 0; cands[i]; ++i) {
        char p[64];
        std::snprintf(p, sizeof p, "%s/vtpovl.probe", cands[i]);
        FILE* f = std::fopen(p, "wb");
        if (!f) continue;
        bool ok = std::fwrite("ok", 1, 2, f) == 2;
        std::fclose(f);
        unlink(p);
        if (ok) { g_temp_dir = cands[i]; log_line("file textures in %s", g_temp_dir); return true; }
    }
    return false;
}

bool use_pixels() {
    if (g_tex_mode_pref == 2) return false;
    if (g_tex_mode_pref == 1) return (g_caps & OVL_CAP_PIXELS) != 0;
    return (g_caps & OVL_CAP_PIXELS) != 0;
}

void tex_dispose(Tex& t) {
    if (MonoObject* a = target(t.asset)) call(g_dispose_asset, a, nullptr);
    if (MonoObject* x = target(t.texture)) call(g_dispose_tex, x, nullptr);
    release(t.asset);
    release(t.texture);
    if (t.uri[0]) {
        unlink(t.uri + 7);   /* strip file:// */
        t.uri[0] = 0;
    }
}

/* creates the managed texture for one pending entry; data is moved out under the lock */
bool tex_upload(std::uint32_t id, Tex& t, std::vector<std::uint8_t>& data, bool png) {
    tex_dispose(t);
    if (!png && use_pixels()) {
        int w = t.w, h = t.h, fmt = 1;   /* PixelFormat.Rgba */
        bool mip = false;
        void* targs[] = {&w, &h, &mip, &fmt};
        MonoObject* obj = mono_object_new_(g_domain, g_texture_class);
        if (!obj) return false;
        std::uint32_t th = mono_gchandle_new_(obj, 1);
        if (!call(g_texture_ctor, target(th), targs)) { release(th); return false; }
        MonoArray* arr = mono_array_new(g_domain, mono_get_byte_class(), data.size());
        std::uint32_t ah = arr ? mono_gchandle_new_(reinterpret_cast<MonoObject*>(arr), 1) : 0;
        if (!ah) { release(th); return false; }
        std::memcpy(mono_array_addr_with_size(reinterpret_cast<MonoArray*>(target(ah)), 1, 0), data.data(), data.size());
        int level = 0;
        void* pargs[] = {&level, target(ah)};
        bool ok = call(g_set_pixels, target(th), pargs);
        release(ah);
        if (!ok) { release(th); return false; }
        MonoObject* asset = mono_object_new_(g_domain, g_asset_class);
        std::uint32_t sh = asset ? mono_gchandle_new_(asset, 1) : 0;
        void* aargs[] = {target(th)};
        if (!sh || !call(g_asset_tex_ctor, target(sh), aargs)) { release(sh); release(th); return false; }
        t.texture = th;
        t.asset = sh;
        return true;
    }
    /* file mode: write the PNG where SceShellUI can read it */
    if (!png || !ensure_temp_dir()) return false;
    char path[96];
    std::snprintf(path, sizeof path, "%s/vtp%u.png", g_temp_dir, id);
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = std::fclose(f) == 0 && ok;
    if (!ok) { unlink(path); return false; }
    std::snprintf(t.uri, sizeof t.uri, "file://%s", path);
    if (g_asset_uri_ctor && setter_of(g_image_class, "Image")) {
        MonoString* s = mono_string_new_(g_domain, t.uri);
        MonoObject* asset = mono_object_new_(g_domain, g_asset_class);
        std::uint32_t sh = asset ? mono_gchandle_new_(asset, 1) : 0;
        void* aargs[] = {s};
        if (sh && call(g_asset_uri_ctor, target(sh), aargs)) t.asset = sh;
        else release(sh);
    }
    return true;
}

/* ------------------------------------------------------------- widgets (UI thread) */

MonoClass* class_of_type(std::uint8_t type) {
    return type == OVL_W_IMAGE ? g_image_class : type == OVL_W_PANEL ? g_panel_class : g_label_class;
}

std::uint32_t new_widget(std::uint8_t type, int index) {
    MonoClass* c = class_of_type(type);
    if (!c) return 0;
    MonoObject* o = mono_object_new_(g_domain, c);
    if (!o) return 0;
    std::uint32_t h = mono_gchandle_new_(o, 1);
    mono_runtime_object_init_(target(h));
    MonoObject* w = target(h);
    char name[32];
    std::snprintf(name, sizeof name, "id_vtpovl_%d", index);
    set_o(c, w, "Name", mono_string_new_(g_domain, name));
    set_b(c, w, "Visible", false);
    if (setter_of(c, "Focusable")) set_b(c, w, "Focusable", false);
    if (type == OVL_W_IMAGE) {
        if (setter_of(c, "ImageScaleType")) set_i(c, w, "ImageScaleType", 1);          /* stretch */
        if (setter_of(c, "DisposeImageOutOfScreen")) set_b(c, w, "DisposeImageOutOfScreen", false);
    } else if (type == OVL_W_PANEL) {
        std::uint8_t clear[4] = {0, 0, 0, 0};
        set_color(c, w, "BackgroundColor", clear);
    } else {
        set_i(c, w, "VerticalAlignment", 0);
        set_b(c, w, "FitHeightToText", true);
        if (setter_of(c, "EnableThemedColor")) set_b(c, w, "EnableThemedColor", false);
    }
    return h;
}

void destroy_live(MonoObject* parent) {
    for (auto& l : g_live) {
        if (MonoObject* w = target(l.handle)) {
            if (parent) { void* a[] = {w}; call(g_remove, parent, a); }
        }
        release(l.handle);
        release(l.font_handle);
    }
    g_live.clear();
}

bool build_live(MonoObject* root, const std::vector<std::uint8_t>& types) {
    MonoObject* parent = root;
    if (g_panel_class && !g_container) {
        std::uint32_t h = new_widget(OVL_W_PANEL, 9999);
        if (h) {
            MonoObject* p = target(h);
            set_f(g_panel_class, p, "X", 0.0f);
            set_f(g_panel_class, p, "Y", 0.0f);
            set_f(g_panel_class, p, "Width", g_scene_w.load());
            set_f(g_panel_class, p, "Height", g_scene_h.load());
            set_b(g_panel_class, p, "Visible", true);
            void* a[] = {p};
            if (call(g_append, root, a)) g_container = h;
            else release(h);
        }
    }
    if (MonoObject* c = target(g_container)) parent = c;
    g_live.assign(types.size(), WLive{});
    int made = 0;
    for (std::size_t i = 0; i < types.size(); ++i) {
        g_live[i].type = types[i];
        std::uint32_t h = new_widget(types[i], (int)i);
        if (!h) continue;
        void* a[] = {target(h)};
        if (!call(g_append, parent, a)) { release(h); continue; }
        g_live[i].handle = h;
        ++made;
    }
    log_line("widgets built %d/%zu container=%d scale=%.3f", made, types.size(), g_container != 0, (double)g_scale);
    return made > 0;
}

void apply_one(std::size_t i, const WDesired& d, std::uint32_t mask, std::uint32_t& retry) {
    WLive& l = g_live[i];
    MonoObject* w = target(l.handle);
    if (!w) return;
    MonoClass* c = class_of_type(l.type);
    const float s = g_scale;
    if (mask & OVL_F_GEOM) {
        set_f(c, w, "X", d.x * s);
        set_f(c, w, "Y", d.y * s);
        if (l.type != OVL_W_LABEL) {
            set_f(c, w, "Width", d.w * s);
            set_f(c, w, "Height", d.h * s);
        } else if (d.w > 0) {
            set_b(c, w, "FitWidthToText", false);
            set_f(c, w, "Width", d.w * s);
        } else {
            set_b(c, w, "FitWidthToText", true);
        }
    }
    if (mask & OVL_F_ALPHA) set_f(c, w, "Opacity", d.alpha);
    if (mask & OVL_F_COLOR) {
        if (l.type == OVL_W_IMAGE) { if (g_caps & OVL_CAP_FILTER) set_color(c, w, "FilterColor", d.c); }
        else if (l.type == OVL_W_PANEL) set_color(c, w, "BackgroundColor", d.c);
        else set_color(c, w, "TextColor", d.c);
    }
    if ((mask & OVL_F_TEX) && l.type == OVL_W_IMAGE) {
        Tex* t = d.tex ? tex_get(d.tex) : nullptr;
        if (d.tex == 0) {
            if (cached_setter(c, "Image")) set_o(c, w, "Image", nullptr);
            l.tex = 0;
        } else if (!t || t->pending || (!t->asset && !t->uri[0])) {
            if (t && t->failures > 2) {}   /* give up quietly */
            else retry |= OVL_F_TEX;       /* not uploaded yet: try next frame */
        } else if (t->asset) {
            set_o(c, w, "Image", target(t->asset));
            l.tex = d.tex;
        } else {
            void* a[] = {mono_string_new_(g_domain, t->uri)};
            call(g_load_async, w, a);
            l.tex = d.tex;
        }
    }
    if ((mask & OVL_F_TEXT) && l.type == OVL_W_LABEL) {
        int px = (int)(d.font * s + 0.5f);
        if (px < 6) px = 6;
        if (px != l.font || !l.font_handle) {
            MonoObject* boxed = mono_object_new_(g_domain, g_font_class);
            std::uint32_t fh = boxed ? mono_gchandle_new_(boxed, 1) : 0;
            if (fh) {
                int style = 0, weight = 0;
                void* fa[] = {&px, &style, &weight};
                void* real = mono_object_unbox_(target(fh));
                if (call(g_font_ctor, real, fa) && set_o(c, w, "Font", real)) {
                    release(l.font_handle);
                    l.font_handle = fh;
                    l.font = (std::uint16_t)px;
                } else release(fh);
            }
        }
        set_i(c, w, "HorizontalAlignment", d.align);
        if (g_caps & OVL_CAP_TRIM) {
            set_i(c, w, "MaxLineCount", d.lines ? d.lines : 1);
            set_i(c, w, "TextTrimming", 1);
        }
        set_o(c, w, "Text", mono_string_new_(g_domain, d.text));
    }
    if (mask & OVL_F_VIS) set_b(c, w, "Visible", d.vis != 0);
}

/* one UI-thread pass: scene tracking, texture uploads, (re)build and property deltas */
void ui_tick() {
    if (!g_runtime_ready.load()) return;
    if (!refresh_game_scene()) {
        if (!g_live.empty() || g_container) {
            g_live.clear();      /* the scene and its widgets are gone with the game */
            release(g_container);
            g_live_root = nullptr;
            log_line("game scene closed; overlay idle");
        }
        g_scene_up.store(0);
        return;
    }
    MonoObject* root = get_root_widget();
    if (!root) { g_scene_up.store(0); return; }
    if (root != g_live_root) {
        if (g_live_root) { g_live.clear(); release(g_container); }
        g_live_root = root;
        MonoMethod* gw = getter_of(g_widget_class, "Width");
        MonoMethod* gh = getter_of(g_widget_class, "Height");
        MonoObject* rw = nullptr; MonoObject* rh = nullptr;
        call(gw, root, nullptr, &rw);
        call(gh, root, nullptr, &rh);
        float sw = rw ? *static_cast<float*>(mono_object_unbox_(rw)) : kCanvasW;
        float sh = rh ? *static_cast<float*>(mono_object_unbox_(rh)) : 1080.0f;
        if (sw < 320.0f || sw > 8192.0f) sw = kCanvasW;
        if (sh < 240.0f || sh > 8192.0f) sh = 1080.0f;
        g_scene_w.store(sw);
        g_scene_h.store(sh);
        g_scale = sw / kCanvasW;
        g_live_gen = 0;   /* rebuild on this scene */
        log_line("game scene %.0fx%.0f scale %.3f", (double)sw, (double)sh, (double)g_scale);
    }
    g_scene_up.store(1);

    /* texture frees + uploads (bounded per frame so a burst never stalls the UI) */
    std::vector<std::uint32_t> frees;
    std::vector<std::pair<std::uint32_t, bool>> todo;
    std::vector<std::vector<std::uint8_t>> payloads;
    pthread_mutex_lock(&g_state_mx);
    frees.swap(g_tex_free);
    std::size_t budget = 3u << 20;
    for (auto& kv : g_tex) {
        if (!kv.second.pending || budget == 0) continue;
        if (todo.size() >= 8) break;
        budget = kv.second.data.size() > budget ? 0 : budget - kv.second.data.size();
        todo.push_back({kv.first, kv.second.png});
        payloads.emplace_back(std::move(kv.second.data));
        kv.second.data.clear();
        kv.second.pending = false;
    }
    pthread_mutex_unlock(&g_state_mx);
    for (std::uint32_t id : frees) {
        Tex* tf = tex_get(id);
        if (!tf) continue;
        for (std::size_t i = 0; i < g_live.size(); ++i)
            if (g_live[i].tex == id)
                if (MonoObject* w = target(g_live[i].handle)) { set_o(g_image_class, w, "Image", nullptr); g_live[i].tex = 0; }
        tex_dispose(*tf);
        pthread_mutex_lock(&g_state_mx);
        g_tex.erase(id);
        pthread_mutex_unlock(&g_state_mx);
    }
    for (std::size_t k = 0; k < todo.size(); ++k) {
        Tex* tu = tex_get(todo[k].first);
        if (!tu) continue;
        if (!tex_upload(todo[k].first, *tu, payloads[k], todo[k].second)) {
            static int logged = 0;
            if (logged++ < 10) log_line("texture %u upload failed (%s)", todo[k].first, todo[k].second ? "png" : "rgba");
            tu->failures++;
        } else {
            /* widgets already showing this id get the new image */
            pthread_mutex_lock(&g_state_mx);
            for (std::size_t i = 0; i < g_desired.size(); ++i)
                if (g_desired[i].tex == todo[k].first) g_desired[i].dirty |= OVL_F_TEX;
            pthread_mutex_unlock(&g_state_mx);
        }
    }

    /* (re)build widgets for the current DEFINE */
    pthread_mutex_lock(&g_state_mx);
    std::uint32_t gen = g_define_gen;
    std::vector<std::uint8_t> types;
    if (gen != g_live_gen) {
        types.reserve(g_desired.size());
        for (auto& d : g_desired) { types.push_back(d.type); d.dirty = 0xFFFF; }
    }
    pthread_mutex_unlock(&g_state_mx);
    if (gen != g_live_gen) {
        MonoObject* cont = target(g_container);
        destroy_live(cont ? cont : root);
        if (!types.empty() && !build_live(root, types)) return;
        g_live_gen = gen;
    }

    /* property deltas */
    std::vector<std::pair<std::size_t, WDesired>> work;
    pthread_mutex_lock(&g_state_mx);
    if (g_desired.size() == g_live.size()) {
        for (std::size_t i = 0; i < g_desired.size(); ++i) {
            if (!g_desired[i].dirty) continue;
            work.push_back({i, g_desired[i]});
            g_desired[i].dirty = 0;
        }
    }
    pthread_mutex_unlock(&g_state_mx);
    std::vector<std::pair<std::size_t, std::uint32_t>> retry;
    for (auto& wk : work) {
        std::uint32_t again = 0;
        apply_one(wk.first, wk.second, wk.second.dirty, again);
        if (again) retry.push_back({wk.first, again});
    }
    if (!retry.empty()) {
        pthread_mutex_lock(&g_state_mx);
        for (auto& r : retry)
            if (r.first < g_desired.size()) g_desired[r.first].dirty |= r.second;
        pthread_mutex_unlock(&g_state_mx);
    }
}

/* ------------------------------------------------------------- receiver (own thread) */

int g_listen_fd = -1;

bool read_full(int fd, void* buf, std::size_t n) {
    auto* p = static_cast<std::uint8_t*>(buf);
    while (n) {
        ssize_t k = recv(fd, p, n, 0);
        if (k <= 0) return false;
        p += k;
        n -= (std::size_t)k;
    }
    return true;
}

bool send_msg(int fd, std::uint16_t type, const void* payload, std::uint32_t len) {
    ovl_msg h{OVL_MAGIC, type, OVL_VERSION, len};
    if (send(fd, &h, sizeof h, 0) != (ssize_t)sizeof h) return false;
    const auto* p = static_cast<const std::uint8_t*>(payload);
    while (len) {
        ssize_t k = send(fd, p, len, 0);
        if (k <= 0) return false;
        p += k;
        len -= (std::uint32_t)k;
    }
    return true;
}

template <typename T> bool take(const std::uint8_t*& p, const std::uint8_t* end, T& v) {
    if ((std::size_t)(end - p) < sizeof(T)) return false;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return true;
}

void handle_frame(const std::uint8_t* p, const std::uint8_t* end) {
    std::uint16_t count = 0;
    if (!take(p, end, count)) return;
    pthread_mutex_lock(&g_state_mx);
    for (std::uint16_t n = 0; n < count; ++n) {
        std::uint16_t id = 0, mask = 0;
        if (!take(p, end, id) || !take(p, end, mask)) break;
        WDesired dummy;
        WDesired& d = id < g_desired.size() ? g_desired[id] : dummy;
        bool ok = true;
        if (mask & OVL_F_GEOM) ok = take(p, end, d.x) && take(p, end, d.y) && take(p, end, d.w) && take(p, end, d.h);
        if (ok && (mask & OVL_F_VIS)) ok = take(p, end, d.vis);
        if (ok && (mask & OVL_F_ALPHA)) ok = take(p, end, d.alpha);
        if (ok && (mask & OVL_F_COLOR)) ok = take(p, end, d.c);
        if (ok && (mask & OVL_F_TEX)) ok = take(p, end, d.tex);
        if (ok && (mask & OVL_F_TEXT)) {
            std::uint16_t len = 0;
            ok = take(p, end, d.font) && take(p, end, d.align) && take(p, end, d.lines) && take(p, end, len) &&
                 (std::size_t)(end - p) >= len;
            if (ok) {
                std::size_t n = len < sizeof d.text - 1 ? len : sizeof d.text - 1;
                std::memcpy(d.text, p, n);
                d.text[n] = 0;
                p += len;
                std::size_t pad = (4 - (len & 3)) & 3;
                p = (std::size_t)(end - p) >= pad ? p + pad : end;
            }
        }
        if (!ok) break;
        d.dirty |= mask;
    }
    pthread_mutex_unlock(&g_state_mx);
}

void handle_message(int fd, const ovl_msg& h, std::vector<std::uint8_t>& body) {
    const std::uint8_t* p = body.data();
    const std::uint8_t* end = p + body.size();
    switch (h.type) {
    case OVL_TEX:
    case OVL_TEXPNG: {
        std::uint32_t id = 0;
        std::uint16_t w = 0, hh = 0;
        if (!take(p, end, id) || !take(p, end, w) || !take(p, end, hh)) return;
        if (h.type == OVL_TEX && (std::size_t)(end - p) != (std::size_t)w * hh * 4) return;
        pthread_mutex_lock(&g_state_mx);
        Tex& t = g_tex[id];
        t.data.assign(p, end);
        t.w = w; t.h = hh;
        t.png = h.type == OVL_TEXPNG;
        t.pending = true;
        t.failures = 0;
        pthread_mutex_unlock(&g_state_mx);
        break;
    }
    case OVL_TEXFREE: {
        std::uint32_t id = 0;
        if (!take(p, end, id)) return;
        pthread_mutex_lock(&g_state_mx);
        g_tex_free.push_back(id);
        pthread_mutex_unlock(&g_state_mx);
        break;
    }
    case OVL_DEFINE: {
        std::uint16_t count = 0;
        if (!take(p, end, count) || count > OVL_MAX_WIDGETS || (std::size_t)(end - p) < count) return;
        pthread_mutex_lock(&g_state_mx);
        g_desired.assign(count, WDesired{});
        for (std::uint16_t i = 0; i < count; ++i) { g_desired[i].type = p[i]; g_desired[i].dirty = 0xFFFF; }
        ++g_define_gen;
        pthread_mutex_unlock(&g_state_mx);
        log_line("define %u widgets", count);
        break;
    }
    case OVL_FRAME:
        handle_frame(p, end);
        break;
    case OVL_CONFIG: {
        ovl_config c{};
        if (take(p, end, c)) g_tex_mode_pref = c.tex_mode;
        break;
    }
    case OVL_PING:
        send_msg(fd, OVL_PING, nullptr, 0);
        break;
    default:
        break;
    }
}

void hide_everything() {
    pthread_mutex_lock(&g_state_mx);
    for (auto& d : g_desired) { d.vis = 0; d.dirty |= OVL_F_VIS; }
    pthread_mutex_unlock(&g_state_mx);
}

void serve_client(int fd) {
    ovl_hello hello{};
    hello.caps = g_caps | (g_scene_up.load() ? OVL_CAP_SCENE : 0);
    hello.scene_w = g_scene_w.load();
    hello.scene_h = g_scene_h.load();
    hello.pid = getpid();
    pthread_mutex_lock(&g_state_mx);
    hello.textures = (std::uint32_t)g_tex.size();
    hello.widgets = (std::uint32_t)g_desired.size();
    pthread_mutex_unlock(&g_state_mx);
    if (!send_msg(fd, OVL_HELLO, &hello, sizeof hello)) return;
    g_client_connected.store(1);
    std::vector<std::uint8_t> body;
    for (;;) {
        ovl_msg h{};
        if (!read_full(fd, &h, sizeof h)) break;
        if (h.magic != OVL_MAGIC || h.version != OVL_VERSION || h.len > OVL_MAX_MSG) {
            log_line("bad message magic=%08x version=%u len=%u", h.magic, h.version, h.len);
            break;
        }
        body.resize(h.len);
        if (h.len && !read_full(fd, body.data(), h.len)) break;
        handle_message(fd, h, body);
        /* surface queued renderer log lines to the injector's log */
        LogLine lines[32];
        unsigned nl;
        pthread_mutex_lock(&g_log_mx);
        nl = g_log_n;
        std::memcpy(lines, g_log_out, nl * sizeof lines[0]);
        g_log_n = 0;
        pthread_mutex_unlock(&g_log_mx);
        for (unsigned k = 0; k < nl; ++k) send_msg(fd, OVL_STATUS, lines[k].text, (std::uint32_t)std::strlen(lines[k].text));
    }
    g_client_connected.store(0);
    hide_everything();   /* the injector went away: never leave a stale overlay on screen */
}

bool initialize_server() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { log_line("server socket failed"); return false; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(OVL_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 || listen(fd, 2) < 0) {
        log_line("server bind/listen failed port=%u", OVL_PORT);
        close(fd);
        return false;
    }
    g_listen_fd = fd;
    log_line("server listening 127.0.0.1:%u", OVL_PORT);
    return true;
}

void* background_render_thread(void*) {
    mono_thread_attach_(g_domain);
    for (;;) {
        ui_tick();
        usleep(16000);
    }
    return nullptr;
}

[[noreturn]] void run_server_loop() {
    if (g_background_render_mode) {
        pthread_t t;
        pthread_create(&t, nullptr, background_render_thread, nullptr);
    }
    for (;;) {
        int c = accept(g_listen_fd, nullptr, nullptr);
        if (c < 0) { usleep(100000); continue; }
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        serve_client(c);
        close(c);
    }
}


void application_update_hook(MonoObject* instance) {
    ui_tick();

    if (g_application_update_original)
        g_application_update_original(instance);
}
bool install_update_hook(MonoClass* application_class) {
    record_stage("hook_lookup");
    MonoMethod* update = application_class
        ? mono_class_get_method_from_name_(
              application_class,
              "Update",
              0)
        : nullptr;
    record_stage("hook_compile");
    auto* address = update
        ? static_cast<std::uint8_t*>(mono_compile_method_(update))
        : nullptr;
    if (!address) {
        log_line("Application.Update compile failed");
        return false;
    }

    record_stage("hook_read_start");
    std::array<std::uint8_t, kAtomicPatchSize> expected{};
    if (!probe_hook_bytes(address, expected)) {
        return false;
    }
    record_stage("hook_read_ok");

    static constexpr std::uint8_t kAbsoluteJumpPrefix[6] = {
        0xff, 0x25, 0x00, 0x00, 0x00, 0x00,
    };

    const bool eta_chain =
        std::memcmp(
            expected.data(),
            kAbsoluteJumpPrefix,
            sizeof(kAbsoluteJumpPrefix)) == 0;

    record_stage("hook_prepare");
    std::size_t displaced_size = kAbsoluteJumpSize;
    application_update_t original_call = nullptr;

    if (eta_chain) {
        std::uint64_t previous_destination = 0;
        std::memcpy(
            &previous_destination,
            expected.data() + sizeof(kAbsoluteJumpPrefix),
            sizeof(previous_destination));

        if (previous_destination ==
            reinterpret_cast<std::uint64_t>(&application_update_hook)) {
            if (g_application_update_original &&
                g_hook_install_confirmed) {
                log_line(
                    "Application.Update hook already online method=%p",
                    static_cast<void*>(address));
                return true;
            }
            log_line(
                "Application.Update belongs to another resident renderer "
                "method=%p",
                static_cast<void*>(address));
            return false;
        }

        if (previous_destination < 0x10000ULL ||
            previous_destination >= 0x0000800000000000ULL ||
            previous_destination == reinterpret_cast<std::uint64_t>(address)) {
            log_line(
                "Application.Update etaHEN destination invalid method=%p "
                "destination=%p",
                static_cast<void*>(address),
                reinterpret_cast<void*>(previous_destination));
            return false;
        }

        original_call = reinterpret_cast<application_update_t>(
            previous_destination);
    } else {
        displaced_size = native_patch_length(expected.data());
        if (displaced_size == 0) {
            log_line(
                "Application.Update native prologue rejected "
                "method=%p bytes="
                "%02x%02x%02x%02x%02x%02x%02x%02x"
                "%02x%02x%02x%02x%02x%02x%02x%02x",
                static_cast<void*>(address),
                expected[0], expected[1], expected[2], expected[3],
                expected[4], expected[5], expected[6], expected[7],
                expected[8], expected[9], expected[10], expected[11],
                expected[12], expected[13], expected[14], expected[15]);
            return false;
        }

        record_stage("hook_trampoline");
        if (!prepare_trampoline(
                expected.data(),
                displaced_size,
                address + displaced_size)) {
            log_line(
                "Application.Update trampoline prepare failed method=%p "
                "mode=native-stopped-mdbg displaced=%zu",
                static_cast<void*>(address),
                displaced_size);
            return false;
        }
        original_call = reinterpret_cast<application_update_t>(
            &commonfps_update_trampoline);
    }

    std::array<std::uint8_t, kAtomicPatchSize> desired = expected;
    encode_absolute_jump(
        desired.data(),
        reinterpret_cast<const void*>(&application_update_hook));
    for (std::size_t i = kAbsoluteJumpSize;
         i < displaced_size;
         ++i) {
        desired[i] = 0x90;
    }

    g_application_update_original = original_call;

    std::uint64_t nonce = 0;
    record_stage("hook_request");
    if (!publish_hook_request(
            address,
            expected,
            desired,
            displaced_size,
            original_call,
            nonce)) {
        log_line(
            "Application.Update request publish failed method=%p",
            static_cast<void*>(address));
        return false;
    }

    log_line(
        "Application.Update request ready mode=%s method=%p hook=%p "
        "original=%p displaced=%zu nonce=0x%llx",
        eta_chain ? "etahen-stopped-chain" : "native-stopped-mdbg",
        static_cast<void*>(address),
        reinterpret_cast<void*>(&application_update_hook),
        reinterpret_cast<void*>(original_call),
        displaced_size,
        static_cast<unsigned long long>(nonce));

    ShellUiHookAck ack{};
    record_stage("hook_ack_wait");
    if (!wait_for_native_hook_ack(getpid(), nonce, ack)) {
        log_line(
            "Application.Update request timeout method=%p "
            "nonce=0x%llx",
            static_cast<void*>(address),
            static_cast<unsigned long long>(nonce));
        return false;
    }

    if (ack.status != static_cast<std::int32_t>(
            ShellUiHookStatus::Success) ||
        ack.phase != static_cast<std::uint8_t>(ShellUiHookAckPhase::Patch) ||
        ack.verified == 0 || ack.detached == 0 ||
        ack.auth_restored == 0) {
        log_line(
            "Application.Update request failed method=%p "
            "status=%d read_rc=%d write_rc=%d verified=%u "
            "restored=%u detached=%u auth_restored=%u",
            static_cast<void*>(address),
            ack.status,
            ack.read_rc,
            ack.write_rc,
            static_cast<unsigned>(ack.verified),
            static_cast<unsigned>(ack.restored),
            static_cast<unsigned>(ack.detached),
            static_cast<unsigned>(ack.auth_restored));
        return false;
    }

    log_line(
        "Application.Update hook online method=%p "
        "mode=%s original=%p hook=%p "
        "displaced=%zu stopped_patch=1 verified=1",
        static_cast<void*>(address),
        eta_chain ? "etahen-stopped-chain" : "native-stopped-mdbg",
        reinterpret_cast<void*>(original_call),
        reinterpret_cast<void*>(&application_update_hook),
        displaced_size);
    g_hook_install_confirmed = true;
    record_stage("hook_confirmed");
    return true;
}

}

bool initialize_runtime() {
    if (g_runtime_ready.load())
        return true;

    if (g_hook_attempted)
        return false;

    record_stage("mono_root_call");
    g_domain = mono_get_root_domain_();
    if (!g_domain) {
        log_line("root domain failed");
        return false;
    }
    mono_thread_attach_(g_domain);

    record_stage("mono_attached");

    g_pui_image = open_image(kPuiDll);
    MonoImage* app_system = open_image(kAppSystemDll);
    if (!g_pui_image || !app_system) {
        log_line("managed image open failed pui=%p app=%p",
                 static_cast<void*>(g_pui_image),
                 static_cast<void*>(app_system));
        return false;
    }

    record_stage("managed_images_ready");

    MonoClass* layer_manager = mono_class_from_name_(
        app_system,
        "Sce.Vsh.ShellUI.AppSystem",
        "LayerManager");
    g_find_scene_method = layer_manager
        ? mono_class_get_method_from_name_(
              layer_manager,
              "FindContainerSceneByPath",
              1)
        : nullptr;
    if (!g_find_scene_method) {
        log_line("FindContainerSceneByPath unavailable");
        return false;
    }

    if (!refresh_game_scene()) {
        log_line("Game ContainerScene no disponible al iniciar; overlay armado, "
                 "esperando a que se abra un juego");
    }

    record_stage("scene_ready");

    g_hook_attempted = true;
    MonoClass* application_class = mono_class_from_name_(
        g_pui_image,
        "Sce.PlayStation.PUI",
        "Application");
    record_stage("hook_setup");
    if (application_class && install_update_hook(application_class)) {
        g_background_render_mode = false;
        log_line("renderer backend selected mode=application-update-hook");
    } else {
        log_line("Application.Update hook failed or unavailable, falling back to legacy-guard");
        const MainThreadGuardResult guard = install_main_thread_guard();
        if (guard == MainThreadGuardResult::Installed) {
            g_background_render_mode = true;
            log_line(
                "renderer backend selected mode=legacy-background-pui "
                "Application.Update=untouched");
        } else {
            log_line(
                "renderer backend selection failed; fail-closed without "
                "Application.Update write");
            return false;
        }
    }

    bind_engine();
    g_runtime_ready.store(true);
    log_line(
        "runtime ready pid=%d scene_handle=%u gc_mode=pinned backend=%s",
        getpid(),
        g_game_scene_handle,
        g_background_render_mode
            ? "legacy-background-pui"
            : "application-update-hook");
    return true;
}

bool initialize_receiver() {
    return initialize_server();
}

[[noreturn]] void run_receiver_loop() {
    run_server_loop();
}

} // namespace common_fps::ps5::shellui
