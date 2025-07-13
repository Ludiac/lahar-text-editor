module;

#include "macros.hpp"
#include "primitive_types.hpp"

export module vulkan_app:VMA;

import vulkan_hpp;
import std;
import vk_mem_alloc_hpp;

export struct VmaBuffer {
  vma::Allocator allocator_{nullptr};
  vk::Buffer buffer_{nullptr};
  vma::Allocation allocation_{nullptr};
  vma::AllocationInfo allocationInfo_{};
  vk::DeviceSize size_ = 0;
  void *pMappedData_ = nullptr;

  vk::Buffer &operator*() { return buffer_; }
  const vk::Buffer &operator*() const { return buffer_; }

  VmaBuffer() = default;

  VmaBuffer(vma::Allocator allocator, vk::Buffer buffer, vma::Allocation allocation,
            const vma::AllocationInfo &allocInfo, vk::DeviceSize size)
      : allocator_(allocator), buffer_(buffer), allocation_(allocation), allocationInfo_(allocInfo),
        size_(size) {

    if (allocInfo.pMappedData != nullptr) {
      pMappedData_ = allocInfo.pMappedData;
    }
  }

  ~VmaBuffer() { release(); }

  VmaBuffer(VmaBuffer &&other) noexcept
      : allocator_(other.allocator_), buffer_(other.buffer_), allocation_(other.allocation_),
        allocationInfo_(other.allocationInfo_), size_(other.size_),
        pMappedData_(other.pMappedData_) {

    other.allocator_ = nullptr;
    other.buffer_ = nullptr;
    other.allocation_ = nullptr;
    other.pMappedData_ = nullptr;
    other.size_ = 0;
  }

  VmaBuffer &operator=(VmaBuffer &&other) noexcept {
    if (this != &other) {
      release();

      allocator_ = other.allocator_;
      buffer_ = other.buffer_;
      allocation_ = other.allocation_;
      allocationInfo_ = other.allocationInfo_;
      size_ = other.size_;
      pMappedData_ = other.pMappedData_;

      other.allocator_ = nullptr;
      other.buffer_ = nullptr;
      other.allocation_ = nullptr;
      other.pMappedData_ = nullptr;
      other.size_ = 0;
    }
    return *this;
  }

  VmaBuffer(const VmaBuffer &) = delete;
  VmaBuffer &operator=(const VmaBuffer &) = delete;

  void release() {
    if (allocator_ && buffer_ && allocation_) {

      allocator_.destroyBuffer(buffer_, allocation_);
    }
    allocator_ = nullptr;
    buffer_ = nullptr;
    allocation_ = nullptr;
    pMappedData_ = nullptr;
    size_ = 0;
    allocationInfo_ = vma::AllocationInfo{};
  }

  [[nodiscard]] vk::Buffer get() const { return buffer_; }
  [[nodiscard]] vma::Allocation getAllocation() const { return allocation_; }
  [[nodiscard]] const vma::AllocationInfo &getAllocationInfo() const { return allocationInfo_; }
  [[nodiscard]] vk::DeviceSize getSize() const { return size_; }
  [[nodiscard]] void *getMappedData() const { return pMappedData_; }

  explicit operator bool() const { return buffer_ && allocation_; }
};

export struct VmaImage {
  vma::Allocator allocator_ = nullptr;
  vk::Image image_ = nullptr;
  vma::Allocation allocation_ = nullptr;
  vma::AllocationInfo allocationInfo_{};
  vk::Format format_ = vk::Format::eUndefined;
  vk::Extent3D extent_ = {.width = 0, .height = 0, .depth = 0};

  VmaImage() = default;

  VmaImage(vma::Allocator allocator, vk::Image image, vma::Allocation allocation,
           const vma::AllocationInfo &allocInfo, vk::Format format, vk::Extent3D extent)
      : allocator_(allocator), image_(image), allocation_(allocation), allocationInfo_(allocInfo),
        format_(format), extent_(extent) {}

  ~VmaImage() { release(); }

  VmaImage(VmaImage &&other) noexcept
      : allocator_(other.allocator_), image_(other.image_), allocation_(other.allocation_),
        allocationInfo_(other.allocationInfo_), format_(other.format_), extent_(other.extent_) {
    other.allocator_ = nullptr;
    other.image_ = nullptr;
    other.allocation_ = nullptr;
    other.format_ = vk::Format::eUndefined;
    other.extent_ = {.width = 0, .height = 0, .depth = 0};
  }

  VmaImage &operator=(VmaImage &&other) noexcept {
    if (this != &other) {
      release();
      allocator_ = other.allocator_;
      image_ = other.image_;
      allocation_ = other.allocation_;
      allocationInfo_ = other.allocationInfo_;
      format_ = other.format_;
      extent_ = other.extent_;

      other.allocator_ = nullptr;
      other.image_ = nullptr;
      other.allocation_ = nullptr;
      other.format_ = vk::Format::eUndefined;
      other.extent_ = {.width = 0, .height = 0, .depth = 0};
    }
    return *this;
  }

  VmaImage(const VmaImage &) = delete;
  VmaImage &operator=(const VmaImage &) = delete;

  void release() {
    if (allocator_ && image_ && allocation_) {
      allocator_.destroyImage(image_, allocation_);
    }
    allocator_ = nullptr;
    image_ = nullptr;
    allocation_ = nullptr;
    format_ = vk::Format::eUndefined;
    extent_ = {.width = 0, .height = 0, .depth = 0};
    allocationInfo_ = vma::AllocationInfo{};
  }

  [[nodiscard]] vk::Image get() const { return image_; }
  [[nodiscard]] vma::Allocation getAllocation() const { return allocation_; }
  [[nodiscard]] const vma::AllocationInfo &getAllocationInfo() const { return allocationInfo_; }
  [[nodiscard]] vk::Format getFormat() const { return format_; }
  [[nodiscard]] vk::Extent3D getExtent() const { return extent_; }

  explicit operator bool() const { return image_ && allocation_; }
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

  vma::Allocator allocator_handle;
  vk::Result result = vma::createAllocator(&allocatorInfo, &allocator_handle);
  if (result != vk::Result::eSuccess) {
    return std::unexpected("Failed to create VMA allocator: " + vk::to_string(result));
  }
  return allocator_handle;
}
