#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "skywalker/core/Log.h"

int main(int argc, char** argv) {
    sky::log::setStderrEnabled(false);  // keep test output readable
    doctest::Context context;
    context.applyCommandLine(argc, argv);
    return context.run();
}
