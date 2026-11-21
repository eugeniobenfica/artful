/* artful-wgsl-check — each WGSL file named put through a WebGPU implementation's compiler (wgpu-native's naga, or
 * whatever webgpu.h is linked against), as spirv-val is for SPIR-V: artful-wgsl-check a.wgsl b.wgsl ... says which
 * are refused and why, and fails if any is. Built with ARTFUL_WEBGPU, beside the backend. */
#include <webgpu/webgpu.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static WGPUAdapter adapter;
static WGPUDevice device;
static char refusal[16384];
static int refused;

static void gotAdapter(WGPURequestAdapterStatus status, WGPUAdapter a, WGPUStringView message, void* u1, void* u2) {
    (void)u1; (void)u2;
    if (status == WGPURequestAdapterStatus_Success) adapter = a;
    else fprintf(stderr, "no adapter: %.*s\n", (int)message.length, message.data ? message.data : "");
}
static void gotDevice(WGPURequestDeviceStatus status, WGPUDevice d, WGPUStringView message, void* u1, void* u2) {
    (void)u1; (void)u2;
    if (status == WGPURequestDeviceStatus_Success) device = d;
    else fprintf(stderr, "no device: %.*s\n", (int)message.length, message.data ? message.data : "");
}
static void popped(WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView message, void* u1, void* u2) {
    (void)status; (void)u1; (void)u2;
    if (type == WGPUErrorType_NoError) return;
    refused = 1;
    snprintf(refusal, sizeof refusal, "%.*s", (int)message.length, message.data ? message.data : "");
}

static char* readAll(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* text = malloc((size_t)n + 1);
    *size = fread(text, 1, (size_t)n, f);
    text[*size] = 0;
    fclose(f);
    return text;
}

int main(int argc, char** argv) {
    WGPUInstance instance = wgpuCreateInstance(NULL);
    WGPURequestAdapterCallbackInfo adapterInfo = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    adapterInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    adapterInfo.callback = gotAdapter;
    wgpuInstanceRequestAdapter(instance, NULL, adapterInfo);
    wgpuInstanceProcessEvents(instance);
    if (!adapter) return 2;
    WGPURequestDeviceCallbackInfo deviceInfo = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    deviceInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    deviceInfo.callback = gotDevice;
    wgpuAdapterRequestDevice(adapter, NULL, deviceInfo);
    wgpuInstanceProcessEvents(instance);
    if (!device) return 2;
    int bad = 0;
    for (int i = 1; i < argc; ++i) {
        size_t size;
        char* text = readAll(argv[i], &size);
        if (!text) { fprintf(stderr, "%s: cannot read\n", argv[i]); bad = 1; continue; }
        WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
        wgsl.code.data = text;
        wgsl.code.length = size;
        WGPUShaderModuleDescriptor d = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
        d.nextInChain = &wgsl.chain;
        refused = 0;
        wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
        WGPUShaderModule module = wgpuDeviceCreateShaderModule(device, &d);
        WGPUPopErrorScopeCallbackInfo pop = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
        pop.mode = WGPUCallbackMode_AllowSpontaneous;
        pop.callback = popped;
        wgpuDevicePopErrorScope(device, pop);
        wgpuInstanceProcessEvents(instance);
        if (refused) {
            printf("%s: refused\n%s\n\n", argv[i], refusal);
            bad = 1;
        }
        if (module) wgpuShaderModuleRelease(module);
        free(text);
    }
    printf("%d file%s, %s\n", argc - 1, argc == 2 ? "" : "s", bad ? "some refused" : "all taken");
    wgpuDeviceRelease(device);
    wgpuAdapterRelease(adapter);
    wgpuInstanceRelease(instance);
    return bad;
}
