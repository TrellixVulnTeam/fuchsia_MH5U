// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#define PRINT_STDERR(format, ...) \
  fprintf(stderr, "%s:%d " format "\n", __FILE__, __LINE__, ##__VA_ARGS__)

namespace {

class VkPriorityTest {
 public:
  explicit VkPriorityTest(bool different_priority) : different_priority_(different_priority) {}

  bool Initialize();
  bool Exec();

 private:
  bool InitVulkan();
  bool InitCommandPool();
  bool InitCommandBuffer(VkCommandBuffer* command_buffer, uint32_t executions);

  bool is_initialized_ = false;
  VkPhysicalDevice vk_physical_device_;
  VkDevice vk_device_;
  VkQueue low_prio_vk_queue_;
  VkQueue high_prio_vk_queue_;
  bool different_priority_;

  VkCommandPool vk_command_pool_;
  VkCommandBuffer low_prio_vk_command_buffer_;
  VkCommandBuffer high_prio_vk_command_buffer_;
  uint32_t low_priority_execution_count_ = 1000;
};

bool VkPriorityTest::Initialize() {
  if (is_initialized_)
    return false;

  if (!InitVulkan()) {
    PRINT_STDERR("failed to initialize Vulkan");
    return false;
  }

  if (!InitCommandPool()) {
    PRINT_STDERR("InitCommandPool failed");
    return false;
  }

  if (!InitCommandBuffer(&low_prio_vk_command_buffer_, low_priority_execution_count_)) {
    PRINT_STDERR("InitImage failed");
    return false;
  }

  if (!InitCommandBuffer(&high_prio_vk_command_buffer_, 1)) {
    PRINT_STDERR("InitImage failed");
    return false;
  }

  is_initialized_ = true;

  return true;
}

bool VkPriorityTest::InitVulkan() {
  VkInstanceCreateInfo create_info{
      VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,  // VkStructureType             sType;
      nullptr,                                 // const void*                 pNext;
      0,                                       // VkInstanceCreateFlags       flags;
      nullptr,                                 // const VkApplicationInfo*    pApplicationInfo;
      0,                                       // uint32_t                    enabledLayerCount;
      nullptr,                                 // const char* const*          ppEnabledLayerNames;
      0,                                       // uint32_t                    enabledExtensionCount;
      nullptr,  // const char* const*          ppEnabledExtensionNames;
  };
  VkAllocationCallbacks* allocation_callbacks = nullptr;
  VkInstance instance;
  VkResult result;

  if ((result = vkCreateInstance(&create_info, allocation_callbacks, &instance)) != VK_SUCCESS) {
    PRINT_STDERR("vkCreateInstance failed %d", result);
    return false;
  }

  uint32_t physical_device_count;
  if ((result = vkEnumeratePhysicalDevices(instance, &physical_device_count, nullptr)) !=
      VK_SUCCESS) {
    PRINT_STDERR("vkEnumeratePhysicalDevices failed %d", result);
    return false;
  }

  if (physical_device_count < 1) {
    PRINT_STDERR("unexpected physical_device_count %d", physical_device_count);
    return false;
  }

  std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
  if ((result = vkEnumeratePhysicalDevices(instance, &physical_device_count,
                                           physical_devices.data())) != VK_SUCCESS) {
    PRINT_STDERR("vkEnumeratePhysicalDevices failed %d", result);
    return false;
  }

  VkPhysicalDeviceProperties properties;
  vkGetPhysicalDeviceProperties(physical_devices[0], &properties);
  if (properties.vendorID == 0x13b5 && properties.deviceID >= 0x1000) {
    printf("Upping low priority execution count for ARM Bifrost GPU");
    // With the default execution count the test completes too quickly and
    // the commands won't be preempted.
    low_priority_execution_count_ = 100000;
  }

  uint32_t queue_family_count;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_devices[0], &queue_family_count, nullptr);

  if (queue_family_count < 1) {
    PRINT_STDERR("invalid queue_family_count %d", queue_family_count);
    return false;
  }

  std::vector<VkQueueFamilyProperties> queue_family_properties(queue_family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_devices[0], &queue_family_count,
                                           queue_family_properties.data());

  int32_t queue_family_index = -1;
  for (uint32_t i = 0; i < queue_family_count; i++) {
    if (queue_family_properties[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
      queue_family_index = i;
      break;
    }
  }

  if (queue_family_index < 0) {
    PRINT_STDERR("couldn't find an appropriate queue");
    return false;
  }

  if (queue_family_properties[queue_family_index].queueCount < 2) {
    PRINT_STDERR("Need 2 queues to use priorities");
    return false;
  }

  float queue_priorities[2] = {1.0, 1.0};
  if (different_priority_) {
    queue_priorities[0] = 0.0;
  }

  VkDeviceQueueCreateInfo queue_create_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                               .pNext = nullptr,
                                               .flags = 0,
                                               .queueFamilyIndex = 0,
                                               .queueCount = 2,
                                               .pQueuePriorities = queue_priorities};

  std::vector<const char*> enabled_extension_names;

  VkDeviceCreateInfo createInfo = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_create_info,
      .enabledLayerCount = 0,
      .ppEnabledLayerNames = nullptr,
      .enabledExtensionCount = static_cast<uint32_t>(enabled_extension_names.size()),
      .ppEnabledExtensionNames = enabled_extension_names.data(),
      .pEnabledFeatures = nullptr};
  VkDevice vkdevice;

  if ((result = vkCreateDevice(physical_devices[0], &createInfo, nullptr /* allocationcallbacks */,
                               &vkdevice)) != VK_SUCCESS) {
    PRINT_STDERR("vkCreateDevice failed: %d", result);
    return false;
  }

  vk_physical_device_ = physical_devices[0];
  vk_device_ = vkdevice;

  vkGetDeviceQueue(vkdevice, queue_family_index, 0, &low_prio_vk_queue_);
  vkGetDeviceQueue(vkdevice, queue_family_index, 1, &high_prio_vk_queue_);

  return true;
}

