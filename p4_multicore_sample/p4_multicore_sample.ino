/**
 * ESP32-P4 dual high-performance core example for Arduino IDE.
 *
 * Two FreeRTOS tasks perform independent work in parallel. One task is pinned
 * to HP core 0 and the other to HP core 1. The Arduino loop task receives their
 * reports through a queue and is the only task that writes to Serial.
 *
 * The ESP32-P4 LP core is a separate ULP coprocessor. The standard Arduino IDE
 * package does not build LP-core firmware; see README.md for details.
 */

#include <Arduino.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#if !CONFIG_IDF_TARGET_ESP32P4
#error "Select an ESP32-P4 board before compiling this sketch"
#endif

#if CONFIG_FREERTOS_UNICORE
#error "This sample requires a dual-core FreeRTOS build"
#endif

namespace {

constexpr uint32_t kSerialBaud = 115200;
constexpr uint32_t kWorkerStackSize = 4096;
constexpr UBaseType_t kWorkerPriority = 2;
constexpr UBaseType_t kReportQueueLength = 8;

struct WorkerConfig {
  const char *name;
  BaseType_t core;
  uint32_t periodMs;
  uint32_t seed;
};

struct WorkerReport {
  const char *name;
  BaseType_t core;
  uint32_t iteration;
  uint32_t checksum;
  UBaseType_t minimumFreeStack;
};

const WorkerConfig kWorker0 = {"HP0", 0, 500, 0x12345678};
const WorkerConfig kWorker1 = {"HP1", 1, 700, 0x87654321};

QueueHandle_t reportQueue = nullptr;
TaskHandle_t worker0Handle = nullptr;
TaskHandle_t worker1Handle = nullptr;

uint32_t performWork(uint32_t state) {
  // A deterministic CPU workload makes it easy to see both cores progressing.
  for (uint32_t i = 0; i < 100000; ++i) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    state += i;
  }
  return state;
}

void workerTask(void *parameter) {
  const WorkerConfig *config = static_cast<const WorkerConfig *>(parameter);
  TickType_t nextWake = xTaskGetTickCount();
  uint32_t iteration = 0;
  uint32_t checksum = config->seed;

  while (true) {
    checksum = performWork(checksum);
    ++iteration;

    const WorkerReport report = {
      config->name,
      xPortGetCoreID(),
      iteration,
      checksum,
      uxTaskGetStackHighWaterMark(nullptr),
    };

    // Do not block the worker if Serial reporting falls behind.
    xQueueSend(reportQueue, &report, 0);
    xTaskDelayUntil(&nextWake, pdMS_TO_TICKS(config->periodMs));
  }
}

bool createWorker(const WorkerConfig &config, TaskHandle_t *handle) {
  return xTaskCreatePinnedToCore(
           workerTask,
           config.name,
           kWorkerStackSize,
           const_cast<WorkerConfig *>(&config),
           kWorkerPriority,
           handle,
           config.core) == pdPASS;
}

void stopWithError(const char *message) {
  Serial.printf("ERROR: %s\n", message);
  while (true) {
    delay(1000);
  }
}

}  // namespace

void setup() {
  Serial.begin(kSerialBaud);
  delay(1000);

  Serial.println();
  Serial.println("ESP32-P4 dual HP-core sample");
  Serial.printf("Arduino setup/loop task is on HP core %d\n", xPortGetCoreID());

  reportQueue = xQueueCreate(kReportQueueLength, sizeof(WorkerReport));
  if (reportQueue == nullptr) {
    stopWithError("could not create the report queue");
  }

  if (!createWorker(kWorker0, &worker0Handle)) {
    stopWithError("could not create the HP core 0 worker");
  }

  if (!createWorker(kWorker1, &worker1Handle)) {
    vTaskDelete(worker0Handle);
    worker0Handle = nullptr;
    stopWithError("could not create the HP core 1 worker");
  }

  Serial.println("Both HP workers started");
}

void loop() {
  WorkerReport report = {};

  if (xQueueReceive(reportQueue, &report, pdMS_TO_TICKS(1000)) == pdTRUE) {
    Serial.printf(
      "%-3s | core=%d | iteration=%lu | checksum=0x%08lx | free stack >= %u bytes\n",
      report.name,
      report.core,
      static_cast<unsigned long>(report.iteration),
      static_cast<unsigned long>(report.checksum),
      static_cast<unsigned int>(report.minimumFreeStack));
  } else {
    Serial.println("No worker report received in the last second");
  }
}
