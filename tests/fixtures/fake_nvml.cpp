#include <cstdint>

extern "C" {

struct FakeNvmlUtilization {
  unsigned int gpu;
  unsigned int memory;
};

struct FakeNvmlMemory {
  unsigned long long total;
  unsigned long long free;
  unsigned long long used;
};

int nvmlInit_v2() { return 0; }
int nvmlShutdown() { return 0; }

int nvmlDeviceGetCount_v2(unsigned int *count) {
  if (count == nullptr)
    return 1;
  *count = 2U;
  return 0;
}

int fake_devices[2]{};

int nvmlDeviceGetHandleByIndex_v2(const unsigned int index, void **device) {
  if (device == nullptr || index >= 2U)
    return 1;
  *device = &fake_devices[index];
  return 0;
}

int nvmlDeviceGetUtilizationRates(void *device,
                                  FakeNvmlUtilization *utilization) {
  if (device == nullptr || utilization == nullptr)
    return 1;
  const auto index = device == &fake_devices[0] ? 0U : 1U;
  utilization->gpu = index == 0U ? 20U : 80U;
  utilization->memory = 10U;
  return 0;
}

int nvmlDeviceGetMemoryInfo(void *device, FakeNvmlMemory *memory) {
  if (device == nullptr || memory == nullptr)
    return 1;
  const auto index = device == &fake_devices[0] ? 0U : 1U;
  memory->total = 8ULL * 1024ULL * 1024ULL;
  memory->used = (index + 1U) * 1024ULL * 1024ULL;
  memory->free = memory->total - memory->used;
  return 0;
}

} // extern "C"
