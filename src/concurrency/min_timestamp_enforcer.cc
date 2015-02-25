// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "concurrency/min_timestamp_enforcer.hpp"

#include "concurrency/signal.hpp"
#include "concurrency/cond_var.hpp"
#include "concurrency/interruptor.hpp"
#include "errors.hpp"

min_timestamp_enforcer_t::~min_timestamp_enforcer_t() {
    assert_thread();
    guarantee(waiter_queue.empty());
}

void min_timestamp_enforcer_t::bump_timestamp(state_timestamp_t new_ts) {
    assert_thread();
    if (new_ts > current_timestamp) {
        current_timestamp = new_ts;
        internal_pump();
    }
}

void min_timestamp_enforcer_t::wait(min_timestamp_token_t token) THROWS_NOTHING {
    cond_t dummy_interruptor;
    wait_interruptible(token, &dummy_interruptor);
}

void min_timestamp_enforcer_t::wait_interruptible(
        min_timestamp_token_t token,
        const signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t) {
    assert_thread();

    // Can we proceed immediately?
    if (token.min_timestamp <= current_timestamp) {
        return;
    } else {
        // Put a waiter on the queue and wait for it to be pulsed
        // Note: The drainer_lock is here just to avoid race conditions between
        // the time `internal_pump()` pops the waiter from the queue and the time
        // where execution in here gets resumed.
        // Generally `min_timestamp_enforcer_t` crashes if it's destructed
        // while there are still waiters on the queue.
        auto drainer_lock = drainer.lock();
        internal_waiter_t waiter(token);
        waiter_queue.push(&waiter);

        try {
            ::wait_interruptible(&waiter.on_runnable, interruptor);
        } catch (const interrupted_exc_t &) {
            // Remove waiter from the queue
            waiter_queue.remove(&waiter);
            throw;
        }
    }
}

void min_timestamp_enforcer_t::internal_pump() THROWS_NOTHING {
    assert_thread();
    ASSERT_NO_CORO_WAITING;
    while (!waiter_queue.empty() &&
           waiter_queue.peek()->token.min_timestamp <= current_timestamp) {
        internal_waiter_t *waiter = waiter_queue.pop();
        waiter->on_runnable.pulse();
    }
}
