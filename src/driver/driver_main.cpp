#include <grpcpp/grpcpp.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>
#include "common/args.hpp"
#include "inspection.grpc.pb.h"

using namespace inspection;

int main(int argc, char** argv) {
  std::string controller = "localhost:50052";
  std::uint64_t slice = 0;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--controller") controller = flag_value(argc, argv, i);
    else if (a == "--slice") {
      try {
        slice = std::stoull(flag_value(argc, argv, i));
      } catch (const std::exception& e) {
        std::fprintf(stderr, "invalid numeric value for --slice: %s\n", e.what());
        return 2;
      }
    }
    else { std::fprintf(stderr, "unknown flag %s\n", a.c_str()); return 2; }
  }
  auto stub = v1::Controller::NewStub(
      grpc::CreateChannel(controller, grpc::InsecureChannelCredentials()));
  grpc::ClientContext ctx;
  // Comfortably above the controller's own internal deadlines (5 min camera
  // stream + 1 min per position), so those fire first with better diagnoses;
  // this bound only catches a hung controller itself.
  ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::minutes(10));
  v1::SliceRequest req;
  req.set_slice_id(slice);
  v1::SliceResults results;
  grpc::Status st = stub->ProcessSlice(&ctx, req, &results);
  if (!st.ok()) {
    std::fprintf(stderr, "ProcessSlice failed: %s\n", st.error_message().c_str());
    return 1;
  }
  // Column meanings and ranking go to stderr, keeping stdout machine-readable
  // data only (tests/e2e.sh parses it; redirecting to a file keeps just numbers).
  std::vector<double> sorted(results.values().begin(), results.values().end());
  std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
  const double median = sorted[sorted.size() / 2];
  std::fprintf(stderr,
               "\nslice %llu: every die is photographed as the same %d frames; the score at\n"
               "position i measures how much the dies disagree on their frame i. Clean\n"
               "positions sit near this slice's noise floor (median score %.6f); a\n"
               "defective position stands clearly above it.\n"
               "position  score\n",
               static_cast<unsigned long long>(slice), results.values_size(), median);
  for (int i = 0; i < results.values_size(); ++i)
    std::printf("%8d  %.6f\n", i, results.values(i));
  std::fflush(stdout);

  std::vector<int> by_score(results.values_size());
  std::iota(by_score.begin(), by_score.end(), 0);
  const int top = std::min(3, results.values_size());
  std::partial_sort(by_score.begin(), by_score.begin() + top, by_score.end(),
                    [&](int a, int b) { return results.values(a) > results.values(b); });
  std::fprintf(stderr, "highest:");
  for (int i = 0; i < top; ++i)
    std::fprintf(stderr, "%s position %d (%.6f)", i ? "," : "", by_score[i],
                 results.values(by_score[i]));
  std::fprintf(stderr, "\n");
  return 0;
}