bool VkPriorityTest::InitCommandPool() {
  VkCommandPoolCreateInfo command_pool_create_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .queueFamilyIndex = 0,
  };
  VkResult result;
  if ((result = vkCreateCommandPool(vk_device_, &command_pool_create_info, nullptr,
                                    &vk_command_pool_)) != VK_SUCCESS) {
    PRINT_STDERR("vkCreateCommandPool failed: %d", result);
    return false;
  }
  return true;
}

bool VkPriorityTest::InitCommandBuffer(VkCommandBuffer* command_buffer, uint32_t executions) {
  VkCommandBufferAllocateInfo command_buffer_create_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .pNext = nullptr,
      .commandPool = vk_command_pool_,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1};
  VkResult result;
  if ((result = vkAllocateCommandBuffers(vk_device_, &command_buffer_create_info,
                                         command_buffer)) != VK_SUCCESS) {
    PRINT_STDERR("vkAllocateCommandBuffers failed: %d", result);
    return false;
  }

  VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .pNext = nullptr,
      .flags = 0,
      .pInheritanceInfo = nullptr,  // ignored for primary buffers
  };
  if ((result = vkBeginCommandBuffer(*command_buffer, &begin_info)) != VK_SUCCESS) {
    PRINT_STDERR("vkBeginCommandBuffer failed: %d", result);
    return false;
  }

  VkShaderModule compute_shader_module_;
  VkShaderModuleCreateInfo sh_info = {};
  sh_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;

#include "priority.comp.h"
  sh_info.codeSize = sizeof(priority_comp);
  sh_info.pCode = priority_comp;
  if ((result = vkCreateShaderModule(vk_device_, &sh_info, NULL, &compute_shader_module_)) !=
      VK_SUCCESS) {
    PRINT_STDERR("vkCreateShaderModule failed: %d", result);
    return false;
  }

  VkPipelineLayout layout;

  VkPipelineLayoutCreateInfo pipeline_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .setLayoutCount = 0,
      .pSetLayouts = nullptr,
      .pushConstantRangeCount = 0,
      .pPushConstantRanges = nullptr};

  if ((result = vkCreatePipelineLayout(vk_device_, &pipeline_create_info, nullptr, &layout)) !=
      VK_SUCCESS) {
    PRINT_STDERR("vkCreatePipelineLayout failed: %d", result);
    return false;
  }

  VkPipeline compute_pipeline;

  VkComputePipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = compute_shader_module_,
                .pName = "main",
                .pSpecializationInfo = nullptr},
      .layout = layout,
      .basePipelineHandle = VK_NULL_HANDLE,
      .basePipelineIndex = 0};

  if ((result = vkCreateComputePipelines(vk_device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                         &compute_pipeline)) != VK_SUCCESS) {
    PRINT_STDERR("vkCreateComputePipelines failed: %d", result);
    return false;
  }

  vkCmdBindPipeline(*command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline);
  vkCmdDispatch(*command_buffer, 1000, executions, 10);

  if ((result = vkEndCommandBuffer(*command_buffer)) != VK_SUCCESS) {
    PRINT_STDERR("vkEndCommandBuffer failed: %d", result);
    return false;
  }

  return true;
}

