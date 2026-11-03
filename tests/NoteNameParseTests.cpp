// Note Inspector pitch entry.

#include "../Source/Utils/NoteNameParse.h"
#include "TestAssert.h"

#include <cmath>
#include <cstdio>

using namespace NoteNameParse;

static bool near(std::optional<float> v, float expected) {
  return v && std::abs(*v - expected) < 1.0e-4f;
}

int main() {
  CHECK(near(parse("C4", 50.0f), 60.0f));
  CHECK(near(parse("c4", 50.0f), 60.0f));
  CHECK(near(parse("F#3", 0.0f), 54.0f));
  CHECK(near(parse("Bb2", 0.0f), 46.0f));
  CHECK(near(parse("C-1", 0.0f), 0.0f));
  CHECK(near(parse("A4 +15", 0.0f), 69.15f));
  CHECK(near(parse("A4 -20ct", 0.0f), 68.8f));
  CHECK(near(parse("+2", 60.3f), 62.3f));
  CHECK(near(parse("-1.5", 60.0f), 58.5f));
  CHECK(near(parse("+30 ct", 60.0f), 60.3f));
  CHECK(!parse("H4", 60.0f));
  CHECK(!parse("C", 60.0f));
  CHECK(!parse("", 60.0f));
  CHECK(!parse("+x", 60.0f));
  CHECK(nameFor(60.0f) == "C4");
  CHECK(nameFor(69.12f) == "A4 +12 ct");
  CHECK(nameFor(68.9f) == "A4 -10 ct");
  CHECK(nameFor(-1.0f) == "B-2");
  std::printf("NoteNameParseTests passed\n");
  return 0;
}
