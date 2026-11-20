#include <sys/types.h>  // Required before the official Zygisk API header.
#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <exception>
#include <mutex>

#include "zygisk.hpp"
#include "lsplant.hpp"
#include "dobby.h"
#include "art_elf.hpp"
#include "native-entry-chain.hpp"
#include "embedded_dex.hpp"

namespace {
constexpr const char* kTag = "MD3Runtime";
constexpr const char* kBuild = "native build oem-chain-v1; late-init; preserve OEM ART stubs and continuations";
ArtElf g_art;  // Must outlive every LSPlant resolver callback.
std::atomic<bool> g_ready{false};
std::atomic<bool> g_worker_queued{false};
jobject g_loader = nullptr;
jobject g_buffer = nullptr;
jclass g_hooks = nullptr;

struct NativeRoute {
    void* logical = nullptr;
    void* actual = nullptr;
    void* replacement = nullptr;
    void* backup = nullptr;
    md3::EntryChain chain;
};
std::array<NativeRoute, 32> g_routes;
std::mutex g_routes_lock;

bool IsMappedExecutable(const void* address, std::size_t length) {
    auto start = reinterpret_cast<std::uintptr_t>(address);
    if (!start || length > UINTPTR_MAX - start) return false;
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (!maps) return false;
    char line[1024], permissions[5];
    unsigned long low, high;
    bool result = false;
    while (std::fgets(line, sizeof(line), maps)) {
        if (std::sscanf(line, "%lx-%lx %4s", &low, &high, permissions) == 3 &&
            start >= low && start + length <= high && permissions[0] == 'r' &&
            permissions[2] == 'x') { result = true; break; }
    }
    std::fclose(maps);
    return result;
}

bool ValidateAbsoluteJump(const void* target, const void* replacement) {
    uint32_t code[5];
    std::memcpy(code, target, sizeof(code));
    std::uintptr_t decoded = 0;
    for (unsigned i = 0; i < 4; ++i) {
        auto expected = i ? 0xf2800011U : 0xd2800011U;
        if ((code[i] & 0xff80001fU) != expected || ((code[i] >> 21) & 3) != i) return false;
        decoded |= std::uintptr_t{(code[i] >> 5) & 0xffffU} << (16 * i);
    }
    return code[4] == 0xd61f0220U && decoded == reinterpret_cast<std::uintptr_t>(replacement);
}

bool ClearException(JNIEnv* env, const char* where) {
    if (!env->ExceptionCheck()) return false;
    __android_log_print(ANDROID_LOG_ERROR, kTag, "JNI exception at %s; leaving stock behavior", where);
    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}

bool ReadEntryCode(std::uintptr_t address, md3::EntryCode& code) {
    auto* pointer = reinterpret_cast<void*>(address);
    if (!IsMappedExecutable(pointer, sizeof(code))) return false;
    std::memcpy(code.data(), pointer, sizeof(code));
    return true;
}

bool PreservedEntriesIntact(const md3::EntryChain& chain) {
    for (std::size_t i = 0; i < chain.count; ++i) {
        md3::EntryCode code;
        const auto& entry = chain.preserved[i];
        if (!ReadEntryCode(entry.address, code) || code != entry.code) return false;
    }
    return true;
}

bool DestroyActual(void* actual) {
    auto page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
    auto address = reinterpret_cast<std::uintptr_t>(actual);
    if (!page || page == static_cast<std::uintptr_t>(-1) || address > UINTPTR_MAX - 64 - page) return false;
    auto first = address & ~(page - 1);
    auto end = (address + 64 + page - 1) & ~(page - 1);
    if (mprotect(reinterpret_cast<void*>(first), end - first, PROT_READ | PROT_WRITE | PROT_EXEC)) return false;
    bool result = DobbyDestroy(actual) == 0;
    mprotect(reinterpret_cast<void*>(first), end - first, PROT_READ | PROT_EXEC);
    return result;
}

void* InlineHook(void* target, void* replacement) {
    // Upstream Dobby ignores mprotect errors. Check before entering its patch path,
    // using the ACTUAL device page size (4k or 16k), not a hardcoded 4096.
    if (!target || !replacement) return nullptr;
    if (!g_art.IsExecutable(target, 20) || (reinterpret_cast<std::uintptr_t>(target) & 3) ||
        !IsMappedExecutable(replacement, 4)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "refusing non-executable ART hook %p -> %p", target, replacement);
        return nullptr;
    }
    std::lock_guard lock(g_routes_lock);
    NativeRoute* route = nullptr;
    for (auto& candidate : g_routes) {
        if (candidate.logical == target) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "duplicate logical ART hook refused: %p", target);
            return nullptr;
        }
        if (!candidate.logical && !route) route = &candidate;
    }
    if (!route) return nullptr;

    md3::EntryChain chain;
    if (!md3::ResolveEntryChain(reinterpret_cast<std::uintptr_t>(target),
                               reinterpret_cast<std::uintptr_t>(replacement), ReadEntryCode, chain)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "OEM chain invalid/cyclic/unmapped at ART %p; leaving entry untouched", target);
        return nullptr;
    }
    auto* actual = reinterpret_cast<void*>(chain.actual);
    for (const auto& candidate : g_routes) {
        if (candidate.logical && candidate.actual == actual) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "duplicate downstream hook refused: ART %p actual=%p", target, actual);
            return nullptr;
        }
        // Do not follow our own installed veneers into LSPlant callbacks.
        for (std::size_t i = 0; candidate.logical && i < chain.count; ++i) {
            if (chain.preserved[i].address == reinterpret_cast<std::uintptr_t>(candidate.actual)) {
                __android_log_print(ANDROID_LOG_ERROR, kTag, "OEM chain intersects an existing MD3 route; refusing ART %p", target);
                return nullptr;
            }
        }
    }
    for (std::size_t i = 0; i < chain.count; ++i) {
        const auto& node = chain.preserved[i];
        auto destination = i + 1 < chain.count ? chain.preserved[i + 1].address : chain.actual;
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "OEM chain hop ART+%zx [%zu] %p -> %p; preserving entry/continuation bytes",
            static_cast<std::size_t>(g_art.Offset(target)), i,
            reinterpret_cast<void*>(node.address), reinterpret_cast<void*>(destination));
    }
    const auto& before = chain.actual_code;
    __android_log_print(ANDROID_LOG_INFO, kTag,
        "native hook ART+%zx logical=%p actual=%p -> %p hops=%zu prologue=%08x/%08x/%08x/%08x/%08x",
        static_cast<std::size_t>(g_art.Offset(target)), target, actual, replacement, chain.count,
        before[0], before[1], before[2], before[3], before[4]);
    auto page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
    if (!page || page == static_cast<std::uintptr_t>(-1)) return nullptr;
    if (chain.actual > UINTPTR_MAX - 64 - page) return nullptr;
    auto first = chain.actual & ~(page - 1);
    auto end = (chain.actual + 64 + page - 1) & ~(page - 1);
    if (mprotect(reinterpret_cast<void*>(first), end - first, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "native actual hook mprotect refused: %p", actual);
        return nullptr;
    }
    void* original = nullptr;
    const int result = DobbyHook(actual, reinterpret_cast<dobby_dummy_func_t>(replacement),
                                 reinterpret_cast<dobby_dummy_func_t*>(&original));
    mprotect(reinterpret_cast<void*>(first), end - first, PROT_READ | PROT_EXEC);
    if (result != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "DobbyHook failed: logical=%p actual=%p (%d)", target, actual, result);
        return nullptr;
    }
    if (!original || !IsMappedExecutable(original, 20) || !ValidateAbsoluteJump(actual, replacement) ||
        !PreservedEntriesIntact(chain)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "native routing validation FAILED logical=%p actual=%p -> %p backup=%p; undoing actual only", target, actual, replacement, original);
        DestroyActual(actual);
        return nullptr;
    }
    *route = {target, actual, replacement, original, chain};
    __android_log_print(ANDROID_LOG_INFO, kTag,
        "native routing validated ART+%zx actual=%p -> %p backup=%p (absolute64; OEM preserved=%zu)",
        static_cast<std::size_t>(g_art.Offset(target)), actual, replacement, original, chain.count);
    return original;
}

