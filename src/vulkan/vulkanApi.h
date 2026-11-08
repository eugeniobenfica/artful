/* artful — the part of Vulkan this library calls, declared here so that building it needs no SDK.
 * Names lose their Vk prefix, except the handles. */
#ifndef ARTFUL_VULKAN_API_H
#define ARTFUL_VULKAN_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define VKAPI __stdcall
#else
#define VKAPI
#endif

#ifndef VULKAN_CORE_H_
typedef void* VkInstance;
typedef void* VkPhysicalDevice;
typedef void* VkDevice;
typedef void* VkQueue;
typedef void* VkCommandBuffer;
typedef uint64_t VkSurfaceKHR, VkSwapchainKHR, VkImage, VkImageView, VkBuffer, VkDeviceMemory, VkSemaphore, VkFence, VkCommandPool, VkRenderPass, VkFramebuffer,
    VkShaderModule, VkPipeline, VkPipelineLayout, VkPipelineCache, VkDescriptorSetLayout, VkDescriptorPool, VkDescriptorSet, VkSampler;
typedef uint32_t VkFlags, VkBool32;
typedef uint64_t VkDeviceSize;
typedef int32_t VkResult;
#endif

enum {
    VK_OK = 0, VK_TIMEOUT_ = 2, VK_SUBOPTIMAL = 1000001003, VK_ERR_DEVICE_LOST = -4, VK_ERR_OUT_OF_DATE = -1000001004,
    ST_APPLICATION_INFO = 0, ST_INSTANCE_CREATE_INFO = 1, ST_DEVICE_QUEUE_CREATE_INFO = 2, ST_DEVICE_CREATE_INFO = 3, ST_SUBMIT_INFO = 4, ST_MEMORY_ALLOCATE_INFO = 5,
    ST_FENCE_CREATE_INFO = 8, ST_SEMAPHORE_CREATE_INFO = 9, ST_BUFFER_CREATE_INFO = 12, ST_IMAGE_CREATE_INFO = 14, ST_IMAGE_VIEW_CREATE_INFO = 15, ST_SHADER_MODULE_CREATE_INFO = 16,
    ST_PIPELINE_SHADER_STAGE = 18, ST_PIPELINE_VERTEX_INPUT = 19, ST_PIPELINE_INPUT_ASSEMBLY = 20, ST_PIPELINE_VIEWPORT = 22, ST_PIPELINE_RASTERIZATION = 23, ST_PIPELINE_MULTISAMPLE = 24,
    ST_PIPELINE_DEPTH_STENCIL = 25, ST_PIPELINE_COLOR_BLEND = 26, ST_PIPELINE_DYNAMIC = 27, ST_GRAPHICS_PIPELINE = 28, ST_COMPUTE_PIPELINE = 29, ST_PIPELINE_LAYOUT = 30, ST_SAMPLER = 31,
    ST_DESCRIPTOR_SET_LAYOUT = 32, ST_DESCRIPTOR_POOL = 33, ST_DESCRIPTOR_SET_ALLOCATE = 34, ST_WRITE_DESCRIPTOR_SET = 35, ST_FRAMEBUFFER = 37, ST_RENDER_PASS = 38, ST_COMMAND_POOL = 39,
    ST_COMMAND_BUFFER_ALLOCATE = 40, ST_COMMAND_BUFFER_BEGIN = 42, ST_RENDER_PASS_BEGIN = 43, ST_BUFFER_MEMORY_BARRIER = 44, ST_IMAGE_MEMORY_BARRIER = 45, ST_MEMORY_BARRIER = 46,
    ST_SWAPCHAIN_CREATE_INFO = 1000001000, ST_PRESENT_INFO = 1000001001, ST_XLIB_SURFACE = 1000004000, ST_WAYLAND_SURFACE = 1000006000, ST_ANDROID_SURFACE = 1000008000, ST_WIN32_SURFACE = 1000009000,
    ST_METAL_SURFACE = 1000217000, ST_PHYSICAL_DEVICE_FEATURES2 = 1000059000, ST_VULKAN12_FEATURES = 51, ST_BINDING_FLAGS = 1000161000
};

