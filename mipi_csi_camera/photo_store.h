#pragma once

#include <Arduino.h>
#include <atomic>
#include "jpeg_output_buffer.h"

struct PhotoMetadata {
  uint64_t photo_id = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t quality = 0;
  uint32_t size = 0;
  uint64_t captured_ms = 0;
};

// Immutable after publication. Ownership is managed only through retain/release.
class PhotoBlob {
public:
  static PhotoBlob *create(DetachedJpegOutputBuffer jpeg, uint32_t size,
                           uint32_t width, uint32_t height, uint32_t quality,
                           uint64_t captured_ms);

  const uint8_t *data() const;
  uint32_t size() const;
  const PhotoMetadata &metadata() const;
  uint32_t referenceCount() const;

private:
  PhotoBlob(DetachedJpegOutputBuffer jpeg, uint32_t size, uint32_t width,
            uint32_t height, uint32_t quality, uint64_t captured_ms);
  ~PhotoBlob();
  PhotoBlob(const PhotoBlob &) = delete;
  PhotoBlob &operator=(const PhotoBlob &) = delete;

  friend class PhotoStore;
  void retain();
  void release();

  DetachedJpegOutputBuffer jpeg_;
  PhotoMetadata metadata_;
  std::atomic<uint32_t> references_{1};
};

class PhotoStore {
public:
  PhotoStore();
  ~PhotoStore();
  PhotoStore(const PhotoStore &) = delete;
  PhotoStore &operator=(const PhotoStore &) = delete;

  PhotoBlob *acquireLatest();
  PhotoBlob *acquireById(uint64_t photo_id);
  bool publish(PhotoBlob *candidate);
  void release(PhotoBlob *blob);
  bool metadataSnapshot(PhotoMetadata *out);
  bool ready() const;

private:
  SemaphoreHandle_t mutex_ = nullptr;
  PhotoBlob *current_ = nullptr;
  uint64_t next_photo_id_ = 1;
};

// Boot-time ownership stress gate for the Arduino implementation.
bool runPhotoStoreValidation(uint32_t replacements, uint32_t readers);
