/* SPDX-License-Identifier: MIT */
/* Plan construction from verifier output, not a replacement package verifier. */
#include "../userland/kobox2_adapter/module_package.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static void prepare(struct ph_package *package, uint8_t bytes[4096]) {
    static const struct kobox_package_operations operations = {0};
    static const unsigned char images[3][64] = {{1}, {2}, {3}};
    const char *names[] = {"core", "one", "two"};
    kb2_closure_manifest_artifact_t artifacts[3] = {0};
    for (size_t i = 0; i < 3; ++i) {
        artifacts[i] = (kb2_closure_manifest_artifact_t){.node_id = i + 1,
            .kind = i ? KB2_CLOSURE_ARTIFACT_RELOCATABLE_MODULE : KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER,
            .flags = KB2_CLOSURE_ARTIFACT_FLAG_NATIVE_LINUX | (i == 2 ? KB2_CLOSURE_ARTIFACT_FLAG_ROOT : 0),
            .content_size = sizeof(images[i]), .namespace_name = {.data = names[i], .length = strlen(names[i])}};
        memset(artifacts[i].content_digest, i + 1, 32);
        package->common.artifacts[i] = (struct kobox_boot_blob){.data = images[i], .size = sizeof(images[i])};
    }
    kb2_closure_manifest_dependency_t dependencies[] = {
        {.consumer_node_id = 2, .provider_node_id = 1}, {.consumer_node_id = 3, .provider_node_id = 2}};
    kb2_closure_manifest_source_t source = {.artifacts = artifacts, .artifact_count = 3,
        .dependencies = dependencies, .dependency_count = 2};
    size_t size;
    assert(kb2_closure_manifest_encode(bytes, 4096, &size, &source) == KB2_PROTOCOL_OK);
    assert(kb2_closure_manifest_decode(bytes, size, &package->common.manifest) == KB2_PROTOCOL_OK);
    package->common.artifact_count = 3;
    package->common.operations = &operations;
}

int main(void) {
    struct ph_package package = {0};
    uint8_t manifest[4096];
    prepare(&package, manifest);
    struct ph_module_package modules = {0};
    struct ph_module_package untouched = modules;
    const void *second = package.common.artifacts[2].data;
    package.common.artifacts[2].data = NULL;
    assert(ph_module_package_open(&modules, &package) == -EINVAL);
    assert(!memcmp(&modules, &untouched, sizeof(modules))); /* Later error publishes nothing. */
    package.common.artifacts[2].data = second;
    --package.common.artifacts[1].size;
    assert(ph_module_package_open(&modules, &package) == -EINVAL);
    ++package.common.artifacts[1].size;
    --package.common.artifact_count;
    assert(ph_module_package_open(&modules, &package) == -EINVAL);
    ++package.common.artifact_count;
    assert(!ph_module_package_open(&modules, &package) && modules.count == 2);
    for (size_t i = 0; i < 2; ++i) {
        assert(modules.modules[i].image == package.common.artifacts[i + 1].data);
        assert(modules.modules[i].length == 64 && modules.modules[i].name == modules.names[i]);
        assert(!modules.modules[i].parameters);
    }
    assert(!strcmp(modules.modules[0].name, "one") && !strcmp(modules.modules[1].name, "two"));
    assert(ph_module_package_open(&modules, &package) == -EINVAL);
    assert(ph_module_package_open(NULL, &package) == -EINVAL);
    puts("kobox2 verified module package plan: PASS");
    return 0;
}