typedef struct ApplicationInfo { int sType; const void* pNext; const char* appName; uint32_t appVersion; const char* engineName; uint32_t engineVersion; uint32_t apiVersion; } ApplicationInfo;
typedef struct InstanceCreateInfo { int sType; const void* pNext; VkFlags flags; const ApplicationInfo* app; uint32_t layerCount; const char* const* layers; uint32_t extensionCount; const char* const* extensions; } InstanceCreateInfo;
typedef struct DeviceQueueCreateInfo { int sType; const void* pNext; VkFlags flags; uint32_t family; uint32_t count; const float* priorities; } DeviceQueueCreateInfo;
typedef struct DeviceCreateInfo { int sType; const void* pNext; VkFlags flags; uint32_t queueCount; const DeviceQueueCreateInfo* queues; uint32_t layerCount; const char* const* layers; uint32_t extensionCount; const char* const* extensions; const void* features; } DeviceCreateInfo;

/* The feature bits, in their order (55 of them). */
typedef struct PhysicalDeviceFeatures {
    VkBool32 robustBufferAccess, fullDrawIndexUint32, imageCubeArray, independentBlend, geometryShader, tessellationShader, sampleRateShading, dualSrcBlend, logicOp, multiDrawIndirect,
        drawIndirectFirstInstance, depthClamp, depthBiasClamp, fillModeNonSolid, depthBounds, wideLines, largePoints, alphaToOne, multiViewport, samplerAnisotropy,
        textureCompressionETC2, textureCompressionASTC_LDR, textureCompressionBC, occlusionQueryPrecise, pipelineStatisticsQuery, vertexPipelineStoresAndAtomics, fragmentStoresAndAtomics, rest[28];
} PhysicalDeviceFeatures;
typedef struct PhysicalDeviceFeatures2 { int sType; void* pNext; PhysicalDeviceFeatures features; } PhysicalDeviceFeatures2;
/* Vulkan 1.2's features, in their order (47 of them). */
typedef struct Vulkan12Features {
    int sType; void* pNext;
    VkBool32 samplerMirrorClampToEdge, drawIndirectCount, storageBuffer8BitAccess, uniformAndStorageBuffer8BitAccess, storagePushConstant8, shaderBufferInt64Atomics, shaderSharedInt64Atomics,
        shaderFloat16, shaderInt8, descriptorIndexing, shaderInputAttachmentArrayDynamicIndexing, shaderUniformTexelBufferArrayDynamicIndexing, shaderStorageTexelBufferArrayDynamicIndexing,
        shaderUniformBufferArrayNonUniformIndexing, shaderSampledImageArrayNonUniformIndexing, shaderStorageBufferArrayNonUniformIndexing, shaderStorageImageArrayNonUniformIndexing,
        shaderInputAttachmentArrayNonUniformIndexing, shaderUniformTexelBufferArrayNonUniformIndexing, shaderStorageTexelBufferArrayNonUniformIndexing,
        descriptorBindingUniformBufferUpdateAfterBind, descriptorBindingSampledImageUpdateAfterBind, descriptorBindingStorageImageUpdateAfterBind, descriptorBindingStorageBufferUpdateAfterBind,
        descriptorBindingUniformTexelBufferUpdateAfterBind, descriptorBindingStorageTexelBufferUpdateAfterBind, descriptorBindingUpdateUnusedWhilePending, descriptorBindingPartiallyBound,
        descriptorBindingVariableDescriptorCount, runtimeDescriptorArray, rest[17];
} Vulkan12Features;

typedef struct PhysicalDeviceProperties {
    uint32_t apiVersion, driverVersion, vendorID, deviceID;
    int deviceType;
    char deviceName[256];
    uint8_t uuid[16];
    uint32_t pad; /* (the limits start on 8 bytes) */
    uint32_t limitsStart[17];
    uint32_t maxPerStageDescriptorSamplers;
    uint8_t rest[1024];  /* (the other limits and the sparse properties: more than they take) */
} PhysicalDeviceProperties;

typedef struct MemoryType { VkFlags propertyFlags; uint32_t heapIndex; } MemoryType;
typedef struct MemoryHeap { VkDeviceSize size; VkFlags flags; } MemoryHeap;
typedef struct PhysicalDeviceMemoryProperties { uint32_t typeCount; MemoryType types[32]; uint32_t heapCount; MemoryHeap heaps[16]; } PhysicalDeviceMemoryProperties;
typedef struct Extent3D { uint32_t width, height, depth; } Extent3D;
typedef struct QueueFamilyProperties { VkFlags flags; uint32_t count; uint32_t timestampValidBits; Extent3D minImageTransferGranularity; } QueueFamilyProperties;

