/* SPDX-License-Identifier: MIT */
#ifndef GPUD_CONTROLLER_FIXTURE_H
#define GPUD_CONTROLLER_FIXTURE_H

#include <kobox2/closure.h>
#include <string.h>
#ifndef GPUD_TEST_CHECK
#include <assert.h>
#define GPUD_TEST_CHECK(condition) assert(condition)
#endif

/* Artificial two-artifact closure for HOST ACTION tests, not a loaded Linux
 * package. Callers explicitly account for native resources before completing
 * resource actions; no action is silently completed by this constructor. */
static kb2_controller_t *gpud_test_controller_create(kb2_allocate_fn allocate,
    kb2_deallocate_fn deallocate, void *context) {
    kb2_controller_t *controller;
    kb2_closure_builder_t *builder;
    kb2_closure_t *closure;
    uint8_t digest[KB2_DIGEST_SIZE];
    memset(digest, 0x41, sizeof(digest));
    GPUD_TEST_CHECK(kb2_controller_create(allocate, deallocate, context, &controller) == KB2_STATUS_OK);
    GPUD_TEST_CHECK(kb2_closure_builder_create(allocate, deallocate, context, digest, sizeof(digest),
        &builder) == KB2_STATUS_OK);
    GPUD_TEST_CHECK(kb2_closure_builder_add_artifact(builder, 1, KB2_ARTIFACT_SHARED_PROVIDER,
        digest, sizeof(digest), "core", 4) == KB2_STATUS_OK);
    GPUD_TEST_CHECK(kb2_closure_builder_set_native_lifecycle(builder, 1) == KB2_STATUS_OK);
    GPUD_TEST_CHECK(kb2_closure_builder_add_artifact(builder, 2, KB2_ARTIFACT_RELOCATABLE_MODULE,
        digest, sizeof(digest), "driver", 6) == KB2_STATUS_OK);
    GPUD_TEST_CHECK(kb2_closure_builder_set_native_lifecycle(builder, 2) == KB2_STATUS_OK);
    GPUD_TEST_CHECK(kb2_closure_builder_add_dependency(builder, 2, 1) == KB2_STATUS_OK);
    GPUD_TEST_CHECK(kb2_closure_builder_mark_root(builder, 2) == KB2_STATUS_OK);
    GPUD_TEST_CHECK(kb2_closure_builder_seal(builder, &closure) == KB2_STATUS_OK);
    GPUD_TEST_CHECK(kb2_controller_set_closure(controller, closure) == KB2_STATUS_OK);
    kb2_closure_destroy(closure);
    kb2_closure_builder_destroy(builder);
    for (unsigned int kind = KB2_DIGEST_PROFILE; kind <= KB2_DIGEST_CHANNEL_SET; ++kind)
        GPUD_TEST_CHECK(kb2_controller_set_digest(controller, kind, digest, sizeof(digest)) == KB2_STATUS_OK);
    const uint64_t limits[] = {1u << 20, 2, 4, 64};
    for (unsigned int kind = 0; kind <= KB2_LIMIT_OUTSTANDING_REQUEST_COUNT; ++kind)
        GPUD_TEST_CHECK(kb2_controller_set_limit(controller, kind, limits[kind]) == KB2_STATUS_OK);
    return controller;
}

static void gpud_test_complete(kb2_controller_t *controller, kb2_action_type_t type,
    uint64_t resource_id, uint64_t sandbox_id) {
    const kb2_action_t *action = kb2_controller_pending_action(controller);
    GPUD_TEST_CHECK(action && kb2_action_type(action) == type);
    GPUD_TEST_CHECK(kb2_controller_complete_action(controller, kb2_action_generation(action),
        kb2_action_token(action), KB2_STATUS_OK, resource_id, sandbox_id) == KB2_STATUS_OK);
}

#endif
