#include <array>
#include <boost/program_options.hpp>
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
#include "pcicat.hpp"

using namespace std;
namespace fs = std::filesystem;
namespace po = boost::program_options;

struct axis_word_128 {
  uint32_t data[4];
} __attribute__((packed));

void lfsr128(struct axis_word_128 *target, struct axis_word_128 *result);
void hexdump(const void *data, size_t size);
int compare_axis_word(struct axis_word_128 *left, struct axis_word_128 *right);
struct timespec timediff(struct timespec start, struct timespec end);

int main(int argc, char const *argv[]) {
  po::options_description desc("Allowed options");
  desc.add_options()("help,h", "print usage message");
  desc.add_options()("size,s", po::value<string>()->default_value("0x40000000"),
                     "Transfer size in bytes");
  desc.add_options()("fname,f", po::value<string>()->default_value("dump.bin"),
                     "Name of dump file");
  po::variables_map vm;
  store(parse_command_line(argc, argv, desc), vm);

  if (vm.count("help")) {
    cout << desc << "\n";
    return 0;
  }

  if (!vm.count("size")) {
    cerr << "Please specify transfer size" << endl;
    exit(1);
  }

  if (!vm.count("fname")) {
    cerr << "Please specify name of dump file" << endl;
    exit(1);
  }
  uint64_t xfer_size = strtoull(vm["size"].as<string>().c_str(), 0, 0);

  unique_ptr<XDMA_udrv::XDMA> xdma = XDMA_udrv::XDMA::XDMA_factory();
  XDMA_udrv::XSGBuffer buffer(xfer_size);
  // For timing
  struct timespec tstart, tend, tdiff;

  buffer.initialize();

  cout << hex;
  for (uint32_t i = 0; i < buffer.getNrPg(); i++) {
    cout << "Virtual address of page " << i << ": "
         << buffer.getDataBufferVaddr(i) << endl;
  }
  for (uint32_t i = 0; i < buffer.getNrPg(); i++) {
    cout << "Phtsical address of page " << i << ": "
         << buffer.getDataBufferPaddr(i) << endl;
  }
  cout << "DescWB virtual address: " << buffer.getDescWBVaddr() << endl;
  cout << "DescWB physical address: " << buffer.getDescWBPaddr() << endl;
  cout << dec;
  hexdump(buffer.getDescWBVaddr(), 32 * 24);

  // Set C2H channel 0 first descriptor block
  xdma->ctrl_reg_write(XDMA_udrv::XDMA_ADDR_TARGET::C2H_SGDMA, 0, 0x80,
                       buffer.getDescWBPaddr());
  printf("descriptor lo readback: 0x%" PRIX32 "\n",
         xdma->ctrl_reg_read(XDMA_udrv::XDMA_ADDR_TARGET::C2H_SGDMA, 0, 0x80));
  xdma->ctrl_reg_write(XDMA_udrv::XDMA_ADDR_TARGET::C2H_SGDMA, 0, 0x84,
                       buffer.getDescWBPaddr() >> 32);
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
  while (1) {
    uint32_t ret =
        xdma->ctrl_reg_read(XDMA_udrv::XDMA_ADDR_TARGET::C2H_CHANNEL, 0, 0x40);
    if (ret == 0xFFFFFFFF)
      continue;
    else if (ret & (1 << 2))
      break;
    else
      continue;
  }
  printf(
      "C2H channel 0 status: 0x%08X\n",
      xdma->ctrl_reg_read(XDMA_udrv::XDMA_ADDR_TARGET::C2H_CHANNEL, 0, 0x40));
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
  hexdump(buffer.getDataBufferVaddr(0), sizeof(axis_word_128) * 8);
  // // Check result
  // int correct = 1;
  // for (int i = 1; i < xfer_size / sizeof(axis_word_128); i++) {
  //   struct axis_word_128 *this_word, *prev_word, lfsr_result;
  //   // 0x4000000 axis_word_128 per 1 GiB buffer
  //   uint32_t this_word_pg_idx = i / 0x4000000;
  //   uint32_t this_word_pg_off = i % 0x4000000;
  //   uint32_t last_word_pg_idx = (i - 1) / 0x4000000;
  //   uint32_t last_word_pg_off = (i - 1) % 0x4000000;
  //   this_word = &((struct axis_word_128 *)buffer.getDataBufferVaddr(
  //       this_word_pg_idx))[this_word_pg_off];
  //   prev_word = &((struct axis_word_128 *)buffer.getDataBufferVaddr(
  //       last_word_pg_idx))[last_word_pg_off];
  //   lfsr128(prev_word, &lfsr_result);
  //   if (!compare_axis_word(this_word, &lfsr_result)) {
  //     printf("word[%d]:\n", i);
  //     hexdump(this_word, sizeof(axis_word_128));
  //     printf("Should be:\n");
  //     hexdump(&lfsr_result, sizeof(axis_word_128));
  //     correct = 0;
  //     break;
  //   }
  // }
  // if (correct)
  //   cout << "Done verifying data" << endl;

  cout << "Requested " << xfer_size << ", Received " << transfer_byte_cnt
       << endl;

  // Write to file
  int fd;
  fd = open(vm["fname"].as<string>().c_str(), O_WRONLY | O_CREAT | O_TRUNC,
            S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
  if (fd == -1) {
    perror("open()");
    exit(1);
  }
  // Full 1GiB
  for (int i = 0; i < transfer_byte_cnt / (1 << 30); i++) {
    ssize_t xfered_cnt = write(fd, buffer.getDataBufferVaddr(i), 1UL << 30);
    if (xfered_cnt == -1) {
      perror("write()");
      cerr << "Aborted" << endl;
      exit(1);
    } else if (xfered_cnt != (1 << 30)) {
      cerr << "Wrote " << xfered_cnt << " byte(s) < " << (1UL << 30) << endl;
      cerr << "Aborted" << endl;
      exit(1);
    }
  }
  // Residual
  if (transfer_byte_cnt % (1UL << 30)) {
    ssize_t xfered_cnt =
        write(fd, buffer.getDataBufferVaddr(transfer_byte_cnt / (1L << 30)),
              transfer_byte_cnt % (1UL << 30));
    if (xfered_cnt == -1) {
      perror("write()");
      cerr << "Aborted" << endl;
      exit(1);
    } else if (xfered_cnt != transfer_byte_cnt % (1L << 30)) {
      cerr << "Wrote " << xfered_cnt << " byte(s) < "
           << transfer_byte_cnt % (1L << 30) << endl;
      cerr << "Aborted" << endl;
      exit(1);
    }
  }

  close(fd);
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