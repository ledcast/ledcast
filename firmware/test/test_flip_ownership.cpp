#include "check.h"
#include "flip_ownership.h"

using hub75_flip::Ownership;

namespace {

void confirmation_before_deadline_releases_old_front() {
    Ownership state{0};
    CHECK(state.writable());
    CHECK_EQ(state.writable_id(), 0);
    CHECK_EQ(state.begin_flip(), 0);
    CHECK(!state.writable());
    state.confirm();
    CHECK(state.writable());
    CHECK_EQ(state.writable_id(), 1);
    CHECK_EQ(state.visible_id(), 0);
}

void timeout_before_boundary_keeps_both_buffers_closed_until_confirmation() {
    Ownership state{0};
    CHECK_EQ(state.begin_flip(), 0);
    state.timeout();
    CHECK(state.pending());
    CHECK(!state.writable());
    CHECK_EQ(state.pending_id(), 0);

    // A later hardware observation confirms the already armed transition.
    state.confirm();
    CHECK(!state.pending());
    CHECK(state.writable());
    CHECK_EQ(state.writable_id(), 1);
    CHECK_EQ(state.visible_id(), 0);
}

void timeout_after_boundary_is_treated_as_confirmation() {
    Ownership state{0};
    CHECK_EQ(state.begin_flip(), 0);

    // flip_dma_output_buffer() re-reads BF0 after its semaphore wait expires.
    // If the boundary raced the timeout, it reports success and the wrapper
    // confirms ownership instead of entering the ambiguous timeout state.
    state.confirm();
    CHECK(!state.pending());
    CHECK(state.writable());
    CHECK_EQ(state.writable_id(), 1);
    CHECK_EQ(state.visible_id(), 0);
}

void repeated_wait_does_not_change_the_pending_target() {
    Ownership state{1};
    CHECK_EQ(state.begin_flip(), 1);
    state.timeout();
    CHECK_EQ(state.pending_id(), 1);
    state.timeout();
    CHECK_EQ(state.pending_id(), 1);
    state.confirm();
    CHECK_EQ(state.writable_id(), 0);
}

}  // namespace

int main() {
    confirmation_before_deadline_releases_old_front();
    timeout_before_boundary_keeps_both_buffers_closed_until_confirmation();
    timeout_after_boundary_is_treated_as_confirmation();
    repeated_wait_does_not_change_the_pending_target();
    return hosttest::report("flip_ownership");
}
