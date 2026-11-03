#include <grpcpp/grpcpp.h>
#include <cstdio>
#include <string>
#include "common/args.hpp"
#include "controller/controller_service.hpp"

using namespace inspection;

int main(int argc, char** argv) {
  std::string listen = "localhost:50052", camera = "localhost:50051";
  std::string manager = "localhost:50050";
  ControllerService::Timing timing;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--listen") listen = flag_value(argc, argv, i);
    else if (a == "--camera") camera = flag_value(argc, argv, i);
    else if (a == "--manager") manager = flag_value(argc, argv, i);
    else if (a == "--camera-stream-deadline-s")
      timing.camera_stream_deadline = std::chrono::seconds(flag_size(argc, argv, i));
    else if (a == "--position-deadline-s")
      timing.position_deadline = std::chrono::seconds(flag_size(argc, argv, i));
    else { std::fprintf(stderr, "unknown flag %s\n", a.c_str()); return 2; }
  }
  MemboxControl membox(grpc::CreateChannel(manager, grpc::InsecureChannelCredentials()));
  ControllerService service(
      grpc::CreateChannel(camera, grpc::InsecureChannelCredentials()), membox, timing);
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
