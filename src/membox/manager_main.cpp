#include <grpcpp/grpcpp.h>
#include <pthread.h>
#include <signal.h>
#include <cstdio>
#include <string>
#include <thread>
#include "common/args.hpp"
#include "membox/manager_service.hpp"
#include "membox/shm_segment.hpp"

// Flags: --listen addr --slots N --frame-bytes N --shm name
int main(int argc, char** argv) {
  std::string listen = "localhost:50050", shm_name = "/algctl-membox";
  std::size_t slots = 512, frame_bytes = 65536;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--listen") listen = inspection::flag_value(argc, argv, i);
    else if (a == "--slots") slots = inspection::flag_size(argc, argv, i);
    else if (a == "--frame-bytes") frame_bytes = inspection::flag_size(argc, argv, i);
    else if (a == "--shm") shm_name = inspection::flag_value(argc, argv, i);
    else { std::fprintf(stderr, "unknown flag %s\n", a.c_str()); return 2; }
  }
  auto segment = inspection::ShmSegment::create(shm_name, slots * frame_bytes);
  inspection::ManagerService service(slots, frame_bytes);
  // The segment outlives the process in /dev/shm unless its owning destructor
  // runs, so SIGTERM/SIGINT (the launcher's teardown) must unwind main rather
  // than kill the process — at production frame sizes a leaked segment pins
  // gigabytes of RAM until the next run. Signals are blocked before gRPC
  // spawns its worker threads so the sigwait thread is the sole receiver.
  sigset_t term_signals;
  sigemptyset(&term_signals);
  sigaddset(&term_signals, SIGTERM);
  sigaddset(&term_signals, SIGINT);
  pthread_sigmask(SIG_BLOCK, &term_signals, nullptr);
  grpc::ServerBuilder b;
  b.AddListeningPort(listen, grpc::InsecureServerCredentials());
  b.RegisterService(&service);
  auto server = b.BuildAndStart();
  if (!server) { std::fprintf(stderr, "cannot listen on %s\n", listen.c_str()); return 1; }
  std::printf("membox-manager on %s, %zu slots x %zu bytes\n", listen.c_str(), slots,
              frame_bytes);
  // Flushed so the line survives even when stdout is a block-buffered pipe and
  // the process is later killed by the launcher's teardown.
  std::fflush(stdout);
  std::thread waiter([&server, &term_signals] {
    int sig;
    sigwait(&term_signals, &sig);
    server->Shutdown();
  });
  server->Wait();
  waiter.join();
  return 0;
}