bool InlineUnhook(void* target) {
    std::lock_guard lock(g_routes_lock);
    for (auto& route : g_routes) {
        if (route.logical != target) continue;
        if (!DestroyActual(route.actual)) return false;
        bool intact = PreservedEntriesIntact(route.chain);
        __android_log_print(ANDROID_LOG_INFO, kTag, "native unhook logical=%p actual=%p; OEM entries intact=%d", target, route.actual, intact);
        route = {};
        return true;
    }
    return false;  // Never destroy an OEM-owned logical ART entry.
}

bool ValidateNativeRoutes() {
    std::lock_guard lock(g_routes_lock);
    for (const auto& route : g_routes) {
        if (route.logical && (!PreservedEntriesIntact(route.chain) ||
                              !ValidateAbsoluteJump(route.actual, route.replacement))) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "native route changed after init logical=%p actual=%p", route.logical, route.actual);
            return false;
        }
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "all native routes validated; OEM ART entries and +12 continuations untouched");
    return true;
}

void AllowHiddenApis(JNIEnv* env) {
    jclass vm = env->FindClass("dalvik/system/VMRuntime");
    if (ClearException(env, "VMRuntime") || !vm) return;
    jmethodID get = env->GetStaticMethodID(vm, "getRuntime", "()Ldalvik/system/VMRuntime;");
    if (ClearException(env, "VMRuntime.getRuntime") || !get) return;
    jmethodID set = env->GetMethodID(vm, "setHiddenApiExemptions", "([Ljava/lang/String;)V");
    if (ClearException(env, "VMRuntime.setHiddenApiExemptions") || !set) return;
    jobject runtime = env->CallStaticObjectMethod(vm, get);
    if (ClearException(env, "getRuntime()") || !runtime) return;
    jclass string = env->FindClass("java/lang/String");
    if (ClearException(env, "String") || !string) return;
    jstring prefix = env->NewStringUTF("");
    if (ClearException(env, "hidden-api prefix") || !prefix) return;
    jobjectArray exemptions = env->NewObjectArray(1, string, prefix);
    if (ClearException(env, "hidden-api exemptions") || !exemptions) return;
    env->CallVoidMethod(runtime, set, exemptions);
    if (!ClearException(env, "setHiddenApiExemptions()")) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "SystemUI hidden API exemptions enabled");
    }
}

