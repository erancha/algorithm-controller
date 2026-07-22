#include <grpcpp/grpcpp.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>
#include "camera/camera_service.hpp"
#include "common/args.hpp"

using namespace inspection;

namespace {

double parse_frame_rate(const std::string& v) {
  try {
    return std::stod(v);
  } catch (const std::exception&) {
    std::fprintf(stderr, "invalid numeric value '%s' for --frame-rate\n", v.c_str());
    std::exit(2);
  }
}

}  // namespace

int main(int argc, char** argv) {
  bool generate = false, simulate = false;
  std::string fixtures = "fixtures", listen = "localhost:50051";
  std::string manager = "localhost:50050", shm = "/algctl-membox";
  std::string die_counts = "20";
  double frame_rate = 200.0;
  std::size_t frame_bytes = 65536;
  Geometry g;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--generate") generate = true;
    else if (a == "--simulate") simulate = true;
    else if (a == "--fixtures") fixtures = flag_value(argc, argv, i);
    else if (a == "--listen") listen = flag_value(argc, argv, i);
    else if (a == "--manager") manager = flag_value(argc, argv, i);
    else if (a == "--shm") shm = flag_value(argc, argv, i);
    else if (a == "--frame-rate") frame_rate = parse_frame_rate(flag_value(argc, argv, i));
    else if (a == "--frame-bytes") frame_bytes = flag_size(argc, argv, i);
    else if (a == "--die-counts") die_counts = flag_value(argc, argv, i);
    else if (a == "--frame-dim")
      g.frame_width = g.frame_height = static_cast<std::uint32_t>(flag_size(argc, argv, i));
    else if (a == "--frames-per-die")
      g.frames_per_die = static_cast<std::uint32_t>(flag_size(argc, argv, i));
    else { std::fprintf(stderr, "unknown flag %s\n", a.c_str()); return 2; }
  }

  if (generate) {
    std::vector<std::uint32_t> dies;
    std::stringstream ss(die_counts);
    for (std::string part; std::getline(ss, part, ',');) dies.push_back(std::stoul(part));
    // Two seeded defects at distinct positions — the integration test's ground
    // truth. Coordinates and radii describe the default 256×256 geometry and
    // scale with the dimension: a larger frame images the same physical die at
    // finer pixel pitch, so the defect's share of the frame's pixels — and its
    // score contribution — stays resolution-independent.
    std::vector<Defect> defects = {
        {.die = 3, .frame_in_die = 7, .x = 100, .y = 120, .radius = 6, .delta = 90},
        {.die = 11, .frame_in_die = 19, .x = 40, .y = 200, .radius = 4, .delta = -70},
    };
    const double scale = g.frame_width / 256.0;
    for (auto& d : defects) {
      d.x = static_cast<std::uint32_t>(std::lround(d.x * scale));
      d.y = static_cast<std::uint32_t>(std::lround(d.y * scale));
      d.radius = std::max<std::uint32_t>(
          1, static_cast<std::uint32_t>(std::lround(d.radius * scale)));
    }
    generate_fixtures(fixtures, g, dies, defects);
    std::printf("generated %zu slice(s) under %s\n", dies.size(), fixtures.c_str());
    return 0;
  }
  if (!simulate) { std::fprintf(stderr, "need --generate or --simulate\n"); return 2; }

  auto channel = grpc::CreateChannel(manager, grpc::InsecureChannelCredentials());
  Membox box(channel, ShmSegment::open_existing(shm, /*writable=*/true), frame_bytes);
  CameraService service(fixtures, box, frame_rate);
  grpc::ServerBuilder b;
  b.AddListeningPort(listen, grpc::InsecureServerCredentials());
  b.RegisterService(&service);
  auto server = b.BuildAndStart();
  if (!server) { std::fprintf(stderr, "cannot listen on %s\n", listen.c_str()); return 1; }
  std::printf("camera (simulate) on %s serving %s\n", listen.c_str(), fixtures.c_str());
  std::fflush(stdout);  // survives teardown-by-signal when stdout is a pipe
  server->Wait();
  return 0;
}
