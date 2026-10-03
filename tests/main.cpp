#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdlib>

#include "skywalker/core/Log.h"

int main(int argc, char** argv) {
    setenv("SKYWALKER_AUDIO", "null", 0);  // never open a sound device during tests
    sky::log::setStderrEnabled(false);  // keep test output readable
    doctest::Context context;
    context.applyCommandLine(argc, argv);
    return context.run();
}
