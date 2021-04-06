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
  unique_ptr<XDMA_udrv::XDMA> xdma(XDMA_udrv::XDMA::XDMA_factory());

  cout << *xdma << endl;

  int fd;
  fd = open("/dev/zero", O_RDWR);
  cout << "fd: " << fd << endl;
  close(fd);

  return 0;
}