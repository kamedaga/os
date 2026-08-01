#include "pine-api.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct _PineApi {
  grefcount ref_count;
  gchar *base_url;
  gchar *cookie_path;
  GMutex request_lock;
};

typedef struct {
  PineApi *api;
  gchar *method;
  gchar *path;
  gchar *json_body;
} RequestWork;

typedef struct {
  gchar *data;
  gsize length;
} ResponseBuffer;

static PineApi *pine_api_ref(PineApi *api) {
  g_ref_count_inc(&api->ref_count);
  return api;
}

static void pine_api_unref(PineApi *api) {
  if (!g_ref_count_dec(&api->ref_count)) {
    return;
  }
  g_mutex_clear(&api->request_lock);
  g_free(api->base_url);
  g_free(api->cookie_path);
  g_free(api);
}

static GQuark pine_api_error_quark(void) {
  return g_quark_from_static_string("pine-api-error");
}

static gboolean ensure_cookie_file_secure(PineApi *api, GError **error) {
  gchar *state_dir = g_path_get_dirname(api->cookie_path);
  gint cookie_fd;
  struct stat cookie_stat;

  if (g_mkdir_with_parents(state_dir, 0700) != 0 ||
      g_chmod(state_dir, 0700) != 0) {
    const gint saved_errno = errno;
    g_set_error(
      error,
      pine_api_error_quark(),
      1,
      "Cookie保存先を安全に準備できませんでした: %s",
      g_strerror(saved_errno)
    );
    g_free(state_dir);
    return FALSE;
  }
  g_free(state_dir);

  cookie_fd = g_open(
    api->cookie_path,
    O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW,
    0600
  );
  if (cookie_fd < 0) {
    const gint saved_errno = errno;
    g_set_error(
      error,
      pine_api_error_quark(),
      1,
      "Cookieファイルを安全に開けませんでした: %s",
      g_strerror(saved_errno)
    );
    return FALSE;
  }

  if (fstat(cookie_fd, &cookie_stat) != 0) {
    const gint saved_errno = errno;
    close(cookie_fd);
    g_set_error(
      error,
      pine_api_error_quark(),
      1,
      "Cookieファイルの安全な権限を保証できませんでした: %s",
      g_strerror(saved_errno)
    );
    return FALSE;
  }
  if (!S_ISREG(cookie_stat.st_mode)) {
    close(cookie_fd);
    g_set_error_literal(
      error,
      pine_api_error_quark(),
      1,
      "Cookieファイルの安全な権限を保証できませんでした: "
      "通常ファイルではありません"
    );
    return FALSE;
  }
  if (fchmod(cookie_fd, 0600) != 0) {
    const gint saved_errno = errno;
    close(cookie_fd);
    g_set_error(
      error,
      pine_api_error_quark(),
      1,
      "Cookieファイルの安全な権限を保証できませんでした: %s",
      g_strerror(saved_errno)
    );
    return FALSE;
  }

  close(cookie_fd);
  return TRUE;
}

static size_t append_response(
  const char *data,
  size_t size,
  size_t count,
  void *user_data
) {
  ResponseBuffer *buffer = user_data;
  const gsize incoming = size * count;
  gchar *resized = g_try_realloc(buffer->data, buffer->length + incoming + 1);

  if (resized == NULL) {
    return 0;
  }

  buffer->data = resized;
  memcpy(buffer->data + buffer->length, data, incoming);
  buffer->length += incoming;
  buffer->data[buffer->length] = '\0';
  return incoming;
}

static void request_work_free(RequestWork *work) {
  if (work == NULL) {
    return;
  }
  g_free(work->method);
  g_free(work->path);
  g_free(work->json_body);
  pine_api_unref(work->api);
  g_free(work);
}

void pine_api_response_free(PineApiResponse *response) {
  if (response == NULL) {
    return;
  }
  g_free(response->body);
  g_free(response);
}