typedef struct Extent2D { uint32_t width, height; } Extent2D;
typedef struct Offset2D { int32_t x, y; } Offset2D;
typedef struct Offset3D { int32_t x, y, z; } Offset3D;
typedef struct Rect2D { Offset2D offset; Extent2D extent; } Rect2D;
typedef struct SurfaceCapabilities { uint32_t minImageCount, maxImageCount; Extent2D current, minExtent, maxExtent; uint32_t maxLayers; VkFlags supportedTransforms, currentTransform, supportedAlpha, supportedUsage; } SurfaceCapabilities;
typedef struct SurfaceFormat { int format, colorSpace; } SurfaceFormat;
typedef struct SwapchainCreateInfo { int sType; const void* pNext; VkFlags flags; VkSurfaceKHR surface; uint32_t minImageCount; int format, colorSpace; Extent2D extent; uint32_t layers; VkFlags usage; int sharing; uint32_t queueFamilyCount; const uint32_t* queueFamilies; VkFlags preTransform, compositeAlpha; int presentMode; VkBool32 clipped; VkSwapchainKHR old; } SwapchainCreateInfo;
typedef struct PresentInfo { int sType; const void* pNext; uint32_t waitCount; const VkSemaphore* waits; uint32_t swapchainCount; const VkSwapchainKHR* swapchains; const uint32_t* imageIndices; VkResult* results; } PresentInfo;

typedef struct Win32SurfaceCreateInfo { int sType; const void* pNext; VkFlags flags; void* hinstance; void* hwnd; } Win32SurfaceCreateInfo;
typedef struct XlibSurfaceCreateInfo { int sType; const void* pNext; VkFlags flags; void* display; unsigned long window; } XlibSurfaceCreateInfo;
typedef struct WaylandSurfaceCreateInfo { int sType; const void* pNext; VkFlags flags; void* display; void* surface; } WaylandSurfaceCreateInfo;
typedef struct AndroidSurfaceCreateInfo { int sType; const void* pNext; VkFlags flags; void* window; } AndroidSurfaceCreateInfo;
typedef struct MetalSurfaceCreateInfo { int sType; const void* pNext; VkFlags flags; const void* layer; } MetalSurfaceCreateInfo;

typedef struct MemoryRequirements { VkDeviceSize size, alignment; uint32_t typeBits; } MemoryRequirements;
typedef struct MemoryAllocateInfo { int sType; const void* pNext; VkDeviceSize size; uint32_t typeIndex; } MemoryAllocateInfo;
typedef struct BufferCreateInfo { int sType; const void* pNext; VkFlags flags; VkDeviceSize size; VkFlags usage; int sharing; uint32_t queueFamilyCount; const uint32_t* queueFamilies; } BufferCreateInfo;
typedef struct ImageCreateInfo { int sType; const void* pNext; VkFlags flags; int imageType, format; Extent3D extent; uint32_t mipLevels, arrayLayers; int samples, tiling; VkFlags usage; int sharing; uint32_t queueFamilyCount; const uint32_t* queueFamilies; int initialLayout; } ImageCreateInfo;
typedef struct ImageSubresourceRange { VkFlags aspect; uint32_t baseMip, mipCount, baseLayer, layerCount; } ImageSubresourceRange;
typedef struct ComponentMapping { int r, g, b, a; } ComponentMapping;
typedef struct ImageViewCreateInfo { int sType; const void* pNext; VkFlags flags; VkImage image; int viewType, format; ComponentMapping components; ImageSubresourceRange range; } ImageViewCreateInfo;
typedef struct SamplerCreateInfo { int sType; const void* pNext; VkFlags flags; int magFilter, minFilter, mipmapMode, addressU, addressV, addressW; float mipLodBias; VkBool32 anisotropyEnable; float maxAnisotropy; VkBool32 compareEnable; int compareOp; float minLod, maxLod; int borderColor; VkBool32 unnormalized; } SamplerCreateInfo;

