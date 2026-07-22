#include <grpcpp/grpcpp.h>
#include <chrono>
#include <cstdio>
#include <string>
#include "common/args.hpp"
#include "compute_node/node_service.hpp"

using namespace inspection;

int main(int argc, char** argv) {
  std::string listen = "localhost:50061", manager = "localhost:50050";
  std::string controller = "localhost:50052";
  std::string shm = "/algctl-membox";
  std::size_t frame_bytes = 65536;
  int compute_ms = 200;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--listen") listen = flag_value(argc, argv, i);
    else if (a == "--controller") controller = flag_value(argc, argv, i);
    else if (a == "--manager") manager = flag_value(argc, argv, i);
    else if (a == "--shm") shm = flag_value(argc, argv, i);
    else if (a == "--frame-bytes") frame_bytes = flag_size(argc, argv, i);
    else if (a == "--compute-ms") compute_ms = static_cast<int>(flag_size(argc, argv, i));
    else { std::fprintf(stderr, "unknown flag %s\n", a.c_str()); return 2; }
  }
  auto channel = grpc::CreateChannel(manager, grpc::InsecureChannelCredentials());
  Membox box(channel, ShmSegment::open_existing(shm, /*writable=*/false), frame_bytes);
  NodeService service(box, compute_ms);
  grpc::ServerBuilder b;
  b.AddListeningPort(listen, grpc::InsecureServerCredentials());
  b.RegisterService(&service);
  auto server = b.BuildAndStart();
  if (!server) { std::fprintf(stderr, "cannot listen on %s\n", listen.c_str()); return 1; }
  grpc::Status st = register_with_controller(controller, listen, std::chrono::seconds(30));
  if (!st.ok()) {
    std::fprintf(stderr, "cannot register with controller %s: %s\n", controller.c_str(),
                 st.error_message().c_str());
    return 1;
  }
  std::printf("compute-node on %s (registered with %s)\n", listen.c_str(), controller.c_str());
  std::fflush(stdout);  // survives teardown-by-signal when stdout is a pipe
  server->Wait();
  return 0;
}