static void perform_request(
  GTask *task,
  gpointer source_object,
  gpointer task_data,
  GCancellable *cancellable
) {
  (void)source_object;
  (void)cancellable;
  RequestWork *work = task_data;
  ResponseBuffer buffer = {g_strdup(""), 0};
  struct curl_slist *headers = NULL;
  CURL *curl = NULL;
  gchar *url = NULL;
  PineApiResponse *response = NULL;
  CURLcode result;

  if (g_task_return_error_if_cancelled(task)) {
    g_free(buffer.data);
    return;
  }

  g_mutex_lock(&work->api->request_lock);
  {
    GError *cookie_error = NULL;
    if (!ensure_cookie_file_secure(work->api, &cookie_error)) {
      g_mutex_unlock(&work->api->request_lock);
      g_free(buffer.data);
      g_task_return_error(task, cookie_error);
      return;
    }
  }
  curl = curl_easy_init();
  if (curl == NULL) {
    g_mutex_unlock(&work->api->request_lock);
    g_free(buffer.data);
    g_task_return_new_error(
      task,
      pine_api_error_quark(),
      1,
      "HTTPクライアントを初期化できませんでした"
    );
    return;
  }

  url = g_str_has_prefix(work->path, "https://") ||
        g_str_has_prefix(work->path, "http://")
    ? g_strdup(work->path)
    : g_strconcat(work->api->base_url, work->path, NULL);
  headers = curl_slist_append(headers, "Accept: */*");
  if (work->json_body != NULL) {
    headers = curl_slist_append(headers, "Content-Type: application/json");
  }

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Pine2-GTK/0.1");
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 45L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_COOKIEFILE, work->api->cookie_path);
  curl_easy_setopt(curl, CURLOPT_COOKIEJAR, work->api->cookie_path);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_response);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

  if (g_strcmp0(work->method, "GET") != 0) {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, work->method);
  }
  if (work->json_body != NULL) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, work->json_body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(work->json_body));
  }

  result = curl_easy_perform(curl);
  if (result != CURLE_OK) {
    g_task_return_new_error(
      task,
      pine_api_error_quark(),
      (gint)result,
      "通信に失敗しました: %s",
      curl_easy_strerror(result)
    );
  } else if (g_task_return_error_if_cancelled(task)) {
    /* Cancellation is observed after libcurl returns. */
  } else {
    response = g_new0(PineApiResponse, 1);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response->status);
    response->body = g_steal_pointer(&buffer.data);
    response->body_length = buffer.length;
    g_task_return_pointer(task, response, (GDestroyNotify)pine_api_response_free);
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  g_mutex_unlock(&work->api->request_lock);
  g_free(url);
  g_free(buffer.data);
}

PineApi *pine_api_new(const gchar *base_url) {
  PineApi *api = g_new0(PineApi, 1);
  gchar *state_dir = g_build_filename(g_get_user_state_dir(), "pine2-gtk", NULL);
  GError *cookie_error = NULL;

  g_ref_count_init(&api->ref_count);
  api->base_url = g_strdup(base_url);
  while (g_str_has_suffix(api->base_url, "/")) {
    api->base_url[strlen(api->base_url) - 1] = '\0';
  }
  api->cookie_path = g_build_filename(state_dir, "cookies.txt", NULL);
  g_mutex_init(&api->request_lock);
  if (!ensure_cookie_file_secure(api, &cookie_error)) {
    g_warning("%s", cookie_error->message);
    g_clear_error(&cookie_error);
  }
  g_free(state_dir);
  return api;
}

void pine_api_free(PineApi *api) {
  if (api == NULL) {
    return;
  }
  pine_api_unref(api);
}

void pine_api_request_async(
  PineApi *api,
  const gchar *method,
  const gchar *path,
  const gchar *json_body,
  GCancellable *cancellable,
  GAsyncReadyCallback callback,
  gpointer user_data
) {
  GTask *task;
  RequestWork *work = g_new0(RequestWork, 1);

  work->api = pine_api_ref(api);
  work->method = g_strdup(method);
  work->path = g_strdup(path);
  work->json_body = g_strdup(json_body);

  task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_task_data(task, work, (GDestroyNotify)request_work_free);
  g_task_run_in_thread(task, perform_request);
  g_object_unref(task);
}

PineApiResponse *pine_api_request_finish(GAsyncResult *result, GError **error) {
  return g_task_propagate_pointer(G_TASK(result), error);
}

void pine_api_clear_session(PineApi *api) {
  g_mutex_lock(&api->request_lock);
  if (g_unlink(api->cookie_path) != 0 && errno != ENOENT) {
    g_warning("Cookieファイルを削除できませんでした: %s", g_strerror(errno));
  }
  g_mutex_unlock(&api->request_lock);
}
