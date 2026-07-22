#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>

namespace inspection {

// Returns the value following a --flag, exiting with a usage error when it is
// missing — argv[argc] is nullptr and must never be dereferenced.
inline std::string flag_value(int argc, char** argv, int& i) {
  if (i + 1 >= argc) {
    std::fprintf(stderr, "missing value for %s\n", argv[i]);
    std::exit(2);
  }
  return argv[++i];
}

inline std::size_t flag_size(int argc, char** argv, int& i) {
  std::string v = flag_value(argc, argv, i);
  try {
    return std::stoul(v);
  } catch (const std::exception&) {
    std::fprintf(stderr, "invalid numeric value '%s' for %s\n", v.c_str(), argv[i - 1]);
    std::exit(2);
  }
}

}  // namespace inspection
