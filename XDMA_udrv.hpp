#ifndef _XDMA_UDRV_HPP_
#define _XDMA_UDRV_HPP_

#include <cstdint>
#include <cstdlib>
#include <memory>

#define PCIE_MAX_BARS 6

#define UIO_SYS_PATH "/sys/class/uio/"

#define XDMA_UIO_NAME "xdma_uio"
#define XDMA_REGISTER_LEN 65536
#define XDMA_CONFIG_IDENTIFIER_MASKED 0x1FC30000

using namespace std;

namespace XDMA_udrv {
class BAR_wrapper {
public:
  BAR_wrapper() = delete;
  BAR_wrapper(uint64_t start, size_t len, off64_t offset);
  ~BAR_wrapper();

  void *getVAddr() { return this->vaddr; }

  size_t getLen() { return this->len; }

private:
  void *vaddr;
  size_t len;
};

class XDMA {
public:
  XDMA() = delete;
  XDMA(int uio_index) { this->uio_index = uio_index; }

  static unique_ptr<XDMA> XDMA_factory(int32_t uio_index = -1);

  int32_t get_num_of_bars() { return this->num_of_bars; }
  int32_t get_xdma_bar_index() { return this->xdma_bar_index; }
  int get_uio_index() { return this->uio_index; }
  void *bar_vaddr(int bar_index);
  size_t bar_len(int bar_index);
  friend ostream &operator<<(ostream &os, const XDMA &xdma);

private:
  int uio_index;
  int32_t num_of_bars;
  int32_t xdma_bar_index;
  array<unique_ptr<BAR_wrapper>, PCIE_MAX_BARS> bars;
};

} // namespace XDMA_udrv

#endif