typedef struct AttachmentDescription { VkFlags flags; int format, samples, loadOp, storeOp, stencilLoadOp, stencilStoreOp, initialLayout, finalLayout; } AttachmentDescription;
typedef struct AttachmentReference { uint32_t attachment; int layout; } AttachmentReference;
typedef struct SubpassDescription { VkFlags flags; int bindPoint; uint32_t inputCount; const AttachmentReference* inputs; uint32_t colorCount; const AttachmentReference* colors; const AttachmentReference* resolves; const AttachmentReference* depth; uint32_t preserveCount; const uint32_t* preserves; } SubpassDescription;
typedef struct SubpassDependency { uint32_t src, dst; VkFlags srcStage, dstStage, srcAccess, dstAccess, flags; } SubpassDependency;
typedef struct RenderPassCreateInfo { int sType; const void* pNext; VkFlags flags; uint32_t attachmentCount; const AttachmentDescription* attachments; uint32_t subpassCount; const SubpassDescription* subpasses; uint32_t dependencyCount; const SubpassDependency* dependencies; } RenderPassCreateInfo;
typedef struct FramebufferCreateInfo { int sType; const void* pNext; VkFlags flags; VkRenderPass renderPass; uint32_t attachmentCount; const VkImageView* attachments; uint32_t width, height, layers; } FramebufferCreateInfo;

typedef struct ShaderModuleCreateInfo { int sType; const void* pNext; VkFlags flags; size_t size; const uint32_t* code; } ShaderModuleCreateInfo;
typedef struct PipelineShaderStage { int sType; const void* pNext; VkFlags flags; VkFlags stage; VkShaderModule module; const char* name; const void* specialization; } PipelineShaderStage;
typedef struct VertexBinding { uint32_t binding, stride; int inputRate; } VertexBinding;
typedef struct VertexAttribute { uint32_t location, binding; int format; uint32_t offset; } VertexAttribute;
typedef struct PipelineVertexInput { int sType; const void* pNext; VkFlags flags; uint32_t bindingCount; const VertexBinding* bindings; uint32_t attributeCount; const VertexAttribute* attributes; } PipelineVertexInput;
typedef struct PipelineInputAssembly { int sType; const void* pNext; VkFlags flags; int topology; VkBool32 primitiveRestart; } PipelineInputAssembly;
typedef struct Viewport { float x, y, width, height, minDepth, maxDepth; } Viewport;
typedef struct PipelineViewport { int sType; const void* pNext; VkFlags flags; uint32_t viewportCount; const Viewport* viewports; uint32_t scissorCount; const Rect2D* scissors; } PipelineViewport;
typedef struct PipelineRasterization { int sType; const void* pNext; VkFlags flags; VkBool32 depthClamp, discard; int polygonMode; VkFlags cullMode; int frontFace; VkBool32 depthBias; float biasConstant, biasClamp, biasSlope, lineWidth; } PipelineRasterization;
typedef struct PipelineMultisample { int sType; const void* pNext; VkFlags flags; int samples; VkBool32 sampleShading; float minSampleShading; const uint32_t* sampleMask; VkBool32 alphaToCoverage, alphaToOne; } PipelineMultisample;
typedef struct StencilOpState { int failOp, passOp, depthFailOp, compareOp; uint32_t compareMask, writeMask, reference; } StencilOpState;
typedef struct PipelineDepthStencil { int sType; const void* pNext; VkFlags flags; VkBool32 depthTest, depthWrite; int depthCompare; VkBool32 depthBounds, stencilTest; StencilOpState front, back; float minBounds, maxBounds; } PipelineDepthStencil;
typedef struct ColorBlendAttachment { VkBool32 blend; int srcColor, dstColor, colorOp, srcAlpha, dstAlpha, alphaOp; VkFlags writeMask; } ColorBlendAttachment;
typedef struct PipelineColorBlend { int sType; const void* pNext; VkFlags flags; VkBool32 logicEnable; int logicOp; uint32_t attachmentCount; const ColorBlendAttachment* attachments; float constants[4]; } PipelineColorBlend;
typedef struct PipelineDynamic { int sType; const void* pNext; VkFlags flags; uint32_t stateCount; const int* states; } PipelineDynamic;
typedef struct GraphicsPipelineCreateInfo { int sType; const void* pNext; VkFlags flags; uint32_t stageCount; const PipelineShaderStage* stages; const PipelineVertexInput* vertexInput; const PipelineInputAssembly* inputAssembly; const void* tessellation; const PipelineViewport* viewport; const PipelineRasterization* rasterization; const PipelineMultisample* multisample; const PipelineDepthStencil* depthStencil; const PipelineColorBlend* colorBlend; const PipelineDynamic* dynamic; VkPipelineLayout layout; VkRenderPass renderPass; uint32_t subpass; VkPipeline base; int32_t baseIndex; } GraphicsPipelineCreateInfo;
typedef struct ComputePipelineCreateInfo { int sType; const void* pNext; VkFlags flags; PipelineShaderStage stage; VkPipelineLayout layout; VkPipeline base; int32_t baseIndex; } ComputePipelineCreateInfo;
typedef struct PipelineCacheCreateInfo { int sType; const void* pNext; VkFlags flags; size_t size; const void* data; } PipelineCacheCreateInfo;
typedef struct QueryPoolCreateInfo { int sType; const void* pNext; VkFlags flags; int queryType; uint32_t count; VkFlags statistics; } QueryPoolCreateInfo;
typedef VkPipelineCache VkQueryPool;
typedef struct PushConstantRange { VkFlags stages; uint32_t offset, size; } PushConstantRange;
typedef struct PipelineLayoutCreateInfo { int sType; const void* pNext; VkFlags flags; uint32_t setCount; const VkDescriptorSetLayout* sets; uint32_t pushCount; const void* pushes; } PipelineLayoutCreateInfo;

