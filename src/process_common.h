#ifndef PROCESS_COMMON_H
#define PROCESS_COMMON_H

#include "defs.h"
#include "extraction.h"
#include "station.h"

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) ||             \
    defined(_M_IX86)
#include <immintrin.h>
#endif

typedef struct {
  const char *fname;
  int cpus;
  bool mmap;
  bool skip_align;
  bool debug;
} config_t;

typedef struct {
  pthread_t tid;
  stations_t stations;
  char fname[255];
  int fd;
  char *buf;
  size_t start;
  size_t end;
  config_t cfg;
} thread_info_t;

INLINE int get_newline_positions_mask_in_chunk(const char *chunk_start) {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) ||             \
    defined(_M_IX86)
  // Original x86/x64 implementation with AVX2
  const __m256i endls = _mm256_set1_epi8('\n');

  // const __m256i chunk = _mm256_load_si256((__m256i*) chunk_start);  // PERF:
  // Should be faster, but seems slower, chunk is aligned here (iouring) const
  // __m256i chunk = _mm256_loadu_si256((__m256i*) chunk_start); // PERF: Same
  const __m256i chunk =
      _mm256_lddqu_si256((__m256i *)chunk_start); // PERF: Seems faster a bit

  const __m256i xcomp = _mm256_cmpeq_epi8(chunk, endls);
  return _mm256_movemask_epi8(xcomp);

#elif defined(__aarch64__) || defined(__arm64__)
  // ARM64 implementation - TODO: optimize with NEON when needed
  int msk = 0;
  for (int i = 0; i < 32; i++) {
    msk |= (chunk_start[i] == '\n') << i;
  }
  return msk;

#else
  // Fallback implementation for other architectures
  int msk = 0;
  for (int i = 0; i < 32; i++) {
    msk |= (chunk_start[i] == '\n') << i;
  }
  return msk;
#endif
}

// returns pointer to the next record
INLINE char *process_record(char *start, char *end, stations_t *stations) {
  ___ASSERT(end - start >= MIN_RECORD_BYTES, "min record size\n");
  ___ASSERT(end - start <= MAX_RECORD_BYTES, "max record size\n");
  ___ASSERT(('A' <= start[0] && start[0] <= 'Z') || start[0] == "İ"[0] ||
                start[0] == "Ü"[0],
            "token2\n");
  ___ASSERT('\n' == *end, "token-end\n");

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) ||             \
    defined(_M_IX86)
  // Original x86/x64 implementation with SIMD
  const __m128i chunk = _mm_lddqu_si128(
      (__m128i *)(end - 16)); // PERF: Seems slightly stable/faster than using
                              // simd 256 (less heat?)
  const __m128i semis =
      _mm_set1_epi8(';'); // PERF: same if used here or moved outside
  const __m128i cmp = _mm_cmpeq_epi8(chunk, semis);
  const int msk = _mm_movemask_epi8(cmp);
  ___ASSERT(msk, "mask2");

  const int offset = __builtin_clz(msk) - 15; // PERF: Slowdown 8% ??
  char *semi = end - offset;
  ___ASSERT(*semi == ';', "semi3");

  station_data_t *s = get_station(stations, start, semi - start);
  _mm_prefetch(s, _MM_HINT_T0); // Likely useless, would be nice to move it some
                                // 100-200 CPU cycles before usage

  // const int16_t value = 0; // PERF: #0 530 ms BASELINE const int16_t value =
  // extract_temperature_1__direct_calc(end);                // PERF: #1 780 ms
  // const int16_t value = extract_temperature_2__hash_lookup(semi, end); //
  // PERF: #2 900 ms const int16_t value =
  // extract_temperature_3__combined_lookups(end);           // PERF: #3 630 ms
  // const int16_t value =
  // extract_temperature_4__simd_swapped_hash_lookup(chunk); // PERF: #5 610 ms
  // WTF?, map is 2x shorter here [tag: HASH-SWAP]
  const int16_t value =
      extract_temperature_5__simd_hash_lookup(chunk); // PERF: #4 580 ms -> 582

  s->cnt++; // PERF: Cache misses on `s`
  s->sum += value;
  s->min = min16(s->min, value);
  s->max = max16(s->max, value);

