// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone, disposable Vulkan validation. Usage: tess_lds_gpu vertex.spv tcs.spv tese.spv
// Selects NVIDIA explicitly and verifies separate versus batched writer/reader draws.
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <vulkan/vulkan.h>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

#define CHECK(expr) do { VkResult result_ = (expr); if (result_ != VK_SUCCESS) { \
    std::fprintf(stderr, "%s failed: %d\n", #expr, int(result_)); throw std::runtime_error(#expr); } } while (0)
#define LOAD_I(name) auto name = reinterpret_cast<PFN_##name>(get_instance(instance, #name)); if (!name) throw std::runtime_error(#name)
#define LOAD_D(name) auto name = reinterpret_cast<PFN_##name>(get_device(device, #name)); if (!name) throw std::runtime_error(#name)

static std::vector<uint32_t> ReadSpv(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error(path);
    const auto bytes = f.tellg();
    if (bytes <= 0 || bytes % 4 != 0) throw std::runtime_error("invalid SPIR-V size");
    std::vector<uint32_t> data(size_t(bytes) / 4);
    f.seekg(0); f.read(reinterpret_cast<char*>(data.data()), bytes);
    if (!f) throw std::runtime_error("SPIR-V read failed");
    return data;
}

int main(int argc, char** argv) try {
    if (argc != 4) { std::fprintf(stderr,"usage: tess_lds_gpu vertex.spv tcs.spv tese.spv\n"); return 2; }
    HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
    if (!loader) throw std::runtime_error("Vulkan loader unavailable");
    auto get_instance = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
    if (!get_instance) throw std::runtime_error("vkGetInstanceProcAddr");
    auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(get_instance(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create_instance) throw std::runtime_error("vkCreateInstance");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &app;
    VkInstance instance{}; CHECK(create_instance(&ici, nullptr, &instance));
    LOAD_I(vkEnumeratePhysicalDevices); LOAD_I(vkGetPhysicalDeviceProperties);
    LOAD_I(vkGetPhysicalDeviceFeatures); LOAD_I(vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_I(vkGetPhysicalDeviceMemoryProperties); LOAD_I(vkCreateDevice); LOAD_I(vkGetDeviceProcAddr);
    LOAD_I(vkDestroyInstance);
    uint32_t count=0; CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> physicals(count); CHECK(vkEnumeratePhysicalDevices(instance, &count, physicals.data()));
    VkPhysicalDevice physical{}; VkPhysicalDeviceProperties properties{};
    for (auto candidate : physicals) { VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(candidate, &p);
        if (p.vendorID == 0x10de && p.apiVersion >= VK_API_VERSION_1_2) { physical=candidate; properties=p; break; } }
    if (!physical) throw std::runtime_error("NVIDIA Vulkan 1.2 device unavailable");
    VkPhysicalDeviceFeatures supported{}; vkGetPhysicalDeviceFeatures(physical, &supported);
    if (!supported.tessellationShader || !supported.shaderInt64 || !supported.vertexPipelineStoresAndAtomics)
        throw std::runtime_error("required tessellation/int64/vertex atomics unsupported");
    uint32_t families_count=0; vkGetPhysicalDeviceQueueFamilyProperties(physical,&families_count,nullptr);
    std::vector<VkQueueFamilyProperties> families(families_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&families_count,families.data());
    uint32_t family=0; while (family<families_count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    if (family==families_count) throw std::runtime_error("graphics queue unavailable");
    const float priority=1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
    VkPhysicalDeviceBufferDeviceAddressFeatures bda{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES}; bda.bufferDeviceAddress=VK_TRUE;
    VkPhysicalDeviceFeatures features{}; features.tessellationShader=VK_TRUE; features.shaderInt64=VK_TRUE; features.vertexPipelineStoresAndAtomics=VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.pNext=&bda; dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qi; dci.pEnabledFeatures=&features;
    VkDevice device{}; CHECK(vkCreateDevice(physical,&dci,nullptr,&device));
    auto get_device=vkGetDeviceProcAddr;
    LOAD_D(vkDestroyDevice); LOAD_D(vkGetDeviceQueue); LOAD_D(vkCreateBuffer); LOAD_D(vkGetBufferMemoryRequirements);
    LOAD_D(vkAllocateMemory); LOAD_D(vkBindBufferMemory); LOAD_D(vkMapMemory); LOAD_D(vkUnmapMemory);
    LOAD_D(vkGetBufferDeviceAddress); LOAD_D(vkDestroyBuffer); LOAD_D(vkFreeMemory);
    LOAD_D(vkCreateDescriptorSetLayout); LOAD_D(vkCreateDescriptorPool); LOAD_D(vkAllocateDescriptorSets); LOAD_D(vkUpdateDescriptorSets);
    LOAD_D(vkCreatePipelineLayout); LOAD_D(vkCreateShaderModule); LOAD_D(vkCreateRenderPass); LOAD_D(vkCreateFramebuffer);
    LOAD_D(vkCreateGraphicsPipelines); LOAD_D(vkCreateCommandPool); LOAD_D(vkAllocateCommandBuffers);
    LOAD_D(vkResetCommandBuffer); LOAD_D(vkBeginCommandBuffer); LOAD_D(vkEndCommandBuffer);
    LOAD_D(vkCmdBeginRenderPass); LOAD_D(vkCmdEndRenderPass); LOAD_D(vkCmdBindPipeline);
    LOAD_D(vkCmdBindDescriptorSets); LOAD_D(vkCmdPushConstants); LOAD_D(vkCmdDraw); LOAD_D(vkCmdPipelineBarrier);
    LOAD_D(vkCreateFence); LOAD_D(vkResetFences); LOAD_D(vkQueueSubmit); LOAD_D(vkWaitForFences);
    LOAD_D(vkDestroyFence); LOAD_D(vkDestroyCommandPool); LOAD_D(vkDestroyPipeline);
    LOAD_D(vkDestroyFramebuffer); LOAD_D(vkDestroyRenderPass); LOAD_D(vkDestroyShaderModule);
    LOAD_D(vkDestroyPipelineLayout); LOAD_D(vkDestroyDescriptorPool); LOAD_D(vkDestroyDescriptorSetLayout);
    VkQueue queue{}; vkGetDeviceQueue(device,family,0,&queue);
    VkPhysicalDeviceMemoryProperties mp{}; vkGetPhysicalDeviceMemoryProperties(physical,&mp);
    struct Buffer { VkBuffer handle{}; VkDeviceMemory memory{}; void* mapped{}; VkDeviceAddress address{}; };
    auto make_buffer = [&](VkDeviceSize size, bool address) {
        Buffer out{}; VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=size;
        bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | (address ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
        CHECK(vkCreateBuffer(device,&bi,nullptr,&out.handle));
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device,out.handle,&req);
        uint32_t type=0; const auto needed=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        while (type<mp.memoryTypeCount && (!(req.memoryTypeBits & (1u<<type)) || (mp.memoryTypes[type].propertyFlags & needed)!=needed)) ++type;
        if (type==mp.memoryTypeCount) throw std::runtime_error("host coherent memory unavailable");
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO}; flags.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.pNext=address?&flags:nullptr; ai.allocationSize=req.size; ai.memoryTypeIndex=type;
        CHECK(vkAllocateMemory(device,&ai,nullptr,&out.memory)); CHECK(vkBindBufferMemory(device,out.handle,out.memory,0));
        CHECK(vkMapMemory(device,out.memory,0,size,0,&out.mapped));
        if (address) { VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO}; info.buffer=out.handle;
            out.address=vkGetBufferDeviceAddress(device,&info); if (!out.address || (out.address&3) || uint32_t(out.address>>32)==~0u) throw std::runtime_error("invalid scratch address"); }
        return out;
    };
    std::array<Buffer,2> scratch{make_buffer(64,true),make_buffer(64,true)};
    std::array<Buffer,2> output{make_buffer(16,false),make_buffer(16,false)};
    VkDescriptorSetLayoutBinding binding{}; binding.binding=0; binding.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; binding.descriptorCount=1; binding.stageFlags=VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; sl.bindingCount=1; sl.pBindings=&binding;
    VkDescriptorSetLayout set_layout{}; CHECK(vkCreateDescriptorSetLayout(device,&sl,nullptr,&set_layout));
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2}; VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pi.maxSets=2; pi.poolSizeCount=1; pi.pPoolSizes=&ps;
    VkDescriptorPool pool{}; CHECK(vkCreateDescriptorPool(device,&pi,nullptr,&pool));
    std::array<VkDescriptorSetLayout,2> layouts{set_layout,set_layout}; VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; sai.descriptorPool=pool; sai.descriptorSetCount=2; sai.pSetLayouts=layouts.data();
    std::array<VkDescriptorSet,2> sets{}; CHECK(vkAllocateDescriptorSets(device,&sai,sets.data()));
    for (size_t i=0;i<2;++i) { VkDescriptorBufferInfo db{output[i].handle,0,16}; VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet=sets[i]; w.dstBinding=0; w.descriptorCount=1; w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo=&db; vkUpdateDescriptorSets(device,1,&w,0,nullptr); }
    constexpr VkShaderStageFlags stages=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT|VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    VkPushConstantRange push{stages,0,120}; VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; li.setLayoutCount=1; li.pSetLayouts=&set_layout; li.pushConstantRangeCount=1; li.pPushConstantRanges=&push;
    VkPipelineLayout layout{}; CHECK(vkCreatePipelineLayout(device,&li,nullptr,&layout));
    std::array<VkShaderModule,3> modules{}; std::array<VkPipelineShaderStageCreateInfo,3> si{};
    const std::array<VkShaderStageFlagBits,3> kinds{VK_SHADER_STAGE_VERTEX_BIT,VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT};
    for(size_t i=0;i<3;++i) { const auto code=ReadSpv(argv[i+1]); VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sm.codeSize=code.size()*4; sm.pCode=code.data();
        CHECK(vkCreateShaderModule(device,&sm,nullptr,&modules[i])); si[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; si[i].stage=kinds[i]; si[i].module=modules[i]; si[i].pName="main"; }
    VkSubpassDescription sub{}; sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rpi.subpassCount=1; rpi.pSubpasses=&sub;
    VkRenderPass pass{}; CHECK(vkCreateRenderPass(device,&rpi,nullptr,&pass));
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fi.renderPass=pass; fi.width=1; fi.height=1; fi.layers=1;
    VkFramebuffer fb{}; CHECK(vkCreateFramebuffer(device,&fi,nullptr,&fb));
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology=VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    VkPipelineTessellationStateCreateInfo ts{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO}; ts.patchControlPoints=1;
    VkViewport viewport{0,0,1,1,0,1}; VkRect2D scissor{{0,0},{1,1}};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount=1; vp.pViewports=&viewport; vp.scissorCount=1; vp.pScissors=&scissor;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.rasterizerDiscardEnable=VK_TRUE; rs.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; gi.stageCount=3; gi.pStages=si.data(); gi.pVertexInputState=&vi; gi.pInputAssemblyState=&ia; gi.pTessellationState=&ts; gi.pViewportState=&vp; gi.pRasterizationState=&rs; gi.pMultisampleState=&ms; gi.layout=layout; gi.renderPass=pass;
    VkPipeline pipeline{}; CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gi,nullptr,&pipeline));
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpi.queueFamilyIndex=family;
    VkCommandPool cp{}; CHECK(vkCreateCommandPool(device,&cpi,nullptr,&cp)); VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool=cp; cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount=1;
    VkCommandBuffer cmd{}; CHECK(vkAllocateCommandBuffers(device,&cai,&cmd)); VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; CHECK(vkCreateFence(device,&fci,nullptr,&fence));
    auto run = [&](bool batch) {
        for(auto& b:scratch) std::memset(b.mapped,0,64);
        for(auto& b:output) std::memset(b.mapped,0xff,16);
        CHECK(vkResetFences(device,1,&fence)); CHECK(vkResetCommandBuffer(cmd,0));
        VkCommandBufferBeginInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd,&cb));
        VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rb.renderPass=pass; rb.framebuffer=fb; rb.renderArea={{0,0},{1,1}};
        const auto begin = [&] { vkCmdBeginRenderPass(cmd,&rb,VK_SUBPASS_CONTENTS_INLINE); vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline); };
        const auto draw = [&](size_t i, bool writer) { std::array<uint32_t,30> words{}; words[0]=std::bit_cast<uint32_t>(i==0?10.0f:20.0f); const uint64_t address=scratch[i].address | uint64_t(writer); words[28]=uint32_t(address); words[29]=uint32_t(address>>32);
            vkCmdPushConstants(cmd,layout,stages,0,120,words.data()); vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&sets[i],0,nullptr); vkCmdDraw(cmd,3,1,0,0); };
        const auto barrier = [&] { VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; m.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT; m.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&m,0,nullptr,0,nullptr); };
        if (batch) { begin(); draw(0,true); draw(1,true); vkCmdEndRenderPass(cmd); barrier(); begin(); draw(0,false); draw(1,false); vkCmdEndRenderPass(cmd); }
        else for(size_t i=0;i<2;++i) { begin(); draw(i,true); vkCmdEndRenderPass(cmd); barrier(); begin(); draw(i,false); vkCmdEndRenderPass(cmd); }
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        CHECK(vkEndCommandBuffer(cmd)); VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&cmd;
        CHECK(vkQueueSubmit(queue,1,&submit,fence)); CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,10'000'000'000ull));
        for(size_t i=0;i<2;++i) { const auto* words=static_cast<const uint32_t*>(output[i].mapped); const uint32_t expected0=std::bit_cast<uint32_t>(i==0?12.0f:22.0f), expected1=std::bit_cast<uint32_t>(i==0?13.0f:23.0f);
            if(words[0]!=expected0 || words[1]!=expected1) { std::fprintf(stderr,"%s draw %zu got %08x %08x expected %08x %08x\n",batch?"batch":"serial",i,words[0],words[1],expected0,expected1); throw std::runtime_error("readback mismatch"); } }
    };
    run(false); run(true);
    std::printf("PASS NVIDIA %s: serial and batched TCS LDS writer/barrier/reader, 2 draws, 2 checked patches each\n",properties.deviceName);
    vkDestroyFence(device,fence,nullptr); vkDestroyCommandPool(device,cp,nullptr); vkDestroyPipeline(device,pipeline,nullptr);
    vkDestroyFramebuffer(device,fb,nullptr); vkDestroyRenderPass(device,pass,nullptr);
    for(auto module:modules) vkDestroyShaderModule(device,module,nullptr);
    vkDestroyPipelineLayout(device,layout,nullptr); vkDestroyDescriptorPool(device,pool,nullptr); vkDestroyDescriptorSetLayout(device,set_layout,nullptr);
    for(auto* buffers:{&output,&scratch}) for(auto& b:*buffers) { vkUnmapMemory(device,b.memory); vkDestroyBuffer(device,b.handle,nullptr); vkFreeMemory(device,b.memory,nullptr); }
    vkDestroyDevice(device,nullptr); vkDestroyInstance(instance,nullptr); FreeLibrary(loader); return 0;
} catch (const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
