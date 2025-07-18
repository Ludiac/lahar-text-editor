module;

#include "macros.hpp"
#include "primitive_types.hpp"

export module vulkan_app:VMA;

import vulkan_hpp;
import std;
import vk_mem_alloc_hpp;

export struct VmaBuffer {
  vma::Allocator allocator{nullptr};
  vk::Buffer buffer{nullptr};
  vma::Allocation allocation{nullptr};
  vma::AllocationInfo allocationInfo{};
  vk::DeviceSize size = 0;
  void *pMappedData = nullptr;

  vk::Buffer &operator*() { return buffer; }
  const vk::Buffer &operator*() const { return buffer; }

  VmaBuffer() = default;

  VmaBuffer(vma::Allocator allocator, vk::Buffer buffer, vma::Allocation allocation,
            const vma::AllocationInfo &allocInfo, vk::DeviceSize size)
      : allocator(allocator), buffer(buffer), allocation(allocation), allocationInfo(allocInfo),
        size(size) {

    if (allocInfo.pMappedData != nullptr) {
      pMappedData = allocInfo.pMappedData;
    }
  }

  ~VmaBuffer() { release(); }

  VmaBuffer(VmaBuffer &&other) noexcept
      : allocator(other.allocator), buffer(other.buffer), allocation(other.allocation),
        allocationInfo(other.allocationInfo), size(other.size),
        pMappedData(other.pMappedData) {

    other.allocator = nullptr;
    other.buffer = nullptr;
    other.allocation = nullptr;
    other.pMappedData = nullptr;
    other.size = 0;
  }

  VmaBuffer &operator=(VmaBuffer &&other) noexcept {
    if (this != &other) {
      release();

      allocator = other.allocator;
      buffer = other.buffer;
      allocation = other.allocation;
      allocationInfo = other.allocationInfo;
      size = other.size;
      pMappedData = other.pMappedData;

      other.allocator = nullptr;
      other.buffer = nullptr;
      other.allocation = nullptr;
      other.pMappedData = nullptr;
      other.size = 0;
    }
    return *this;
  }

  VmaBuffer(const VmaBuffer &) = delete;
  VmaBuffer &operator=(const VmaBuffer &) = delete;

  void release() {
    if (allocator && buffer && allocation) {

      allocator.destroyBuffer(buffer, allocation);
    }
    allocator = nullptr;
    buffer = nullptr;
    allocation = nullptr;
    pMappedData = nullptr;
    size = 0;
    allocationInfo = vma::AllocationInfo{};
  }

  [[nodiscard]] vk::Buffer get() const { return buffer; }
  [[nodiscard]] vma::Allocation getAllocation() const { return allocation; }
  [[nodiscard]] const vma::AllocationInfo &getAllocationInfo() const { return allocationInfo; }
  [[nodiscard]] vk::DeviceSize getSize() const { return size; }
  [[nodiscard]] void *getMappedData() const { return pMappedData; }

  explicit operator bool() const { return buffer && allocation; }
};

export struct VmaImage {
  vma::Allocator allocator = nullptr;
  vk::Image image = nullptr;
  vma::Allocation allocation = nullptr;
  vma::AllocationInfo allocationInfo{};
  vk::Format format = vk::Format::eUndefined;
  vk::Extent3D extent = {.width = 0, .height = 0, .depth = 0};

  VmaImage() = default;

  VmaImage(vma::Allocator allocator, vk::Image image, vma::Allocation allocation,
           const vma::AllocationInfo &allocInfo, vk::Format format, vk::Extent3D extent)
      : allocator(allocator), image(image), allocation(allocation), allocationInfo(allocInfo),
        format(format), extent(extent) {}

  ~VmaImage() { release(); }

  VmaImage(VmaImage &&other) noexcept
      : allocator(other.allocator), image(other.image), allocation(other.allocation),
        allocationInfo(other.allocationInfo), format(other.format), extent(other.extent) {
    other.allocator = nullptr;
    other.image = nullptr;
    other.allocation = nullptr;
    other.format = vk::Format::eUndefined;
    other.extent = {.width = 0, .height = 0, .depth = 0};
  }

  VmaImage &operator=(VmaImage &&other) noexcept {
    if (this != &other) {
      release();
      allocator = other.allocator;
      image = other.image;
      allocation = other.allocation;
      allocationInfo = other.allocationInfo;
      format = other.format;
      extent = other.extent;

      other.allocator = nullptr;
      other.image = nullptr;
      other.allocation = nullptr;
      other.format = vk::Format::eUndefined;
      other.extent = {.width = 0, .height = 0, .depth = 0};
    }
    return *this;
  }

  VmaImage(const VmaImage &) = delete;
  VmaImage &operator=(const VmaImage &) = delete;

  void release() {
    if (allocator && image && allocation) {
      allocator.destroyImage(image, allocation);
    }
    allocator = nullptr;
    image = nullptr;
    allocation = nullptr;
    format = vk::Format::eUndefined;
    extent = {.width = 0, .height = 0, .depth = 0};
    allocationInfo = vma::AllocationInfo{};
  }

  [[nodiscard]] vk::Image get() const { return image; }
  [[nodiscard]] vma::Allocation getAllocation() const { return allocation; }
  [[nodiscard]] const vma::AllocationInfo &getAllocationInfo() const { return allocationInfo; }
  [[nodiscard]] vk::Format getFormat() const { return format; }
  [[nodiscard]] vk::Extent3D getExtent() const { return extent; }

  explicit operator bool() const { return image && allocation; }
};

export std::expected<vma::Allocator, std::string>
createVmaAllocator(vk::Instance instance, vk::PhysicalDevice physicalDevice, vk::Device device) {
  uint32_t apiVersion = vk::makeApiVersion(0, 1, 0, 0);
  auto instanceVersionResult = vk::enumerateInstanceVersion();
  if (instanceVersionResult.result == vk::Result::eSuccess) {
    apiVersion = instanceVersionResult.value;
  }
  vma::AllocatorCreateInfo allocatorInfo = {
      .physicalDevice = static_cast<vk::PhysicalDevice>(physicalDevice),
      .device = static_cast<vk::Device>(device),
      .instance = static_cast<vk::Instance>(instance),
      .vulkanApiVersion = apiVersion,
  };

  vma::Allocator allocatorHandle;
  vk::Result result = vma::createAllocator(&allocatorInfo, &allocatorHandle);
  if (result != vk::Result::eSuccess) {
    return std::unexpected("Failed to create VMA allocator: " + vk::to_string(result));
  }
  return allocatorHandle;
}
