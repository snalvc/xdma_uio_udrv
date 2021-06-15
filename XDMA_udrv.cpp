#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <vector>

#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include "XDMA_udrv.hpp"

using namespace std;
namespace fs = std::filesystem;

namespace {} // namespace

namespace XDMA_udrv {

HugePageWrapper::HugePageWrapper(HugePageSizeType size) {
  int flag = MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB;
  if (size == HUGE_1GiB) {
    flag |= (30 << MAP_HUGE_SHIFT);
    this->length = 1 << 30;
  } else {
    flag |= (21 << MAP_HUGE_SHIFT);
    this->length = 1 << 21;
  }
  this->size_type = size;
  this->virt_addr =
      mmap((void *)0x0UL, this->length, PROT_READ | PROT_WRITE, flag, -1, 0);
  if (this->virt_addr == (void *)-1) {
    throw system_error(error_code(errno, generic_category()), "mmap()");
  }
  // Harmless write to enable allocated hugepage
  uint32_t temp;
  temp = *((uint32_t *)this->virt_addr);
  *((uint32_t *)this->virt_addr) = 87;
  *((uint32_t *)this->virt_addr) = temp;
  // Lookup physical address
  off64_t addr_off;
  int fd_pgm, rv;
  uint64_t pfn;
  fd_pgm = open("/proc/self/pagemap", O_RDONLY);
  if (fd_pgm < 0) {
    throw system_error(error_code(errno, generic_category()), "open() pagemap");
  }
  addr_off = lseek64(
      fd_pgm, (uintptr_t)this->virt_addr / getpagesize() * sizeof(uintptr_t),
      SEEK_SET);
  if (addr_off < 0) {
    throw system_error(error_code(errno, generic_category()), "lseek64()");
  }
  rv = read(fd_pgm, &pfn, sizeof(uintptr_t));
  if (rv <= 0) {
    throw system_error(error_code(errno, generic_category()), "read() pagemap");
  }
  this->phy_addr = pfn * getpagesize();
  close(fd_pgm);
  if (this->phy_addr == 0) {
    throw system_error(error_code(EACCES, generic_category()),
                       "get physical address");
  }
}

HugePageWrapper::~HugePageWrapper() {
  if (this->virt_addr != (void *)-1) {
    munmap(this->virt_addr, this->length);
  }
}

BAR_wrapper::BAR_wrapper(uint64_t start, size_t len, off64_t offset) {
  int mem_fd;
  mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
  if (mem_fd == -1) {
    throw system_error(error_code(errno, generic_category()), "open()");
  }
  this->vaddr =
      mmap((void *)0, len, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, start);
  if (this->vaddr == (void *)-1) {
    throw system_error(error_code(errno, generic_category()), "mmap()");
  }
  close(mem_fd);
  this->len = len;
}

BAR_wrapper::~BAR_wrapper() {
  if (vaddr) {
    int rv;
    rv = munmap(vaddr, len);
    if (rv) {
      perror("munmap()");
    }
  }
}

unique_ptr<XDMA> XDMA::XDMA_factory(int32_t uio_index) {
  string uio_sys_p(UIO_SYS_PATH);
  regex re_uio_id("\\/sys\\/class\\/uio\\/uio(\\d+)");
  regex re_uio_id_map_id("\\/sys\\/class\\/uio\\/uio(\\d+)\\/maps\\/map(\\d+)");
  vector<fs::path> xdma_uio_list;
  vector<int> xdma_uio_id;
  int target_id = -1;

  // Find all XDMA UIO in /sys/class/uio by name
  for (const auto &uios : fs::directory_iterator(uio_sys_p)) {
    // Check if it is XDMA UIO
    string uio_path = uios.path().string();
    ifstream fs_name(uio_path + "/name");
    string uio_name;
    smatch sm_uio_id;

    if (!regex_match(uio_path, sm_uio_id, re_uio_id))
      continue;

    fs_name >> uio_name;
    if (uio_name != XDMA_UIO_NAME)
      continue;

    xdma_uio_list.push_back(uios.path());
    xdma_uio_id.push_back(stol(sm_uio_id[1]));
  }

  if (xdma_uio_list.size() == 0) {
    throw system_error(error_code(-ENOENT, generic_category()), "no xdma uio");
  }

  // specifiy which UIO id
  if (uio_index != -1) {
    for (const auto &id : xdma_uio_id) {
      if (id == uio_index) {
        target_id = id;
      }
    }
    if (target_id == -1)
      throw system_error(error_code(-EINVAL, generic_category()),
                         "specified uio not found");
  } else {
    target_id = 0;
  }

  unique_ptr<XDMA> ret = make_unique<XDMA>(target_id);

  fs::path target_uio_d = xdma_uio_list[target_id];
  fs::path maps_d(target_uio_d.string() + "/maps");
  int num_of_bars = 0;

  // Handle symlink
  if (fs::is_symlink(maps_d)) {
    maps_d = fs::read_symlink(maps_d);
  }

  // Iterate over maps and create correspond BAR mapping
  for (const auto &map_p : fs::directory_iterator(maps_d)) {
    string map_path_s = map_p.path().string();
    int map_id;
    string addr_f = map_path_s + "/addr";
    uint64_t addr;
    string offset_f = map_path_s + "/offset";
    off64_t offset;
    string len_f = map_path_s + "/size";
    size_t len;
    ifstream fs;
    string value;
    smatch sm_uio_map_id;

    if (!fs::exists(addr_f) || !fs::exists(offset_f) || !fs::exists(len_f)) {
      throw system_error(error_code(-ENOENT, generic_category()),
                         "missing one of the map attributes");
    }

    if (!regex_match(map_path_s, sm_uio_map_id, re_uio_id_map_id)) {
      cerr << "regex failed to match map id" << endl;
      continue;
    }

    // Map id
    map_id = stol(sm_uio_map_id[2]);

    if (map_id > PCIE_MAX_BARS - 1) {
      throw system_error(error_code(-EINVAL, generic_category()),
                         "invalid map index");
    }

    // Read address
    fs.open(addr_f);
    fs >> value;
    addr = stoull(value, 0, 0);
    fs.close();

    // Read offset
    fs.open(offset_f);
    fs >> value;
    offset = stoull(value, 0, 0);
    fs.close();

    // Read length
    fs.open(len_f);
    fs >> value;
    len = stoull(value, 0, 0);
    fs.close();

    unique_ptr<XDMA_udrv::BAR_wrapper> pbar;
    try {
      pbar = make_unique<XDMA_udrv::BAR_wrapper>(addr, len, offset);
    } catch (exception &e) {
      cerr << e.what() << endl;
    }
    ret->bars[map_id] = move(pbar);
    num_of_bars++;
  }
  ret->num_of_bars = num_of_bars;

  // Who the fxxk decided to place XDMA register randomly?
  // If only 1 BAR exists, XDMA register would reside in BAR0
  if (num_of_bars == 1) {
    ret->xdma_bar_index = 0;
  }
  // If there're 3 BARs, XDMA register would reside in BAR1
  else if (num_of_bars == 3) {
    ret->xdma_bar_index = 1;
  }
  // Chaos evil
  else if (num_of_bars == 2) {
    uint64_t bar0_len, bar1_len;
    uint32_t bar0_config, bar1_config;
    bar0_config = *((uint32_t *)((uint64_t)ret->bars[0]->getVAddr() + 0x3000));
    bar1_config = *((uint32_t *)((uint64_t)ret->bars[1]->getVAddr() + 0x3000));
    bar0_config &= 0xFFFF0000;
    bar1_config &= 0xFFFF0000;
    bar0_len = ret->bars[0]->getLen();
    bar1_len = ret->bars[1]->getLen();

    // The most tricky case
    if (bar0_len == bar1_len && bar0_len == XDMA_REGISTER_LEN) {
      if (bar0_len == bar1_len) {
        throw system_error(error_code(-EINVAL, generic_category()),
                           "Can't distinguish XDMA register");
      }
      if (bar0_config == XDMA_CONFIG_IDENTIFIER_MASKED)
        ret->xdma_bar_index = 0;
      else
        ret->xdma_bar_index = 1;
    } else if (bar0_len == XDMA_REGISTER_LEN) {
      if (bar0_config == XDMA_CONFIG_IDENTIFIER_MASKED)
        ret->xdma_bar_index = 0;
      else
        throw system_error(error_code(-EINVAL, generic_category()),
                           "Config identifier mismatched");
    } else if (bar1_len == XDMA_REGISTER_LEN) {
      if (bar1_config == XDMA_CONFIG_IDENTIFIER_MASKED)
        ret->xdma_bar_index = 1;
      else
        throw system_error(error_code(-EINVAL, generic_category()),
                           "Config identifier mismatched");
    } else {
      throw system_error(error_code(-EINVAL, generic_category()),
                         "Failed to identify XDMA register");
    }
  }

  return ret;
}

void *XDMA::bar_vaddr(int bar_index) {
  if (bar_index < 0 || bar_index > PCIE_MAX_BARS) {
    return nullptr;
  } else if (!this->bars[bar_index]) {
    return nullptr;
  } else {
    return this->bars[bar_index]->getVAddr();
  }
}

size_t XDMA::bar_len(int bar_index) {
  if (bar_index < 0 || bar_index > PCIE_MAX_BARS) {
    return 0;
  } else if (!this->bars[bar_index]) {
    return 0;
  } else {
    return this->bars[bar_index]->getLen();
  }
}

ostream &operator<<(ostream &os, const XDMA &xdma) {
  os << "XDMA: " << endl;
  os << "uio: uio" << xdma.uio_index << endl;
  os << "# of BARs: " << xdma.num_of_bars << endl;
  os << "XDMA BAR index: " << xdma.xdma_bar_index << endl;

  return os;
}

uint32_t XDMA::ctrl_reg_write(const uint32_t xdma_reg_addr,
                              const uint32_t data) {
  *((volatile uint32_t *)((uint64_t)this->bar_vaddr(
                              this->get_xdma_bar_index()) +
                          (xdma_reg_addr & 0x0000FFFF))) = htole32(data);
  return le32toh(*((volatile uint32_t *)((uint64_t)this->bar_vaddr(
                                             this->get_xdma_bar_index()) +
                                         (xdma_reg_addr & 0x0000FFFF))));
}

uint32_t XDMA::ctrl_reg_write(const XDMA_ADDR_TARGET target,
                              const uint32_t channel,
                              const uint32_t byte_offset, const uint32_t data) {
  uint32_t xdma_reg_addr = 0;
  xdma_reg_addr |= (target << 12);
  xdma_reg_addr |= ((channel & 0xF) << 8);
  xdma_reg_addr |= (byte_offset & 0xFF);
  return this->ctrl_reg_write(xdma_reg_addr, data);
}

uint32_t XDMA::ctrl_reg_read(const uint32_t xdma_reg_addr) {
  return le32toh(*((volatile uint32_t *)((uint64_t)this->bar_vaddr(
                                             this->get_xdma_bar_index()) +
                                         (xdma_reg_addr & 0x0000FFFF))));
}
uint32_t XDMA::ctrl_reg_read(const XDMA_ADDR_TARGET target,
                             const uint32_t channel,
                             const uint32_t byte_offset) {
  uint32_t xdma_reg_addr = 0;
  xdma_reg_addr |= (target << 12);
  xdma_reg_addr |= ((channel & 0xF) << 8);
  xdma_reg_addr |= (byte_offset & 0xFF);
  return this->ctrl_reg_read(xdma_reg_addr);
}

/*
Not sure if this is a good way.
Encapsulate descriptor and huge page buffer related resources and methods in
this class.
data_buf: 1 GiB huge page for data
desc_buf: 2 MiB huge page for descriptors and descriptor writeback. Lower half
(1 MiB) is for descriptors and upper half (1 MiB) is for descriptor writeback.
*/
XHugeBuffer::XHugeBuffer()
    : data_buf(HugePageSizeType::HUGE_1GiB),
      desc_buf(HugePageSizeType::HUGE_2MiB) {
  memset((void *)this->desc_buf.getVAddr(), 0, this->desc_buf.getLen());
}

// Preliminary. Chunk size should be configurable?
void XHugeBuffer::initialize(size_t xfer_size) {
  if (xfer_size > this->data_buf.getLen()) {
    throw std::range_error("Request size over range");
  }
  // Clear descriptor buffer
  memset((void *)this->desc_buf.getVAddr(), 0, this->desc_buf.getLen());

  uint32_t n_desc =
      xfer_size / MEM_CHUNK_SIZE + ((xfer_size % MEM_CHUNK_SIZE) ? (1) : (0));
  uint32_t last_block_idx = xfer_size / MEM_CHUNK_SIZE - 1;

  // Store # of desc for later use
  this->n_desc = n_desc;

  // Fill in descriptors
  // !!! Skipped max adjacent descriptors constraint (16) !!!
  // Since 1 GiB buffer and 128 MiB chunk is used here, there will be no more
  // than 8 descriptors
  struct xdma_desc *pdesc = (struct xdma_desc *)this->desc_buf.getVAddr();
  // Magic, next_adj and length
  // !!! Endianess is not handled since we're on x86 !!!
  for (uint32_t i = 0, nxt_adj = n_desc - 2; i < n_desc; i++, nxt_adj--) {
    nxt_adj = (nxt_adj < 0) ? 0 : nxt_adj;
    pdesc[i].control |= __MASK_SHIFT__(16, 16, XDMA_DESC_MAGIC);
    pdesc[i].control |= __MASK_SHIFT__(8, 6, nxt_adj);
    pdesc[i].bytes = MEM_CHUNK_SIZE;
  }
  // Set stop and completed flag at the last descriptor
  pdesc[last_block_idx].control |= __MASK_SHIFT__(0, 1, 1);
  pdesc[last_block_idx].control |= __MASK_SHIFT__(1, 1, 1);
  // Chain descriptors
  for (uint32_t i = 0; i < n_desc - 1; i++) {
    uint64_t addr = this->desc_buf.getPAddr() + (i + 1) * sizeof(xdma_desc);
    pdesc[i].next_lo = addr;
    pdesc[i].next_hi = addr >> 32;
  }
  // Set buffer address and WB address
  for (uint32_t i = 0; i < n_desc; i++) {
    uint64_t buff_addr = this->data_buf.getPAddr() + i * MEM_CHUNK_SIZE;
    uint64_t wb_addr = this->desc_buf.getPAddr() + this->desc_buf.getLen() / 2 +
                       i * sizeof(c2h_wb);
    pdesc[i].dst_addr_lo = buff_addr;
    pdesc[i].dst_addr_hi = buff_addr >> 32;
    pdesc[i].src_addr_lo = wb_addr;
    pdesc[i].src_addr_hi = wb_addr >> 32;
  }
}

uint64_t XHugeBuffer::getXferedSize() {
  c2h_wb *pwb = (c2h_wb *)((uintptr_t)this->desc_buf.getVAddr() +
                           this->desc_buf.getLen() / 2);
  uint64_t xfered_size = 0;
  for (uint32_t i = 0; i < this->n_desc; i++) {
    xfered_size += pwb[i].length;
  }
  return xfered_size;
}

XSGBuffer::XSGBuffer(const uint64_t size)
    : desc_wb_buf(HugePageSizeType::HUGE_2MiB) {
  uint32_t nr_1gibp;

  this->size = size;

  // Currently descriptor buffer size is 1MiB (share 2 MiB hugepage with C2H WB)
  // 1 MiB / sizeof(desc) = 32768
  // Max size per descriptor = 128 MiB
  // => 4096 GiB buffer if 128MiB chunk is used
  // Manually set a 3 GiB upper limit for current use
  if (size > XSGB_MAX_SIZE) {
    throw std::runtime_error("Can't receive more than 3 GiB (soft constraint)");
  }

  nr_1gibp = size / (1UL << 30) + (size % (1UL << 30) ? 1 : 0);
  for (uint32_t i = 0; i < nr_1gibp; i++) {
    this->data_buf.push_back(
        make_unique<HugePageWrapper>(HugePageSizeType::HUGE_1GiB));
  }
}

void XSGBuffer::initialize() {
  uint32_t nr_desc;

  // # of chunks = # of descriptors
  nr_desc = this->size / MEM_CHUNK_SIZE + (this->size % MEM_CHUNK_SIZE ? 1 : 0);
  this->nr_desc = nr_desc;

  struct xdma_desc *pdesc = (struct xdma_desc *)this->desc_wb_buf.getVAddr();

  // Magic and length
  // !!! Endianess is not handled since we're on x86 !!!
  for (uint32_t i = 0; i < nr_desc; i++) {
    pdesc[i].control |= __MASK_SHIFT__(16, 16, XDMA_DESC_MAGIC);
    pdesc[i].bytes = MEM_CHUNK_SIZE;
  }

  // nxt_adj
  // use 8 desc/block <=> 1GiB/block
  // full block
  for (uint32_t full_1gib = 0; full_1gib < nr_desc / 8; full_1gib++) {
    for (int i = 0; i < 8; i++) {
      pdesc[8 * full_1gib + i].control |=
          __MASK_SHIFT__(8, 6, ((6 - i) < 0) ? 0 : (6 - i));
    }
  }
  // residual
  if (nr_desc % 8) {
    for (int i = 0; i < nr_desc % 8; i++) {
      pdesc[8 * (nr_desc / 8) + i].control |=
          __MASK_SHIFT__(8, 6,
                         (((int32_t)nr_desc % 8 - 2 - i) < 0)
                             ? 0
                             : ((int32_t)nr_desc % 8 - 2 - i));
    }
  }

  // Set stop and completed flag at the last descriptor
  pdesc[nr_desc - 1].control |= __MASK_SHIFT__(0, 1, 1); // Stop
  pdesc[nr_desc - 1].control |= __MASK_SHIFT__(1, 1, 1); // Completed

  // Chain descriptors
  for (uint32_t i = 0; i < nr_desc - 1; i++) {
    uint64_t addr = this->desc_wb_buf.getPAddr() + (i + 1) * sizeof(xdma_desc);
    pdesc[i].next_lo = addr;
    pdesc[i].next_hi = addr >> 32;
  }

  // Set buffer address and WB address
  for (uint32_t i = 0; i < nr_desc; i++) {
    uint64_t buff_addr =
        this->data_buf[i / 8]->getPAddr() + (i % 8) * MEM_CHUNK_SIZE;
    uint64_t wb_addr = this->desc_wb_buf.getPAddr() +
                       this->desc_wb_buf.getLen() / 2 + i * sizeof(c2h_wb);
    pdesc[i].dst_addr_lo = buff_addr;
    pdesc[i].dst_addr_hi = buff_addr >> 32;
    pdesc[i].src_addr_lo = wb_addr;
    pdesc[i].src_addr_hi = wb_addr >> 32;
  }
}

void *XSGBuffer::getDataBufferVaddr(uint32_t index) {
  if (index > this->data_buf.size())
    return (void *)(0);
  return this->data_buf[index]->getVAddr();
}

uint64_t XSGBuffer::getDataBufferPaddr(uint32_t index) {
  if (index > this->data_buf.size())
    return 0;
  return this->data_buf[index]->getPAddr();
}

uint64_t XSGBuffer::getXferedSize() {
  c2h_wb *pwb = (c2h_wb *)((uintptr_t)this->desc_wb_buf.getVAddr() +
                           this->desc_wb_buf.getLen() / 2);
  uint64_t xfered_size = 0;
  for (uint32_t i = 0; i < this->nr_desc; i++) {
    xfered_size += pwb[i].length;
  }
  return xfered_size;
}

} // namespace XDMA_udrv