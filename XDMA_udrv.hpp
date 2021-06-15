#ifndef _XDMA_UDRV_HPP_
#define _XDMA_UDRV_HPP_

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

#define PCIE_MAX_BARS 6

#define UIO_SYS_PATH "/sys/class/uio/"

#define XDMA_UIO_NAME "xdma_uio"
#define XDMA_REGISTER_LEN 65536
#define XDMA_CONFIG_IDENTIFIER_MASKED 0x1FC30000

using namespace std;

namespace XDMA_udrv {

enum HugePageSizeType { HUGE_1GiB, HUGE_2MiB };
// XDMA allows max size of (1 << 28) - 1 bytes, we choose 1 << 27 chunk
const uint32_t MEM_CHUNK_SIZE = 1UL << 27;
// Max 3GiB
const uint64_t XSGB_MAX_SIZE = 3 * (1UL << 30);

#define __GET_MASK__(_len) ((1 << _len) - 1)
#define __GET_SHIFTED_MASK__(_offset, _len) (__GET_MASK__(_len) << _offset)
#define __MASK_SHIFT__(_offset, _len, _value)                                  \
  ((_value & __GET_MASK__(_len)) << _offset)

// XDMA register constants
#define XDMA_DESC_MAGIC 0xAD4B

class HugePageWrapper {
public:
  HugePageWrapper() = delete;
  HugePageWrapper(enum HugePageSizeType);
  ~HugePageWrapper();

  void *getVAddr() { return this->virt_addr; }
  uint64_t getPAddr() { return this->phy_addr; }
  size_t getLen() { return this->length; }
  HugePageSizeType getSizeType() { return this->size_type; }

private:
  size_t length;
  uint64_t phy_addr;
  void *virt_addr;
  HugePageSizeType size_type;
};

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

enum XDMA_ADDR_TARGET : int {
  H2C_CHANNEL = 0,
  C2H_CHANNEL,
  IRQ_BLOCK,
  CONFIG,
  H2C_SGDMA,
  C2H_SGDMA,
  SGDMA_COMMON,
  MSIX
};

class XDMA {
public:
  XDMA() = delete;
  XDMA(int uio_index) { this->uio_index = uio_index; }

  static unique_ptr<XDMA> XDMA_factory(int32_t uio_index = -1);

  uint32_t ctrl_reg_write(const uint32_t xdma_reg_addr, const uint32_t data);
  uint32_t ctrl_reg_write(const XDMA_ADDR_TARGET target, const uint32_t channel,
                          const uint32_t byte_offset, const uint32_t data);
  uint32_t ctrl_reg_read(const uint32_t xdma_reg_addr);
  uint32_t ctrl_reg_read(const XDMA_ADDR_TARGET target, const uint32_t channel,
                         const uint32_t byte_offset);

  int32_t get_num_of_bars() { return this->num_of_bars; }
  int32_t get_xdma_bar_index() { return this->xdma_bar_index; }
  int get_uio_index() { return this->uio_index; }
  void *bar_vaddr(int bar_index);
  size_t bar_len(int bar_index);
  friend ostream &operator<<(ostream &os, const XDMA &xdma);

  static const int num_of_bars_max = PCIE_MAX_BARS;

private:
  int uio_index;
  int32_t num_of_bars;
  int32_t xdma_bar_index;
  array<unique_ptr<BAR_wrapper>, PCIE_MAX_BARS> bars;
};

struct xdma_desc {
  uint32_t control;
  uint32_t bytes;       /* transfer length in bytes */
  uint32_t src_addr_lo; /* source address (low 32-bit) */
  uint32_t src_addr_hi; /* source address (high 32-bit) */
  uint32_t dst_addr_lo; /* destination address (low 32-bit) */
  uint32_t dst_addr_hi; /* destination address (high 32-bit) */
  /*
   * next descriptor in the single-linked list of descriptors;
   * this is the PCIe (bus) address of the next descriptor in the
   * root complex memory
   */
  uint32_t next_lo; /* next desc address (low 32-bit) */
  uint32_t next_hi; /* next desc address (high 32-bit) */
} __attribute__((packed));

struct c2h_wb {
  uint32_t status;
  uint32_t length;
} __attribute__((packed));

class XHugeBuffer {
public:
  XHugeBuffer();

  void initialize(size_t xfer_size);
  uint64_t getXferedSize();
  void *getDataBufferVaddr() { return this->data_buf.getVAddr(); }
  uint64_t getDataBufferPaddr() { return this->data_buf.getPAddr(); }
  void *getDescBufferVaddr() { return this->desc_buf.getVAddr(); }
  uint64_t getDescBufferPaddr() { return this->desc_buf.getPAddr(); }

private:
  HugePageWrapper data_buf;
  HugePageWrapper desc_buf;
  uint32_t n_desc;
};

// XDMA SG buffer base on huge page
class XSGBuffer {
public:
  XSGBuffer(const uint64_t size);
  void initialize();
  void *getDescWBVaddr() { return this->desc_wb_buf.getVAddr(); }
  uint64_t getDescWBPaddr() { return this->desc_wb_buf.getPAddr(); }
  uint32_t getNrPg() { return this->data_buf.size(); }
  void *getDataBufferVaddr(uint32_t index);
  uint64_t getDataBufferPaddr(uint32_t index);
  uint64_t getXferedSize();

private:
  uint64_t size;
  uint32_t nr_desc;
  HugePageWrapper desc_wb_buf;
  std::vector<unique_ptr<HugePageWrapper>> data_buf;
};

} // namespace XDMA_udrv

#endif