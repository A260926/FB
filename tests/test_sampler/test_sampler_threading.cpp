#include <TestHelpers.hpp>
#include <thread>

using namespace SoftRasterizer;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(Sampler, ReproducibleAcrossThreadScheduling) {
  std::vector<float> serial(2048), parallel(2048);
  for (std::size_t i = 0; i < serial.size(); ++i) {
    Sampler rng(23, i, 17);
    serial[i] = rng.next() + rng.next();
  }
  std::vector<std::thread> threads;
  for (std::size_t t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      for (std::size_t i = t; i < parallel.size(); i += 4) {
        Sampler rng(23, i, 17);
        parallel[i] = rng.next() + rng.next();
      }
    });
  }
  for (auto &t : threads) {
    t.join();
  }
  ASSERT_TRUE(serial == parallel) << "sampler depends on scheduling";
}

} // namespace
