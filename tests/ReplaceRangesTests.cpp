// Replace Ranges arithmetic (plug-in Transfer).

#include "../Source/Utils/ReplaceRanges.h"
#include "TestAssert.h"

#include <cstdio>

using namespace ReplaceRanges;

int main() {
  // Sorted, joined, tiny pieces dropped, reversed edges fixed.
  {
    const auto n = normalize({{5.0, 6.0}, {1.0, 2.0}, {1.5, 3.0}, {8.0, 8.001}, {10.0, 9.0}});
    CHECK(n.size() == 3);
    CHECK(n[0].first == 1.0 && n[0].second == 3.0);
    CHECK(n[1].first == 5.0 && n[2].first == 9.0 && n[2].second == 10.0);
  }

  // A new transfer joins the ranges it touches.
  {
    auto r = add({{0.0, 2.0}, {4.0, 6.0}}, 1.5, 4.5);
    CHECK(r.size() == 1 && r[0].first == 0.0 && r[0].second == 6.0);
    CHECK(contains(r, 3.0) && !contains(r, 6.0));
  }

  // The nearest edge wins.
  {
    bool isStart = false;
    const List r{{1.0, 2.0}, {2.05, 3.0}};
    CHECK(edgeAt(r, 2.04, 0.1, isStart) == 1 && isStart);
    CHECK(edgeAt(r, 1.99, 0.1, isStart) == 0 && !isStart);
    CHECK(edgeAt(r, 5.0, 0.1, isStart) == -1);
  }

  // Set Replace Ranges to Notes: holes where notes are missing; short gaps
  // between notes stay replaced; nothing outside the transferred ranges.
  {
    const List ranges{{0.0, 10.0}};
    const List notes{{1.0, 2.0}, {2.05, 3.0}, {5.0, 6.0}, {9.5, 12.0}};
    const auto c = clipToNotes(ranges, notes, 0.1);
    CHECK(c.size() == 3);
    CHECK(c[0].first == 1.0 && c[0].second == 3.0);
    CHECK(c[1].first == 5.0 && c[1].second == 6.0);
    CHECK(c[2].first == 9.5 && c[2].second == 10.0);
    // No ranges yet ("everything"): the notes are the ranges.
    CHECK(clipToNotes({}, {{1.0, 2.0}}).size() == 1);
  }

  std::printf("ReplaceRangesTests passed\n");
  return 0;
}