#elif defined(__aarch64__) || defined(__arm64__)
  // ARM64 implementation - fallback to direct calculation for now
  char *semi = NULL;
  for (char *p = end - 1; p > start; p--) {
    if (*p == ';') {
      semi = p;
      break;
    }
  }
  ___ASSERT(semi != NULL, "semicolon not found");

  station_data_t *s = get_station(stations, start, semi - start);

  // Use direct calculation - most reliable method
  const int16_t value = extract_temperature_1__direct_calc(end);

  s->cnt++;
  s->sum += value;
  s->min = min16(s->min, value);
  s->max = max16(s->max, value);

#else
  // Fallback implementation for other architectures
  char *semi = NULL;
  for (char *p = end - 1; p > start; p--) {
    if (*p == ';') {
      semi = p;
      break;
    }
  }
  ___ASSERT(semi != NULL, "semicolon not found");

  station_data_t *s = get_station(stations, start, semi - start);

  const int16_t value = extract_temperature_1__direct_calc(end);

  s->cnt++;
  s->sum += value;
  s->min = min16(s->min, value);
  s->max = max16(s->max, value);
#endif
  return end + 1;
}

// returns pointer to the next record
INLINE char *process_record_mmap_slowstart(char *start, char *end,
                                           stations_t *stations) {
  ___ASSERT(end - start >= MIN_RECORD_BYTES, "mmap-slow-min record size\n");
  ___ASSERT(end - start <= MAX_RECORD_BYTES, "mmap-slow-max record size\n");
  ___ASSERT(('A' <= start[0] && start[0] <= 'Z') || start[0] == "İ"[0] ||
                start[0] == "Ü"[0],
            "mmap-slow-token2\n");
  ___ASSERT('\n' == *end, "mmap-slow-token-end\n");

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) ||             \
    defined(_M_IX86)
  // Original x86/x64 implementation with SIMD
  ///////////////////////////////////
  // diff from the normal `process_record_mmap`
  const size_t len = end - start;
  char buffer[16];
  memset(buffer, 0, 16);
  // printf("\nAA: %ld [%.14s..%.4s]\n", len, start, end-4);
  memcpy(buffer + 16 - 6, end - 6, min64(len, 6));

  // printf("\nBB: %ld [%.4s]\n", len, buffer + 16 - 4);

  const __m128i chunk = _mm_lddqu_si128((__m128i *)buffer);
  ///////////////////////////////////

  const __m128i semis = _mm_set1_epi8(';');
  const __m128i cmp = _mm_cmpeq_epi8(chunk, semis);
  const int msk = _mm_movemask_epi8(cmp);
  ___ASSERT(msk, "mmap-slow-mask2");

  const int offset = __builtin_clz(msk) - 15; // PERF: Slowdown 8% ??
  char *semi = end - offset;
  ___ASSERT(*semi == ';', "mmap-slow-semi3");

  station_data_t *s = get_station(stations, start, semi - start);
  _mm_prefetch(s, _MM_HINT_T0); // Likely useless, would be nice to move it some
                                // 100-200 CPU cycles before usage

  const int16_t value =
      extract_temperature_5__simd_hash_lookup(chunk); // PERF: #4 590 ms

  s->cnt++; // PERF: Cache misses on `s`
  s->sum += value;
  s->min = min16(s->min, value);
  s->max = max16(s->max, value);

#elif defined(__aarch64__) || defined(__arm64__)
  // ARM64 implementation - fallback to direct calculation
  char *semi = NULL;
  for (char *p = end - 1; p > start; p--) {
    if (*p == ';') {
      semi = p;
      break;
    }
  }
  ___ASSERT(semi != NULL, "mmap-slow-semicolon not found");

  station_data_t *s = get_station(stations, start, semi - start);

  const int16_t value = extract_temperature_1__direct_calc(end);

  s->cnt++;
  s->sum += value;
  s->min = min16(s->min, value);
  s->max = max16(s->max, value);

#else
  // Fallback implementation for other architectures
  char *semi = NULL;
  for (char *p = end - 1; p > start; p--) {
    if (*p == ';') {
      semi = p;
      break;
    }
  }
  ___ASSERT(semi != NULL, "mmap-slow-semicolon not found");

  station_data_t *s = get_station(stations, start, semi - start);

  const int16_t value = extract_temperature_1__direct_calc(end);

  s->cnt++;
  s->sum += value;
  s->min = min16(s->min, value);
  s->max = max16(s->max, value);
#endif
  return end + 1;
}

#endif // PROCESS_COMMON_H
