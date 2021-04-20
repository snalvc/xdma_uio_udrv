#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <unistd.h>

#include "XDMA_udrv.hpp"
#include "st_huge_pg.hpp"

using namespace std;
namespace fs = std::filesystem;

struct axis_word_128 {
  uint32_t data[4];
} __attribute__((packed));

void hexdump(const void *data, size_t size);
int compare_axis_word(struct axis_word_128 *left, struct axis_word_128 *right);
void lfsr128(struct axis_word_128 *target, struct axis_word_128 *result);
struct timespec timediff(struct timespec start, struct timespec end);

int main(int argc, char const *argv[]) {
  unique_ptr<XDMA_udrv::XDMA> xdma = XDMA_udrv::XDMA::XDMA_factory();
  XDMA_udrv::XHugeBuffer buffer;

  cout << *xdma << endl;

  for (int i = 0; i < XDMA_udrv::XDMA::num_of_bars_max; i++) {
    if (xdma->bar_len(i)) {
      cout << "BAR" << i << endl;
      cout << "length: " << xdma->bar_len(i) << endl;
      cout << "virtual address: " << xdma->bar_vaddr(i) << endl;
    } else
      continue;
    cout << endl;
  }

  uint32_t cbi = xdma->ctrl_reg_read(XDMA_udrv::CONFIG, 0, 0);

  cout << hex;
  cout << "Core Identifier: 0x" << ((cbi & 0xFFF00000) >> 20) << endl;
  cout << "Config Identifier: 0x" << ((cbi & 0x000F0000) >> 16) << endl;
  cout << "Version: 0x" << (cbi & 0x0000000F) << endl << endl;
  cout << dec;

  printf("Data buffer physical address: 0x%08lX\n",
         (uintptr_t)buffer.getDataBufferPaddr());
  printf("Desc buffer physical address: 0x%08lX\n",
         (uintptr_t)buffer.getDescBufferPaddr());

  // For timing
  struct timespec tstart, tend, tdiff;

  // Start a 1 GiB transfer
  uint32_t xfer_size = (1 << 29);
  buffer.initialize(xfer_size);

  // dump descriptors for check
  cout << "Descriptor dump" << endl;
  hexdump(buffer.getDescBufferVaddr(), 8 * sizeof(XDMA_udrv::xdma_desc));
  cout << endl;

  // Set C2H channel 0 first descriptor block
  xdma->ctrl_reg_write(XDMA_udrv::XDMA_ADDR_TARGET::C2H_SGDMA, 0, 0x80,
                       buffer.getDescBufferPaddr());
  printf("descriptor lo readback: 0x%" PRIX32 "\n",
         xdma->ctrl_reg_read(XDMA_udrv::XDMA_ADDR_TARGET::C2H_SGDMA, 0, 0x80));
  xdma->ctrl_reg_write(XDMA_udrv::XDMA_ADDR_TARGET::C2H_SGDMA, 0, 0x84,
                       buffer.getDescBufferPaddr() >> 32);
  printf("descriptor hi readback: 0x%" PRIX32 "\n",
         xdma->ctrl_reg_read(XDMA_udrv::XDMA_ADDR_TARGET::C2H_SGDMA, 0, 0x84));
  // Set C2H channel 0 ie_descriptor_completed
  xdma->ctrl_reg_write(XDMA_udrv::XDMA_ADDR_TARGET::C2H_CHANNEL, 0, 0x08,
                       1 << 2);
  printf(
      "channel control readback: 0x%" PRIX32 "\n",
      xdma->ctrl_reg_read(XDMA_udrv::XDMA_ADDR_TARGET::C2H_CHANNEL, 0, 0x04));
  // Cycle run bit to start
  xdma->ctrl_reg_write(XDMA_udrv::XDMA_ADDR_TARGET::C2H_CHANNEL, 0, 0x0C, 1);

  // record start time
  clock_gettime(CLOCK_MONOTONIC, &tstart);
  xdma->ctrl_reg_write(XDMA_udrv::XDMA_ADDR_TARGET::C2H_CHANNEL, 0, 0x08, 1);
  // Poll the descriptor complete
  while (
      !(xdma->ctrl_reg_read(XDMA_udrv::XDMA_ADDR_TARGET::C2H_CHANNEL, 0, 0x40) &
        (1 << 2)))
    ;
  // record end time
  clock_gettime(CLOCK_MONOTONIC, &tend);

  // clear descriptor_completed flag
  xdma->ctrl_reg_write(XDMA_udrv::XDMA_ADDR_TARGET::C2H_CHANNEL, 0, 0x40,
                       1 << 2);

  // Show the amount of transfered bytes
  size_t transfer_byte_cnt = buffer.getXferedSize();
  cout << "Transfered " << transfer_byte_cnt << " byte(s)" << endl;

  // Calculate timediff and average throughput in MiB/s
  tdiff = timediff(tstart, tend);
  uint64_t duration_ns = tdiff.tv_sec * 1000000000ULL + tdiff.tv_nsec;
  double avg_tp = transfer_byte_cnt;
  avg_tp /= duration_ns;
  avg_tp *= 1000000000ULL;
  avg_tp /= 1 << 20;

  // Show average throughput
  printf("Transfer completed in %" PRIu64 " nanoseconds\n", duration_ns);
  printf("Average throughput %.5lf MiB/s\n", avg_tp);

  // Dump first 8 AXIS word
  hexdump(buffer.getDataBufferVaddr(), sizeof(axis_word_128) * 8);
  // Check result
  int correct = 1;
  for (int i = 1; i < xfer_size / sizeof(axis_word_128); i++) {
    struct axis_word_128 *this_word, *prev_word, lfsr_result;
    this_word = &((struct axis_word_128 *)buffer.getDataBufferVaddr())[i];
    prev_word = &((struct axis_word_128 *)buffer.getDataBufferVaddr())[i - 1];
    lfsr128(prev_word, &lfsr_result);
    if (!compare_axis_word(this_word, &lfsr_result)) {
      printf("word[%d]:\n", i);
      hexdump(this_word, sizeof(axis_word_128));
      printf("Should be:\n");
      hexdump(&lfsr_result, sizeof(axis_word_128));
      correct = 0;
      break;
    }
  }
  if (correct)
    cout << "Done verifying data" << endl;

  return 0;
}

