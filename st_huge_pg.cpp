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
#include <sys/mman.h>
#include <unistd.h>

#include "XDMA_udrv.hpp"
#include "st_huge_pg.hpp"

using namespace std;
namespace fs = std::filesystem;

struct axis_word_128 {
  uint32_t data[4];
} __attribute__((packed));

int main(int argc, char const *argv[]) {
  unique_ptr<XDMA_udrv::XDMA> xdma = XDMA_udrv::XDMA::XDMA_factory();

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
  cout << "Version: 0x" << (cbi & 0x0000000F) << endl;

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