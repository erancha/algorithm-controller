#pragma once
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>
#include "camera/fixtures.hpp"
#include "common/pgm.hpp"
#include "inspection.grpc.pb.h"
#include "membox/membox.hpp"

namespace inspection {

// Serves ImageSlice from a fixture tree: every frame goes through
// write_frame (③) and is reported as a FrameStored message (④); normal
// stream end is the completion signal (⑤).
class CameraService final : public v1::Camera::Service {
 public:
  CameraService(std::filesystem::path fixtures_root, Membox& box, double frame_rate)
      : root_(std::move(fixtures_root)), box_(box), frame_rate_(frame_rate) {}

  grpc::Status ImageSlice(grpc::ServerContext*, const v1::SliceRequest* req,
                          grpc::ServerWriter<v1::FrameStored>* writer) override {
    namespace fs = std::filesystem;
    fs::path slice_dir = root_ / ("slice_" + std::to_string(req->slice_id()));
    if (!fs::exists(slice_dir))
      return {grpc::StatusCode::NOT_FOUND, "no fixtures for slice"};
    for (std::uint32_t die = 0;; ++die) {
      fs::path die_dir = slice_dir / ("die_" + std::to_string(die));
      if (!fs::exists(die_dir)) break;
      for (std::uint32_t frame = 0;; ++frame) {
        fs::path file = die_dir / ("frame_" + std::to_string(frame) + ".pgm");
        if (!fs::exists(file)) break;
        SlotOffset slot;
        try {
          Image img = read_pgm(file);
          auto bytes = std::as_bytes(std::span(img.pixels));
          slot = box_.write_frame(bytes);
        } catch (const std::exception& e) {
          return {grpc::StatusCode::INTERNAL, e.what()};
        }
        v1::FrameStored msg;
        msg.set_die(die);
        msg.set_frame_in_die(frame);
        msg.set_slot(slot);
        if (!writer->Write(msg)) {
          // The controller never learned of this slot, so nobody else can
          // release it. Frames reported before this point are the
          // controller's to release on its stream-error path.
          try {
            SlotOffset orphan[] = {slot};
            box_.release_slots(orphan);
          } catch (const std::exception& e) {
            std::fprintf(stderr, "releasing unreported slot failed: %s\n", e.what());
          }
          return {grpc::StatusCode::CANCELLED, "client went away"};
        }
        if (frame_rate_ > 0)
          std::this_thread::sleep_for(std::chrono::duration<double>(1.0 / frame_rate_));
      }
    }
    return grpc::Status::OK;
  }

 private:
  std::filesystem::path root_;
  Membox& box_;
  double frame_rate_;
};

}  // namespace inspection
