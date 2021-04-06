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
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include "XDMA_udrv.hpp"

using namespace std;
namespace fs = std::filesystem;

namespace {
// Use glibc functions but not std::filesystem
vector<string> get_folder_entries(string path) {}
} // namespace

namespace XDMA_udrv {

BAR_wrapper::BAR_wrapper(uint64_t start, size_t len, off64_t offset) {
  int rv, mem_fd;
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
    cout << "unmap " << this->len << " bytes @" << this->vaddr << endl;
    rv = munmap(vaddr, len);
    cout << "rv = " << rv << endl;
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
    cout << "Mapped " << pbar->getLen() << " bytes @" << pbar->getVAddr()
         << endl;
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
  } else {
    return this->bars[bar_index]->getVAddr();
  }
}

size_t XDMA::bar_len(int bar_index) {
  if (bar_index < 0 || bar_index > PCIE_MAX_BARS) {
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

} // namespace XDMA_udrv