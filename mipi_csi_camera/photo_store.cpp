#include "photo_store.h"

#include <cstring>
#include <esp_heap_caps.h>
#include <new>

PhotoBlob::PhotoBlob(DetachedJpegOutputBuffer jpeg, uint32_t size,
                     uint32_t width, uint32_t height, uint32_t quality,
                     uint64_t captured_ms)
  : jpeg_(jpeg) {
  metadata_.width = width;
  metadata_.height = height;
  metadata_.quality = quality;
  metadata_.size = size;
  metadata_.captured_ms = captured_ms;
}

PhotoBlob::~PhotoBlob() {
  JpegOutputBuffer::releaseDetached(jpeg_);
}

PhotoBlob *PhotoBlob::create(DetachedJpegOutputBuffer jpeg, uint32_t size,
                             uint32_t width, uint32_t height, uint32_t quality,
                             uint64_t captured_ms) {
  if (jpeg.data == nullptr || jpeg.capacity == 0 || size == 0
      || size > jpeg.capacity) {
    JpegOutputBuffer::releaseDetached(jpeg);
    return nullptr;
  }
  PhotoBlob *blob = new (std::nothrow)
    PhotoBlob(jpeg, size, width, height, quality, captured_ms);
  if (blob == nullptr) {
    JpegOutputBuffer::releaseDetached(jpeg);
  }
  return blob;
}

const uint8_t *PhotoBlob::data() const { return jpeg_.data; }
uint32_t PhotoBlob::size() const { return metadata_.size; }
const PhotoMetadata &PhotoBlob::metadata() const { return metadata_; }
uint32_t PhotoBlob::referenceCount() const {
  return references_.load(std::memory_order_acquire);
}

void PhotoBlob::retain() {
  references_.fetch_add(1, std::memory_order_relaxed);
}

void PhotoBlob::release() {
  if (references_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    delete this;
  }
}

PhotoStore::PhotoStore() : mutex_(xSemaphoreCreateMutex()) {}

PhotoStore::~PhotoStore() {
  if (mutex_ != nullptr) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    PhotoBlob *current = current_;
    current_ = nullptr;
    xSemaphoreGive(mutex_);
    if (current != nullptr) {
      current->release();
    }
    vSemaphoreDelete(mutex_);
    mutex_ = nullptr;
  }
}

PhotoBlob *PhotoStore::acquireLatest() {
  if (mutex_ == nullptr || xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
    return nullptr;
  }
  PhotoBlob *result = current_;
  if (result != nullptr) {
    result->retain();
  }
  xSemaphoreGive(mutex_);
  return result;
}

PhotoBlob *PhotoStore::acquireById(uint64_t photo_id) {
  if (mutex_ == nullptr || xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
    return nullptr;
  }
  PhotoBlob *result = nullptr;
  if (current_ != nullptr && current_->metadata_.photo_id == photo_id) {
    result = current_;
    result->retain();
  }
  xSemaphoreGive(mutex_);
  return result;
}

bool PhotoStore::publish(PhotoBlob *candidate) {
  if (candidate == nullptr || mutex_ == nullptr
      || xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
    if (candidate != nullptr) {
      candidate->release();
    }
    return false;
  }
  candidate->metadata_.photo_id = next_photo_id_++;
  PhotoBlob *previous = current_;
  current_ = candidate;
  xSemaphoreGive(mutex_);
  if (previous != nullptr) {
    previous->release();
  }
  return true;
}

void PhotoStore::release(PhotoBlob *blob) {
  if (blob != nullptr) {
    blob->release();
  }
}

bool PhotoStore::metadataSnapshot(PhotoMetadata *out) {
  if (out == nullptr || mutex_ == nullptr
      || xSemaphoreTake(mutex_, portMAX_DELAY) != pdTRUE) {
    return false;
  }
  const bool available = current_ != nullptr;
  if (available) {
    *out = current_->metadata_;
  }
  xSemaphoreGive(mutex_);
  return available;
}

