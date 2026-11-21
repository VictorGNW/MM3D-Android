#include "android_host.h"

#include <SDL.h>
#include <android/log.h>
#include <vulkan/vulkan.h>
#include <cstring>
#include <vector>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <jni.h>
#include <mutex>
#include <stdexcept>

#include "fast/backends/startup_checkpoints.h"
#include "ship/Context.h"

namespace {

std::mutex installerPageMutex;
std::string installerPage;
std::atomic<bool> pauseRequested{false};

const char* AndroidGpuFamily(const VkPhysicalDeviceProperties& gpu) {
    if (gpu.vendorID == 0x5143 || std::strstr(gpu.deviceName, "Adreno")) return "Adreno";
    if (gpu.vendorID == 0x13b5 || std::strstr(gpu.deviceName, "Mali")) return "Mali";
    if (std::strstr(gpu.deviceName, "Xclipse")) return "Xclipse";
    return "Unknown";
}

// The probe uses the system loader. An integrated driver must supply both
// this probe and the game renderer with the same Vulkan dispatch.
jint ProbeSystemVulkan() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    const VkResult created = vkCreateInstance(&info, nullptr, &instance);
    if (created == VK_ERROR_INCOMPATIBLE_DRIVER) return 0;
    if (created != VK_SUCCESS) return -1;
    struct InstanceGuard {
        VkInstance instance;
        ~InstanceGuard() { vkDestroyInstance(instance, nullptr); }
    } guard{instance};

    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS) return -1;
    if (count == 0) return 0;
    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) != VK_SUCCESS) return -1;

    jint mode = 0;
    for (uint32_t index = 0; index < count; ++index) {
        VkPhysicalDeviceProperties gpu{};
        vkGetPhysicalDeviceProperties(devices[index], &gpu);
        __android_log_print(ANDROID_LOG_INFO, "TriAevum",
            "Vulkan probe driver=system family=%s gpu=%s api=%u.%u.%u",
            AndroidGpuFamily(gpu), gpu.deviceName, VK_VERSION_MAJOR(gpu.apiVersion),
            VK_VERSION_MINOR(gpu.apiVersion), VK_VERSION_PATCH(gpu.apiVersion));
        if (gpu.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        if (gpu.apiVersion >= VK_API_VERSION_1_2) mode = 2;
        else if (gpu.apiVersion >= VK_API_VERSION_1_1 && mode == 0) mode = 1;
    }
    return mode;
}

// org.triaevum.android.diagnostics.StartupDiagnostics.nativeStage(String):
// resolved once on SDL's main thread (a Java thread, so the app's class
// loader finds the class), then called from any thread SDL attaches.
jclass diagnosticsClass = nullptr;
jmethodID nativeStageMethod = nullptr;

void ReportStartupStage(const char* stage) noexcept {
    if (diagnosticsClass == nullptr || nativeStageMethod == nullptr) return;
    auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (env == nullptr) return;
    if (jstring text = env->NewStringUTF(stage)) {
        env->CallStaticVoidMethod(diagnosticsClass, nativeStageMethod, text);
        env->DeleteLocalRef(text);
    }
    // Diagnostics never stop the game.
    if (env->ExceptionCheck()) env->ExceptionClear();
}

void InstallStartupCheckpoints() {
    auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (env == nullptr) return;
    jclass local = env->FindClass("org/triaevum/android/diagnostics/StartupDiagnostics");
    if (local != nullptr) {
        nativeStageMethod = env->GetStaticMethodID(local, "nativeStage", "(Ljava/lang/String;)V");
        if (nativeStageMethod != nullptr)
            diagnosticsClass = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (diagnosticsClass != nullptr) Fast::SetStartupCheckpointSink(ReportStartupStage);
}

} // namespace

bool RequestAndroidInstallerPage(std::string_view page, std::string* error) {
    if (page != "saves" && page != "diagnostics") {
        if (error != nullptr) *error = "unknown installer page";
        return false;
    }
    std::lock_guard lock(installerPageMutex);
    installerPage = page;
    std::fprintf(stderr, "TRIAEVUM_INSTALLER_PAGE %s requested; quitting the game\n", installerPage.c_str());
    return true;
}

extern "C" {

JNIEXPORT jint JNICALL Java_org_triaevum_android_NativeHostBridge_probeSystemVulkan(JNIEnv*, jobject) {
    try {
        return ProbeSystemVulkan();
    } catch (const std::exception& error) {
        __android_log_print(ANDROID_LOG_ERROR, "TriAevum", "Vulkan probe: %s", error.what());
        return -1;
    }
}


// org.triaevum.android.NativeHostBridge.takeInstallerPage(): the page the
// menu asked for, once, or null. Called by the activity as the game ends.
JNIEXPORT jstring JNICALL Java_org_triaevum_android_NativeHostBridge_takeInstallerPage(JNIEnv* env, jobject) {
    std::string page;
    {
        std::lock_guard lock(installerPageMutex);
        page.swap(installerPage);
    }
    return page.empty() ? nullptr : env->NewStringUTF(page.c_str());
}

// org.triaevum.android.NativeHostBridge.requestPause(): the Activity is
// leaving the foreground (UI thread).
JNIEXPORT void JNICALL Java_org_triaevum_android_NativeHostBridge_requestPause(JNIEnv*, jobject) {
    pauseRequested.store(true, std::memory_order_release);
}

} // extern "C"

bool TakeAndroidPauseRequest() noexcept {
    return pauseRequested.exchange(false, std::memory_order_acq_rel);
}

void InitializeAndroidGameHost() {
    InstallStartupCheckpoints();
    Fast::StartupCheckpoint("native_host_started");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    const char* root = SDL_AndroidGetExternalStoragePath();
    if (!root || !*root) {
        throw std::runtime_error("Android application data directory is unavailable");
    }
    std::filesystem::current_path(root);
    // The managed data tree's logs (UX plan WP-05C): D/logs, D = <app
    // storage>/data, established here before any argument is parsed. Android
    // does not preserve a console stream; stdout and stderr are kept there
    // too. A launch profile's --log-directory may still route the runtime log.
    const auto logs = std::filesystem::path(root) / "data" / "logs";
    std::filesystem::create_directories(logs);
    Ship::Context::SetLogDirectory(logs.string());
    if (!std::freopen((logs / "native-stdout.log").c_str(), "w", stdout) ||
        !std::freopen((logs / "native-stderr.log").c_str(), "w", stderr)) {
        throw std::runtime_error("Cannot open Android runtime logs");
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    SDL_Log("TriAevum game data: %s", root);
}