jobject Hook(JNIEnv* env, jclass, jobject target, jobject hooker, jobject callback) {
    if (!g_ready.load(std::memory_order_acquire) || !target || !hooker || !callback) return nullptr;
    try {
        jobject backup = lsplant::Hook(env, target, hooker, callback);
        if (ClearException(env, "lsplant::Hook")) return nullptr;
        if (!backup) __android_log_print(ANDROID_LOG_WARN, kTag, "Java method hook rejected");
        return backup;
    } catch (const std::exception& ex) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "native hook failure: %s", ex.what());
    } catch (...) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "unknown native hook failure");
    }
    ClearException(env, "native Hook catch");
    return nullptr;
}

jboolean Deopt(JNIEnv* env, jclass, jobject method) {
    if (!g_ready.load(std::memory_order_acquire) || !method) return JNI_FALSE;
    try {
        bool result = lsplant::Deoptimize(env, method);
        if (ClearException(env, "lsplant::Deoptimize")) return JNI_FALSE;
        return result ? JNI_TRUE : JNI_FALSE;
    } catch (const std::exception& ex) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "deoptimization failure: %s", ex.what());
    } catch (...) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "unknown deoptimization failure");
    }
    ClearException(env, "native Deopt catch");
    return JNI_FALSE;
}

void TrustHelperDex(JNIEnv* env, jobject loader) {
    // Independent fallback if OEM VMRuntime exemptions are unavailable. This
    // only marks the embedded helper's DexFile trusted, never modifies APK data.
    jclass base = env->FindClass("dalvik/system/BaseDexClassLoader");
    if (ClearException(env, "BaseDexClassLoader") || !base) return;
    jfieldID path_field = env->GetFieldID(base, "pathList", "Ldalvik/system/DexPathList;");
    if (ClearException(env, "BaseDexClassLoader.pathList") || !path_field) return;
    jobject path = env->GetObjectField(loader, path_field);
    if (ClearException(env, "helper pathList") || !path) return;
    jclass path_class = env->FindClass("dalvik/system/DexPathList");
    if (ClearException(env, "DexPathList") || !path_class) return;
    jfieldID elements_field = env->GetFieldID(path_class, "dexElements", "[Ldalvik/system/DexPathList$Element;");
    if (ClearException(env, "DexPathList.dexElements") || !elements_field) return;
    auto elements = static_cast<jobjectArray>(env->GetObjectField(path, elements_field));
    if (ClearException(env, "helper dexElements") || !elements) return;
    jclass element_class = env->FindClass("dalvik/system/DexPathList$Element");
    if (ClearException(env, "DexPathList.Element") || !element_class) return;
    jfieldID dex_field = env->GetFieldID(element_class, "dexFile", "Ldalvik/system/DexFile;");
    if (ClearException(env, "Element.dexFile") || !dex_field) return;
    jclass dex_class = env->FindClass("dalvik/system/DexFile");
    if (ClearException(env, "DexFile") || !dex_class) return;
    jfieldID cookie_field = env->GetFieldID(dex_class, "mCookie", "Ljava/lang/Object;");
    if (ClearException(env, "DexFile.mCookie") || !cookie_field) return;
    jsize count = env->GetArrayLength(elements);
    if (ClearException(env, "helper elements count")) return;
    for (jsize i = 0; i < count; ++i) {
        jobject element = env->GetObjectArrayElement(elements, i);
        if (ClearException(env, "helper element") || !element) continue;
        jobject dex = env->GetObjectField(element, dex_field);
        if (ClearException(env, "helper DexFile") || !dex) {
            env->DeleteLocalRef(element);
            continue;
        }
        jobject cookie = env->GetObjectField(dex, cookie_field);
        if (!ClearException(env, "helper dex cookie") && cookie) {
            bool trusted = lsplant::MakeDexFileTrusted(env, cookie);
            if (!ClearException(env, "MakeDexFileTrusted") && trusted) {
                __android_log_print(ANDROID_LOG_INFO, kTag, "embedded helper dex marked trusted");
            }
            env->DeleteLocalRef(cookie);
        }
        env->DeleteLocalRef(dex);
        env->DeleteLocalRef(element);
    }
}