// Software implementation of 128-bit LFSR (bit 127, 125, 100, 98)
void lfsr128(struct axis_word_128 *target, struct axis_word_128 *result) {
  int zcnt = 0;
  zcnt += (target->data[3] >> 31 & 1) ? 0 : 1;
  zcnt += (target->data[3] >> 29 & 1) ? 0 : 1;
  zcnt += (target->data[3] >> 4 & 1) ? 0 : 1;
  zcnt += (target->data[3] >> 2 & 1) ? 0 : 1;
  result->data[3] =
      (target->data[3] << 1) | ((target->data[2] & (1 << 31)) ? 1 : 0);
  result->data[2] =
      (target->data[2] << 1) | ((target->data[1] & (1 << 31)) ? 1 : 0);
  result->data[1] =
      (target->data[1] << 1) | ((target->data[0] & (1 << 31)) ? 1 : 0);
  result->data[0] = (target->data[0] << 1) | (zcnt & 1 ? 0 : 1);
}

int compare_axis_word(struct axis_word_128 *left, struct axis_word_128 *right) {
  int i;
  for (i = 0; i < 4; i++) {
    if (left->data[i] != right->data[i])
      return 0;
  }
  return 1;
}

void hexdump(const void *data, size_t size) {
  char ascii[17];
  size_t i, j;
  ascii[16] = '\0';
  for (i = 0; i < size; ++i) {
    printf("%02X ", ((unsigned char *)data)[i]);
    if (((unsigned char *)data)[i] >= ' ' &&
        ((unsigned char *)data)[i] <= '~') {
      ascii[i % 16] = ((unsigned char *)data)[i];
    } else {
      ascii[i % 16] = '.';
    }
    if ((i + 1) % 8 == 0 || i + 1 == size) {
      printf(" ");
      if ((i + 1) % 16 == 0) {
        printf("|  %s \n", ascii);
      } else if (i + 1 == size) {
        ascii[(i + 1) % 16] = '\0';
        if ((i + 1) % 16 <= 8) {
          printf(" ");
        }
        for (j = (i + 1) % 16; j < 16; ++j) {
          printf("   ");
        }
        printf("|  %s \n", ascii);
      }
    }
  }
}

struct timespec timediff(struct timespec start, struct timespec end) {
  struct timespec temp;
  if ((end.tv_nsec - start.tv_nsec) < 0) {
    temp.tv_sec = end.tv_sec - start.tv_sec - 1;
    temp.tv_nsec = 1000000000 + end.tv_nsec - start.tv_nsec;
  } else {
    temp.tv_sec = end.tv_sec - start.tv_sec;
    temp.tv_nsec = end.tv_nsec - start.tv_nsec;
  }
  return temp;
}