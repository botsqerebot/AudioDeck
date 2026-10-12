// psram.h — put the library index and big vectors in the 8 MB PSRAM
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <vector>

static inline void *psramMalloc(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(n);   // falls back to internal RAM if PSRAM is off
}

// ArduinoJson allocator: the whole library.json document lives in PSRAM
struct SpiRamAllocator : ArduinoJson::Allocator {
  void *allocate(size_t n) override { return psramMalloc(n); }
  void deallocate(void *p) override { heap_caps_free(p); }
  void *reallocate(void *p, size_t n) override {
    void *r = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return r ? r : realloc(p, n);
  }
};
static SpiRamAllocator spiRamAllocator;

// STL allocator so std::vector storage lands in PSRAM
template <class T> struct PsramAlloc {
  typedef T value_type;
  PsramAlloc() = default;
  template <class U> PsramAlloc(const PsramAlloc<U> &) {}
  T *allocate(size_t n) {
    T *p = (T *)psramMalloc(n * sizeof(T));
    if (!p) abort();
    return p;
  }
  void deallocate(T *p, size_t) { heap_caps_free(p); }
};
template <class T, class U> bool operator==(const PsramAlloc<T> &, const PsramAlloc<U> &) { return true; }
template <class T, class U> bool operator!=(const PsramAlloc<T> &, const PsramAlloc<U> &) { return false; }
template <class T> using PVec = std::vector<T, PsramAlloc<T>>;
