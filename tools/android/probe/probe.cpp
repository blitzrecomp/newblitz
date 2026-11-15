// BLITZ DS probe, native side: memory reservation as the game does it, CPU speed, and Vulkan on every screen.
#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <sys/mman.h>
#include <unistd.h>
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include "lab_shaders.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {
    using Clock = std::chrono::steady_clock;
    double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

    char trace_path[512] = {};

    struct Out {
        std::string s;
        void operator()(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
            char b[1024];
            va_list a;
            va_start(a, fmt);
            vsnprintf(b, sizeof(b), fmt, a);
            va_end(a);
            s += b;
            s += '\n';
            // As it happens, so a driver that hangs or crashes still leaves the line before it (ProbeActivity reads this file).
            __android_log_print(ANDROID_LOG_INFO, "BlitzProbeNative", "%s", b);
            if (trace_path[0]) {
                if (FILE* f = fopen(trace_path, "a")) { fprintf(f, "%s\n", b); fclose(f); }
            }
        }
    };

    // The app reserves 4 GiB of address space and maps 512 MiB read-write for the game's memory (as on iOS).
    void memory(Out& out) {
        out("page size %ld, cpus %ld online", sysconf(_SC_PAGESIZE), sysconf(_SC_NPROCESSORS_ONLN));
        const size_t reserve = 4ull << 30, rw = 512ull << 20;
        void* base = mmap(nullptr, reserve, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        out("reserve 4 GiB: %s", base == MAP_FAILED ? "FAILED" : "ok");
        if (base != MAP_FAILED) {
            int prot = mprotect(base, rw, PROT_READ | PROT_WRITE);
            out("512 MiB read-write: %s", prot == 0 ? "ok" : "FAILED");
            if (prot == 0) {
                auto t = Clock::now();
                memset(base, 0x5A, 64 << 20);  // only the N64's memory and a bit more are ever touched
                out("touch 64 MiB: %.1f ms", ms_since(t));
            }
            munmap(base, reserve);
        }
    }

    void cpu(Out& out) {
        std::ifstream info("/proc/cpuinfo");
        std::string line, parts;
        while (std::getline(info, line)) {
            if (line.rfind("CPU part", 0) == 0) parts += line.substr(line.find(':') + 1);
            if (line.rfind("Hardware", 0) == 0) out("%s", line.c_str());
        }
        out("cpu parts:%s", parts.c_str());
        for (int c = 0; c < 8; c++) {
            std::ifstream f("/sys/devices/system/cpu/cpu" + std::to_string(c) + "/cpufreq/cpuinfo_max_freq");
            std::string v;
            if (f >> v) out("cpu%d max %s kHz", c, v.c_str());
        }
        // A rough single-thread figure, compared across devices by the same numbers.
        auto t = Clock::now();
        volatile float sink = 0;
        float a = 1.0f;
        for (int i = 0; i < 40000000; i++) a = a * 0.9999f + 0.5f;
        sink = a;
        double fl = ms_since(t);
        t = Clock::now();
        uint32_t x = 1;
        for (int i = 0; i < 200000000; i++) x = x * 1664525u + 1013904223u;
        volatile uint32_t sink2 = x;
        double in = ms_since(t);
        std::vector<uint8_t> src(32 << 20, 1), dst(32 << 20);
        t = Clock::now();
        for (int i = 0; i < 16; i++) {
            src[(size_t)i * 4096] = (uint8_t)i;
            memcpy(dst.data(), src.data(), src.size());
            sink2 = sink2 + dst[(size_t)i * 8192];
        }
        double mc = ms_since(t);
        (void)sink;
        (void)sink2;
        out("cpu: float %.0f ms, integer %.0f ms, memcpy %.0f MB/s", fl, in, 512.0 * 1000.0 / mc);
    }

    struct Chain {
        ANativeWindow* window = nullptr;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        std::vector<VkImage> images;
        VkExtent2D extent{};
        VkSemaphore acquired = VK_NULL_HANDLE, drawn = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
    };

    void barrier(VkCommandBuffer c, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // Draws n frames on the screens given, the fences first so one stuck screen is named. separate: a present call each.
    bool run_frames(Out& out, VkDevice dev, VkQueue q, std::vector<Chain>& all, std::vector<size_t> which, bool separate, int n, const char* name) {
        std::vector<Chain> chains;
        for (size_t i : which) chains.push_back(all[i]);
        size_t used = chains.size();
        std::vector<double> times;
        times.reserve(n);
        auto start = Clock::now();
        for (int f = 0; f < n; f++) {
            auto t = Clock::now();
            std::vector<uint32_t> idx(used);
            std::vector<VkSwapchainKHR> swaps(used);
            std::vector<VkSemaphore> waits(used);
            // As the game's present thread does: everything acquired, then one submit, then the presents.
            for (size_t i = 0; i < used; i++) {
                Chain& c = chains[i];
                if (vkWaitForFences(dev, 1, &c.fence, VK_TRUE, 3000000000ull) != VK_SUCCESS) {
                    out("%s: screen %zu's last frame never finished, at frame %d", name, which[i], f);
                    return false;
                }
                vkResetFences(dev, 1, &c.fence);
            }
            std::vector<VkPipelineStageFlags> stages(used, VK_PIPELINE_STAGE_TRANSFER_BIT);
            std::vector<VkSemaphore> acquired(used);
            std::vector<VkCommandBuffer> cmds(used);
            for (size_t i = 0; i < used; i++) {
                Chain& c = chains[i];
                VkResult r = vkAcquireNextImageKHR(dev, c.swapchain, 2000000000ull, c.acquired, VK_NULL_HANDLE, &idx[i]);
                if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
                    out("%s: acquire on screen %zu failed (%d) at frame %d", name, which[i], r, f);
                    return false;
                }
                VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(c.cmd, &bi);
                barrier(c.cmd, c.images[idx[i]], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
                float pulse = 0.25f + 0.25f * std::sin(f * 0.1f);
                VkClearColorValue color{};
                color.float32[0] = which[i] == 0 ? pulse : 0.05f;
                color.float32[1] = 0.05f;
                color.float32[2] = which[i] == 0 ? 0.05f : pulse;
                color.float32[3] = 1.0f;
                VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(c.cmd, c.images[idx[i]], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
                barrier(c.cmd, c.images[idx[i]], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
                vkEndCommandBuffer(c.cmd);
                acquired[i] = c.acquired;
                cmds[i] = c.cmd;
                swaps[i] = c.swapchain;
                waits[i] = c.drawn;
            }
            // Each screen its own submit with its own semaphores, so the fences name which one stalls.
            for (size_t i = 0; i < used; i++) {
                VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                si.waitSemaphoreCount = 1;
                si.pWaitSemaphores = &acquired[i];
                si.pWaitDstStageMask = &stages[i];
                si.commandBufferCount = 1;
                si.pCommandBuffers = &cmds[i];
                si.signalSemaphoreCount = 1;
                si.pSignalSemaphores = &waits[i];
                vkQueueSubmit(q, 1, &si, chains[i].fence);
            }
            auto present = [&](size_t from, size_t count) {
                VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
                pi.waitSemaphoreCount = (uint32_t)count;
                pi.pWaitSemaphores = &waits[from];
                pi.swapchainCount = (uint32_t)count;
                pi.pSwapchains = &swaps[from];
                pi.pImageIndices = &idx[from];
                return vkQueuePresentKHR(q, &pi);
            };
            VkResult r = VK_SUCCESS;
            if (separate) { for (size_t i = 0; i < used; i++) r = present(i, 1); }
            else r = present(0, used);
            if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
                out("%s: present failed (%d) at frame %d", name, r, f);
                return false;
            }
            times.push_back(ms_since(t));
        }
        double total = ms_since(start);
        std::sort(times.begin(), times.end());
        out("%s: %d frames, %.1f fps, frame ms median %.1f, 99%% %.1f, worst %.1f", name, n, n * 1000.0 / total, times[times.size() / 2], times[times.size() * 99 / 100], times.back());
        return true;
    }

    // Shader lab: the renderer's compute shaders the Adreno 650 refused (2026-11-01), after the fix (FbCommon.hlsli), and small shaders with one suspect each, in a
    // device set up as the game's. The driver's own compiler messages follow, from logcat.
    struct LabBinding { uint32_t set, binding; VkDescriptorType type; };
    struct LabShader { const char* name; const unsigned char* code; size_t size; std::vector<LabBinding> bindings; };

    std::vector<uint32_t> lab_words(const unsigned char* code, size_t size) {
        std::vector<uint32_t> w(size / 4);
        memcpy(w.data(), code, w.size() * 4);
        return w;
    }

    VkResult lab_pipeline(VkDevice dev, const std::vector<uint32_t>& code, std::vector<LabBinding> bindings, VkShaderStageFlags stages) {
        uint32_t set_count = 0;
        for (const auto& b : bindings) set_count = std::max(set_count, b.set + 1);
        std::vector<VkDescriptorSetLayout> layouts(set_count);
        for (uint32_t s = 0; s < set_count; s++) {
            std::vector<VkDescriptorSetLayoutBinding> vb;
            for (const auto& b : bindings) {
                if (b.set == s) vb.push_back({b.binding, b.type, 1, stages, nullptr});
            }
            VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            li.bindingCount = (uint32_t)vb.size();
            li.pBindings = vb.data();
            vkCreateDescriptorSetLayout(dev, &li, nullptr, &layouts[s]);
        }
        VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, 36};
        VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pi.setLayoutCount = set_count;
        pi.pSetLayouts = layouts.data();
        pi.pushConstantRangeCount = 1;
        pi.pPushConstantRanges = &pc;
        VkPipelineLayout layout;
        VkResult r = vkCreatePipelineLayout(dev, &pi, nullptr, &layout);
        if (r == VK_SUCCESS) {
            VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            mi.codeSize = code.size() * 4;
            mi.pCode = code.data();
            VkShaderModule module;
            r = vkCreateShaderModule(dev, &mi, nullptr, &module);
            if (r == VK_SUCCESS) {
                VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                ci.stage.module = module;
                ci.stage.pName = "CSMain";
                ci.layout = layout;
                VkPipeline pipeline = VK_NULL_HANDLE;
                r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
                if (pipeline) vkDestroyPipeline(dev, pipeline, nullptr);
                vkDestroyShaderModule(dev, module, nullptr);
            }
            vkDestroyPipelineLayout(dev, layout, nullptr);
        }
        for (auto l : layouts) vkDestroyDescriptorSetLayout(dev, l, nullptr);
        return r;
    }

    uint32_t lab_memory(VkPhysicalDevice gpu, uint32_t bits, VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties m;
        vkGetPhysicalDeviceMemoryProperties(gpu, &m);
        for (uint32_t i = 0; i < m.memoryTypeCount; i++) {
            if ((bits & (1u << i)) && (m.memoryTypes[i].propertyFlags & flags) == flags) return i;
        }
        return 0;
    }

    // Runs the renderer's texture decoder on a known TMEM (every byte below the palette 0x11, every palette colour 0xF801,
    // pure red with alpha set as RGBA16), and reads back the first pixel it wrote.
    void lab_decode(Out& out, VkPhysicalDevice gpu, VkDevice dev, uint32_t family, const char* name, const unsigned char* code, size_t size) {
        VkQueue queue;
        vkGetDeviceQueue(dev, family, 0, &queue);
        const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        auto make_buffer = [&](VkDeviceSize bytes, VkBufferUsageFlags usage, VkBuffer& b, VkDeviceMemory& mem) {
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = bytes;
            bi.usage = usage;
            vkCreateBuffer(dev, &bi, nullptr, &b);
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(dev, b, &req);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = lab_memory(gpu, req.memoryTypeBits, host);
            vkAllocateMemory(dev, &ai, nullptr, &mem);
            vkBindBufferMemory(dev, b, mem, 0);
        };
        auto make_image = [&](VkImageType type, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImage& img, VkDeviceMemory& mem, VkImageView& view) {
            VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ii.imageType = type;
            ii.format = format;
            ii.extent = {w, h, 1};
            ii.mipLevels = ii.arrayLayers = 1;
            ii.samples = VK_SAMPLE_COUNT_1_BIT;
            ii.usage = usage;
            vkCreateImage(dev, &ii, nullptr, &img);
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(dev, img, &req);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = lab_memory(gpu, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            vkAllocateMemory(dev, &ai, nullptr, &mem);
            vkBindImageMemory(dev, img, mem, 0);
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = img;
            vi.viewType = type == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D;
            vi.format = format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCreateImageView(dev, &vi, nullptr, &view);
        };

        VkBuffer staging, readback;
        VkDeviceMemory staging_mem, readback_mem;
        make_buffer(4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, staging_mem);
        make_buffer(16 * 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback, readback_mem);
        unsigned char* tmem;
        vkMapMemory(dev, staging_mem, 0, VK_WHOLE_SIZE, 0, (void**)&tmem);
        for (int i = 0; i < 4096; i++) tmem[i] = i < 0x800 ? 0x11 : (i & 1) ? 0x01 : 0xF8;
        vkUnmapMemory(dev, staging_mem);

        VkImage tmem_image, output;
        VkDeviceMemory tmem_mem, output_mem;
        VkImageView tmem_view, output_view;
        make_image(VK_IMAGE_TYPE_1D, VK_FORMAT_R8_UINT, 4096, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, tmem_image, tmem_mem, tmem_view);
        make_image(VK_IMAGE_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT, 8, 8, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, output, output_mem, output_view);

        VkDescriptorSetLayoutBinding bindings[] = {
            {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_ALL, nullptr},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_ALL, nullptr},
        };
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 2;
        li.pBindings = bindings;
        VkDescriptorSetLayout set_layout;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &set_layout);
        VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &set_layout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pc;
        VkPipelineLayout layout;
        vkCreatePipelineLayout(dev, &pli, nullptr, &layout);
        std::vector<uint32_t> words = lab_words(code, size);
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = words.size() * 4;
        mi.pCode = words.data();
        VkShaderModule module;
        vkCreateShaderModule(dev, &mi, nullptr, &module);
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "CSMain";
        ci.layout = layout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
        if (r != VK_SUCCESS) {
            out("decoder %s: pipeline refused %d", name, r);
        }
        else {
            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
            VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            dpi.maxSets = 1;
            dpi.poolSizeCount = 2;
            dpi.pPoolSizes = sizes;
            VkDescriptorPool pool;
            vkCreateDescriptorPool(dev, &dpi, nullptr, &pool);
            VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            dai.descriptorPool = pool;
            dai.descriptorSetCount = 1;
            dai.pSetLayouts = &set_layout;
            VkDescriptorSet set;
            vkAllocateDescriptorSets(dev, &dai, &set);
            VkDescriptorImageInfo tmem_info{VK_NULL_HANDLE, tmem_view, VK_IMAGE_LAYOUT_GENERAL};
            VkDescriptorImageInfo output_info{VK_NULL_HANDLE, output_view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet writes[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
            writes[0].dstSet = set; writes[0].dstBinding = 1; writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; writes[0].pImageInfo = &tmem_info;
            writes[1].dstSet = set; writes[1].dstBinding = 2; writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[1].pImageInfo = &output_info;
            vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);

            struct Case { const char* what; uint32_t fmt, siz, tlut, palette; };
            const Case cases[] = {
                {"CI4 RGBA16 palette 0", 2, 0, 2u << 14, 0},
                {"CI4 RGBA16 palette 4", 2, 0, 2u << 14, 4},
                {"CI8 RGBA16", 2, 1, 2u << 14, 0},
                {"CI4 IA16", 2, 0, 3u << 14, 0},
                {"RGBA16, no palette", 0, 2, 0, 0},
            };
            VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            cpi.queueFamilyIndex = family;
            VkCommandPool cmd_pool;
            vkCreateCommandPool(dev, &cpi, nullptr, &cmd_pool);
            for (const Case& c : cases) {
                VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                cai.commandPool = cmd_pool;
                cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                cai.commandBufferCount = 1;
                VkCommandBuffer cb;
                vkAllocateCommandBuffers(dev, &cai, &cb);
                VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                vkBeginCommandBuffer(cb, &begin);
                auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to) {
                    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
                    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                    b.oldLayout = from;
                    b.newLayout = to;
                    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.image = image;
                    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
                };
                barrier(tmem_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {4096, 1, 1};
                vkCmdCopyBufferToImage(cb, staging, tmem_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                barrier(tmem_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
                barrier(output, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
                const uint32_t constants[8] = {8, 8, c.fmt, c.siz, 0, 32, c.tlut, c.palette};
                vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), constants);
                vkCmdDispatch(cb, 1, 1, 1);
                barrier(output, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                copy.imageExtent = {8, 8, 1};
                vkCmdCopyImageToBuffer(cb, output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
                vkEndCommandBuffer(cb);
                VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                si.commandBufferCount = 1;
                si.pCommandBuffers = &cb;
                vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
                vkQueueWaitIdle(queue);
                float* px;
                vkMapMemory(dev, readback_mem, 0, VK_WHOLE_SIZE, 0, (void**)&px);
                out("decoder %-4s %-22s pixel %.2f %.2f %.2f %.2f", name, c.what, px[0], px[1], px[2], px[3]);
                vkUnmapMemory(dev, readback_mem);
            }
            vkDestroyCommandPool(dev, cmd_pool, nullptr);
            vkDestroyDescriptorPool(dev, pool, nullptr);
            vkDestroyPipeline(dev, pipeline, nullptr);
        }
        vkDestroyShaderModule(dev, module, nullptr);
        vkDestroyPipelineLayout(dev, layout, nullptr);
        vkDestroyDescriptorSetLayout(dev, set_layout, nullptr);
        vkDestroyImageView(dev, tmem_view, nullptr);
        vkDestroyImageView(dev, output_view, nullptr);
        vkDestroyImage(dev, tmem_image, nullptr);
        vkDestroyImage(dev, output, nullptr);
        vkFreeMemory(dev, tmem_mem, nullptr);
        vkFreeMemory(dev, output_mem, nullptr);
        vkDestroyBuffer(dev, staging, nullptr);
        vkDestroyBuffer(dev, readback, nullptr);
        vkFreeMemory(dev, staging_mem, nullptr);
        vkFreeMemory(dev, readback_mem, nullptr);
    }

    void shader_lab(Out& out) {
        const VkDescriptorType STB = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, UTB = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
            SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, TEX = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, IMG = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        const std::vector<LabBinding> write_set = {{0, 1, STB}, {1, 0, TEX}};
        const std::vector<LabBinding> read_set = {{0, 1, UTB}, {0, 2, UTB}, {0, 3, SB}, {1, 0, IMG}, {1, 1, IMG}, {1, 2, IMG}};
        const std::vector<LabShader> shaders = {
            {"Idle", lab_IdleCS, sizeof(lab_IdleCS), {}},
            {"plain write", lab_v0, sizeof(lab_v0), write_set},
            {"old swap", lab_v1, sizeof(lab_v1), write_set},
            {"multiply, shift", lab_v2, sizeof(lab_v2), write_set},
            {"multiply, divide", lab_v3, sizeof(lab_v3), write_set},
            {"push offset 32", lab_v4, sizeof(lab_v4), write_set},
            {"mask FF00 alone", lab_v5, sizeof(lab_v5), write_set},
            {"shift, no masks", lab_v6, sizeof(lab_v6), write_set},
            {"new FbWriteDepth", lab_FbWriteDepthCS, sizeof(lab_FbWriteDepthCS), write_set},
            {"new FbWriteDepthMS", lab_FbWriteDepthCSMS, sizeof(lab_FbWriteDepthCSMS), write_set},
            {"new FbWriteColor", lab_FbWriteColorCS, sizeof(lab_FbWriteColorCS), write_set},
            {"new FbReadAnyFull", lab_FbReadAnyFullCS, sizeof(lab_FbReadAnyFullCS), read_set},
            {"new FbReadAnyChanges", lab_FbReadAnyChangesCS, sizeof(lab_FbReadAnyChangesCS), read_set},
        };

        out("\n== shader lab ==");
        const uint32_t apis[] = {VK_API_VERSION_1_2};
        for (uint32_t api : apis) {
            VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
            app.apiVersion = api;
            VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            ii.pApplicationInfo = &app;
            VkInstance inst;
            if (vkCreateInstance(&ii, nullptr, &inst) != VK_SUCCESS) { out("instance 1.%u failed", VK_VERSION_MINOR(api)); continue; }
            uint32_t n = 1;
            VkPhysicalDevice gpu;
            vkEnumeratePhysicalDevices(inst, &n, &gpu);
            uint32_t families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families, nullptr);
            std::vector<VkQueueFamilyProperties> fp(families);
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families, fp.data());
            uint32_t family = 0;
            while (family + 1 < families && !(fp[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) family++;
            uint32_t ext_count = 0;
            vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ext_count, nullptr);
            std::vector<VkExtensionProperties> have(ext_count);
            vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ext_count, have.data());

            for (int gamelike = 1; gamelike < 2; gamelike++) {
                float priority = 1;
                VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                qi.queueFamilyIndex = family;
                qi.queueCount = 1;
                qi.pQueuePriorities = &priority;
                // As the game: every feature on, its optional extensions and their features.
                VkPhysicalDeviceDescriptorIndexingFeatures indexing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
                VkPhysicalDeviceScalarBlockLayoutFeatures scalar{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES};
                VkPhysicalDeviceRobustness2FeaturesEXT robust{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
                VkPhysicalDeviceBufferDeviceAddressFeatures address{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
                VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
                std::vector<const char*> exts;
                VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
                di.queueCreateInfoCount = 1;
                di.pQueueCreateInfos = &qi;
                if (gamelike) {
                    const char* wanted[] = {VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME, VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
                        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME, VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME,
                        VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME, VK_KHR_MAINTENANCE3_EXTENSION_NAME};
                    for (const char* w : wanted) {
                        for (const auto& e : have) if (!strcmp(e.extensionName, w)) exts.push_back(w);
                    }
                    indexing.pNext = &scalar;
                    scalar.pNext = &robust;
                    robust.pNext = &address;
                    f2.pNext = &indexing;
                    vkGetPhysicalDeviceFeatures2(gpu, &f2);
                    di.pNext = &indexing;
                    di.pEnabledFeatures = &f2.features;
                    di.enabledExtensionCount = (uint32_t)exts.size();
                    di.ppEnabledExtensionNames = exts.data();
                }
                VkDevice dev;
                VkResult r = vkCreateDevice(gpu, &di, nullptr, &dev);
                if (r != VK_SUCCESS) { out("instance 1.%u, %s device: %d", VK_VERSION_MINOR(api), gamelike ? "game's" : "plain", r); continue; }
                out("-- instance 1.%u, %s device (%zu extensions) --", VK_VERSION_MINOR(api), gamelike ? "game's" : "plain", exts.size());

                auto report = [&](const char* shader, const char* change, VkResult res) {
                    out("%-20s %-18s %s", shader, change, res == VK_SUCCESS ? "ok" : ("REFUSED " + std::to_string((int)res)).c_str());
                };
                for (const auto& s : shaders) {
                    std::vector<uint32_t> w = lab_words(s.code, s.size);
                    report(s.name, "stock", lab_pipeline(dev, w, s.bindings, VK_SHADER_STAGE_ALL));
                }
                // One texture, from a pool of one: the set the game couldn't allocate, as its report describes it.
                {
                    VkDescriptorSetLayoutBinding b{0, TEX, 1, VK_SHADER_STAGE_ALL, nullptr};
                    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
                    li.bindingCount = 1;
                    li.pBindings = &b;
                    VkDescriptorSetLayout layout;
                    vkCreateDescriptorSetLayout(dev, &li, nullptr, &layout);
                    VkDescriptorPoolSize size{TEX, 1};
                    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
                    pi.maxSets = 1;
                    pi.poolSizeCount = 1;
                    pi.pPoolSizes = &size;
                    VkDescriptorPool pool;
                    vkCreateDescriptorPool(dev, &pi, nullptr, &pool);
                    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                    ai.descriptorPool = pool;
                    ai.descriptorSetCount = 1;
                    ai.pSetLayouts = &layout;
                    VkDescriptorSet set;
                    report("one texture set", "pool of one", vkAllocateDescriptorSets(dev, &ai, &set));
                    vkDestroyDescriptorPool(dev, pool, nullptr);
                    vkDestroyDescriptorSetLayout(dev, layout, nullptr);
                }
                lab_decode(out, gpu, dev, family, "old", lab_OldDecode, sizeof(lab_OldDecode));
                lab_decode(out, gpu, dev, family, "new", lab_TextureDecodeCS, sizeof(lab_TextureDecodeCS));
                vkDestroyDevice(dev, nullptr);
            }
            vkDestroyInstance(inst, nullptr);
        }

        out("\n== driver messages ==");
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "logcat -d -t 2000 --pid=%d 2>&1", getpid());
        if (FILE* p = popen(cmd, "r")) {
            char line[900];
            int lines = 0;
            while (fgets(line, sizeof(line), p) && lines < 80) {
                if (strstr(line, "BlitzProbeNative")) continue;
                if (strstr(line, "dreno") || strstr(line, "QGL") || strstr(line, "qgl") || strstr(line, "ulkan") || strstr(line, "rror") || strstr(line, "ailed") || strstr(line, "compile") || strstr(line, "link")) {
                    line[strcspn(line, "\n")] = 0;
                    out("%s", line);
                    lines++;
                }
            }
            pclose(p);
        }
    }

    std::string vulkan(JNIEnv* env, jobjectArray surfaces) {
        Out out;
        uint32_t loader = VK_API_VERSION_1_0;
        vkEnumerateInstanceVersion(&loader);
        out("loader %u.%u.%u", VK_VERSION_MAJOR(loader), VK_VERSION_MINOR(loader), VK_VERSION_PATCH(loader));

        const char* inst_ext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "BlitzProbe";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ii.pApplicationInfo = &app;
        ii.enabledExtensionCount = 2;
        ii.ppEnabledExtensionNames = inst_ext;
        VkInstance inst;
        VkResult r = vkCreateInstance(&ii, nullptr, &inst);
        if (r != VK_SUCCESS) { out("vkCreateInstance FAILED: %d", r); return out.s; }

        out("instance ok");
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(inst, &count, nullptr);
        std::vector<VkPhysicalDevice> gpus(count);
        vkEnumeratePhysicalDevices(inst, &count, gpus.data());
        out("gpus: %u", count);
        if (count == 0) return out.s;
        VkPhysicalDevice gpu = gpus[0];

        VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &driver;
        vkGetPhysicalDeviceProperties2(gpu, &p2);
        const VkPhysicalDeviceProperties& p = p2.properties;
        out("gpu \"%s\" vendor 0x%x device 0x%x api %u.%u.%u driver 0x%x", p.deviceName, p.vendorID, p.deviceID, VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion), p.driverVersion);
        out("driver \"%s\" \"%s\"", driver.driverName, driver.driverInfo);

        VkPhysicalDeviceDescriptorIndexingFeatures indexing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &indexing;
        vkGetPhysicalDeviceFeatures2(gpu, &f2);
#define FEATURE(name) out("  %-40s %s", #name, f2.features.name ? "yes" : "NO")
        out("features RT64 leans on:");
        FEATURE(dualSrcBlend); FEATURE(independentBlend); FEATURE(sampleRateShading); FEATURE(fragmentStoresAndAtomics);
        FEATURE(shaderInt64); FEATURE(shaderInt16); FEATURE(shaderFloat64); FEATURE(depthClamp); FEATURE(depthBiasClamp); FEATURE(geometryShader);
        FEATURE(multiDrawIndirect); FEATURE(drawIndirectFirstInstance); FEATURE(fillModeNonSolid); FEATURE(samplerAnisotropy);
        FEATURE(shaderSampledImageArrayDynamicIndexing); FEATURE(shaderStorageImageMultisample); FEATURE(shaderClipDistance);
        FEATURE(shaderStorageImageReadWithoutFormat); FEATURE(shaderStorageImageWriteWithoutFormat);
#undef FEATURE
#define INDEXING(name) out("  %-40s %s", #name, indexing.name ? "yes" : "NO")
        INDEXING(runtimeDescriptorArray); INDEXING(descriptorBindingPartiallyBound); INDEXING(descriptorBindingVariableDescriptorCount);
        INDEXING(shaderSampledImageArrayNonUniformIndexing); INDEXING(descriptorBindingSampledImageUpdateAfterBind);
#undef INDEXING
        const VkPhysicalDeviceLimits& l = p.limits;
        out("limits: image2D %u, samplers/stage %u, sampled/stage %u, ubo/stage %u, ssbo/stage %u, push %u, framebuffer msaa 0x%x, sampled msaa 0x%x, subpixel %u, viewports %u",
            l.maxImageDimension2D, l.maxPerStageDescriptorSamplers, l.maxPerStageDescriptorSampledImages, l.maxPerStageDescriptorUniformBuffers, l.maxPerStageDescriptorStorageBuffers, l.maxPushConstantsSize, l.framebufferColorSampleCounts, l.sampledImageColorSampleCounts, l.subPixelPrecisionBits, l.maxViewports);

        VkPhysicalDeviceMemoryProperties mem;
        vkGetPhysicalDeviceMemoryProperties(gpu, &mem);
        for (uint32_t h = 0; h < mem.memoryHeapCount; h++) out("heap %u: %llu MB%s", h, (unsigned long long)(mem.memoryHeaps[h].size >> 20), (mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " device-local" : "");

        vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> exts(count);
        vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, exts.data());
        std::string names;
        for (const auto& e : exts) { names += e.extensionName; names += ' '; }
        out("extensions (%u):", count);
        for (size_t at = 0; at < names.size(); at += 900) out("  %s", names.substr(at, 900).c_str());

        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families.data());

        // Surfaces first: the queue must present to every one.
        std::vector<Chain> chains;
        jsize n = env->GetArrayLength(surfaces);
        for (jsize i = 0; i < n; i++) {
            jobject s = env->GetObjectArrayElement(surfaces, i);
            Chain c;
            c.window = ANativeWindow_fromSurface(env, s);
            if (c.window == nullptr) { out("screen %d: no native window", i); continue; }
            VkAndroidSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
            sci.window = c.window;
            r = vkCreateAndroidSurfaceKHR(inst, &sci, nullptr, &c.surface);
            out("screen %d: window %dx%d, surface %s", i, ANativeWindow_getWidth(c.window), ANativeWindow_getHeight(c.window), r == VK_SUCCESS ? "ok" : "FAILED");
            if (r == VK_SUCCESS) chains.push_back(c);
        }
        int family = -1;
        for (uint32_t i = 0; i < families.size() && family < 0; i++) {
            if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            bool all = true;
            for (const Chain& c : chains) {
                VkBool32 ok = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, c.surface, &ok);
                all = all && ok;
            }
            if (all) family = (int)i;
        }
        out("queue family presenting to all %zu screens: %d of %zu", chains.size(), family, families.size());
        if (family < 0) return out.s;

        float priority = 1.0f;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = (uint32_t)family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        const char* dev_ext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &qi;
        di.enabledExtensionCount = 1;
        di.ppEnabledExtensionNames = dev_ext;
        VkDevice dev;
        r = vkCreateDevice(gpu, &di, nullptr, &dev);
        out("device: %s", r == VK_SUCCESS ? "ok" : "FAILED");
        if (r != VK_SUCCESS) return out.s;
        VkQueue q;
        vkGetDeviceQueue(dev, (uint32_t)family, 0, &q);
        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpi.queueFamilyIndex = (uint32_t)family;
        VkCommandPool pool;
        vkCreateCommandPool(dev, &cpi, nullptr, &pool);

        size_t made = 0;
        for (size_t i = 0; i < chains.size(); i++) {
            Chain& c = chains[i];
            VkSurfaceCapabilitiesKHR caps;
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, c.surface, &caps);
            uint32_t fc = 0, pc = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, c.surface, &fc, nullptr);
            std::vector<VkSurfaceFormatKHR> formats(fc);
            vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, c.surface, &fc, formats.data());
            vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, c.surface, &pc, nullptr);
            std::vector<VkPresentModeKHR> modes(pc);
            vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, c.surface, &pc, modes.data());
            std::string fs, ms;
            for (const auto& f : formats) fs += std::to_string((int)f.format) + " ";
            for (auto m : modes) ms += std::to_string((int)m) + " ";
            out("screen %zu: %ux%u, images %u-%u, transform 0x%x, usage 0x%x, formats %s, present modes %s", i, caps.currentExtent.width, caps.currentExtent.height, caps.minImageCount, caps.maxImageCount, caps.currentTransform, caps.supportedUsageFlags, fs.c_str(), ms.c_str());
            VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
            sc.surface = c.surface;
            sc.minImageCount = std::max(caps.minImageCount, 2u);
            sc.imageFormat = formats[0].format;
            sc.imageColorSpace = formats[0].colorSpace;
            sc.imageExtent = caps.currentExtent;
            sc.imageArrayLayers = 1;
            sc.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            sc.preTransform = caps.currentTransform;
            sc.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
            sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
            sc.clipped = VK_TRUE;
            out("screen %zu: creating swapchain", i);
            r = vkCreateSwapchainKHR(dev, &sc, nullptr, &c.swapchain);
            out("screen %zu swapchain: %s (%d)", i, r == VK_SUCCESS ? "ok" : "FAILED", r);
            if (r != VK_SUCCESS) continue;
            made++;
            uint32_t ic = 0;
            vkGetSwapchainImagesKHR(dev, c.swapchain, &ic, nullptr);
            c.images.resize(ic);
            vkGetSwapchainImagesKHR(dev, c.swapchain, &ic, c.images.data());
            c.extent = caps.currentExtent;
            VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            vkCreateSemaphore(dev, &semi, nullptr, &c.acquired);
            vkCreateSemaphore(dev, &semi, nullptr, &c.drawn);
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkCreateFence(dev, &fi, nullptr, &c.fence);
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            vkAllocateCommandBuffers(dev, &ai, &c.cmd);
        }
        if (made != chains.size()) {
            out("couldn't make a swapchain on every screen");
            return out.s;
        }
        // A stuck screen stalls the queue for everything after it, so each screen goes alone first.
        std::vector<bool> ok(chains.size());
        std::vector<size_t> working;
        for (size_t i = 0; i < chains.size(); i++) {
            std::string name = "screen " + std::to_string(i) + " alone";
            ok[i] = run_frames(out, dev, q, chains, {i}, false, 180, name.c_str());
            if (ok[i]) working.push_back(i);
            else break;
        }
        if (working.size() == chains.size() && chains.size() > 1) {
            // Taking turns, in case drawing both in one frame is what stalls (the emulator's does).
            auto t = Clock::now();
            int turns = 0;
            Out quiet;
            for (; turns < 120; turns++) {
                size_t screen = (size_t)turns % chains.size();
                if (!run_frames(quiet, dev, q, chains, {screen}, false, 1, "turn")) break;
            }
            out("screens taking turns: %d of 120 frames, %.1f fps, %s", turns, turns * 1000.0 / ms_since(t), turns == 120 ? "ok" : quiet.s.c_str());
            run_frames(out, dev, q, chains, working, false, 180, "both screens, one present");
            run_frames(out, dev, q, chains, working, true, 180, "both screens, separate presents");
            run_frames(out, dev, q, chains, working, false, 60, "both screens again");
        }
        out("VERDICT dual-source blending: %s; screens drawn by one device: %zu", f2.features.dualSrcBlend ? "reported" : "NOT reported", chains.size());
        shader_lab(out);
        return out.s;
    }
}

extern "C" JNIEXPORT void JNICALL Java_com_blitzrecomp_probe_ProbeActivity_nativeTrace(JNIEnv* env, jclass, jstring path) {
    const char* p = env->GetStringUTFChars(path, nullptr);
    snprintf(trace_path, sizeof(trace_path), "%s", p);
    env->ReleaseStringUTFChars(path, p);
}

extern "C" JNIEXPORT jstring JNICALL Java_com_blitzrecomp_probe_ProbeActivity_nativeMachine(JNIEnv* env, jclass) {
    Out out;
    memory(out);
    cpu(out);
    return env->NewStringUTF(out.s.c_str());
}

extern "C" JNIEXPORT jstring JNICALL Java_com_blitzrecomp_probe_ProbeActivity_nativeVulkan(JNIEnv* env, jclass, jobjectArray surfaces) {
    std::string s = vulkan(env, surfaces);
    return env->NewStringUTF(s.c_str());
}
