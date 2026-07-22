#include <grpcpp/grpcpp.h>
#include <cstdio>
#include <string>
#include "common/args.hpp"
#include "controller/controller_service.hpp"

using namespace inspection;

int main(int argc, char** argv) {
  std::string listen = "localhost:50052", camera = "localhost:50051";
  std::string manager = "localhost:50050";
  std::string shm = "/algctl-membox";
  std::size_t frame_bytes = 65536;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--listen") listen = flag_value(argc, argv, i);
    else if (a == "--camera") camera = flag_value(argc, argv, i);
    else if (a == "--manager") manager = flag_value(argc, argv, i);
    else if (a == "--shm") shm = flag_value(argc, argv, i);
    else if (a == "--frame-bytes") frame_bytes = flag_size(argc, argv, i);
    else { std::fprintf(stderr, "unknown flag %s\n", a.c_str()); return 2; }
  }
  auto mgr_channel = grpc::CreateChannel(manager, grpc::InsecureChannelCredentials());
  Membox box(mgr_channel, ShmSegment::open_existing(shm, /*writable=*/false), frame_bytes);
  ControllerService service(
      grpc::CreateChannel(camera, grpc::InsecureChannelCredentials()), box);
  grpc::ServerBuilder b;
  b.AddListeningPort(listen, grpc::InsecureServerCredentials());
  b.RegisterService(&service);
  auto server = b.BuildAndStart();
  if (!server) { std::fprintf(stderr, "cannot listen on %s\n", listen.c_str()); return 1; }
  std::printf("controller on %s (camera %s, awaiting node registrations)\n", listen.c_str(),
              camera.c_str());
  std::fflush(stdout);  // survives teardown-by-signal when stdout is a pipe
  server->Wait();
  return 0;
}
