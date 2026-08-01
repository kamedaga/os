#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _PineApi PineApi;

typedef struct {
  long status;
  gchar *body;
  gsize body_length;
} PineApiResponse;

PineApi *pine_api_new(const gchar *base_url);
void pine_api_free(PineApi *api);

void pine_api_request_async(
  PineApi *api,
  const gchar *method,
  const gchar *path,
  const gchar *json_body,
  GCancellable *cancellable,
  GAsyncReadyCallback callback,
  gpointer user_data
);

PineApiResponse *pine_api_request_finish(GAsyncResult *result, GError **error);
void pine_api_response_free(PineApiResponse *response);
void pine_api_clear_session(PineApi *api);

G_END_DECLS
