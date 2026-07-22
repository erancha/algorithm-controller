#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "camera/fixtures.hpp"
#include "common/pgm.hpp"

using namespace inspection;
namespace fs = std::filesystem;

TEST(Fixtures, GeneratesTreeManifestAndVisibleDefect) {
  Geometry g{.frames_per_die = 4, .frame_width = 64, .frame_height = 64};
  fs::path root = fs::temp_directory_path() / "algctl-fixtures-test";
  fs::remove_all(root);
  std::uint32_t dies[] = {3, 2};                       // slice 0: 3 dies, slice 1: 2 dies
  Defect defects[] = {{.die = 1, .frame_in_die = 2, .x = 30, .y = 30, .radius = 5, .delta = 90}};
  generate_fixtures(root, g, dies, defects);

  EXPECT_TRUE(fs::exists(root / "slice_0/die_2/frame_3.pgm"));
  EXPECT_TRUE(fs::exists(root / "slice_1/die_1/frame_0.pgm"));
  EXPECT_FALSE(fs::exists(root / "slice_1/die_2"));    // slice 1 has only 2 dies

  // Dies are near-identical copies: same position, different dies, tiny difference.
  Image a = read_pgm(root / "slice_0/die_0/frame_0.pgm");
  Image b = read_pgm(root / "slice_0/die_2/frame_0.pgm");
  long diff = 0;
  for (std::size_t i = 0; i < a.pixels.size(); ++i)
    diff += std::abs(int(a.pixels[i]) - int(b.pixels[i]));
  EXPECT_LT(diff / double(a.pixels.size()), 3.0);

  // The defective frame differs strongly from the same position in a clean die.
  Image clean = read_pgm(root / "slice_0/die_0/frame_2.pgm");
  Image bad = read_pgm(root / "slice_0/die_1/frame_2.pgm");
  int max_diff = 0;
  for (std::size_t i = 0; i < clean.pixels.size(); ++i)
    max_diff = std::max(max_diff, std::abs(int(clean.pixels[i]) - int(bad.pixels[i])));
  EXPECT_GT(max_diff, 60);

  std::ifstream manifest(root / "manifest.txt");
  std::string word;
  manifest >> word;
  EXPECT_EQ(word, "defect");
}

TEST(Fixtures, RejectsDefectOutsideFrameGeometry) {
  Geometry g{.frames_per_die = 4, .frame_width = 64, .frame_height = 64};
  fs::path root = fs::temp_directory_path() / "algctl-fixtures-bounds-test";
  fs::remove_all(root);
  std::uint32_t dies[] = {1};

  Defect clipped[] = {{.die = 0, .frame_in_die = 0, .x = 62, .y = 30, .radius = 5, .delta = 90}};
  EXPECT_THROW(generate_fixtures(root, g, dies, clipped), std::invalid_argument);

  Defect late[] = {{.die = 0, .frame_in_die = 4, .x = 30, .y = 30, .radius = 5, .delta = 90}};
  EXPECT_THROW(generate_fixtures(root, g, dies, late), std::invalid_argument);
}