typedef struct DescriptorSetLayoutBinding { uint32_t binding; int type; uint32_t count; VkFlags stages; const VkSampler* immutableSamplers; } DescriptorSetLayoutBinding;
typedef struct DescriptorSetLayoutCreateInfo { int sType; const void* pNext; VkFlags flags; uint32_t bindingCount; const DescriptorSetLayoutBinding* bindings; } DescriptorSetLayoutCreateInfo;
typedef struct BindingFlagsCreateInfo { int sType; const void* pNext; uint32_t bindingCount; const VkFlags* flags; } BindingFlagsCreateInfo;
typedef struct DescriptorPoolSize { int type; uint32_t count; } DescriptorPoolSize;
typedef struct DescriptorPoolCreateInfo { int sType; const void* pNext; VkFlags flags; uint32_t maxSets; uint32_t poolSizeCount; const DescriptorPoolSize* poolSizes; } DescriptorPoolCreateInfo;
typedef struct DescriptorSetAllocateInfo { int sType; const void* pNext; VkDescriptorPool pool; uint32_t setCount; const VkDescriptorSetLayout* layouts; } DescriptorSetAllocateInfo;
typedef struct DescriptorBufferInfo { VkBuffer buffer; VkDeviceSize offset, range; } DescriptorBufferInfo;
typedef struct DescriptorImageInfo { VkSampler sampler; VkImageView view; int layout; } DescriptorImageInfo;
typedef struct WriteDescriptorSet { int sType; const void* pNext; VkDescriptorSet set; uint32_t binding, arrayElement, count; int type; const DescriptorImageInfo* image; const DescriptorBufferInfo* buffer; const void* texelView; } WriteDescriptorSet;

typedef struct CommandPoolCreateInfo { int sType; const void* pNext; VkFlags flags; uint32_t family; } CommandPoolCreateInfo;
typedef struct CommandBufferAllocateInfo { int sType; const void* pNext; VkCommandPool pool; int level; uint32_t count; } CommandBufferAllocateInfo;
typedef struct CommandBufferBeginInfo { int sType; const void* pNext; VkFlags flags; const void* inheritance; } CommandBufferBeginInfo;
typedef struct ClearRect { Rect2D rect; uint32_t baseArrayLayer, layerCount; } ClearRect;
typedef union ClearValue { float color[4]; struct { float depth; uint32_t stencil; } depthStencil; } ClearValue;
typedef struct ClearAttachment { VkFlags aspect; uint32_t colorAttachment; ClearValue value; } ClearAttachment;
typedef struct RenderPassBeginInfo { int sType; const void* pNext; VkRenderPass renderPass; VkFramebuffer framebuffer; Rect2D area; uint32_t clearCount; const ClearValue* clears; } RenderPassBeginInfo;
typedef struct SubmitInfo { int sType; const void* pNext; uint32_t waitCount; const VkSemaphore* waits; const VkFlags* waitStages; uint32_t commandCount; const VkCommandBuffer* commands; uint32_t signalCount; const VkSemaphore* signals; } SubmitInfo;
typedef struct FenceCreateInfo { int sType; const void* pNext; VkFlags flags; } FenceCreateInfo;
typedef struct SemaphoreCreateInfo { int sType; const void* pNext; VkFlags flags; } SemaphoreCreateInfo;
typedef struct GlobalBarrier { int sType; const void* pNext; VkFlags srcAccess, dstAccess; } GlobalBarrier;
typedef struct ImageMemoryBarrier { int sType; const void* pNext; VkFlags srcAccess, dstAccess; int oldLayout, newLayout; uint32_t srcFamily, dstFamily; VkImage image; ImageSubresourceRange range; } ImageMemoryBarrier;
typedef struct BufferCopy { VkDeviceSize src, dst, size; } BufferCopy;
typedef struct ImageSubresourceLayers { VkFlags aspect; uint32_t mip, baseLayer, layerCount; } ImageSubresourceLayers;
typedef struct BufferImageCopy { VkDeviceSize offset; uint32_t rowLength, imageHeight; ImageSubresourceLayers subresource; Offset3D imageOffset; Extent3D imageExtent; } BufferImageCopy;

