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
