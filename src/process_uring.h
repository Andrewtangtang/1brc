#ifndef PROCESS_URING_H
#define PROCESS_URING_H

#define _GNU_SOURCE

#include "defs.h"
#include "process_common.h"
#include "uring_file_reader.h"
#include <sys/mman.h> //mlock
#include <fcntl.h>    

static void *process_fsegment_uring(void *thread_info) {
  thread_info_t *arg = (thread_info_t *)thread_info;

  mlock(&arg->stations,
        sizeof(arg->stations)); // PERF: Maybe improves a bit, maybe not

  char *buf = NULL;
  char *record = NULL;
  uint16_t buf_len = 0;
  uint16_t pos = 0;

  ring_file_reader_t reader = rfr_create(arg->fd, arg->start, arg->end, arg->cfg.iopoll);

  record = reader.buf_start + reader.buf_len;
  *(record - 1) =
      '\n'; // Required because in case of 3-letter city-name hashing takes more
            // 1 byte before beginning (which is always '\n')

  if (LIKELY(!arg->cfg.skip_align)) {
    // Slow processing till aligned byte [tag: SLOWSTART]
    // PERF: Seems to speedup some 0-5% (and somewhat more stable time)

    const uintptr_t aligned_address =
        (arg->start + DISK_CLUSTER - 1) & ~(DISK_CLUSTER - 1);
    if (arg->start != aligned_address) {
      const ssize_t start_offset = aligned_address - arg->start;

      int open_flags = O_RDONLY | O_NONBLOCK;
      // For the slow start part, we don't use O_DIRECT,
      // we'll read the unaligned beginning separately
      const int fd = open(arg->fname, open_flags);
      ___EXPECT(fd, "file open thread");

      record -= start_offset;
      buf = record;
      *(record - 1) =
          '\n'; // Required because in case of 3-letter city-name hashing takes
                // more 1 byte before beginning (which is always '\n')

      const int ret = lseek(fd, arg->start, SEEK_SET);
      ___EXPECT(ret != -1, "file seek align begin");

      const ssize_t bytes_read = read(fd, buf, start_offset);
      ___EXPECT(bytes_read == start_offset, "slow-start-fread");

      close(fd);

      reader.fpos += start_offset;

      *(buf - 1) =
          '\n'; // Required because in case of 3-letter city-name hashing takes
                // more 1 byte before beginning (which is always '\n')

      for (ssize_t pos = 0; pos < start_offset; pos++) {
        if (*(buf + pos) == '\n')
          record = process_record(record, buf + pos, &arg->stations);
      }
    }
  }

  // Fast processing using simd on aligned buffer part
  while (rfr_request_next_blocks(&reader)) {
    record -=
        RING_ENTRIES * READ_BLOCK_LEN; // Previous token is always in the last
                                       // block, so move it to pre-data block

    while (reader.blocks_in_queue) {
      const int nr = rfr_wait_for_block(&reader);
      buf = reader.buf[nr];
      buf_len = reader.lengths[nr];

      for (pos = 0; pos <= buf_len - 32; pos += 32) {
        char *chunk = buf + pos;

        for (int mask = get_newline_positions_mask_in_chunk(chunk); mask;
             mask &= mask - 1) {
          const int newline_pos = __builtin_ctz(mask);
          record = process_record(record, chunk + newline_pos, &arg->stations);
        }
      }
    }
  }

  // Slow processing on remainig data (less than simd block - 32-byte/256-bit)
  for (; pos < buf_len; pos++)
    if (buf[pos] == '\n')
      record = process_record(record, (char *)&buf[pos], &arg->stations);

  // After the main uring loop, we need to handle the tail of the file segment
  // that was not read by O_DIRECT due to alignment constraints. There might also
  // be a partial record left in the last uring buffer.
  const int64_t tail_start = reader.fpos;
  const int64_t tail_len = arg->end - tail_start;

  size_t partial_len = 0;
  if (buf) { // Check if the uring loop ran at least once
      partial_len = (buf + buf_len) - record;
  }

  if (tail_len > 0 || partial_len > 0) {
    char *final_buf = malloc(partial_len + tail_len + 1);
    ___EXPECT(final_buf, "final_buf malloc");

    // Copy the partial part of the record from the last uring buffer, if any.
    if (partial_len > 0) {
        memcpy(final_buf, record, partial_len);
    }

    // Read the actual unaligned tail from the file, if any.
    if (tail_len > 0) {
      int tail_fd = open(arg->fname, O_RDONLY);
      ___EXPECT(tail_fd >= 0, "tail fd open");
      lseek(tail_fd, tail_start, SEEK_SET);
      ssize_t bytes_read = read(tail_fd, final_buf + partial_len, tail_len);
      ___EXPECT(bytes_read == tail_len, "tail read failed");
      close(tail_fd);
    }

    // Process the combined final buffer
    char *p = final_buf;
    const size_t final_len = partial_len + tail_len;
    for (size_t i = 0; i < final_len; i++) {
        if (final_buf[i] == '\n') {
            record = process_record(p, final_buf + i, &arg->stations);
            p = final_buf + i + 1;
        }
    }
    free(final_buf);
  }

  rfr_destroy(&reader);
  munlock(&arg->stations, sizeof(arg->stations));
  return NULL;
}

#endif // PROCESS_URING_H
