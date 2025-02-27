#include <linux/perf_event.h>
#include <linux/version.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "BPF.h"
#include "catch.hpp"

struct cb_data {
  uint64_t data_received;
  std::atomic<size_t> sleep_ms;
};

void handle_data_fn(void* cb_cookie, void*, int data_size) {
  struct cb_data* cookie_data = static_cast<struct cb_data*>(cb_cookie);
  cookie_data->data_received += data_size;
  // Force the handler to take a little while so that the ring buffer consumer
  // is slower than the producer.
  std::this_thread::sleep_for(
      std::chrono::milliseconds(cookie_data->sleep_ms.load()));
}
void handle_data_loss_fn(void*, unsigned long) {}

class RAIIThreadCloser {
 public:
  RAIIThreadCloser(std::atomic<bool>* done, std::thread* thread)
      : done_(done), thread_(thread) {}
  ~RAIIThreadCloser() {
    done_->store(true);
    thread_->join();
  }

 private:
  std::atomic<bool>* done_;
  std::thread* thread_;
};

/**
 * This test demonstrates a bug in perf_reader where perf_reader_event_read can
 * loop over the ring buffer more than once in a single call, if the consumer of
 * the event data (i.e. raw_cb) is slower than the producer (the kernel pushing
 * events from ebpf). To demonstrate this bug we have a thread that countinually
 * writes to /dev/null, then we deploy a BPF program that looks for writes from
 * this PID and for each write it will submit 30kB to the perf buffer. Then we
 * artificially slow down the perf buffer data callback so that its slower than
 * the kernel producing data. If we removed the timeout in the test the
 * perf_reader->poll() call could potentially run indefinitely (depending on the
 * num_pages and sleep_ms it might not run forever but if sleep_ms is large
 * enough it will). Instead we set a timeout and check that the amount of data
 * read from a single `poll()` call is less than the size of the kernel ring
 * buffer.
 */
TEST_CASE("test perf buffer poll full ring buf", "[bpf_perf_event]") {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
  std::atomic<bool> write_done(false);
  // This thread writes to /dev/null continuouosly so it should trigger many
  // write syscalls which will fill up the perf ring buf.
  std::thread write_thread([&]() {
    std::ofstream out("/dev/null");
    while (!write_done.load()) {
      out << "test";
      out.flush();
    }
  });
  RAIIThreadCloser closer(&write_done, &write_thread);

#define MSG_SIZE (32 * 1024)
  const std::string BPF_PROGRAM_FORMAT_STR = R"(
    #include <uapi/linux/ptrace.h>
    #define MSG_SIZE %d
    struct event_t {
      char msg[MSG_SIZE];
    };
    BPF_PERF_OUTPUT(events);
    BPF_PERCPU_ARRAY(events_heap, struct event_t, 1);
    // Probe that submits a 32kB event every time write is called.
    int syscall__probe_entry_write(struct pt_regs* ctx, int fd, char* buf, size_t count) {
      uint32_t kZero = 0;
      struct event_t* event = events_heap.lookup(&kZero);
      if (event == NULL) {
        return 0;
      }
      uint32_t tgid = bpf_get_current_pid_tgid() >> 32;
      if (tgid != %lu) {
        return 0;
      }
      events.perf_submit(ctx, event, sizeof(struct event_t));
      return 0;
    }
  )";
  unsigned long pid = getpid();

  // Use the BPF program as a format string to insert the current test
  // processes' PID so that we only submit events generated by the current
  // process.
  auto extra_length = snprintf(nullptr, 0, "%d %lu", MSG_SIZE, pid);
  char bpf_program[BPF_PROGRAM_FORMAT_STR.size() + extra_length];
  auto n = snprintf(bpf_program, sizeof(bpf_program),
                    BPF_PROGRAM_FORMAT_STR.c_str(), MSG_SIZE, pid);
  const std::string BPF_PROGRAM(bpf_program, n);

  auto num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
  auto page_size = sysconf(_SC_PAGE_SIZE);

  ebpf::BPF bpf;
  ebpf::StatusTuple res(0);
  res = bpf.init(BPF_PROGRAM, {"-DNUM_CPUS=" + std::to_string(num_cpus)}, {});
  REQUIRE(res.code() == 0);

  std::string write_fnname = bpf.get_syscall_fnname("write");
  res = bpf.attach_kprobe(write_fnname, "syscall__probe_entry_write");
  REQUIRE(res.code() == 0);

  struct cb_data cb_cookie = {};
  cb_cookie.sleep_ms = 200;
  cb_cookie.data_received = 0;

  int num_pages = 64;
  std::string perf_buffer_name("events");
  res = bpf.open_perf_buffer(perf_buffer_name, handle_data_fn,
                             handle_data_loss_fn, &cb_cookie, num_pages);
  REQUIRE(res.code() == 0);

  std::atomic<bool> poll_done(false);
  int cnt = 0;
  std::thread poll_thread([&]() {
    auto perf_buffer = bpf.get_perf_buffer(perf_buffer_name);
    REQUIRE(perf_buffer != nullptr);
    cnt = perf_buffer->poll(0);
    poll_done = true;
  });

  auto start = std::chrono::steady_clock::now();
  std::chrono::seconds timeout(20);
  while (!poll_done.load() &&
         (std::chrono::steady_clock::now() - start) < timeout) {
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  // After the timeout we set the sleep time to 0, so the reader should catch up
  // and terminate.
  cb_cookie.sleep_ms = 0;
  poll_thread.join();

  res = bpf.close_perf_buffer(perf_buffer_name);
  REQUIRE(res.code() == 0);
  res = bpf.detach_kprobe(write_fnname);
  REQUIRE(res.code() == 0);

  // cnt is the number of perf_readers the poll() call read from. So we should
  // not have received more data then 1 full ring buffer per perf_reader.
  REQUIRE(cb_cookie.data_received <= (cnt * num_pages * page_size));
#endif
}