bool LoadHelper(JNIEnv* env) {
    jclass base = env->FindClass("java/lang/ClassLoader");
    if (ClearException(env, "ClassLoader") || !base) return false;
    jmethodID system = env->GetStaticMethodID(base, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
    if (ClearException(env, "getSystemClassLoader method") || !system) return false;
    jobject parent = env->CallStaticObjectMethod(base, system);
    if (ClearException(env, "getSystemClassLoader()") || !parent) return false;
    jclass memory = env->FindClass("dalvik/system/InMemoryDexClassLoader");
    if (ClearException(env, "InMemoryDexClassLoader") || !memory) return false;
    jmethodID init = env->GetMethodID(memory, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    if (ClearException(env, "InMemoryDexClassLoader constructor") || !init) return false;
    jobject buffer = env->NewDirectByteBuffer(const_cast<unsigned char*>(md3::kDex), md3::kDexSize);
    if (ClearException(env, "embedded dex buffer") || !buffer) return false;
    jobject loader = env->NewObject(memory, init, buffer, parent);
    if (ClearException(env, "InMemoryDexClassLoader()") || !loader) return false;
    TrustHelperDex(env, loader);
    jmethodID load = env->GetMethodID(base, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    if (ClearException(env, "ClassLoader.loadClass method") || !load) return false;
    jstring name = env->NewStringUTF("md3.Hooks");
    if (ClearException(env, "helper class name") || !name) return false;
    auto hooks = static_cast<jclass>(env->CallObjectMethod(loader, load, name));
    if (ClearException(env, "md3.Hooks load") || !hooks) return false;
    // Late mode must never fall back to boot(): Application.attach has already
    // happened, and patching it here would reintroduce the early startup path.
    jmethodID boot = env->GetStaticMethodID(hooks, "bootLate", "()V");
    if (ClearException(env, "md3.Hooks.bootLate method") || !boot) return false;
    JNINativeMethod methods[] = {{
        const_cast<char*>("hook"),
        const_cast<char*>("(Ljava/lang/reflect/Member;Ljava/lang/Object;Ljava/lang/reflect/Method;)Ljava/lang/reflect/Method;"),
        reinterpret_cast<void*>(Hook),
    }, {
        const_cast<char*>("deopt"),
        const_cast<char*>("(Ljava/lang/reflect/Member;)Z"),
        reinterpret_cast<void*>(Deopt),
    }};
    int registered = env->RegisterNatives(hooks, methods, 2);
    if (ClearException(env, "md3.Hooks native registration") || registered != JNI_OK) return false;
    g_loader = env->NewGlobalRef(loader);
    if (ClearException(env, "loader global ref") || !g_loader) return false;
    g_buffer = env->NewGlobalRef(buffer);
    if (ClearException(env, "buffer global ref") || !g_buffer) return false;
    g_hooks = static_cast<jclass>(env->NewGlobalRef(hooks));
    if (ClearException(env, "Hooks global ref") || !g_hooks) return false;
    __android_log_print(ANDROID_LOG_INFO, kTag, "calling md3.Hooks.bootLate() on attached worker; setup posts to main");
    env->CallStaticVoidMethod(hooks, boot);
    if (ClearException(env, "md3.Hooks.bootLate()")) return false;
    __android_log_print(ANDROID_LOG_INFO, kTag, "helper bootLate completed (%zu-byte embedded dex); Java setup queued", md3::kDexSize);
    return true;
}

void StartLate(JNIEnv* env) {
    if (env->PushLocalFrame(64) != JNI_OK) {
        ClearException(env, "PushLocalFrame");
        return;
    }
    try {
        __android_log_print(ANDROID_LOG_INFO, kTag, "late LSPlant init beginning AFTER currentApplication and OEM settle");
        if (g_art.Open()) {
            lsplant::InitInfo info{
                .inline_hooker = InlineHook,
                .inline_unhooker = InlineUnhook,
                .art_symbol_resolver = [](std::string_view name) { return g_art.Find(name); },
                .art_symbol_prefix_resolver = [](std::string_view name) { return g_art.Find(name, true); },
                .generated_class_name = "MD3Hooker_",
                .generated_source_name = "MD3Runtime",
                .generated_field_name = "hooker",
                .generated_method_name = "{target}",
            };
            bool ready = lsplant::Init(env, info);
            if (ClearException(env, "lsplant::Init")) ready = false;
            if (ready) ready = ValidateNativeRoutes();
            // bootLate posts Java callbacks to another thread; publish LSPlant's
            // completed initialization before it can call Hook()/Deopt().
            g_ready.store(ready, std::memory_order_release);
            if (ready) {
                if (!LoadHelper(env)) __android_log_print(ANDROID_LOG_ERROR, kTag, "late helper startup failed; no early-boot fallback");
            }
            else __android_log_print(ANDROID_LOG_ERROR, kTag, "LSPlant init failed; stock SystemUI retained");
        }
    } catch (const std::exception& ex) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "initialization failure: %s", ex.what());
    } catch (...) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "unknown initialization failure");
    }
    ClearException(env, "StartLate exit");
    env->PopLocalFrame(nullptr);
}