bool PhotoStore::ready() const { return mutex_ != nullptr; }

namespace {
struct ValidationContext {
  PhotoStore *store = nullptr;
  SemaphoreHandle_t completed = nullptr;
  uint32_t loops = 0;
  volatile bool failed = false;
};

PhotoBlob *makeValidationBlob(uint8_t marker) {
  JpegOutputBuffer output;
  if (!output.allocate(64)) {
    return nullptr;
  }
  memset(output.data(), marker, output.capacity());
  DetachedJpegOutputBuffer detached = output.detach();
  return PhotoBlob::create(detached, 64, 1920, 1080, 90, millis());
}

void photoStoreReader(void *argument) {
  ValidationContext *context = static_cast<ValidationContext *>(argument);
  for (uint32_t index = 0; index < context->loops; ++index) {
    PhotoBlob *blob = context->store->acquireLatest();
    if (blob == nullptr) {
      context->failed = true;
      continue;
    }
    const PhotoMetadata metadata = blob->metadata();
    const uint8_t marker = blob->data() == nullptr ? 0 : blob->data()[0];
    delay(1);
    if (marker != static_cast<uint8_t>(metadata.photo_id)
        || blob->size() != metadata.size) {
      context->failed = true;
    }
    context->store->release(blob);
  }
  xSemaphoreGive(context->completed);
  vTaskDelete(nullptr);
}
}  // namespace

bool runPhotoStoreValidation(uint32_t replacements, uint32_t readers) {
  if (replacements == 0 || readers == 0 || readers > 8) {
    return false;
  }
  PhotoStore store;
  if (!store.ready()) {
    return false;
  }
  PhotoBlob *initial = makeValidationBlob(1);
  if (initial == nullptr || !store.publish(initial)) {
    return false;
  }

  const size_t psram_before = ESP.getFreePsram();
  const size_t psram_largest_before =
    heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

  ValidationContext context;
  context.store = &store;
  context.loops = replacements;
  context.completed = xSemaphoreCreateCounting(readers, 0);
  if (context.completed == nullptr) {
    return false;
  }
  uint32_t started = 0;
  for (; started < readers; ++started) {
    if (xTaskCreate(photoStoreReader, "photo-reader", 3072, &context,
                    1, nullptr) != pdPASS) {
      context.failed = true;
      break;
    }
  }
  for (uint32_t generation = 2; generation <= replacements + 1; ++generation) {
    PhotoBlob *candidate = makeValidationBlob(static_cast<uint8_t>(generation));
    if (candidate == nullptr || !store.publish(candidate)) {
      context.failed = true;
      break;
    }
    delay(1);
  }
  for (uint32_t reader = 0; reader < started; ++reader) {
    xSemaphoreTake(context.completed, portMAX_DELAY);
  }
  vSemaphoreDelete(context.completed);
  PhotoMetadata latest;
  const bool snapshot_ready = store.metadataSnapshot(&latest);
  const size_t psram_after = ESP.getFreePsram();
  const size_t psram_largest_after =
    heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  const bool memory_stable = psram_after >= psram_before
                             && psram_largest_after >= psram_largest_before;
  const bool passed = !context.failed && snapshot_ready && memory_stable
                      && latest.photo_id == replacements + 1;
  Serial.printf(
    "photo store validation status=%s replacements=%lu readers=%lu latest_id=%llu "
    "psram_before=%lu psram_after=%lu psram_largest_before=%lu "
    "psram_largest_after=%lu\n",
    passed ? "passed" : "failed", static_cast<unsigned long>(replacements),
    static_cast<unsigned long>(started),
    static_cast<unsigned long long>(snapshot_ready ? latest.photo_id : 0),
    static_cast<unsigned long>(psram_before), static_cast<unsigned long>(psram_after),
    static_cast<unsigned long>(psram_largest_before),
    static_cast<unsigned long>(psram_largest_after));
  return passed;
}
