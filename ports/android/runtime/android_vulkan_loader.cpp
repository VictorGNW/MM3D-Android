#include <adrenotools/driver.h>
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <cstdlib>
#include <mutex>
#include <string>

namespace {
std::mutex loaderMutex;
void* loaderHandle = nullptr;

template <typename Function>
Function VulkanEntry(const char* name) {
    std::lock_guard lock(loaderMutex);
    if (!loaderHandle) loaderHandle = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    auto entry = loaderHandle ? reinterpret_cast<Function>(dlsym(loaderHandle, name)) : nullptr;
    if (!entry) {
        __android_log_print(ANDROID_LOG_FATAL, "TriAevum", "Missing Vulkan loader entry: %s", name);
        std::abort();
    }
    return entry;
}

bool CheckTurnip(void* handle) {
    auto proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "vkGetInstanceProcAddr"));
    if (!proc) return false;
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(proc(nullptr, "vkCreateInstance"));
    if (!create) return false;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    VkInstance instance{};
    if (create(&info, nullptr, &instance) != VK_SUCCESS) return false;
    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        proc(instance, "vkEnumeratePhysicalDevices"));
    const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        proc(instance, "vkGetPhysicalDeviceProperties2"));
    const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(proc(instance, "vkDestroyInstance"));
    uint32_t count = 1;
    VkPhysicalDevice gpu{};
    const auto result = enumerate(instance, &count, &gpu);
    bool supported = false;
    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && count != 0) {
        VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 device{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
        properties(gpu, &device);
        supported = driver.driverID == VK_DRIVER_ID_MESA_TURNIP &&
                    device.properties.apiVersion >= VK_API_VERSION_1_2;
        __android_log_print(ANDROID_LOG_INFO, "TriAevum", "Turnip requested gpu=%s api=%u.%u.%u driver=%s",
            device.properties.deviceName, VK_VERSION_MAJOR(device.properties.apiVersion),
            VK_VERSION_MINOR(device.properties.apiVersion), VK_VERSION_PATCH(device.properties.apiVersion),
            driver.driverName);
    }
    destroy(instance, nullptr);
    return supported;
}
}

#include "android_vulkan_dispatch.inc"

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance instance, const char* name) {
    return __wrap_vkGetInstanceProcAddr(instance, name);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_triaevum_android_NativeHostBridge_selectTurnip(JNIEnv* env, jobject, jstring nativeDir) {
    const char* path = env->GetStringUTFChars(nativeDir, nullptr);
    if (!path) return JNI_FALSE;
    const std::string directory = std::string(path) + "/";
    env->ReleaseStringUTFChars(nativeDir, path);
    std::lock_guard lock(loaderMutex);
    if (loaderHandle) return JNI_FALSE;
    void* handle = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr,
        directory.c_str(), directory.c_str(), "libvulkan_freedreno.so", nullptr, nullptr);
    if (!handle) return JNI_FALSE;
    if (!CheckTurnip(handle)) {
        dlclose(handle);
        return JNI_FALSE;
    }
    loaderHandle = handle;
    return JNI_TRUE;
}
