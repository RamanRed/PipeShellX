// Test entry point. Raises the open-handle limit once before any test runs —
// the suite creates many reactors, pipes and children, and macOS's default
// soft RLIMIT_NOFILE of 256 is otherwise reached under sanitizer/fd overhead,
// making fd-hungry paths fail with EMFILE. The application does the same in
// main(), so this mirrors production rather than masking a leak (the soak
// tests still assert the descriptor count is unchanged).

#include <gtest/gtest.h>

#include "psx/os/system.hpp"

int main(int argc, char** argv) {
    (void)psx::os::raiseHandleLimit();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