bool SleepMilliseconds(long milliseconds) {
    timespec remaining{milliseconds / 1000, (milliseconds % 1000) * 1000000L};
    while (nanosleep(&remaining, &remaining) != 0) {
        if (errno != EINTR) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "late worker sleep failed: %d; leaving stock startup", errno);
            return false;
        }
    }
    return true;
}

bool WaitForApplication(JNIEnv* env) {
    jclass activity_thread = env->FindClass("android/app/ActivityThread");
    if (ClearException(env, "late ActivityThread") || !activity_thread) return false;
    jmethodID current_application = env->GetStaticMethodID(
        activity_thread, "currentApplication", "()Landroid/app/Application;");
    if (ClearException(env, "ActivityThread.currentApplication method") || !current_application) return false;

    using Clock = std::chrono::steady_clock;
    auto begin = Clock::now();
    auto deadline = begin + std::chrono::seconds(60);
    __android_log_print(ANDROID_LOG_INFO, kTag, "late worker waiting for currentApplication (timeout=60s; ART untouched)");
    while (Clock::now() < deadline) {
        jobject application = env->CallStaticObjectMethod(activity_thread, current_application);
        if (ClearException(env, "ActivityThread.currentApplication()")) return false;
        if (application) {
            env->DeleteLocalRef(application);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin).count();
            __android_log_print(ANDROID_LOG_INFO, kTag,
                "currentApplication ready after %lldms; settling OEM startup for 1000ms before ART hooks",
                static_cast<long long>(elapsed));
            return SleepMilliseconds(1000);
        }
        if (!SleepMilliseconds(100)) return false;
    }
    __android_log_print(ANDROID_LOG_WARN, kTag, "currentApplication wait timed out; no LSPlant/helper init performed");
    return false;
}

