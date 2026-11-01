#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#define IS_ENABLED(x) 0
struct task_struct {void *files;};
static struct task_struct task, other;
static struct task_struct *current=&task;
static bool context_ok=true;
static int release_error, api_puts, object_puts, dmabuf_unmaps;
static int module_puts, task_puts, task_runs, frees, attempts;
struct kobox_i915_mapping_api {int (*release)(void **);};
struct kobox_drm_mapping {void *object; int dmabuf;
 const struct kobox_i915_mapping_api *i915_api;void *i915_cookie;};
struct kobox_linux_drm_mapping_operations {int (*release)(void *);};
struct kobox_linux_drm_mapping {
 const struct kobox_linux_drm_mapping_operations *operations;
 void *module,*private_mapping;struct task_struct *owner;void *files;
};
static bool valid_context(void) {return context_ok;}
static void kobox_linux_drm_i915_mapping_put(void) {++api_puts;}
static void kobox_drm_dmabuf_unmap(void *p) {(void)p;++dmabuf_unmaps;}
static void drm_gem_object_put(void *p) {assert(p);++object_puts;}
static void module_put(void *p) {assert(p);++module_puts;}
static void put_task_struct(void *p) {assert(p);++task_puts;}
static void task_work_run(void) {++task_runs;}
static void kfree(void *p) {assert(p);++frees;free(p);}
static int api_release(void **cookie) {
 assert(cookie && *cookie);++attempts;
 if(release_error)return release_error;
 *cookie=NULL;return 0;
}
#include "mapping_release_actual.inc"
int main(void) {
 const struct kobox_i915_mapping_api api={api_release};
 const struct kobox_linux_drm_mapping_operations ops={release_system_memory_pages};
 struct kobox_drm_mapping *private=calloc(1,sizeof(*private));
 struct kobox_linux_drm_mapping *owner=calloc(1,sizeof(*owner)),*saved=owner;
 int files,cookie,object,module;
 task.files=&files;
 private->object=&object;private->i915_api=&api;private->i915_cookie=&cookie;
 *owner=(struct kobox_linux_drm_mapping){&ops,&module,private,&task,&files};
 assert(kobox_linux_drm_mapping_release(NULL)==-EINVAL);
 context_ok=false;
 assert(kobox_linux_drm_mapping_release(&owner)==-EPERM);
 context_ok=true;current=&other;
 assert(kobox_linux_drm_mapping_release(&owner)==-EPERM);
 current=&task;owner->files=&cookie;
 assert(kobox_linux_drm_mapping_release(&owner)==-EPERM);
 owner->files=&files;
 assert(!attempts);
 release_error=-EBUSY;
 assert(kobox_linux_drm_mapping_release(&owner)==-EBUSY);
 assert(owner==saved && private->i915_cookie==&cookie);
 assert(!api_puts && !object_puts && !dmabuf_unmaps && !module_puts &&
  !task_puts && !task_runs && !frees);
 release_error=-EIO;
 assert(kobox_linux_drm_mapping_release(&owner)==-EIO);
 assert(owner==saved && !frees);
 release_error=0;
 assert(!kobox_linux_drm_mapping_release(&owner) && !owner);
 assert(attempts==3 && api_puts==1 && object_puts==1 && dmabuf_unmaps==1 &&
  module_puts==1 && task_puts==1 && task_runs==1 && frees==2);
 assert(kobox_linux_drm_mapping_release(&owner)==-EINVAL);
 puts("DRM mapping actual provider+core release: failed restoration retains cookie/module/task/BO; retry releases once PASS");
}
