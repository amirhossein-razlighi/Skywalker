#pragma once
// One place that configures miniaudio so every translation unit sees identical type layouts.
// (miniaudio is a single header; exactly one TU, Miniaudio.cpp, defines the implementation.)

#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include "miniaudio.h"