void* LateWorker(void* argument) {
    // Do not attach while Zygote/Nyako is still executing its post-fork path.
    // The Application gate below, not this fixed delay, establishes late mode.
    if (!SleepMilliseconds(2000)) return nullptr;
    auto* vm = static_cast<JavaVM*>(argument);
    JNIEnv* env = nullptr;
    JavaVMAttachArgs attach{JNI_VERSION_1_6, const_cast<char*>("MD3LateInit"), nullptr};
    int result = vm->AttachCurrentThreadAsDaemon(&env, &attach);
    if (result != JNI_OK || !env) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "late worker JVM attach failed: %d; leaving stock startup", result);
        return nullptr;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "late worker JVM attached after 2000ms; no shared JNIEnv or Zygisk API");
    if (env->PushLocalFrame(64) == JNI_OK) {
        try {
            AllowHiddenApis(env);  // SystemUI only, no ART entry patching.
            if (WaitForApplication(env)) StartLate(env);
        } catch (const std::exception& ex) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "late worker failure: %s", ex.what());
        } catch (...) {
            __android_log_print(ANDROID_LOG_ERROR, kTag, "unknown late worker failure");
        }
        ClearException(env, "LateWorker exit");
        env->PopLocalFrame(nullptr);
    } else {
        ClearException(env, "late worker PushLocalFrame");
    }
    result = vm->DetachCurrentThread();
    if (result != JNI_OK) __android_log_print(ANDROID_LOG_ERROR, kTag, "late worker JVM detach failed: %d", result);
    __android_log_print(ANDROID_LOG_INFO, kTag, "late worker finished; resident bridge retained for callbacks");
    return nullptr;
}

void QueueLateWorker(JavaVM* vm) {
    if (!vm) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "JavaVM unavailable; no early initialization fallback");
        return;
    }
    bool expected = false;
    if (!g_worker_queued.compare_exchange_strong(expected, true)) return;
    pthread_attr_t attributes;
    int result = pthread_attr_init(&attributes);
    if (result == 0) {
        result = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        if (result == 0) {
            pthread_t thread;
            result = pthread_create(&thread, &attributes, LateWorker, vm);
        }
        pthread_attr_destroy(&attributes);
    }
    if (result != 0) {
        g_worker_queued.store(false);
        __android_log_print(ANDROID_LOG_ERROR, kTag, "late worker create failed: %d; no early initialization fallback", result);
        return;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
        "late worker queued; returning specialization WITHOUT LSPlant/helper init (delay=2000ms; app gate; settle=1000ms)");
}

class Module final : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api* api, JNIEnv* env) override {
        api_ = api;
        env_ = env;
        if (env_->GetJavaVM(&vm_) != JNI_OK) vm_ = nullptr;
    }
    void preAppSpecialize(zygisk::AppSpecializeArgs* args) override {
        selected_ = false;
        if (args && args->nice_name) {
            const char* process = env_->GetStringUTFChars(args->nice_name, nullptr);
            if (!ClearException(env_, "process name") && process) {
                // Exact match excludes com.android.systemui:* auxiliary processes.
                selected_ = std::strcmp(process, "com.android.systemui") == 0;
                env_->ReleaseStringUTFChars(args->nice_name, process);
                ClearException(env_, "ReleaseStringUTFChars");
            }
        }
        if (!selected_) api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }
    void postAppSpecialize(const zygisk::AppSpecializeArgs*) override {
        if (!selected_) return;
        __android_log_print(ANDROID_LOG_INFO, kTag, "%s", kBuild);
        QueueLateWorker(vm_);
        // Never DLCLOSE selected processes: the queued worker and later ART/Java
        // callbacks all execute resident module code after Zygisk itself unloads.
    }
    void preServerSpecialize(zygisk::ServerSpecializeArgs*) override {
        api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }
private:
    zygisk::Api* api_ = nullptr;
    JNIEnv* env_ = nullptr;
    JavaVM* vm_ = nullptr;
    bool selected_ = false;
};
}  // namespace

REGISTER_ZYGISK_MODULE(Module)
