/* SPDX-License-Identifier: MIT */
#include "module_package.h"
#include <errno.h>
#include <string.h>

int ph_module_package_open(struct ph_module_package *modules, const struct ph_package *package) {
    if (!modules || modules->count || !package || !package->common.operations ||
        package->common.artifact_count < 2 || package->common.artifact_count > PH_BOOTSTRAP_MAX_ARTIFACTS ||
        kb2_closure_manifest_artifact_count(&package->common.manifest) != package->common.artifact_count)
        return -EINVAL;
    /* Preflight all views before publishing any borrowed pointers. */
    for (size_t i = 1; i < package->common.artifact_count; ++i) {
        kb2_closure_manifest_artifact_t artifact;
        const struct kobox_boot_blob *blob = &package->common.artifacts[i];
        if (kb2_closure_manifest_artifact(&package->common.manifest, i, &artifact) != KB2_PROTOCOL_OK ||
            artifact.kind != KB2_CLOSURE_ARTIFACT_RELOCATABLE_MODULE ||
            !(artifact.flags & KB2_CLOSURE_ARTIFACT_FLAG_NATIVE_LINUX) ||
            !blob->data || blob->size != artifact.content_size || !artifact.namespace_name.length ||
            artifact.namespace_name.length >= PH_MODULE_NAME_CAPACITY) return -EINVAL;
    }
    for (size_t i = 1; i < package->common.artifact_count; ++i) {
        kb2_closure_manifest_artifact_t artifact;
        (void)kb2_closure_manifest_artifact(&package->common.manifest, i, &artifact);
        memcpy(modules->names[i - 1], artifact.namespace_name.data, artifact.namespace_name.length);
        modules->names[i - 1][artifact.namespace_name.length] = 0;
        modules->modules[i - 1] = (struct kobox_linux_native_module){
            .image = package->common.artifacts[i].data, .length = package->common.artifacts[i].size,
            .name = modules->names[i - 1]};
    }
    modules->count = package->common.artifact_count - 1;
    return 0;
}