bool VkPriorityTest::Exec() {
  VkResult result;
  result = vkQueueWaitIdle(low_prio_vk_queue_);
  if (result != VK_SUCCESS) {
    PRINT_STDERR("vkQueueWaitIdle failed with result %d", result);
    return false;
  }
  result = vkQueueWaitIdle(high_prio_vk_queue_);
  if (result != VK_SUCCESS) {
    PRINT_STDERR("vkQueueWaitIdle failed with result %d", result);
    return false;
  }

  VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .pNext = nullptr,
      .waitSemaphoreCount = 0,
      .pWaitSemaphores = nullptr,
      .pWaitDstStageMask = nullptr,
      .commandBufferCount = 1,
      .pCommandBuffers = &low_prio_vk_command_buffer_,
      .signalSemaphoreCount = 0,
      .pSignalSemaphores = nullptr,
  };

  auto low_prio_start_time = std::chrono::steady_clock::now();
  if ((result = vkQueueSubmit(low_prio_vk_queue_, 1, &submit_info, VK_NULL_HANDLE)) != VK_SUCCESS) {
    PRINT_STDERR("vkQueueSubmit failed: %d", result);
    return false;
  }

  std::chrono::steady_clock::time_point low_prio_end_time;
  auto low_priority_future = std::async(std::launch::async, [this, &low_prio_end_time]() {
    VkResult result;

    if ((result = vkQueueWaitIdle(low_prio_vk_queue_)) != VK_SUCCESS) {
      PRINT_STDERR("vkQueueWaitIdle failed: %d", result);
      return false;
    }
    low_prio_end_time = std::chrono::steady_clock::now();
    return true;
  });
  // Should be enough time for the first queue to start executing.
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  VkSubmitInfo high_prio_submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .pNext = nullptr,
      .waitSemaphoreCount = 0,
      .pWaitSemaphores = nullptr,
      .pWaitDstStageMask = nullptr,
      .commandBufferCount = 1,
      .pCommandBuffers = &high_prio_vk_command_buffer_,
      .signalSemaphoreCount = 0,
      .pSignalSemaphores = nullptr,
  };

  auto high_prio_start_time = std::chrono::steady_clock::now();
  if ((result = vkQueueSubmit(high_prio_vk_queue_, 1, &high_prio_submit_info, VK_NULL_HANDLE)) !=
      VK_SUCCESS) {
    PRINT_STDERR("vkQueueSubmit failed: %d", result);
    return false;
  }

  std::chrono::steady_clock::time_point high_prio_end_time;
  auto high_priority_future = std::async(std::launch::async, [this, &high_prio_end_time]() {
    VkResult result;
    if ((result = vkQueueWaitIdle(high_prio_vk_queue_)) != VK_SUCCESS) {
      PRINT_STDERR("vkQueueWaitIdle failed: %d", result);
      return false;
    }
    high_prio_end_time = std::chrono::steady_clock::now();
    return true;
  });

  high_priority_future.wait();
  low_priority_future.wait();
  if (!high_priority_future.get() || !low_priority_future.get()) {
    PRINT_STDERR("Queue wait failed");
    return false;
  }
  auto high_prio_duration = high_prio_end_time - high_prio_start_time;
  printf("first vkQueueWaitIdle finished duration: %lld",
         std::chrono::duration_cast<std::chrono::milliseconds>(high_prio_duration).count());
  auto low_prio_duration = low_prio_end_time - low_prio_start_time;
  printf("second vkQueueWaitIdle finished duration: %lld",
         std::chrono::duration_cast<std::chrono::milliseconds>(low_prio_duration).count());

  if (different_priority_) {
    // Depends on the precise scheduling, so may sometimes fail.
    EXPECT_LE(high_prio_duration, low_prio_duration / 10);
  } else {
    // In this case they actually have equal priorities, but the "low priority" one has more
    // work and should be context-switched away from.
    EXPECT_LE(high_prio_duration, low_prio_duration);
  }

  return true;
}

TEST(Vulkan, Priority) {
  VkPriorityTest test(true);
  ASSERT_TRUE(test.Initialize());
  ASSERT_TRUE(test.Exec());
}

TEST(Vulkan, EqualPriority) {
  VkPriorityTest test(false);
  ASSERT_TRUE(test.Initialize());
  ASSERT_TRUE(test.Exec());
}

}  // namespace
