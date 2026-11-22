// M11 item 11, U13: the numbers of the Measure tool.
#include <doctest.h>

#include "tools/measure.h"

using namespace native;

TEST_CASE("measure: distance, signed deltas and the ground length (Y is up)") {
    const tools::Measurement m = tools::measure({0, 0, 0}, {3, 2, 4});
    CHECK(m.dx == doctest::Approx(3));
    CHECK(m.dy == doctest::Approx(2));
    CHECK(m.dz == doctest::Approx(4));
    CHECK(m.ground == doctest::Approx(5));
    CHECK(m.distance == doctest::Approx(std::sqrt(29.0)));
    const tools::Measurement back = tools::measure({3, 2, 4}, {0, 0, 0});
    CHECK(back.dx == doctest::Approx(-3));
    CHECK(back.distance == doctest::Approx(m.distance));
    CHECK(tools::measure({1, 1, 1}, {1, 1, 1}).distance == 0);
}

TEST_CASE("measure: the status text") {
    CHECK(tools::measureText(tools::measure({0, 0, 0}, {3, 2, 4})) == "5.39 m   dX 3.00   dY 2.00 (up)   dZ 4.00   ground 5.00 m");
    CHECK(tools::measureText(tools::measure({0, 0, 0}, {-0.001, 0, 0})) == "0.00 m   dX 0.00   dY 0.00 (up)   dZ 0.00   ground 0.00 m");   // no "-0.00"
    CHECK(tools::measureText(tools::measure({0, 0, 0}, {0, -1.5, 0})).find("dY -1.50 (up)") != std::string::npos);
}