/* X(name, return type, arguments): every function loaded once the device is there. */
#define ARTF_VK_INSTANCE_FUNCTIONS(X) \
    X(vkDestroyInstance, void, (VkInstance, const void*)) \
    X(vkEnumeratePhysicalDevices, VkResult, (VkInstance, uint32_t*, VkPhysicalDevice*)) \
    X(vkGetPhysicalDeviceProperties, void, (VkPhysicalDevice, PhysicalDeviceProperties*)) \
    X(vkGetPhysicalDeviceFeatures2, void, (VkPhysicalDevice, PhysicalDeviceFeatures2*)) \
    X(vkGetPhysicalDeviceMemoryProperties, void, (VkPhysicalDevice, PhysicalDeviceMemoryProperties*)) \
    X(vkGetPhysicalDeviceQueueFamilyProperties, void, (VkPhysicalDevice, uint32_t*, QueueFamilyProperties*)) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR, VkResult, (VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32*)) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, VkResult, (VkPhysicalDevice, VkSurfaceKHR, SurfaceCapabilities*)) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR, VkResult, (VkPhysicalDevice, VkSurfaceKHR, uint32_t*, SurfaceFormat*)) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR, VkResult, (VkPhysicalDevice, VkSurfaceKHR, uint32_t*, int*)) \
    X(vkDestroySurfaceKHR, void, (VkInstance, VkSurfaceKHR, const void*)) \
    X(vkCreateDevice, VkResult, (VkPhysicalDevice, const DeviceCreateInfo*, const void*, VkDevice*)) \
    X(vkGetDeviceProcAddr, void*, (VkDevice, const char*))

