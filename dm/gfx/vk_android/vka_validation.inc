// Optional explicit validation-layer chain for isolated Android shell diagnostics.
// Android's app-layer discovery cannot find a layer beside an adb shell executable.
// No layer is loaded unless DM_VK_VALIDATE_LIBRARY names it; normal app dispatch is unchanged.
#include <vulkan/vk_layer.h>
static PFN_vkGetInstanceProcAddr val_next, val_layer;
static VkInstance val_instance;
static VkBool32 VKAPI_PTR val_message(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT type,
        uint64_t object, size_t location, int32_t code, const char *prefix, const char *message, void *data) {
    (void)flags;(void)type;(void)object;(void)location;(void)code;(void)prefix;(void)data;
    fprintf(stderr,"[vk-validation] %s\n",message);
    return getenv("DM_VK_VALIDATE_PROBE") && (flags&VK_DEBUG_REPORT_ERROR_BIT_EXT) ? VK_TRUE : VK_FALSE;
}
static VkResult VKAPI_PTR val_create_instance(const VkInstanceCreateInfo *in, const VkAllocationCallbacks *a, VkInstance *out) {
    VkLayerInstanceLink link={.pfnNextGetInstanceProcAddr=val_next,
        .pfnNextGetPhysicalDeviceProcAddr=(PFN_GetPhysicalDeviceProcAddr)val_next};
    VkValidationFeatureDisableEXT disable=VK_VALIDATION_FEATURE_DISABLE_UNIQUE_HANDLES_EXT;
    VkValidationFeatureEnableEXT enable=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT features={VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,.pNext=in->pNext,
        .enabledValidationFeatureCount=1,.pEnabledValidationFeatures=&enable,
        .disabledValidationFeatureCount=1,.pDisabledValidationFeatures=&disable};
    VkDebugReportCallbackCreateInfoEXT debug={VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT,.pNext=&features,
        .flags=VK_DEBUG_REPORT_ERROR_BIT_EXT|VK_DEBUG_REPORT_WARNING_BIT_EXT,.pfnCallback=val_message};
    VkLayerInstanceCreateInfo chain={.sType=VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO,.pNext=&debug,
        .function=VK_LAYER_LINK_INFO,.u.pLayerInfo=&link};
    VkInstanceCreateInfo ci=*in;ci.pNext=&chain;ci.enabledLayerCount=0;
    PFN_vkCreateInstance make=(PFN_vkCreateInstance)val_layer(VK_NULL_HANDLE,"vkCreateInstance");
    VkResult r=make(&ci,a,out);
    if(!r) {
        val_instance=*out;
        PFN_vkCreateDebugReportCallbackEXT report=(PFN_vkCreateDebugReportCallbackEXT)val_layer(*out,"vkCreateDebugReportCallbackEXT");
        debug.pNext=NULL;VkDebugReportCallbackEXT callback;
        if(report) report(*out,&debug,NULL,&callback);
    }
    return r;
}
static VkResult VKAPI_PTR val_create_device(VkPhysicalDevice pd,const VkDeviceCreateInfo *in,const VkAllocationCallbacks *a,VkDevice *out) {
    VkLayerDeviceLink link={.pfnNextGetInstanceProcAddr=val_next,
        .pfnNextGetDeviceProcAddr=(PFN_vkGetDeviceProcAddr)val_next(val_instance,"vkGetDeviceProcAddr")};
    VkLayerDeviceCreateInfo chain={.sType=VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,.pNext=in->pNext,
        .function=VK_LAYER_LINK_INFO,.u.pLayerInfo=&link};
    VkDeviceCreateInfo ci=*in;ci.pNext=&chain;
    PFN_vkCreateDevice make=(PFN_vkCreateDevice)val_layer(val_instance,"vkCreateDevice");
    return make(pd,&ci,a,out);
}
static PFN_vkVoidFunction VKAPI_PTR val_gipa(VkInstance inst,const char *name) {
    if(!strcmp(name,"vkCreateInstance"))return(PFN_vkVoidFunction)val_create_instance;
    if(!strcmp(name,"vkCreateDevice"))return(PFN_vkVoidFunction)val_create_device;
    return inst ? val_layer(inst,name) : val_next(inst,name);
}
static int val_open(void) {
    const char *path=getenv("DM_VK_VALIDATE_LIBRARY");if(!path||!*path)return 1;
    void *lib=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if(!lib || !(val_layer=(PFN_vkGetInstanceProcAddr)dlsym(lib,"vkGetInstanceProcAddr"))) {
        VRED("explicit validation layer unavailable: %s",dlerror());return 0;
    }
    val_next=vk.vkGetInstanceProcAddr;vk.vkGetInstanceProcAddr=val_gipa;
    VLOG("explicit Vulkan validation layer enabled for diagnostics");return 1;
}
static void val_probe(void) {
    if(!val_layer || !getenv("DM_VK_VALIDATE_PROBE"))return;
    VkBufferCreateInfo ci={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=16,.usage=0};VkBuffer buffer=VK_NULL_HANDLE;
    VkResult r=vk.vkCreateBuffer(V.dev,&ci,NULL,&buffer);
    VLOG("validation interception probe: invalid buffer usage returned %d",r);
    if(!r)vk.vkDestroyBuffer(V.dev,buffer,NULL);
}