#define ARTF_VK_DEVICE_FUNCTIONS(X) \
    X(vkDestroyDevice, void, (VkDevice, const void*)) \
    X(vkGetDeviceQueue, void, (VkDevice, uint32_t, uint32_t, VkQueue*)) \
    X(vkDeviceWaitIdle, VkResult, (VkDevice)) \
    X(vkQueueSubmit, VkResult, (VkQueue, uint32_t, const SubmitInfo*, VkFence)) \
    X(vkQueueWaitIdle, VkResult, (VkQueue)) \
    X(vkCreateSwapchainKHR, VkResult, (VkDevice, const SwapchainCreateInfo*, const void*, VkSwapchainKHR*)) \
    X(vkDestroySwapchainKHR, void, (VkDevice, VkSwapchainKHR, const void*)) \
    X(vkGetSwapchainImagesKHR, VkResult, (VkDevice, VkSwapchainKHR, uint32_t*, VkImage*)) \
    X(vkAcquireNextImageKHR, VkResult, (VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*)) \
    X(vkQueuePresentKHR, VkResult, (VkQueue, const PresentInfo*)) \
    X(vkAllocateMemory, VkResult, (VkDevice, const MemoryAllocateInfo*, const void*, VkDeviceMemory*)) \
    X(vkFreeMemory, void, (VkDevice, VkDeviceMemory, const void*)) \
    X(vkMapMemory, VkResult, (VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkFlags, void**)) \
    X(vkCreateBuffer, VkResult, (VkDevice, const BufferCreateInfo*, const void*, VkBuffer*)) \
    X(vkDestroyBuffer, void, (VkDevice, VkBuffer, const void*)) \
    X(vkGetBufferMemoryRequirements, void, (VkDevice, VkBuffer, MemoryRequirements*)) \
    X(vkBindBufferMemory, VkResult, (VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize)) \
    X(vkCreateImage, VkResult, (VkDevice, const ImageCreateInfo*, const void*, VkImage*)) \
    X(vkDestroyImage, void, (VkDevice, VkImage, const void*)) \
    X(vkGetImageMemoryRequirements, void, (VkDevice, VkImage, MemoryRequirements*)) \
    X(vkBindImageMemory, VkResult, (VkDevice, VkImage, VkDeviceMemory, VkDeviceSize)) \
    X(vkCreateImageView, VkResult, (VkDevice, const ImageViewCreateInfo*, const void*, VkImageView*)) \
    X(vkDestroyImageView, void, (VkDevice, VkImageView, const void*)) \
    X(vkCreateSampler, VkResult, (VkDevice, const SamplerCreateInfo*, const void*, VkSampler*)) \
    X(vkDestroySampler, void, (VkDevice, VkSampler, const void*)) \
    X(vkCreateRenderPass, VkResult, (VkDevice, const RenderPassCreateInfo*, const void*, VkRenderPass*)) \
    X(vkDestroyRenderPass, void, (VkDevice, VkRenderPass, const void*)) \
    X(vkCreateFramebuffer, VkResult, (VkDevice, const FramebufferCreateInfo*, const void*, VkFramebuffer*)) \
    X(vkDestroyFramebuffer, void, (VkDevice, VkFramebuffer, const void*)) \
    X(vkCreateShaderModule, VkResult, (VkDevice, const ShaderModuleCreateInfo*, const void*, VkShaderModule*)) \
    X(vkDestroyShaderModule, void, (VkDevice, VkShaderModule, const void*)) \
    X(vkCreateGraphicsPipelines, VkResult, (VkDevice, VkPipelineCache, uint32_t, const GraphicsPipelineCreateInfo*, const void*, VkPipeline*)) \
    X(vkCreateComputePipelines, VkResult, (VkDevice, VkPipelineCache, uint32_t, const ComputePipelineCreateInfo*, const void*, VkPipeline*)) \
    X(vkDestroyPipeline, void, (VkDevice, VkPipeline, const void*)) \
    X(vkCreatePipelineLayout, VkResult, (VkDevice, const PipelineLayoutCreateInfo*, const void*, VkPipelineLayout*)) \
    X(vkDestroyPipelineLayout, void, (VkDevice, VkPipelineLayout, const void*)) \
    X(vkCreateDescriptorSetLayout, VkResult, (VkDevice, const DescriptorSetLayoutCreateInfo*, const void*, VkDescriptorSetLayout*)) \
    X(vkDestroyDescriptorSetLayout, void, (VkDevice, VkDescriptorSetLayout, const void*)) \
    X(vkCreateDescriptorPool, VkResult, (VkDevice, const DescriptorPoolCreateInfo*, const void*, VkDescriptorPool*)) \
    X(vkDestroyDescriptorPool, void, (VkDevice, VkDescriptorPool, const void*)) \
    X(vkAllocateDescriptorSets, VkResult, (VkDevice, const DescriptorSetAllocateInfo*, VkDescriptorSet*)) \
    X(vkUpdateDescriptorSets, void, (VkDevice, uint32_t, const WriteDescriptorSet*, uint32_t, const void*)) \
    X(vkCreateCommandPool, VkResult, (VkDevice, const CommandPoolCreateInfo*, const void*, VkCommandPool*)) \
    X(vkDestroyCommandPool, void, (VkDevice, VkCommandPool, const void*)) \
    X(vkAllocateCommandBuffers, VkResult, (VkDevice, const CommandBufferAllocateInfo*, VkCommandBuffer*)) \
    X(vkFreeCommandBuffers, void, (VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer*)) \
    X(vkBeginCommandBuffer, VkResult, (VkCommandBuffer, const CommandBufferBeginInfo*)) \
    X(vkEndCommandBuffer, VkResult, (VkCommandBuffer)) \
    X(vkResetCommandBuffer, VkResult, (VkCommandBuffer, VkFlags)) \
    X(vkCreateSemaphore, VkResult, (VkDevice, const SemaphoreCreateInfo*, const void*, VkSemaphore*)) \
    X(vkDestroySemaphore, void, (VkDevice, VkSemaphore, const void*)) \
    X(vkCreateFence, VkResult, (VkDevice, const FenceCreateInfo*, const void*, VkFence*)) \
    X(vkDestroyFence, void, (VkDevice, VkFence, const void*)) \
    X(vkWaitForFences, VkResult, (VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t)) \
    X(vkResetFences, VkResult, (VkDevice, uint32_t, const VkFence*)) \
    X(vkCmdPipelineBarrier, void, (VkCommandBuffer, VkFlags, VkFlags, VkFlags, uint32_t, const GlobalBarrier*, uint32_t, const void*, uint32_t, const ImageMemoryBarrier*)) \
    X(vkCmdCopyBuffer, void, (VkCommandBuffer, VkBuffer, VkBuffer, uint32_t, const BufferCopy*)) \
    X(vkCmdCopyBufferToImage, void, (VkCommandBuffer, VkBuffer, VkImage, int, uint32_t, const BufferImageCopy*)) \
    X(vkCmdCopyImageToBuffer, void, (VkCommandBuffer, VkImage, int, VkBuffer, uint32_t, const BufferImageCopy*)) \
    X(vkCmdBeginRenderPass, void, (VkCommandBuffer, const RenderPassBeginInfo*, int)) \
    X(vkCmdEndRenderPass, void, (VkCommandBuffer)) \
    X(vkCmdBindPipeline, void, (VkCommandBuffer, int, VkPipeline)) \
    X(vkCmdBindDescriptorSets, void, (VkCommandBuffer, int, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*)) \
    X(vkCmdBindVertexBuffers, void, (VkCommandBuffer, uint32_t, uint32_t, const VkBuffer*, const VkDeviceSize*)) \
    X(vkCmdBindIndexBuffer, void, (VkCommandBuffer, VkBuffer, VkDeviceSize, int)) \
    X(vkCmdSetViewport, void, (VkCommandBuffer, uint32_t, uint32_t, const Viewport*)) \
    X(vkCmdSetScissor, void, (VkCommandBuffer, uint32_t, uint32_t, const Rect2D*)) \
    X(vkCmdDispatch, void, (VkCommandBuffer, uint32_t, uint32_t, uint32_t)) \
    X(vkCmdDispatchBase, void, (VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)) \
    X(vkCmdPushConstants, void, (VkCommandBuffer, VkPipelineLayout, VkFlags, uint32_t, uint32_t, const void*)) \
    X(vkCmdFillBuffer, void, (VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, uint32_t)) \
    X(vkCmdDispatchIndirect, void, (VkCommandBuffer, VkBuffer, VkDeviceSize)) \
    X(vkCmdDrawIndexedIndirectCount, void, (VkCommandBuffer, VkBuffer, VkDeviceSize, VkBuffer, VkDeviceSize, uint32_t, uint32_t)) \
    X(vkCmdClearAttachments, void, (VkCommandBuffer, uint32_t, const ClearAttachment*, uint32_t, const ClearRect*)) \
    X(vkCmdDraw, void, (VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t)) \
    X(vkCreatePipelineCache, VkResult, (VkDevice, const PipelineCacheCreateInfo*, const void*, VkPipelineCache*)) \
    X(vkDestroyPipelineCache, void, (VkDevice, VkPipelineCache, const void*)) \
    X(vkGetPipelineCacheData, VkResult, (VkDevice, VkPipelineCache, size_t*, void*)) \
    X(vkCreateQueryPool, VkResult, (VkDevice, const QueryPoolCreateInfo*, const void*, VkQueryPool*)) \
    X(vkDestroyQueryPool, void, (VkDevice, VkQueryPool, const void*)) \
    X(vkCmdResetQueryPool, void, (VkCommandBuffer, VkQueryPool, uint32_t, uint32_t)) \
    X(vkCmdWriteTimestamp, void, (VkCommandBuffer, VkFlags, VkQueryPool, uint32_t)) \
    X(vkGetQueryPoolResults, VkResult, (VkDevice, VkQueryPool, uint32_t, uint32_t, size_t, void*, VkDeviceSize, VkFlags)) \
    X(vkCmdDrawIndexed, void, (VkCommandBuffer, uint32_t, uint32_t, uint32_t, int32_t, uint32_t)) \
    X(vkCmdDrawIndexedIndirect, void, (VkCommandBuffer, VkBuffer, VkDeviceSize, uint32_t, uint32_t))

#endif
