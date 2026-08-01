#include "pine-api.h"
#include "markdown-renderer.h"

#include <curl/curl.h>
#include <gtk/gtk.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_SERVER "https://pine2-nine.vercel.app"
#define MESSAGE_POLL_SECONDS 4

typedef enum {
  PINE_THEME_LIGHT,
  PINE_THEME_DARK,
} PineTheme;

typedef struct {
  GtkApplication *application;
  GtkWidget *window;
  GtkWidget *root_stack;
  GtkWidget *login_button;
  GtkWidget *login_spinner;
  GtkWidget *login_status;
  GtkWidget *login_title;
  GtkWidget *login_subtitle;
  GtkWidget *auth_toggle_button;
  GtkWidget *username_entry;
  GtkWidget *password_entry;
  GtkWidget *rooms_list;
  GtkWidget *messages_list;
  GtkWidget *messages_scroller;
  GtkWidget *room_title;
  GtkWidget *room_subtitle;
  GtkWidget *connection_label;
  GtkWidget *sidebar;
  GtkWidget *user_label;
  GtkWidget *user_avatar;
  GtkWidget *user_avatar_fallback;
  GtkWidget *invite_button;
  GtkWidget *composer_entry;
  GtkWidget *send_button;
  GtkWidget *settings_dialog;
  GtkWidget *settings_stack;
  GtkWidget *settings_name_entry;
  GtkWidget *settings_profile_text;
  GtkWidget *settings_icon_url_entry;
  GtkWidget *settings_profile_status;
  GtkWidget *settings_profile_avatar;
  GtkWidget *settings_profile_save_button;
  GtkWidget *settings_icon_save_button;
  GtkWidget *settings_preset_combo;
  GtkWidget *articles_list;
  GtkWidget *articles_status;
  GtkWidget *article_page_stack;
  GtkWidget *article_title_label;
  GtkWidget *article_meta_label;
  GtkWidget *article_tags_label;
  GtkWidget *article_social_label;
  GtkWidget *article_body_view;
  GtkWidget *article_edit_button;
  GtkWidget *article_editor_heading;
  GtkWidget *article_editor_title;
  GtkWidget *article_editor_tags;
  GtkWidget *article_editor_content;
  GtkWidget *article_editor_preview;
  GtkWidget *article_editor_notebook;
  GtkWidget *article_editor_status;
  GtkWidget *article_save_button;
  GtkCssProvider *style_provider;
  PineApi *api;
  GHashTable *avatar_cache;
  gchar *server_url;
  gchar *settings_path;
  gchar *user_id;
  gchar *display_name;
  gchar *profile_text;
  gchar *icon_type;
  gchar *icon_value;
  gchar *icon_image_url;
  gchar *selected_article_id;
  gchar *selected_article_title;
  gchar *selected_article_content;
  gchar *selected_article_tags;
  gchar *selected_article_author_id;
  gchar *selected_article_edited_at;
  gchar *active_room_id;
  gchar *active_room_name;
  gchar *message_snapshot;
  gboolean messages_loading;
  gboolean rooms_loading;
  gboolean send_loading;
  gboolean register_mode;
  gboolean profile_loading;
  gboolean profile_saving;
  gboolean articles_loading;
  gboolean articles_loaded;
  gboolean article_loading;
  gboolean article_saving;
  gboolean article_editing;
  PineTheme theme;
  guint avatar_epoch;
  guint poll_source;
  guint rooms_poll_source;
} PineApp;

typedef struct {
  PineApp *app;
  gchar *room_id;
} RoomRequest;

static void load_rooms(PineApp *app);
static void refresh_messages(PineApp *app);
static void apply_theme(PineApp *app);
static void refresh_current_profile(PineApp *app);
static void load_articles(PineApp *app);
static gchar *article_editor_content(PineApp *app);
static void clear_selected_article(PineApp *app);
static void clear_list_box(GtkWidget *list_box);

static const gchar *object_string(json_object *object, const gchar *key) {
  json_object *value = NULL;
  if (
    object != NULL &&
    json_object_object_get_ex(object, key, &value) &&
    json_object_is_type(value, json_type_string)
  ) {
    return json_object_get_string(value);
  }
  return NULL;
}

static gint object_int(json_object *object, const gchar *key) {
  json_object *value = NULL;
  if (object != NULL && json_object_object_get_ex(object, key, &value)) {
    return json_object_get_int(value);
  }
  return 0;
}

static gchar *response_error_message(PineApiResponse *response) {
  json_object *root;
  const gchar *message;

  if (response == NULL || response->body == NULL) {
    return g_strdup("サーバーから応答がありません");
  }
  root = json_tokener_parse(response->body);
  message = object_string(root, "error");
  gchar *result = message != NULL
    ? g_strdup(message)
    : g_strdup_printf("HTTP %ld", response->status);
  if (root != NULL) {
    json_object_put(root);
  }
  return result;
}

static void set_connection_state(
  PineApp *app,
  const gchar *text,
  const gchar *style_class
) {
  GtkStyleContext *context = gtk_widget_get_style_context(app->connection_label);
  gtk_style_context_remove_class(context, "connection-online");
  gtk_style_context_remove_class(context, "connection-connecting");
  gtk_style_context_remove_class(context, "connection-offline");
  gtk_style_context_add_class(context, style_class);
  gtk_label_set_text(GTK_LABEL(app->connection_label), text);
}

static gchar *first_character(const gchar *value) {
  if (value == NULL || *value == '\0') {
    return g_strdup("?");
  }
  const gchar *next = g_utf8_next_char(value);
  return g_strndup(value, next - value);
}

typedef struct {
  GWeakRef widget;
  gint size;
} AvatarTarget;

typedef struct {
  gchar *user_id;
  gchar *icon_text;
  GdkPixbuf *pixbuf;
  GPtrArray *targets;
  gboolean loading;
  gboolean loaded;
  guint generation;
} AvatarEntry;

typedef struct {
  PineApp *app;
  gchar *user_id;
  guint generation;
  guint epoch;
} AvatarRequest;

static void avatar_target_free(AvatarTarget *target) {
  if (target == NULL) {
    return;
  }
  g_weak_ref_clear(&target->widget);
  g_free(target);
}

static void avatar_entry_free(AvatarEntry *entry) {
  if (entry == NULL) {
    return;
  }
  g_free(entry->user_id);
  g_free(entry->icon_text);
  g_clear_object(&entry->pixbuf);
  g_ptr_array_free(entry->targets, TRUE);
  g_free(entry);
}

static void avatar_request_free(AvatarRequest *request) {
  if (request == NULL) {
    return;
  }
  g_free(request->user_id);
  g_free(request);
}

static AvatarEntry *avatar_entry_get(PineApp *app, const gchar *user_id) {
  if (app->avatar_cache == NULL || user_id == NULL || *user_id == '\0') {
    return NULL;
  }
  AvatarEntry *entry = g_hash_table_lookup(app->avatar_cache, user_id);
  if (entry == NULL) {
    entry = g_new0(AvatarEntry, 1);
    entry->user_id = g_strdup(user_id);
    entry->targets = g_ptr_array_new_with_free_func(
      (GDestroyNotify)avatar_target_free
    );
    g_hash_table_insert(app->avatar_cache, g_strdup(user_id), entry);
  }
  return entry;
}

static GdkPixbuf *avatar_pixbuf_scaled(GdkPixbuf *source, gint size) {
  const gint width = gdk_pixbuf_get_width(source);
  const gint height = gdk_pixbuf_get_height(source);
  if (width <= 0 || height <= 0 || size <= 0) {
    return NULL;
  }

  const gdouble scale = MAX((gdouble)size / width, (gdouble)size / height);
  const gint scaled_width = MAX(size, (gint)(width * scale + 0.5));
  const gint scaled_height = MAX(size, (gint)(height * scale + 0.5));
  GdkPixbuf *scaled = gdk_pixbuf_scale_simple(
    source,
    scaled_width,
    scaled_height,
    GDK_INTERP_BILINEAR
  );
  if (scaled == NULL) {
    return NULL;
  }

  cairo_surface_t *surface = cairo_image_surface_create(
    CAIRO_FORMAT_ARGB32,
    size,
    size
  );
  cairo_t *cr = cairo_create(surface);
  cairo_arc(cr, size / 2.0, size / 2.0, size / 2.0, 0, 2 * G_PI);
  cairo_clip(cr);
  gdk_cairo_set_source_pixbuf(
    cr,
    scaled,
    -(scaled_width - size) / 2.0,
    -(scaled_height - size) / 2.0
  );
  cairo_paint(cr);
  cairo_destroy(cr);
  GdkPixbuf *result = gdk_pixbuf_get_from_surface(surface, 0, 0, size, size);
  cairo_surface_destroy(surface);
  g_object_unref(scaled);
  return result;
}

static void avatar_entry_update_targets(AvatarEntry *entry) {
  for (gint i = (gint)entry->targets->len - 1; i >= 0; i--) {
    AvatarTarget *target = g_ptr_array_index(entry->targets, (guint)i);
    GtkWidget *avatar = g_weak_ref_get(&target->widget);
    if (avatar == NULL) {
      g_ptr_array_remove_index(entry->targets, (guint)i);
      continue;
    }

    GtkWidget *fallback = gtk_stack_get_child_by_name(
      GTK_STACK(avatar),
      "fallback"
    );
    GtkWidget *image = gtk_stack_get_child_by_name(GTK_STACK(avatar), "image");
    if (entry->pixbuf != NULL && image != NULL) {
      GdkPixbuf *scaled = avatar_pixbuf_scaled(entry->pixbuf, target->size);
      if (scaled != NULL) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(image), scaled);
        gtk_stack_set_visible_child_name(GTK_STACK(avatar), "image");
        g_object_unref(scaled);
      }
    } else if (entry->icon_text != NULL && fallback != NULL) {
      gtk_label_set_text(GTK_LABEL(fallback), entry->icon_text);
      gtk_stack_set_visible_child_name(GTK_STACK(avatar), "fallback");
    }
    g_object_unref(avatar);
  }
}

static void sync_profile_widgets(PineApp *app, const gchar *status) {
  if (app->settings_dialog == NULL) {
    return;
  }
  if (app->settings_name_entry != NULL) {
    gtk_entry_set_text(
      GTK_ENTRY(app->settings_name_entry),
      app->display_name != NULL ? app->display_name : ""
    );
  }
  if (app->settings_profile_text != NULL) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(
      GTK_TEXT_VIEW(app->settings_profile_text)
    );
    gtk_text_buffer_set_text(
      buffer,
      app->profile_text != NULL ? app->profile_text : "",
      -1
    );
  }
  if (app->settings_icon_url_entry != NULL) {
    const gchar *url = app->icon_image_url;
    if ((url == NULL || *url == '\0') &&
        g_strcmp0(app->icon_type, "url") == 0) {
      url = app->icon_value;
    }
    gtk_entry_set_text(
      GTK_ENTRY(app->settings_icon_url_entry),
      url != NULL ? url : ""
    );
  }
  if (app->settings_profile_status != NULL && status != NULL) {
    gtk_label_set_text(GTK_LABEL(app->settings_profile_status), status);
  }
}

static void set_settings_profile_status(PineApp *app, const gchar *status) {
  if (app->settings_profile_status != NULL) {
    gtk_label_set_text(
      GTK_LABEL(app->settings_profile_status),
      status != NULL ? status : ""
    );
  }
}

static void avatar_entry_set_image_bytes(
  AvatarEntry *entry,
  const guchar *bytes,
  gsize length
) {
  GError *error = NULL;
  GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
  gboolean valid = gdk_pixbuf_loader_write(loader, bytes, length, &error);
  if (valid) {
    valid = gdk_pixbuf_loader_close(loader, &error);
  }
  if (valid) {
    GdkPixbuf *pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
    if (pixbuf != NULL) {
      g_set_object(&entry->pixbuf, pixbuf);
    }
  } else if (error != NULL) {
    g_debug("アイコン画像をデコードできませんでした: %s", error->message);
  }
  g_clear_error(&error);
  g_object_unref(loader);
  entry->loading = FALSE;
  entry->loaded = TRUE;
  avatar_entry_update_targets(entry);
}

static void avatar_image_completed(
  GObject *source,
  GAsyncResult *result,
  gpointer user_data
) {
  (void)source;
  AvatarRequest *request = user_data;
  GError *error = NULL;
  PineApiResponse *response = pine_api_request_finish(result, &error);
  if (request->epoch != request->app->avatar_epoch) {
    g_clear_error(&error);
    pine_api_response_free(response);
    avatar_request_free(request);
    return;
  }
  AvatarEntry *entry = avatar_entry_get(request->app, request->user_id);

  if (entry != NULL && entry->generation == request->generation) {
    if (error == NULL && response->status >= 200 && response->status < 300) {
      avatar_entry_set_image_bytes(
        entry,
        (const guchar *)response->body,
        response->body_length
      );
    } else {
      entry->loading = FALSE;
      entry->loaded = TRUE;
      avatar_entry_update_targets(entry);
    }
  }
  g_clear_error(&error);
  pine_api_response_free(response);
  avatar_request_free(request);
}

static void avatar_download_image(
  PineApp *app,
  AvatarEntry *entry,
  const gchar *source,
  guint generation
) {
  if (g_str_has_prefix(source, "data:image/")) {
    const gchar *comma = strchr(source, ',');
    if (comma != NULL) {
      gsize length = 0;
      guchar *bytes = g_base64_decode(comma + 1, &length);
      avatar_entry_set_image_bytes(entry, bytes, length);
      g_free(bytes);
      return;
    }
  }
  if (!(g_str_has_prefix(source, "https://") ||
        g_str_has_prefix(source, "http://") ||
        g_str_has_prefix(source, "/"))) {
    entry->loading = FALSE;
    entry->loaded = TRUE;
    avatar_entry_update_targets(entry);
    return;
  }

  AvatarRequest *request = g_new0(AvatarRequest, 1);
  request->app = app;
  request->user_id = g_strdup(entry->user_id);
  request->generation = generation;
  request->epoch = app->avatar_epoch;
  pine_api_request_async(
    app->api,
    "GET",
    source,
    NULL,
    NULL,
    avatar_image_completed,
    request
  );
}

static void update_current_profile(
  PineApp *app,
  json_object *profile
) {
  const gchar *display_name = object_string(profile, "displayName");
  const gchar *profile_text = object_string(profile, "profileText");
  const gchar *icon_type = object_string(profile, "icon_type");
  const gchar *icon_value = object_string(profile, "icon_value");
  const gchar *icon_image_url = object_string(profile, "icon_image_url");

  if (display_name != NULL && *display_name != '\0') {
    g_free(app->display_name);
    app->display_name = g_strdup(display_name);
    gtk_label_set_text(GTK_LABEL(app->user_label), app->display_name);
    if (app->user_avatar_fallback != NULL) {
      gchar *initial = first_character(app->display_name);
      gtk_label_set_text(GTK_LABEL(app->user_avatar_fallback), initial);
      g_free(initial);
    }
  }
  g_free(app->profile_text);
  g_free(app->icon_type);
  g_free(app->icon_value);
  g_free(app->icon_image_url);
  app->profile_text = g_strdup(profile_text);
  app->icon_type = g_strdup(icon_type);
  app->icon_value = g_strdup(icon_value);
  app->icon_image_url = g_strdup(icon_image_url);
  sync_profile_widgets(app, "プロフィールを読み込みました");
}

static void avatar_profile_completed(
  GObject *source,
  GAsyncResult *result,
  gpointer user_data
) {
  (void)source;
  AvatarRequest *request = user_data;
  GError *error = NULL;
  PineApiResponse *response = pine_api_request_finish(result, &error);
  if (request->epoch != request->app->avatar_epoch) {
    g_clear_error(&error);
    pine_api_response_free(response);
    avatar_request_free(request);
    return;
  }
  AvatarEntry *entry = avatar_entry_get(request->app, request->user_id);
  json_object *root = NULL;
  json_object *profile = NULL;

  if (entry == NULL || entry->generation != request->generation) {
    goto cleanup;
  }
  if (error != NULL || response->status < 200 || response->status >= 300) {
    entry->loading = FALSE;
    entry->loaded = TRUE;
    avatar_entry_update_targets(entry);
    goto cleanup;
  }

  root = json_tokener_parse(response->body);
  if (root == NULL ||
      !json_object_object_get_ex(root, "profile", &profile) ||
      !json_object_is_type(profile, json_type_object)) {
    entry->loading = FALSE;
    entry->loaded = TRUE;
    avatar_entry_update_targets(entry);
    goto cleanup;
  }

  const gchar *icon_type = object_string(profile, "icon_type");
  const gchar *icon_value = object_string(profile, "icon_value");
  const gchar *icon_image_url = object_string(profile, "icon_image_url");
  const gchar *avatar_url = object_string(profile, "avatarUrl");
  if (g_strcmp0(request->user_id, request->app->user_id) == 0) {
    update_current_profile(request->app, profile);
  }
  g_clear_pointer(&entry->icon_text, g_free);
  g_clear_object(&entry->pixbuf);
  if (g_strcmp0(icon_type, "preset") == 0 &&
      icon_value != NULL && *icon_value != '\0') {
    entry->icon_text = g_strdup(icon_value);
    entry->loading = FALSE;
    entry->loaded = TRUE;
    avatar_entry_update_targets(entry);
  } else {
    const gchar *image_source = icon_image_url != NULL && *icon_image_url != '\0'
      ? icon_image_url
      : avatar_url != NULL && *avatar_url != '\0'
        ? avatar_url
        : icon_value;
    if (image_source != NULL && *image_source != '\0') {
      avatar_download_image(
        request->app,
        entry,
        image_source,
        request->generation
      );
    } else {
      entry->loading = FALSE;
      entry->loaded = TRUE;
      avatar_entry_update_targets(entry);
    }
  }

cleanup:
  if (entry != NULL && entry->generation == request->generation &&
      g_strcmp0(request->user_id, request->app->user_id) == 0) {
    request->app->profile_loading = FALSE;
  }
  if (root != NULL) {
    json_object_put(root);
  }
  g_clear_error(&error);
  pine_api_response_free(response);
  avatar_request_free(request);
}

static void avatar_request_profile(
  PineApp *app,
  const gchar *user_id,
  gboolean force
) {
  AvatarEntry *entry = avatar_entry_get(app, user_id);
  if (entry == NULL || (!force && (entry->loading || entry->loaded))) {
    return;
  }

  entry->generation++;
  entry->loading = TRUE;
  entry->loaded = FALSE;
  if (g_strcmp0(user_id, app->user_id) == 0) {
    app->profile_loading = TRUE;
  }
  if (force) {
    g_clear_pointer(&entry->icon_text, g_free);
    g_clear_object(&entry->pixbuf);
  }
  AvatarRequest *request = g_new0(AvatarRequest, 1);
  request->app = app;
  request->user_id = g_strdup(user_id);
  request->generation = entry->generation;
  request->epoch = app->avatar_epoch;
  gchar *escaped = g_uri_escape_string(user_id, NULL, FALSE);
  gchar *path = g_strdup_printf("/api/profiles?userId=%s", escaped);
  pine_api_request_async(
    app->api,
    "GET",
    path,
    NULL,
    NULL,
    avatar_profile_completed,
    request
  );
  g_free(path);
  g_free(escaped);
}

static void avatar_attach_target(
  PineApp *app,
  const gchar *user_id,
  GtkWidget *avatar,
  gint size
) {
  AvatarEntry *entry = avatar_entry_get(app, user_id);
  if (entry == NULL) {
    return;
  }
  gboolean already_attached = FALSE;
  for (gint i = (gint)entry->targets->len - 1; i >= 0; i--) {
    AvatarTarget *existing = g_ptr_array_index(entry->targets, (guint)i);
    GtkWidget *widget = g_weak_ref_get(&existing->widget);
    if (widget == NULL) {
      g_ptr_array_remove_index(entry->targets, (guint)i);
    } else {
      if (widget == avatar) {
        already_attached = TRUE;
      }
      g_object_unref(widget);
    }
  }
  if (!already_attached) {
    AvatarTarget *target = g_new0(AvatarTarget, 1);
    g_weak_ref_init(&target->widget, avatar);
    target->size = size;
    g_ptr_array_add(entry->targets, target);
  }
  avatar_entry_update_targets(entry);
  avatar_request_profile(app, user_id, FALSE);
}

static GtkWidget *avatar_widget_new(
  PineApp *app,
  const gchar *user_id,
  const gchar *display_name,
  gint size,
  const gchar *style_class,
  GtkWidget **fallback_out
) {
  gchar *initial = first_character(display_name);
  GtkWidget *avatar = gtk_stack_new();
  GtkWidget *fallback = gtk_label_new(initial);
  GtkWidget *image = gtk_image_new();
  g_free(initial);
  gtk_stack_set_transition_type(GTK_STACK(avatar), GTK_STACK_TRANSITION_TYPE_NONE);
  gtk_stack_add_named(GTK_STACK(avatar), fallback, "fallback");
  gtk_stack_add_named(GTK_STACK(avatar), image, "image");
  gtk_stack_set_visible_child_name(GTK_STACK(avatar), "fallback");
  gtk_widget_set_size_request(avatar, size, size);
  gtk_style_context_add_class(gtk_widget_get_style_context(avatar), style_class);
  gtk_style_context_add_class(
    gtk_widget_get_style_context(fallback),
    "avatar-fallback"
  );
  if (fallback_out != NULL) {
    *fallback_out = fallback;
  }
  if (user_id != NULL && *user_id != '\0') {
    avatar_attach_target(app, user_id, avatar, size);
  }
  return avatar;
}

static void refresh_current_profile(PineApp *app) {
  if (app->user_id != NULL) {
    avatar_request_profile(app, app->user_id, TRUE);
  }
}

static void set_login_busy(PineApp *app, gboolean busy) {
  gtk_widget_set_sensitive(app->login_button, !busy);
  gtk_widget_set_sensitive(app->username_entry, !busy);
  gtk_widget_set_sensitive(app->password_entry, !busy);
  if (busy) {
    gtk_spinner_start(GTK_SPINNER(app->login_spinner));
  } else {
    gtk_spinner_stop(GTK_SPINNER(app->login_spinner));
  }
}

static void show_login(PineApp *app, const gchar *message) {
  app->avatar_epoch++;
  g_clear_pointer(&app->user_id, g_free);
  g_clear_pointer(&app->display_name, g_free);
  g_clear_pointer(&app->active_room_id, g_free);
  g_clear_pointer(&app->active_room_name, g_free);
  g_clear_pointer(&app->message_snapshot, g_free);
  g_clear_pointer(&app->profile_text, g_free);
  g_clear_pointer(&app->icon_type, g_free);
  g_clear_pointer(&app->icon_value, g_free);
  g_clear_pointer(&app->icon_image_url, g_free);
  if (app->avatar_cache != NULL) {
    g_hash_table_remove_all(app->avatar_cache);
  }
  app->messages_loading = FALSE;
  app->rooms_loading = FALSE;
  app->articles_loading = FALSE;
  app->articles_loaded = FALSE;
  app->article_loading = FALSE;
  app->article_saving = FALSE;
  clear_selected_article(app);
  if (app->articles_list != NULL) {
    clear_list_box(app->articles_list);
  }
  if (app->article_page_stack != NULL) {
    gtk_stack_set_visible_child_name(
      GTK_STACK(app->article_page_stack),
      "empty"
    );
  }
  gtk_stack_set_visible_child_name(GTK_STACK(app->root_stack), "login");
  gtk_label_set_text(GTK_LABEL(app->login_status), message != NULL ? message : "");
  set_login_busy(app, FALSE);
  gtk_widget_grab_focus(app->username_entry);
}

static gboolean parse_user(PineApp *app, const gchar *body) {
  json_object *root = json_tokener_parse(body);
  json_object *user = NULL;
  const gchar *id;
  const gchar *username;
  const gchar *display_name;
  gboolean valid = FALSE;

  if (
    root != NULL &&
    json_object_object_get_ex(root, "user", &user) &&
    json_object_is_type(user, json_type_object)
  ) {
    id = object_string(user, "id");
    username = object_string(user, "username");
    display_name = object_string(user, "display_name");
    if (id != NULL && username != NULL) {
      g_free(app->user_id);
      g_free(app->display_name);
      app->user_id = g_strdup(id);
      app->display_name = g_strdup(
        display_name != NULL && *display_name != '\0' ? display_name : username
      );
      if (app->user_label != NULL) {
        gtk_label_set_text(GTK_LABEL(app->user_label), app->display_name);
      }
      if (app->user_avatar_fallback != NULL) {
        gchar *initial = first_character(app->display_name);
        gtk_label_set_text(GTK_LABEL(app->user_avatar_fallback), initial);
        g_free(initial);
      }
      if (app->user_avatar != NULL) {
        avatar_attach_target(app, app->user_id, app->user_avatar, 26);
      } else {
        refresh_current_profile(app);
      }
      valid = TRUE;
    }
  }
  if (root != NULL) {
    json_object_put(root);
  }
  return valid;
}

static void auth_completed(GObject *source, GAsyncResult *result, gpointer user_data) {
  (void)source;
  PineApp *app = user_data;
  GError *error = NULL;
  PineApiResponse *response = pine_api_request_finish(result, &error);

  set_login_busy(app, FALSE);
  if (error != NULL) {
    show_login(app, error->message);
    g_error_free(error);
  } else if (response->status >= 200 && response->status < 300 &&
             parse_user(app, response->body)) {
    set_connection_state(app, "● 接続中…", "connection-connecting");
    gtk_stack_set_visible_child_name(GTK_STACK(app->root_stack), "chat");
    load_rooms(app);
  } else {
    gchar *message = response_error_message(response);
    show_login(app, message);
    g_free(message);
  }
  pine_api_response_free(response);
}

static void begin_login(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  PineApp *app = user_data;
  const gchar *username = gtk_entry_get_text(GTK_ENTRY(app->username_entry));
  const gchar *password = gtk_entry_get_text(GTK_ENTRY(app->password_entry));
  json_object *body;
  const gchar *serialized;

  if (*username == '\0' || *password == '\0') {
    gtk_label_set_text(GTK_LABEL(app->login_status), "ユーザー名とパスワードを入力してください");
    return;
  }

  body = json_object_new_object();
  json_object_object_add(body, "username", json_object_new_string(username));
  json_object_object_add(body, "password", json_object_new_string(password));
  if (app->register_mode) {
    json_object_object_add(body, "display_name", json_object_new_string(username));
  }
  serialized = json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN);

  set_login_busy(app, TRUE);
  gtk_label_set_text(GTK_LABEL(app->login_status), "ログインしています…");
  pine_api_request_async(
    app->api,
    "POST",
    app->register_mode ? "/api/auth/register" : "/api/auth/login",
    serialized,
    NULL,
    auth_completed,
    app
  );
  json_object_put(body);
}

static void toggle_auth_mode(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  PineApp *app = user_data;
  app->register_mode = !app->register_mode;
  gtk_label_set_text(
    GTK_LABEL(app->login_title),
    app->register_mode ? "Pine2に参加" : "Kan Accountでログイン"
  );
  gtk_label_set_text(
    GTK_LABEL(app->login_subtitle),
    app->register_mode
      ? "アカウントを作成してPine2を始めましょう"
      : "アカウントでログインしてください"
  );
  gtk_button_set_label(
    GTK_BUTTON(app->login_button),
    app->register_mode ? "アカウント作成" : "ログイン"
  );
  gtk_button_set_label(
    GTK_BUTTON(app->auth_toggle_button),
    app->register_mode
      ? "すでにアカウントをお持ちの方はこちら"
      : "まだアカウントをお持ちでない方はこちら"
  );
  gtk_entry_set_text(GTK_ENTRY(app->username_entry), "");
  gtk_entry_set_text(GTK_ENTRY(app->password_entry), "");
  gtk_label_set_text(GTK_LABEL(app->login_status), "");
  gtk_widget_grab_focus(app->username_entry);
}

static void clear_list_box(GtkWidget *list_box) {
  GList *children = gtk_container_get_children(GTK_CONTAINER(list_box));
  for (GList *item = children; item != NULL; item = item->next) {
    gtk_widget_destroy(GTK_WIDGET(item->data));
  }
  g_list_free(children);
}

static GtkWidget *room_row_new(json_object *room) {
  const gchar *id = object_string(room, "id");
  const gchar *name = object_string(room, "name");
  const gchar *created_at = object_string(room, "created_at");
  gint unread = object_int(room, "unread_count");
  GDateTime *created = created_at != NULL
    ? g_date_time_new_from_iso8601(created_at, NULL)
    : NULL;
  gchar *date_text = created != NULL
    ? g_date_time_format(created, "%Y/%m/%d")
    : g_strdup("");
  GtkWidget *row = gtk_list_box_row_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
  GtkWidget *title = gtk_label_new(name != NULL ? name : "名称未設定");
  GtkWidget *kind = gtk_label_new(date_text);

  gtk_widget_set_margin_start(box, 14);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 11);
  gtk_widget_set_margin_bottom(box, 11);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
  gtk_style_context_add_class(gtk_widget_get_style_context(title), "room-name");
  gtk_label_set_xalign(GTK_LABEL(kind), 0.0f);
  gtk_style_context_add_class(gtk_widget_get_style_context(kind), "room-date");
  gtk_box_pack_start(GTK_BOX(labels), title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(labels), kind, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), labels, TRUE, TRUE, 0);

  if (unread > 0) {
    gchar *count = g_strdup_printf("%d", unread);
    GtkWidget *badge = gtk_label_new(count);
    gtk_style_context_add_class(gtk_widget_get_style_context(badge), "unread-badge");
    gtk_box_pack_end(GTK_BOX(box), badge, FALSE, FALSE, 0);
    g_free(count);
  }

  gtk_container_add(GTK_CONTAINER(row), box);
  g_object_set_data_full(G_OBJECT(row), "room-id", g_strdup(id), g_free);
  g_object_set_data_full(G_OBJECT(row), "room-name", g_strdup(name), g_free);
  g_object_set_data_full(G_OBJECT(row), "room-created-at", g_strdup(date_text), g_free);
  if (created != NULL) {
    g_date_time_unref(created);
  }
  g_free(date_text);
  return row;
}

static void rooms_completed(GObject *source, GAsyncResult *result, gpointer user_data) {
  (void)source;
  PineApp *app = user_data;
  GError *error = NULL;
  PineApiResponse *response = pine_api_request_finish(result, &error);
  json_object *root = NULL;
  json_object *rooms = NULL;
  app->rooms_loading = FALSE;

  if (error != NULL) {
    set_connection_state(app, "● 未接続", "connection-offline");
    g_warning("ルーム一覧: %s", error->message);
    g_error_free(error);
    return;
  }
  if (response->status == 401) {
    pine_api_response_free(response);
    show_login(app, "セッションの有効期限が切れました");
    return;
  }
  if (response->status < 200 || response->status >= 300) {
    gchar *message = response_error_message(response);
    set_connection_state(app, message, "connection-offline");
    g_free(message);
    pine_api_response_free(response);
    return;
  }

  root = json_tokener_parse(response->body);
  if (
    root == NULL ||
    !json_object_object_get_ex(root, "rooms", &rooms) ||
    !json_object_is_type(rooms, json_type_array)
  ) {
    set_connection_state(app, "● 同期データが不正です", "connection-offline");
  } else {
    clear_list_box(app->rooms_list);
    for (gsize i = 0; i < json_object_array_length(rooms); i++) {
      json_object *room = json_object_array_get_idx(rooms, i);
      gtk_container_add(GTK_CONTAINER(app->rooms_list), room_row_new(room));
    }
    gtk_widget_show_all(app->rooms_list);
    set_connection_state(app, "● 接続済み", "connection-online");

    GList *rows = gtk_container_get_children(GTK_CONTAINER(app->rooms_list));
    GtkListBoxRow *target = NULL;
    for (GList *item = rows; item != NULL; item = item->next) {
      const gchar *room_id = g_object_get_data(G_OBJECT(item->data), "room-id");
      if (g_strcmp0(room_id, app->active_room_id) == 0) {
        target = GTK_LIST_BOX_ROW(item->data);
        break;
      }
    }
    if (target == NULL && rows != NULL) {
      if (app->active_room_id != NULL) {
        g_clear_pointer(&app->active_room_id, g_free);
        g_clear_pointer(&app->active_room_name, g_free);
        g_clear_pointer(&app->message_snapshot, g_free);
      }
      target = GTK_LIST_BOX_ROW(rows->data);
    }
    if (target != NULL) {
      gtk_list_box_select_row(GTK_LIST_BOX(app->rooms_list), target);
    }
    g_list_free(rows);
  }

  if (root != NULL) {
    json_object_put(root);
  }
  pine_api_response_free(response);
}

static void load_rooms(PineApp *app) {
  if (app->rooms_loading || app->user_id == NULL) {
    return;
  }
  app->rooms_loading = TRUE;
  pine_api_request_async(
    app->api,
    "GET",
    "/api/user-rooms",
    NULL,
    NULL,
    rooms_completed,
    app
  );
}

static gchar *format_timestamp(gint64 timestamp_ms) {
  GDateTime *date = g_date_time_new_from_unix_local(timestamp_ms / 1000);
  gchar *formatted = date != NULL ? g_date_time_format(date, "%H:%M") : g_strdup("");
  if (date != NULL) {
    g_date_time_unref(date);
  }
  return formatted;
}

static gint64 message_timestamp(json_object *message) {
  json_object *value = NULL;
  if (message != NULL && json_object_object_get_ex(message, "timestamp", &value)) {
    return json_object_get_int64(value);
  }
  return 0;
}

static gchar *format_message_date(gint64 timestamp_ms) {
  GDateTime *date = g_date_time_new_from_unix_local(timestamp_ms / 1000);
  gchar *formatted = date != NULL
    ? g_date_time_format(date, "%Y年%m月%d日")
    : g_strdup("");
  if (date != NULL) {
    g_date_time_unref(date);
  }
  return formatted;
}

static gboolean array_contains_string(json_object *array, const gchar *needle) {
  if (array == NULL || !json_object_is_type(array, json_type_array)) {
    return FALSE;
  }
  for (gsize i = 0; i < json_object_array_length(array); i++) {
    json_object *item = json_object_array_get_idx(array, i);
    if (json_object_is_type(item, json_type_string) &&
        g_strcmp0(json_object_get_string(item), needle) == 0) {
      return TRUE;
    }
  }
  return FALSE;
}

static void open_message_image(GtkWidget *widget, gpointer user_data) {
  PineApp *app = user_data;
  const gchar *source = g_object_get_data(G_OBJECT(widget), "image-url");
  if (source == NULL || *source == '\0') {
    return;
  }
  gchar *uri = g_str_has_prefix(source, "http://") || g_str_has_prefix(source, "https://")
    ? g_strdup(source)
    : g_strconcat(app->server_url, source, NULL);
  gtk_show_uri_on_window(GTK_WINDOW(app->window), uri, GDK_CURRENT_TIME, NULL);
  g_free(uri);
}

static GtkWidget *message_row_new(PineApp *app, json_object *message) {
  const gchar *sender = object_string(message, "senderNickname");
  const gchar *content = object_string(message, "content");
  const gchar *image_url = object_string(message, "imageUrl");
  const gchar *message_user_id = object_string(message, "userId");
  const gboolean own = g_strcmp0(message_user_id, app->user_id) == 0;
  json_object *read_by = NULL;
  const gint64 timestamp = message_timestamp(message);
  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
  GtkWidget *bubble = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *sender_label = gtk_label_new(sender != NULL ? sender : "Unknown");
  GtkWidget *content_label = gtk_label_new(content != NULL ? content : "");
  GtkWidget *avatar = avatar_widget_new(
    app,
    message_user_id,
    sender,
    32,
    own ? "avatar-own" : "avatar-other",
    NULL
  );
  gchar *time_text = format_timestamp(timestamp);
  GtkWidget *time_label = gtk_label_new(time_text);
  g_free(time_text);

  gtk_widget_set_margin_start(outer, 16);
  gtk_widget_set_margin_end(outer, 16);
  gtk_widget_set_margin_top(outer, 6);
  gtk_widget_set_margin_bottom(outer, 6);
  gtk_widget_set_halign(line, own ? GTK_ALIGN_END : GTK_ALIGN_START);
  gtk_widget_set_valign(avatar, GTK_ALIGN_START);
  gtk_widget_set_size_request(avatar, 32, 32);
  gtk_style_context_add_class(gtk_widget_get_style_context(bubble), own ? "message-own" : "message-other");
  gtk_label_set_xalign(GTK_LABEL(sender_label), 0.0f);
  gtk_style_context_add_class(gtk_widget_get_style_context(sender_label), "message-sender");
  gtk_style_context_add_class(gtk_widget_get_style_context(time_label), own ? "message-time-own" : "message-time-other");
  gtk_label_set_xalign(GTK_LABEL(content_label), 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(content_label), TRUE);
  gtk_label_set_line_wrap_mode(GTK_LABEL(content_label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_selectable(GTK_LABEL(content_label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(content_label), 72);
  gtk_widget_set_halign(content_label, GTK_ALIGN_FILL);

  if (!own) {
    gtk_box_pack_start(GTK_BOX(bubble), sender_label, FALSE, FALSE, 0);
  }
  if (content != NULL && *content != '\0') {
    gtk_box_pack_start(GTK_BOX(bubble), content_label, FALSE, FALSE, 0);
  }
  if (image_url != NULL && *image_url != '\0') {
    GtkWidget *image_button = gtk_button_new_with_label("画像を開く");
    gtk_style_context_add_class(gtk_widget_get_style_context(image_button), "image-link");
    g_object_set_data_full(G_OBJECT(image_button), "image-url", g_strdup(image_url), g_free);
    g_signal_connect(image_button, "clicked", G_CALLBACK(open_message_image), app);
    gtk_box_pack_start(GTK_BOX(bubble), image_button, FALSE, FALSE, 0);
  }
  gtk_box_pack_start(GTK_BOX(footer), time_label, FALSE, FALSE, 0);

  if (own && json_object_object_get_ex(message, "readBy", &read_by)) {
    gint readers = MAX(0, (gint)json_object_array_length(read_by) - 1);
    if (readers > 0) {
      gchar *read_text = readers == 1 ? g_strdup("既読") : g_strdup_printf("既読 %d", readers);
      GtkWidget *read_label = gtk_label_new(read_text);
      gtk_label_set_xalign(GTK_LABEL(read_label), 0.0f);
      gtk_style_context_add_class(gtk_widget_get_style_context(read_label), "read-state");
      gtk_box_pack_start(GTK_BOX(footer), read_label, FALSE, FALSE, 0);
      g_free(read_text);
    }
  }
  gtk_box_pack_start(GTK_BOX(bubble), footer, FALSE, FALSE, 0);

  if (own) {
    gtk_box_pack_start(GTK_BOX(line), bubble, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(line), avatar, FALSE, FALSE, 0);
  } else {
    gtk_box_pack_start(GTK_BOX(line), avatar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(line), bubble, FALSE, FALSE, 0);
  }
  gtk_box_pack_start(GTK_BOX(outer), line, TRUE, TRUE, 0);
  return outer;
}

static GtkWidget *date_separator_new(gint64 timestamp_ms) {
  gchar *date_text = format_message_date(timestamp_ms);
  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *label = gtk_label_new(date_text);
  g_free(date_text);
  gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(outer, 12);
  gtk_widget_set_margin_bottom(outer, 8);
  gtk_style_context_add_class(gtk_widget_get_style_context(label), "date-separator");
  gtk_box_pack_start(GTK_BOX(outer), label, TRUE, FALSE, 0);
  return outer;
}

static gboolean scroll_messages_to_bottom(gpointer user_data) {
  PineApp *app = user_data;
  GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(
    GTK_SCROLLED_WINDOW(app->messages_scroller)
  );
  gtk_adjustment_set_value(
    adjustment,
    MAX(0.0, gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment))
  );
  return G_SOURCE_REMOVE;
}

static void mark_messages_read(PineApp *app, json_object *message_ids) {
  if (
    json_object_array_length(message_ids) == 0 ||
    app->active_room_id == NULL ||
    !gtk_window_is_active(GTK_WINDOW(app->window))
  ) {
    return;
  }
  json_object *body = json_object_new_object();
  json_object_object_add(body, "action", json_object_new_string("markAsRead"));
  json_object_object_add(body, "roomId", json_object_new_string(app->active_room_id));
  json_object_object_add(body, "messageIds", json_object_get(message_ids));
  pine_api_request_async(
    app->api,
    "PUT",
    "/api/messages",
    json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN),
    NULL,
    NULL,
    NULL
  );
  json_object_put(body);
}

static void room_request_free(RoomRequest *request) {
  g_free(request->room_id);
  g_free(request);
}

static void messages_completed(GObject *source, GAsyncResult *result, gpointer user_data) {
  (void)source;
  RoomRequest *request = user_data;
  PineApp *app = request->app;
  GError *error = NULL;
  PineApiResponse *response = pine_api_request_finish(result, &error);
  json_object *root = NULL;
  json_object *messages = NULL;
  json_object *unread_ids = json_object_new_array();
  gchar *last_date = NULL;
  gboolean is_active = g_strcmp0(request->room_id, app->active_room_id) == 0;
  gboolean snapshot_changed = FALSE;
  gboolean should_scroll = FALSE;

  if (is_active) {
    app->messages_loading = FALSE;
  }
  if (error != NULL) {
    if (is_active) {
      set_connection_state(app, "● 再接続中…", "connection-connecting");
    }
    g_warning("メッセージ同期: %s", error->message);
    g_error_free(error);
    goto cleanup;
  }
  if (!is_active) {
    goto cleanup;
  }
  if (response->status == 401) {
    show_login(app, "セッションの有効期限が切れました");
    goto cleanup;
  }
  if (response->status < 200 || response->status >= 300) {
    set_connection_state(app, "● 再接続中…", "connection-connecting");
    goto cleanup;
  }

  root = json_tokener_parse(response->body);
  if (
    root == NULL ||
    !json_object_object_get_ex(root, "messages", &messages) ||
    !json_object_is_type(messages, json_type_array)
  ) {
    set_connection_state(app, "● 同期データが不正です", "connection-offline");
    goto cleanup;
  }

  snapshot_changed = g_strcmp0(app->message_snapshot, response->body) != 0;
  if (snapshot_changed) {
    GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(
      GTK_SCROLLED_WINDOW(app->messages_scroller)
    );
    should_scroll = app->message_snapshot == NULL ||
      gtk_adjustment_get_value(adjustment) + gtk_adjustment_get_page_size(adjustment) >=
      gtk_adjustment_get_upper(adjustment) - 64.0;
    g_free(app->message_snapshot);
    app->message_snapshot = g_strdup(response->body);
    clear_list_box(app->messages_list);
  }
  for (gsize i = 0; i < json_object_array_length(messages); i++) {
    json_object *message = json_object_array_get_idx(messages, i);
    json_object *read_by = NULL;
    const gchar *message_id = object_string(message, "id");
    const gchar *message_user_id = object_string(message, "userId");
    json_object_object_get_ex(message, "readBy", &read_by);
    if (snapshot_changed) {
      gchar *current_date = format_message_date(message_timestamp(message));
      if (g_strcmp0(current_date, last_date) != 0) {
        gtk_container_add(
          GTK_CONTAINER(app->messages_list),
          date_separator_new(message_timestamp(message))
        );
        g_free(last_date);
        last_date = g_strdup(current_date);
      }
      g_free(current_date);
      gtk_container_add(GTK_CONTAINER(app->messages_list), message_row_new(app, message));
    }
    if (
      message_id != NULL &&
      g_strcmp0(message_user_id, app->user_id) != 0 &&
      !array_contains_string(read_by, app->user_id)
    ) {
      json_object_array_add(unread_ids, json_object_new_string(message_id));
    }
  }
  if (snapshot_changed) {
    gtk_widget_show_all(app->messages_list);
  }
  set_connection_state(app, "● 接続済み", "connection-online");
  if (snapshot_changed && should_scroll) {
    g_idle_add(scroll_messages_to_bottom, app);
  }
  mark_messages_read(app, unread_ids);

cleanup:
  if (root != NULL) {
    json_object_put(root);
  }
  json_object_put(unread_ids);
  g_free(last_date);
  pine_api_response_free(response);
  room_request_free(request);
}

static void refresh_messages(PineApp *app) {
  if (app->active_room_id == NULL || app->messages_loading) {
    return;
  }

  gchar *escaped = g_uri_escape_string(app->active_room_id, NULL, TRUE);
  gchar *path = g_strdup_printf("/api/messages?roomId=%s", escaped);
  RoomRequest *request = g_new0(RoomRequest, 1);
  request->app = app;
  request->room_id = g_strdup(app->active_room_id);
  app->messages_loading = TRUE;
  pine_api_request_async(
    app->api,
    "GET",
    path,
    NULL,
    NULL,
    messages_completed,
    request
  );
  g_free(path);
  g_free(escaped);
}

static void room_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
  (void)box;
  PineApp *app = user_data;
  if (row == NULL) {
    return;
  }
  const gchar *room_id = g_object_get_data(G_OBJECT(row), "room-id");
  const gchar *room_name = g_object_get_data(G_OBJECT(row), "room-name");
  const gchar *created_at = g_object_get_data(G_OBJECT(row), "room-created-at");
  if (g_strcmp0(room_id, app->active_room_id) == 0) {
    return;
  }

  g_free(app->active_room_id);
  g_free(app->active_room_name);
  g_clear_pointer(&app->message_snapshot, g_free);
  app->active_room_id = g_strdup(room_id);
  app->active_room_name = g_strdup(room_name);
  app->messages_loading = FALSE;
  gtk_label_set_text(GTK_LABEL(app->room_title), room_name);
  gchar *subtitle = created_at != NULL && *created_at != '\0'
    ? g_strdup_printf("作成日: %s", created_at)
    : g_strdup("");
  gtk_label_set_text(GTK_LABEL(app->room_subtitle), subtitle);
  g_free(subtitle);
  gtk_widget_set_sensitive(app->invite_button, TRUE);
  gtk_widget_set_sensitive(app->composer_entry, TRUE);
  gtk_widget_set_sensitive(app->send_button, TRUE);
  clear_list_box(app->messages_list);
  refresh_messages(app);
}

static gchar *composer_get_text(PineApp *app) {
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->composer_entry));
  GtkTextIter start;
  GtkTextIter end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

static void composer_clear(PineApp *app) {
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->composer_entry));
  gtk_text_buffer_set_text(buffer, "", -1);
}

static void send_completed(GObject *source, GAsyncResult *result, gpointer user_data) {
  (void)source;
  PineApp *app = user_data;
  GError *error = NULL;
  PineApiResponse *response = pine_api_request_finish(result, &error);
  app->send_loading = FALSE;
  gtk_widget_set_sensitive(app->send_button, TRUE);
  gtk_widget_set_sensitive(app->composer_entry, TRUE);

  if (error != NULL) {
    set_connection_state(app, "● 送信に失敗しました", "connection-offline");
    g_warning("メッセージ送信: %s", error->message);
    g_error_free(error);
  } else if (response->status >= 200 && response->status < 300) {
    composer_clear(app);
    refresh_messages(app);
  } else {
    gchar *message = response_error_message(response);
    set_connection_state(app, message, "connection-offline");
    g_free(message);
  }
  pine_api_response_free(response);
}

static void send_message(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  PineApp *app = user_data;
  gchar *text = composer_get_text(app);
  g_strstrip(text);
  if (app->active_room_id == NULL || app->send_loading || *text == '\0') {
    g_free(text);
    return;
  }

  gchar *message_id = g_uuid_string_random();
  json_object *message = json_object_new_object();
  json_object *body = json_object_new_object();
  json_object_object_add(message, "id", json_object_new_string(message_id));
  json_object_object_add(message, "content", json_object_new_string(text));
  json_object_object_add(body, "roomId", json_object_new_string(app->active_room_id));
  json_object_object_add(body, "message", message);

  app->send_loading = TRUE;
  gtk_widget_set_sensitive(app->send_button, FALSE);
  gtk_widget_set_sensitive(app->composer_entry, FALSE);
  pine_api_request_async(
    app->api,
    "POST",
    "/api/messages",
    json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN),
    NULL,
    send_completed,
    app
  );
  json_object_put(body);
  g_free(message_id);
  g_free(text);
}

static gboolean composer_key_press(
  GtkWidget *widget,
  GdkEventKey *event,
  gpointer user_data
) {
  if (
    (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) &&
    (event->state & GDK_SHIFT_MASK) == 0
  ) {
    send_message(widget, user_data);
    return TRUE;
  }
  return FALSE;
}

static gboolean poll_messages(gpointer user_data) {
  PineApp *app = user_data;
  if (gtk_window_is_active(GTK_WINDOW(app->window))) {
    refresh_messages(app);
  }
  return G_SOURCE_CONTINUE;
}

static gboolean poll_rooms(gpointer user_data) {
  PineApp *app = user_data;
  if (gtk_window_is_active(GTK_WINDOW(app->window)) && app->user_id != NULL) {
    load_rooms(app);
  }
  return G_SOURCE_CONTINUE;
}

static void logout_completed(GObject *source, GAsyncResult *result, gpointer user_data) {
  (void)source;
  PineApp *app = user_data;
  GError *error = NULL;
  PineApiResponse *response = pine_api_request_finish(result, &error);
  if (error != NULL) {
    g_error_free(error);
  }
  pine_api_response_free(response);
  pine_api_clear_session(app->api);
  clear_list_box(app->rooms_list);
  clear_list_box(app->messages_list);
  gtk_entry_set_text(GTK_ENTRY(app->password_entry), "");
  show_login(app, "ログアウトしました");
}

static void begin_logout(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  PineApp *app = user_data;
  pine_api_request_async(
    app->api,
    "POST",
    "/api/auth/logout",
    "{}",
    NULL,
    logout_completed,
    app
  );
}

static void create_room_completed(
  GObject *source,
  GAsyncResult *result,
  gpointer user_data
) {
  (void)source;
  PineApp *app = user_data;
  GError *error = NULL;
  PineApiResponse *response = pine_api_request_finish(result, &error);
  if (error != NULL) {
    set_connection_state(app, "● ルーム作成に失敗", "connection-offline");
    g_warning("ルーム作成: %s", error->message);
    g_error_free(error);
  } else if (response->status >= 200 && response->status < 300) {
    g_clear_pointer(&app->active_room_id, g_free);
    g_clear_pointer(&app->active_room_name, g_free);
    g_clear_pointer(&app->message_snapshot, g_free);
    set_connection_state(app, "● ルームを作成しました", "connection-online");
    load_rooms(app);
  } else {
    gchar *message = response_error_message(response);
    set_connection_state(app, message, "connection-offline");
    g_free(message);
  }
  pine_api_response_free(response);
}

static void show_create_room_dialog(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  PineApp *app = user_data;
  GtkWidget *dialog = gtk_dialog_new_with_buttons(
    "新しいルーム",
    GTK_WINDOW(app->window),
    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
    "キャンセル",
    GTK_RESPONSE_CANCEL,
    "作成",
    GTK_RESPONSE_ACCEPT,
    NULL
  );
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *label = gtk_label_new("ルーム名を入力");
  GtkWidget *entry = gtk_entry_new();
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_entry_set_max_length(GTK_ENTRY(entry), 50);
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  gtk_widget_set_margin_start(content, 18);
  gtk_widget_set_margin_end(content, 18);
  gtk_widget_set_margin_top(content, 16);
  gtk_widget_set_margin_bottom(content, 10);
  gtk_box_set_spacing(GTK_BOX(content), 8);
  gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 0);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
  gtk_widget_show_all(dialog);

  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
    const gchar *input = gtk_entry_get_text(GTK_ENTRY(entry));
    gchar *name = g_strdup(input);
    g_strstrip(name);
    gchar *lower = g_utf8_strdown(name, -1);
    if (*name != '\0' && strstr(lower, "open") == NULL && strstr(name, "オープン") == NULL) {
      gchar *room_id = g_uuid_string_random();
      json_object *body = json_object_new_object();
      json_object_object_add(body, "roomId", json_object_new_string(room_id));
      json_object_object_add(body, "roomName", json_object_new_string(name));
      json_object_object_add(body, "action", json_object_new_string("create"));
      set_connection_state(app, "● ルームを作成中…", "connection-connecting");
      pine_api_request_async(
        app->api,
        "POST",
        "/api/user-rooms",
        json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN),
        NULL,
        create_room_completed,
        app
      );
      json_object_put(body);
      g_free(room_id);
    } else if (*name != '\0') {
      set_connection_state(app, "● オープンルームは作成できません", "connection-offline");
    }
    g_free(lower);
    g_free(name);
  }
  gtk_widget_destroy(dialog);
}

static void copy_invite_link(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  PineApp *app = user_data;
  if (app->active_room_id == NULL) {
    return;
  }
  gchar *escaped_id = g_uri_escape_string(app->active_room_id, NULL, FALSE);
  gchar *escaped_name = g_uri_escape_string(app->active_room_name, NULL, FALSE);
  gchar *url = g_strdup_printf(
    "%s?room=%s&name=%s",
    app->server_url,
    escaped_id,
    escaped_name
  );
  GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
  gtk_clipboard_set_text(clipboard, url, -1);
  gtk_clipboard_store(clipboard);
  set_connection_state(app, "● 招待リンクをコピーしました", "connection-online");
  g_free(url);
  g_free(escaped_name);
  g_free(escaped_id);
}

static void open_web_section(GtkWidget *widget, gpointer user_data) {
  PineApp *app = user_data;
  const gchar *path = g_object_get_data(G_OBJECT(widget), "web-path");
  gchar *url = g_strconcat(app->server_url, path != NULL ? path : "/", NULL);
  gtk_show_uri_on_window(GTK_WINDOW(app->window), url, GDK_CURRENT_TIME, NULL);
  g_free(url);
}

static void prepare_settings_path(PineApp *app) {
  if (app->settings_path != NULL) {
    return;
  }

  gchar *directory = g_build_filename(
    g_get_user_config_dir(),
    "pine2-gtk",
    NULL
  );
  if (g_mkdir_with_parents(directory, 0700) != 0) {
    g_warning("設定ディレクトリを作成できませんでした: %s", directory);
  }
  app->settings_path = g_build_filename(directory, "settings.ini", NULL);
  g_free(directory);
}

static void load_theme_setting(PineApp *app) {
  GKeyFile *settings = g_key_file_new();
  GError *error = NULL;
  app->theme = PINE_THEME_LIGHT;
  prepare_settings_path(app);

  if (!g_file_test(app->settings_path, G_FILE_TEST_IS_REGULAR)) {
    g_key_file_free(settings);
    return;
  }
  if (!g_key_file_load_from_file(settings, app->settings_path, G_KEY_FILE_NONE, &error)) {
    g_warning("外観設定を読み込めませんでした: %s", error->message);
    g_clear_error(&error);
    g_key_file_free(settings);
    return;
  }

  gchar *theme = g_key_file_get_string(settings, "Appearance", "theme", NULL);
  if (g_strcmp0(theme, "dark") == 0) {
    app->theme = PINE_THEME_DARK;
  }
  g_free(theme);
  g_key_file_free(settings);
}

static void save_theme_setting(PineApp *app) {
  GKeyFile *settings = g_key_file_new();
  GError *error = NULL;
  prepare_settings_path(app);
  g_key_file_set_string(
    settings,
    "Appearance",
    "theme",
    app->theme == PINE_THEME_DARK ? "dark" : "light"
  );
  if (!g_key_file_save_to_file(settings, app->settings_path, &error)) {
    g_warning("外観設定を保存できませんでした: %s", error->message);
    g_clear_error(&error);
  }
  g_key_file_free(settings);
}

static void theme_radio_toggled(GtkToggleButton *button, gpointer user_data) {
  PineApp *app = user_data;
  if (!gtk_toggle_button_get_active(button)) {
    return;
  }

  gint encoded_theme = GPOINTER_TO_INT(
    g_object_get_data(G_OBJECT(button), "pine-theme")
  );
  app->theme = encoded_theme == 2 ? PINE_THEME_DARK : PINE_THEME_LIGHT;
  apply_theme(app);
  if (app->article_body_view != NULL && app->selected_article_content != NULL) {
    pine_markdown_render(
      GTK_TEXT_VIEW(app->article_body_view),
      app->selected_article_content,
      GTK_WINDOW(app->window),
      app->theme == PINE_THEME_DARK
    );
  }
  if (app->article_editor_notebook != NULL &&
      gtk_notebook_get_current_page(GTK_NOTEBOOK(app->article_editor_notebook)) == 1) {
    gchar *content = article_editor_content(app);
    pine_markdown_render(
      GTK_TEXT_VIEW(app->article_editor_preview),
      content,
      GTK_WINDOW(app->window),
      app->theme == PINE_THEME_DARK
    );
    g_free(content);
  }
  save_theme_setting(app);
}

static GtkWidget *build_theme_option(
  const gchar *title,
  const gchar *description,
  const gchar *preview_class,
  GtkRadioButton *group,
  PineTheme theme,
  GtkWidget **radio_out
) {
  GtkWidget *option = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
  GtkWidget *preview = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *preview_toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *preview_body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *preview_sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *preview_content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *radio = group != NULL
    ? gtk_radio_button_new_with_label_from_widget(group, title)
    : gtk_radio_button_new_with_label(NULL, title);
  GtkWidget *detail = gtk_label_new(description);

  gtk_widget_set_size_request(option, 190, -1);
  gtk_widget_set_size_request(preview, -1, 94);
  gtk_widget_set_size_request(preview_toolbar, -1, 20);
  gtk_widget_set_size_request(preview_sidebar, 46, -1);
  gtk_widget_set_hexpand(preview_content, TRUE);
  gtk_widget_set_vexpand(preview_body, TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(option), "theme-option");
  gtk_style_context_add_class(gtk_widget_get_style_context(preview), "theme-preview");
  gtk_style_context_add_class(gtk_widget_get_style_context(preview), preview_class);
  gtk_style_context_add_class(
    gtk_widget_get_style_context(preview_toolbar),
    "preview-toolbar"
  );
  gtk_style_context_add_class(
    gtk_widget_get_style_context(preview_sidebar),
    "preview-sidebar"
  );
  gtk_style_context_add_class(
    gtk_widget_get_style_context(preview_content),
    "preview-content"
  );
  gtk_style_context_add_class(gtk_widget_get_style_context(radio), "theme-radio");
  gtk_style_context_add_class(gtk_widget_get_style_context(detail), "theme-detail");
  gtk_label_set_xalign(GTK_LABEL(detail), 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(detail), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(detail), 25);
  g_object_set_data(
    G_OBJECT(radio),
    "pine-theme",
    GINT_TO_POINTER(theme == PINE_THEME_DARK ? 2 : 1)
  );

  gtk_box_pack_start(GTK_BOX(preview_body), preview_sidebar, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(preview_body), preview_content, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(preview), preview_toolbar, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(preview), preview_body, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(option), preview, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(option), radio, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(option), detail, FALSE, FALSE, 0);
  *radio_out = radio;
  return option;
}

static GtkWidget *settings_page_new(
  const gchar *title,
  const gchar *subtitle,
  GtkWidget **page_out
) {
  GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
  GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget *heading = gtk_label_new(title);
  GtkWidget *description = gtk_label_new(subtitle);
  gtk_scrolled_window_set_policy(
    GTK_SCROLLED_WINDOW(scroller),
    GTK_POLICY_NEVER,
    GTK_POLICY_AUTOMATIC
  );
  gtk_scrolled_window_set_shadow_type(
    GTK_SCROLLED_WINDOW(scroller),
    GTK_SHADOW_NONE
  );
  gtk_widget_set_margin_start(page, 28);
  gtk_widget_set_margin_end(page, 28);
  gtk_widget_set_margin_top(page, 22);
  gtk_widget_set_margin_bottom(page, 22);
  gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
  gtk_label_set_xalign(GTK_LABEL(description), 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(description), TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(page), "settings-page");
  gtk_style_context_add_class(
    gtk_widget_get_style_context(heading),
    "settings-heading"
  );
  gtk_style_context_add_class(
    gtk_widget_get_style_context(description),
    "settings-subtitle"
  );
  gtk_box_pack_start(GTK_BOX(page), heading, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(page), description, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(scroller), page);
  *page_out = page;
  return scroller;
}

static GtkWidget *settings_card_new(const gchar *title) {
  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  GtkWidget *heading = gtk_label_new(title);
  gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
  gtk_style_context_add_class(gtk_widget_get_style_context(card), "settings-card");
  gtk_style_context_add_class(
    gtk_widget_get_style_context(heading),
    "settings-section-title"
  );
  gtk_box_pack_start(GTK_BOX(card), heading, FALSE, FALSE, 0);
  return card;
}

static GtkWidget *settings_category_new(
  const gchar *label_text,
  const gchar *page_name
) {
  GtkWidget *row = gtk_list_box_row_new();
  GtkWidget *label = gtk_label_new(label_text);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_widget_set_margin_start(label, 12);
  gtk_widget_set_margin_end(label, 12);
  gtk_widget_set_margin_top(label, 9);
  gtk_widget_set_margin_bottom(label, 9);
  gtk_container_add(GTK_CONTAINER(row), label);
  g_object_set_data_full(
    G_OBJECT(row),
    "settings-page",
    g_strdup(page_name),
    g_free
  );
  return row;
}

static void settings_category_selected(
  GtkListBox *box,
  GtkListBoxRow *row,
  gpointer user_data
) {
  (void)box;
  PineApp *app = user_data;
  if (row == NULL || app->settings_stack == NULL) {
    return;
  }
  const gchar *page = g_object_get_data(G_OBJECT(row), "settings-page");
  gtk_stack_set_visible_child_name(GTK_STACK(app->settings_stack), page);
}

static void settings_set_profile_busy(PineApp *app, gboolean busy) {
  app->profile_saving = busy;
  if (app->settings_profile_save_button != NULL) {
    gtk_widget_set_sensitive(app->settings_profile_save_button, !busy);
  }
  if (app->settings_icon_save_button != NULL) {
    gtk_widget_set_sensitive(app->settings_icon_save_button, !busy);
  }
}

static void settings_profile_saved(
  GObject *source,
  GAsyncResult *result,
  gpointer user_data
) {
  (void)source;
  PineApp *app = user_data;
  GError *error = NULL;
  PineApiResponse *response = pine_api_request_finish(result, &error);
  settings_set_profile_busy(app, FALSE);
  if (error != NULL) {
    set_settings_profile_status(app, "プロフィールの保存に失敗しました");
    g_warning("プロフィール保存: %s", error->message);
    g_error_free(error);
  } else if (response->status >= 200 && response->status < 300) {
    json_object *root = json_tokener_parse(response->body);
    json_object *profile = NULL;
    if (root != NULL &&
        json_object_object_get_ex(root, "profile", &profile) &&
        json_object_is_type(profile, json_type_object)) {
      update_current_profile(app, profile);
    }
    if (root != NULL) {
      json_object_put(root);
    }
    set_settings_profile_status(app, "プロフィールを保存しました");
  } else {
    gchar *message = response_error_message(response);
    set_settings_profile_status(app, message);
    g_free(message);
  }
  pine_api_response_free(response);
}

static void settings_save_profile(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  PineApp *app = user_data;
  if (app->profile_saving || app->settings_name_entry == NULL ||
      app->settings_profile_text == NULL) {
    return;
  }
  gchar *name = g_strdup(
    gtk_entry_get_text(GTK_ENTRY(app->settings_name_entry))
  );
  g_strstrip(name);
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(
    GTK_TEXT_VIEW(app->settings_profile_text)
  );
  GtkTextIter start;
  GtkTextIter end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  gchar *profile_text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  g_strstrip(profile_text);
  if (*name == '\0' || g_utf8_strlen(name, -1) > 20) {
    set_settings_profile_status(app, "名前は1〜20文字で入力してください");
    g_free(profile_text);
    g_free(name);
    return;
  }
  if (g_utf8_strlen(profile_text, -1) > 100) {
    set_settings_profile_status(app, "自己紹介は100文字以内で入力してください");
    g_free(profile_text);
    g_free(name);
    return;
  }

  json_object *body = json_object_new_object();
  json_object_object_add(body, "nickname", json_object_new_string(name));
  json_object_object_add(
    body,
    "profileText",
    json_object_new_string(profile_text)
  );
  settings_set_profile_busy(app, TRUE);
  set_settings_profile_status(app, "プロフィールを保存しています…");
  pine_api_request_async(
    app->api,
    "POST",
    "/api/profiles",
    json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN),
    NULL,
    settings_profile_saved,
    app
  );
  json_object_put(body);
  g_free(profile_text);
  g_free(name);
}

static void settings_icon_saved(
  GObject *source,
  GAsyncResult *result,
  gpointer user_data
) {
  (void)source;
  PineApp *app = user_data;
  GError *error = NULL;
  PineApiResponse *response = pine_api_request_finish(result, &error);
  settings_set_profile_busy(app, FALSE);
  if (error != NULL) {
    set_settings_profile_status(app, "アイコン設定の保存に失敗しました");
    g_warning("アイコン設定: %s", error->message);
    g_error_free(error);
  } else if (response->status >= 200 && response->status < 300) {
    set_settings_profile_status(app, "アイコンを保存しました。再取得しています…");
    refresh_current_profile(app);
  } else {
    gchar *message = response_error_message(response);
    set_settings_profile_status(app, message);
    g_free(message);
  }
  pine_api_response_free(response);
}

static void settings_save_icon(
  PineApp *app,
  const gchar *icon_type,
  const gchar *icon_value,
  const gchar *icon_image_url
) {
  json_object *body = json_object_new_object();
  json_object_object_add(
    body,
    "iconType",
    json_object_new_string(icon_type)
  );
  json_object_object_add(
    body,
    "iconValue",
    json_object_new_string(icon_value)
  );
  if (icon_image_url != NULL) {
    json_object_object_add(
      body,
      "iconImageUrl",
      json_object_new_string(icon_image_url)
    );
  }
  settings_set_profile_busy(app, TRUE);
  set_settings_profile_status(app, "アイコン設定を保存しています…");
  pine_api_request_async(
    app->api,
    "PUT",
    "/api/icons",
    json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN),
    NULL,
    settings_icon_saved,
    app
  );
  json_object_put(body);
}

static void settings_save_icon_url(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  PineApp *app = user_data;
  if (app->profile_saving) {
    return;
  }
  const gchar *url = gtk_entry_get_text(
    GTK_ENTRY(app->settings_icon_url_entry)
  );
  gchar *scheme = g_uri_parse_scheme(url);
  gboolean valid = g_strcmp0(scheme, "https") == 0 ||
                   g_strcmp0(scheme, "http") == 0;
  g_free(scheme);
  if (!valid) {
    set_settings_profile_status(app, "httpまたはhttpsの画像URLを入力してください");
    return;
  }
  settings_save_icon(app, "url", url, url);
}

static void settings_save_preset(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  PineApp *app = user_data;
  if (app->profile_saving) {
    return;
  }
  const gchar *icon = gtk_combo_box_get_active_id(
    GTK_COMBO_BOX(app->settings_preset_combo)
  );
  if (icon != NULL) {
    settings_save_icon(app, "preset", icon, NULL);
  }
}

static void settings_delete_icon(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  PineApp *app = user_data;
  if (app->profile_saving) {
    return;
  }
  settings_set_profile_busy(app, TRUE);
  set_settings_profile_status(app, "アイコンを削除しています…");
  pine_api_request_async(
    app->api,
    "DELETE",
    "/api/icons",
    NULL,
    NULL,
    settings_icon_saved,
    app
  );
}

static void settings_refresh_icon(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  PineApp *app = user_data;
  if (app->profile_saving) {
    return;
  }
  set_settings_profile_status(app, "プロフィールとアイコンを再取得しています…");
  refresh_current_profile(app);
}

static void settings_logout(GtkWidget *widget, gpointer user_data) {
  PineApp *app = user_data;
  begin_logout(widget, app);
  if (app->settings_dialog != NULL) {
    gtk_dialog_response(GTK_DIALOG(app->settings_dialog), GTK_RESPONSE_CLOSE);
  }
}

static void show_settings_dialog(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  PineApp *app = user_data;
  GtkWidget *dialog = gtk_dialog_new_with_buttons(
    "Pine2 設定",
    GTK_WINDOW(app->window),
    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
    "閉じる",
    GTK_RESPONSE_CLOSE,
    NULL
  );
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *layout = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *navigation = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *navigation_title = gtk_label_new("Pine2 設定");
  GtkWidget *categories = gtk_list_box_new();
  GtkWidget *appearance_row = settings_category_new("外観", "appearance");
  GtkWidget *profile_row = settings_category_new("プロフィール", "profile");
  GtkWidget *people_row = settings_category_new("ユーザー管理", "people");
  GtkWidget *account_row = settings_category_new("アカウント", "account");
  GtkWidget *connection_row = settings_category_new("接続情報", "connection");

  GtkWidget *appearance_page = NULL;
  GtkWidget *appearance_scroll = settings_page_new(
    "外観",
    "アプリ全体の配色を選択します。変更はすぐに反映されます。",
    &appearance_page
  );
  GtkWidget *theme_card = settings_card_new("テーマ");
  GtkWidget *theme_options = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *light_radio = NULL;
  GtkWidget *light_option = build_theme_option(
    "ライト",
    "明るい面と細い境界線を使った、すっきりした表示です。",
    "theme-preview-light",
    NULL,
    PINE_THEME_LIGHT,
    &light_radio
  );
  GtkWidget *dark_radio = NULL;
  GtkWidget *dark_option = build_theme_option(
    "ダーク",
    "暗い面にオレンジを効かせ、夜間でも眩しさを抑えます。",
    "theme-preview-dark",
    GTK_RADIO_BUTTON(light_radio),
    PINE_THEME_DARK,
    &dark_radio
  );
  GtkWidget *saved_note = gtk_label_new(
    "選択したテーマは次回起動時にも引き継がれます。"
  );

  GtkWidget *profile_page = NULL;
  GtkWidget *profile_scroll = settings_page_new(
    "プロフィール",
    "Web版と同じプロフィール情報を編集し、すべてのPine2画面で共有します。",
    &profile_page
  );
  GtkWidget *profile_card = settings_card_new("基本情報");
  GtkWidget *profile_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
  GtkWidget *profile_fields = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
  GtkWidget *name_label = gtk_label_new("表示名（20文字以内）");
  GtkWidget *bio_label = gtk_label_new("自己紹介（100文字以内）");
  GtkWidget *bio_scroller = gtk_scrolled_window_new(NULL, NULL);
  app->settings_name_entry = gtk_entry_new();
  app->settings_profile_text = gtk_text_view_new();
  app->settings_profile_avatar = avatar_widget_new(
    app,
    app->user_id,
    app->display_name,
    64,
    "settings-profile-avatar",
    NULL
  );
  app->settings_profile_save_button = gtk_button_new_with_label(
    "プロフィールを保存"
  );
  GtkWidget *icon_card = settings_card_new("プロフィールアイコン");
  GtkWidget *url_label = gtk_label_new("画像URL");
  GtkWidget *url_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  app->settings_icon_url_entry = gtk_entry_new();
  app->settings_icon_save_button = gtk_button_new_with_label("URLを設定");
  GtkWidget *preset_label = gtk_label_new("プリセット");
  GtkWidget *preset_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  app->settings_preset_combo = gtk_combo_box_text_new();
  GtkWidget *preset_save = gtk_button_new_with_label("絵文字を設定");
  GtkWidget *icon_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *icon_refresh = gtk_button_new_with_label("再取得");
  GtkWidget *icon_delete = gtk_button_new_with_label("削除");
  app->settings_profile_status = gtk_label_new("");

  GtkWidget *people_page = NULL;
  GtkWidget *people_scroll = settings_page_new(
    "ユーザー管理",
    "ブロック中のユーザーや管理機能は、既存のWeb設定と同じ情報を使用します。",
    &people_page
  );
  GtkWidget *people_card = settings_card_new("ブロックと管理");
  GtkWidget *people_note = gtk_label_new(
    "現在は詳細なブロック一覧と管理者機能をWeb版で開きます。"
  );
  GtkWidget *people_web = gtk_button_new_with_label("Web版のユーザー管理を開く");

  GtkWidget *account_page = NULL;
  GtkWidget *account_scroll = settings_page_new(
    "アカウント",
    "現在ログインしているKan Accountの情報です。",
    &account_page
  );
  GtkWidget *account_card = settings_card_new("ログイン情報");
  gchar *account_text = g_strdup_printf(
    "表示名: %s\nユーザーID: %s",
    app->display_name != NULL ? app->display_name : "-",
    app->user_id != NULL ? app->user_id : "-"
  );
  GtkWidget *account_label = gtk_label_new(account_text);
  GtkWidget *logout_button = gtk_button_new_with_label("ログアウト");
  g_free(account_text);

  GtkWidget *connection_page = NULL;
  GtkWidget *connection_scroll = settings_page_new(
    "接続情報",
    "GTKクライアントが使用しているサーバーと同期方式を確認できます。",
    &connection_page
  );
  GtkWidget *connection_card = settings_card_new("Pine2 API");
  gchar *connection_text = g_strdup_printf(
    "サーバー: %s\nメッセージ同期: %d秒\nルーム同期: 15秒\n認証: HttpOnly Cookie",
    app->server_url,
    MESSAGE_POLL_SECONDS
  );
  GtkWidget *connection_label = gtk_label_new(connection_text);
  g_free(connection_text);

  app->settings_dialog = dialog;
  app->settings_stack = gtk_stack_new();
  gtk_window_set_default_size(GTK_WINDOW(dialog), 780, 560);
  gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "settings-window");
  gtk_container_set_border_width(GTK_CONTAINER(content), 0);
  gtk_box_set_spacing(GTK_BOX(content), 0);
  gtk_widget_set_size_request(navigation, 180, -1);
  gtk_widget_set_vexpand(layout, TRUE);
  gtk_widget_set_hexpand(app->settings_stack, TRUE);
  gtk_stack_set_transition_type(
    GTK_STACK(app->settings_stack),
    GTK_STACK_TRANSITION_TYPE_SLIDE_UP_DOWN
  );
  gtk_style_context_add_class(
    gtk_widget_get_style_context(navigation),
    "settings-navigation"
  );
  gtk_style_context_add_class(
    gtk_widget_get_style_context(navigation_title),
    "settings-navigation-title"
  );
  gtk_style_context_add_class(
    gtk_widget_get_style_context(categories),
    "settings-categories"
  );
  gtk_label_set_xalign(GTK_LABEL(navigation_title), 0.0f);
  gtk_widget_set_margin_start(navigation_title, 16);
  gtk_widget_set_margin_end(navigation_title, 16);
  gtk_widget_set_margin_top(navigation_title, 18);
  gtk_widget_set_margin_bottom(navigation_title, 12);
  gtk_container_add(GTK_CONTAINER(categories), appearance_row);
  gtk_container_add(GTK_CONTAINER(categories), profile_row);
  gtk_container_add(GTK_CONTAINER(categories), people_row);
  gtk_container_add(GTK_CONTAINER(categories), account_row);
  gtk_container_add(GTK_CONTAINER(categories), connection_row);
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(categories), GTK_SELECTION_SINGLE);
  gtk_box_pack_start(GTK_BOX(navigation), navigation_title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(navigation), categories, FALSE, FALSE, 0);

  gtk_style_context_add_class(gtk_widget_get_style_context(saved_note), "settings-note");
  gtk_label_set_xalign(GTK_LABEL(saved_note), 0.0f);
  gtk_box_pack_start(GTK_BOX(theme_options), light_option, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(theme_options), dark_option, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(theme_card), theme_options, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(theme_card), saved_note, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(appearance_page), theme_card, FALSE, FALSE, 4);

  gtk_label_set_xalign(GTK_LABEL(name_label), 0.0f);
  gtk_label_set_xalign(GTK_LABEL(bio_label), 0.0f);
  gtk_style_context_add_class(gtk_widget_get_style_context(name_label), "settings-field-label");
  gtk_style_context_add_class(gtk_widget_get_style_context(bio_label), "settings-field-label");
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->settings_profile_text), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(app->settings_profile_text), 8);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(app->settings_profile_text), 8);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(app->settings_profile_text), 7);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(app->settings_profile_text), 7);
  gtk_widget_set_size_request(bio_scroller, -1, 86);
  gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(bio_scroller), GTK_SHADOW_NONE);
  gtk_style_context_add_class(gtk_widget_get_style_context(bio_scroller), "settings-text-input");
  gtk_container_add(GTK_CONTAINER(bio_scroller), app->settings_profile_text);
  gtk_box_pack_start(GTK_BOX(profile_fields), name_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(profile_fields), app->settings_name_entry, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(profile_fields), bio_label, FALSE, FALSE, 3);
  gtk_box_pack_start(GTK_BOX(profile_fields), bio_scroller, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(profile_header), app->settings_profile_avatar, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(profile_header), profile_fields, TRUE, TRUE, 0);
  gtk_style_context_add_class(
    gtk_widget_get_style_context(app->settings_profile_save_button),
    "suggested-action"
  );
  gtk_box_pack_start(GTK_BOX(profile_card), profile_header, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(profile_card), app->settings_profile_save_button, FALSE, FALSE, 0);

  gtk_label_set_xalign(GTK_LABEL(url_label), 0.0f);
  gtk_label_set_xalign(GTK_LABEL(preset_label), 0.0f);
  gtk_style_context_add_class(gtk_widget_get_style_context(url_label), "settings-field-label");
  gtk_style_context_add_class(gtk_widget_get_style_context(preset_label), "settings-field-label");
  gtk_entry_set_placeholder_text(
    GTK_ENTRY(app->settings_icon_url_entry),
    "https://example.com/icon.png"
  );
  gtk_box_pack_start(GTK_BOX(url_row), app->settings_icon_url_entry, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(url_row), app->settings_icon_save_button, FALSE, FALSE, 0);
  const gchar *preset_icons[] = {
    "🐱", "🐶", "🦊", "🐸", "🐧", "🦄", "🤖", "👻", "🌟", "🔥", NULL
  };
  for (gint i = 0; preset_icons[i] != NULL; i++) {
    gtk_combo_box_text_append(
      GTK_COMBO_BOX_TEXT(app->settings_preset_combo),
      preset_icons[i],
      preset_icons[i]
    );
  }
  gtk_combo_box_set_active(GTK_COMBO_BOX(app->settings_preset_combo), 0);
  gtk_box_pack_start(GTK_BOX(preset_row), app->settings_preset_combo, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(preset_row), preset_save, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(icon_actions), icon_refresh, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(icon_actions), icon_delete, TRUE, TRUE, 0);
  gtk_style_context_add_class(gtk_widget_get_style_context(icon_delete), "destructive-action");
  gtk_style_context_add_class(
    gtk_widget_get_style_context(app->settings_profile_status),
    "settings-status"
  );
  gtk_label_set_xalign(GTK_LABEL(app->settings_profile_status), 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(app->settings_profile_status), TRUE);
  gtk_box_pack_start(GTK_BOX(icon_card), url_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(icon_card), url_row, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(icon_card), preset_label, FALSE, FALSE, 2);
  gtk_box_pack_start(GTK_BOX(icon_card), preset_row, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(icon_card), icon_actions, FALSE, FALSE, 2);
  gtk_box_pack_start(GTK_BOX(icon_card), app->settings_profile_status, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(profile_page), profile_card, FALSE, FALSE, 4);
  gtk_box_pack_start(GTK_BOX(profile_page), icon_card, FALSE, FALSE, 0);

  gtk_label_set_xalign(GTK_LABEL(people_note), 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(people_note), TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(people_note), "settings-note");
  g_object_set_data(G_OBJECT(people_web), "web-path", "/settings");
  gtk_box_pack_start(GTK_BOX(people_card), people_note, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(people_card), people_web, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(people_page), people_card, FALSE, FALSE, 4);

  gtk_label_set_xalign(GTK_LABEL(account_label), 0.0f);
  gtk_label_set_selectable(GTK_LABEL(account_label), TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(account_label), "settings-value");
  gtk_style_context_add_class(gtk_widget_get_style_context(logout_button), "destructive-action");
  gtk_box_pack_start(GTK_BOX(account_card), account_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(account_card), logout_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(account_page), account_card, FALSE, FALSE, 4);

  gtk_label_set_xalign(GTK_LABEL(connection_label), 0.0f);
  gtk_label_set_selectable(GTK_LABEL(connection_label), TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(connection_label), "settings-value");
  gtk_box_pack_start(GTK_BOX(connection_card), connection_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(connection_page), connection_card, FALSE, FALSE, 4);

  gtk_stack_add_named(GTK_STACK(app->settings_stack), appearance_scroll, "appearance");
  gtk_stack_add_named(GTK_STACK(app->settings_stack), profile_scroll, "profile");
  gtk_stack_add_named(GTK_STACK(app->settings_stack), people_scroll, "people");
  gtk_stack_add_named(GTK_STACK(app->settings_stack), account_scroll, "account");
  gtk_stack_add_named(GTK_STACK(app->settings_stack), connection_scroll, "connection");
  gtk_box_pack_start(GTK_BOX(layout), navigation, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(layout), app->settings_stack, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(content), layout, TRUE, TRUE, 0);

  gtk_toggle_button_set_active(
    GTK_TOGGLE_BUTTON(app->theme == PINE_THEME_DARK ? dark_radio : light_radio),
    TRUE
  );
  g_signal_connect(light_radio, "toggled", G_CALLBACK(theme_radio_toggled), app);
  g_signal_connect(dark_radio, "toggled", G_CALLBACK(theme_radio_toggled), app);
  g_signal_connect(categories, "row-selected", G_CALLBACK(settings_category_selected), app);
  g_signal_connect(app->settings_profile_save_button, "clicked", G_CALLBACK(settings_save_profile), app);
  g_signal_connect(app->settings_icon_save_button, "clicked", G_CALLBACK(settings_save_icon_url), app);
  g_signal_connect(preset_save, "clicked", G_CALLBACK(settings_save_preset), app);
  g_signal_connect(icon_refresh, "clicked", G_CALLBACK(settings_refresh_icon), app);
  g_signal_connect(icon_delete, "clicked", G_CALLBACK(settings_delete_icon), app);
  g_signal_connect(people_web, "clicked", G_CALLBACK(open_web_section), app);
  g_signal_connect(logout_button, "clicked", G_CALLBACK(settings_logout), app);
  gtk_list_box_select_row(GTK_LIST_BOX(categories), GTK_LIST_BOX_ROW(appearance_row));
  sync_profile_widgets(app, app->profile_loading ? "プロフィールを読み込み中…" : "");
  gtk_widget_show_all(dialog);
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
  app->settings_dialog = NULL;
  app->settings_stack = NULL;
  app->settings_name_entry = NULL;
  app->settings_profile_text = NULL;
  app->settings_icon_url_entry = NULL;
  app->settings_profile_status = NULL;
  app->settings_profile_avatar = NULL;
  app->settings_profile_save_button = NULL;
  app->settings_icon_save_button = NULL;
  app->settings_preset_combo = NULL;
}

static gchar *article_date_text(const gchar *iso8601) {
  GDateTime *date = iso8601 != NULL
    ? g_date_time_new_from_iso8601(iso8601, NULL)
    : NULL;
  gchar *text = date != NULL
    ? g_date_time_format(date, "%Y/%m/%d %H:%M")
    : g_strdup("");
  if (date != NULL) {
    g_date_time_unref(date);
  }
  return text;
}

static gchar *article_tags_text(json_object *tags) {
  if (tags == NULL || !json_object_is_type(tags, json_type_array)) {
    return g_strdup("");
  }
  GString *text = g_string_new("");
  for (gsize i = 0; i < json_object_array_length(tags); i++) {
    json_object *tag = json_object_array_get_idx(tags, i);
    if (!json_object_is_type(tag, json_type_string)) {
      continue;
    }
    if (text->len > 0) {
      g_string_append(text, ", ");
    }
    g_string_append(text, json_object_get_string(tag));
  }
  return g_string_free(text, FALSE);
}

static void clear_selected_article(PineApp *app) {
  g_clear_pointer(&app->selected_article_id, g_free);
  g_clear_pointer(&app->selected_article_title, g_free);
  g_clear_pointer(&app->selected_article_content, g_free);
  g_clear_pointer(&app->selected_article_tags, g_free);
  g_clear_pointer(&app->selected_article_author_id, g_free);
  g_clear_pointer(&app->selected_article_edited_at, g_free);
}

static GtkWidget *article_row_new(json_object *article) {
  const gchar *id = object_string(article, "id");
  const gchar *title_text = object_string(article, "title");
  const gchar *author = object_string(article, "author");
  const gchar *created_at = object_string(article, "created_at");
  json_object *tags = NULL;
  json_object_object_get_ex(article, "tags", &tags);
  gchar *date = article_date_text(created_at);
  gchar *tag_text = article_tags_text(tags);
  GtkWidget *row = gtk_list_box_row_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *title = gtk_label_new(title_text != NULL ? title_text : "無題の記事");
  gchar *meta_text = g_strdup_printf(
    "%s  ·  %s",
    author != NULL ? author : "Unknown",
    date
  );
  GtkWidget *meta = gtk_label_new(meta_text);
  GtkWidget *tag_label = gtk_label_new(tag_text);
  gtk_widget_set_margin_start(box, 13);
  gtk_widget_set_margin_end(box, 11);
  gtk_widget_set_margin_top(box, 10);
  gtk_widget_set_margin_bottom(box, 10);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(meta), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(meta), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(tag_label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(tag_label), PANGO_ELLIPSIZE_END);
  gtk_style_context_add_class(gtk_widget_get_style_context(title), "article-row-title");
  gtk_style_context_add_class(gtk_widget_get_style_context(meta), "article-row-meta");
  gtk_style_context_add_class(gtk_widget_get_style_context(tag_label), "article-row-tags");
  gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), meta, FALSE, FALSE, 0);
  if (*tag_text != '\0') {
    gtk_box_pack_start(GTK_BOX(box), tag_label, FALSE, FALSE, 0);
  }
  gtk_container_add(GTK_CONTAINER(row), box);
  g_object_set_data_full(G_OBJECT(row), "article-id", g_strdup(id), g_free);
  g_free(meta_text);
  g_free(tag_text);
  g_free(date);
  return row;
}

static void load_article_detail(PineApp *app, const gchar *article_id);

static void articles_completed(
  GObject *source,
  GAsyncResult *result,
  gpointer user_data
) {
  (void)source;
  PineApp *app = user_data;
  GError *error = NULL;
  PineApiResponse *response = pine_api_request_finish(result, &error);
  json_object *root = NULL;
  json_object *articles = NULL;
  app->articles_loading = FALSE;

  if (error != NULL) {
    gtk_label_set_text(GTK_LABEL(app->articles_status), "記事一覧を取得できませんでした");
    g_warning("記事一覧: %s", error->message);
    g_error_free(error);
    pine_api_response_free(response);
    return;
  }
  if (response->status == 401) {
    pine_api_response_free(response);
    show_login(app, "セッションの有効期限が切れました");
    return;
  }
  root = json_tokener_parse(response->body);
  if (response->status < 200 || response->status >= 300 || root == NULL ||
      !json_object_object_get_ex(root, "articles", &articles) ||
      !json_object_is_type(articles, json_type_array)) {
    gtk_label_set_text(GTK_LABEL(app->articles_status), "記事一覧の形式が不正です");
  } else {
    clear_list_box(app->articles_list);
    GtkListBoxRow *selected = NULL;
    for (gsize i = 0; i < json_object_array_length(articles); i++) {
      GtkWidget *row = article_row_new(json_object_array_get_idx(articles, i));
      gtk_container_add(GTK_CONTAINER(app->articles_list), row);
      const gchar *id = g_object_get_data(G_OBJECT(row), "article-id");
      if (g_strcmp0(id, app->selected_article_id) == 0) {
        selected = GTK_LIST_BOX_ROW(row);
      }
    }
    app->articles_loaded = TRUE;
    gchar *status = g_strdup_printf(
      "%u件の記事",
      (guint)json_object_array_length(articles)
    );
    gtk_label_set_text(GTK_LABEL(app->articles_status), status);
    g_free(status);
    gtk_widget_show_all(app->articles_list);
    if (selected != NULL) {
      gtk_list_box_select_row(GTK_LIST_BOX(app->articles_list), selected);
    } else if (json_object_array_length(articles) > 0) {
      GtkListBoxRow *first = gtk_list_box_get_row_at_index(
        GTK_LIST_BOX(app->articles_list),
        0
      );
      gtk_list_box_select_row(GTK_LIST_BOX(app->articles_list), first);
    } else {
      gtk_stack_set_visible_child_name(
        GTK_STACK(app->article_page_stack),
        "empty"
      );
    }
  }
  if (root != NULL) {
    json_object_put(root);
  }
  pine_api_response_free(response);
}

static void load_articles(PineApp *app) {
  if (app->articles_loading || app->user_id == NULL) {
    return;
  }
  app->articles_loading = TRUE;
  gtk_label_set_text(GTK_LABEL(app->articles_status), "記事一覧を読み込み中…");
  pine_api_request_async(
    app->api,
    "GET",
    "/api/articles?summary=true",
    NULL,
    NULL,
    articles_completed,
    app
  );
}

static void article_detail_completed(
  GObject *source,
  GAsyncResult *result,
  gpointer user_data
) {
  (void)source;
  PineApp *app = user_data;
  GError *error = NULL;
  PineApiResponse *response = pine_api_request_finish(result, &error);
  json_object *root = NULL;
  json_object *article = NULL;
  app->article_loading = FALSE;
  gtk_widget_set_sensitive(app->articles_list, TRUE);
  if (error != NULL) {
    gtk_label_set_text(GTK_LABEL(app->article_meta_label), "記事を取得できませんでした");
    g_warning("記事詳細: %s", error->message);
    g_error_free(error);
    pine_api_response_free(response);
    return;
  }
  if (response->status == 401) {
    pine_api_response_free(response);
    show_login(app, "セッションの有効期限が切れました");
    return;
  }
  root = json_tokener_parse(response->body);
  if (response->status < 200 || response->status >= 300 || root == NULL ||
      !json_object_object_get_ex(root, "article", &article) ||
      !json_object_is_type(article, json_type_object)) {
    gtk_label_set_text(GTK_LABEL(app->article_meta_label), "記事を表示できませんでした");
    goto cleanup;
  }

  const gchar *id = object_string(article, "id");
  const gchar *title = object_string(article, "title");
  const gchar *content = object_string(article, "content");
  const gchar *author = object_string(article, "author");
  const gchar *author_id = object_string(article, "supabase_author_id");
  const gchar *created_at = object_string(article, "created_at");
  const gchar *edited_at = object_string(article, "edited_at");
  json_object *tags = NULL;
  json_object *comments = NULL;
  json_object_object_get_ex(article, "tags", &tags);
  json_object_object_get_ex(article, "comments", &comments);
  gchar *tag_text = article_tags_text(tags);
  gchar *date = article_date_text(created_at);
  gchar *meta = g_strdup_printf(
    "%s  ·  %s%s",
    author != NULL ? author : "Unknown",
    date,
    edited_at != NULL ? "  ·  編集済み" : ""
  );
  const gint comment_count = comments != NULL && json_object_is_type(comments, json_type_array)
    ? (gint)json_object_array_length(comments)
    : object_int(article, "comment_count");
  gchar *social = g_strdup_printf(
    "いいね %d   よくないね %d   コメント %d",
    object_int(article, "likes"),
    object_int(article, "dislikes"),
    comment_count
  );

  clear_selected_article(app);
  app->selected_article_id = g_strdup(id);
  app->selected_article_title = g_strdup(title);
  app->selected_article_content = g_strdup(content);
  app->selected_article_tags = g_strdup(tag_text);
  app->selected_article_author_id = g_strdup(author_id);
  app->selected_article_edited_at = g_strdup(edited_at);
  gtk_label_set_text(
    GTK_LABEL(app->article_title_label),
    title != NULL ? title : "無題の記事"
  );
  gtk_label_set_text(GTK_LABEL(app->article_meta_label), meta);
  gtk_label_set_text(
    GTK_LABEL(app->article_tags_label),
    *tag_text != '\0' ? tag_text : "タグなし"
  );
  gtk_label_set_text(GTK_LABEL(app->article_social_label), social);
  pine_markdown_render(
    GTK_TEXT_VIEW(app->article_body_view),
    content,
    GTK_WINDOW(app->window),
    app->theme == PINE_THEME_DARK
  );
  gtk_widget_set_visible(
    app->article_edit_button,
    author_id != NULL && g_strcmp0(author_id, app->user_id) == 0
  );
  gtk_stack_set_visible_child_name(GTK_STACK(app->article_page_stack), "detail");
  g_free(social);
  g_free(meta);
  g_free(date);
  g_free(tag_text);

cleanup:
  if (root != NULL) {
    json_object_put(root);
  }
  pine_api_response_free(response);
}

static void load_article_detail(PineApp *app, const gchar *article_id) {
  if (article_id == NULL || *article_id == '\0' || app->article_loading) {
    return;
  }
  app->article_loading = TRUE;
  gtk_widget_set_sensitive(app->articles_list, FALSE);
  gtk_label_set_text(GTK_LABEL(app->article_title_label), "記事を読み込み中…");
  gtk_label_set_text(GTK_LABEL(app->article_meta_label), "");
  gtk_stack_set_visible_child_name(GTK_STACK(app->article_page_stack), "detail");
  gchar *escaped = g_uri_escape_string(article_id, NULL, FALSE);
  gchar *path = g_strdup_printf("/api/articles/%s", escaped);
  pine_api_request_async(
    app->api,
    "GET",
    path,
    NULL,
    NULL,
    article_detail_completed,
    app
  );
  g_free(path);
  g_free(escaped);
}

static void article_row_selected(
  GtkListBox *box,
  GtkListBoxRow *row,
  gpointer user_data
) {
  (void)box;
  PineApp *app = user_data;
  if (row == NULL) {
    return;
  }
  const gchar *id = g_object_get_data(G_OBJECT(row), "article-id");
  load_article_detail(app, id);
}

static void show_chat_page(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  PineApp *app = user_data;
  gtk_stack_set_visible_child_name(GTK_STACK(app->root_stack), "chat");
}

static void show_articles_page(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  PineApp *app = user_data;
  gtk_stack_set_visible_child_name(GTK_STACK(app->root_stack), "articles");
  if (!app->articles_loaded) {
    load_articles(app);
  }
}

static void refresh_articles_clicked(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  PineApp *app = user_data;
  app->articles_loaded = FALSE;
  load_articles(app);
}

static gchar *article_editor_content(PineApp *app) {
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(
    GTK_TEXT_VIEW(app->article_editor_content)
  );
  GtkTextIter start;
  GtkTextIter end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

static void article_editor_open(
  PineApp *app,
  gboolean editing
) {
  app->article_editing = editing;
  gtk_label_set_text(
    GTK_LABEL(app->article_editor_heading),
    editing ? "記事を編集" : "新しい記事"
  );
  gtk_entry_set_text(
    GTK_ENTRY(app->article_editor_title),
    editing && app->selected_article_title != NULL
      ? app->selected_article_title
      : ""
  );
  gtk_entry_set_text(
    GTK_ENTRY(app->article_editor_tags),
    editing && app->selected_article_tags != NULL
      ? app->selected_article_tags
      : ""
  );
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(
    GTK_TEXT_VIEW(app->article_editor_content)
  );
  gtk_text_buffer_set_text(
    buffer,
    editing && app->selected_article_content != NULL
      ? app->selected_article_content
      : "",
    -1
  );
  gtk_label_set_text(GTK_LABEL(app->article_editor_status), "");
  gtk_notebook_set_current_page(GTK_NOTEBOOK(app->article_editor_notebook), 0);
  gtk_stack_set_visible_child_name(GTK_STACK(app->article_page_stack), "editor");
  gtk_widget_grab_focus(app->article_editor_title);
}

static void article_new_clicked(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  article_editor_open(user_data, FALSE);
}

static void article_edit_clicked(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  article_editor_open(user_data, TRUE);
}

static void article_editor_cancel(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  PineApp *app = user_data;
  gtk_stack_set_visible_child_name(
    GTK_STACK(app->article_page_stack),
    app->selected_article_id != NULL ? "detail" : "empty"
  );
}

static void article_editor_preview(
  GtkNotebook *notebook,
  GtkWidget *page,
  guint page_number,
  gpointer user_data
) {
  (void)notebook;
  (void)page;
  if (page_number != 1) {
    return;
  }
  PineApp *app = user_data;
  gchar *content = article_editor_content(app);
  pine_markdown_render(
    GTK_TEXT_VIEW(app->article_editor_preview),
    content,
    GTK_WINDOW(app->window),
    app->theme == PINE_THEME_DARK
  );
  g_free(content);
}

static json_object *article_tags_array(const gchar *input) {
  json_object *tags = json_object_new_array();
  gchar **parts = g_strsplit(input != NULL ? input : "", ",", -1);
  for (gint i = 0; parts[i] != NULL && json_object_array_length(tags) < 10; i++) {
    gchar *tag = g_strdup(parts[i]);
    g_strstrip(tag);
    if (*tag != '\0') {
      if (g_utf8_strlen(tag, -1) > 40) {
        gchar *truncated = g_utf8_substring(tag, 0, 40);
        g_free(tag);
        tag = truncated;
      }
      json_object_array_add(tags, json_object_new_string(tag));
    }
    g_free(tag);
  }
  g_strfreev(parts);
  return tags;
}

static void article_saved(
  GObject *source,
  GAsyncResult *result,
  gpointer user_data
) {
  (void)source;
  PineApp *app = user_data;
  GError *error = NULL;
  PineApiResponse *response = pine_api_request_finish(result, &error);
  app->article_saving = FALSE;
  gtk_widget_set_sensitive(app->article_save_button, TRUE);
  if (error != NULL) {
    gtk_label_set_text(GTK_LABEL(app->article_editor_status), "記事を保存できませんでした");
    g_warning("記事保存: %s", error->message);
    g_error_free(error);
  } else if (response->status >= 200 && response->status < 300) {
    json_object *root = json_tokener_parse(response->body);
    json_object *article = NULL;
    const gchar *id = NULL;
    if (root != NULL && json_object_object_get_ex(root, "article", &article)) {
      id = object_string(article, "id");
    }
    if (id != NULL) {
      g_free(app->selected_article_id);
      app->selected_article_id = g_strdup(id);
    }
    if (root != NULL) {
      json_object_put(root);
    }
    app->articles_loaded = FALSE;
    load_articles(app);
    if (app->selected_article_id != NULL) {
      load_article_detail(app, app->selected_article_id);
    }
  } else {
    gchar *message = response_error_message(response);
    gtk_label_set_text(GTK_LABEL(app->article_editor_status), message);
    g_free(message);
  }
  pine_api_response_free(response);
}

static void article_save(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  PineApp *app = user_data;
  if (app->article_saving) {
    return;
  }
  gchar *title = g_strdup(
    gtk_entry_get_text(GTK_ENTRY(app->article_editor_title))
  );
  gchar *content = article_editor_content(app);
  const gchar *tags_text = gtk_entry_get_text(GTK_ENTRY(app->article_editor_tags));
  g_strstrip(title);
  g_strstrip(content);
  if (*title == '\0' || *content == '\0') {
    gtk_label_set_text(GTK_LABEL(app->article_editor_status), "タイトルと本文を入力してください");
    g_free(content);
    g_free(title);
    return;
  }
  if (g_utf8_strlen(title, -1) > 200 || g_utf8_strlen(content, -1) > 100000) {
    gtk_label_set_text(GTK_LABEL(app->article_editor_status), "記事のサイズ制限を超えています");
    g_free(content);
    g_free(title);
    return;
  }
  json_object *body = json_object_new_object();
  json_object_object_add(body, "title", json_object_new_string(title));
  json_object_object_add(body, "content", json_object_new_string(content));
  json_object_object_add(body, "tags", article_tags_array(tags_text));
  gchar *path = NULL;
  const gchar *method = "POST";
  if (app->article_editing && app->selected_article_id != NULL) {
    method = "PATCH";
    gchar *escaped = g_uri_escape_string(app->selected_article_id, NULL, FALSE);
    path = g_strdup_printf("/api/articles/%s", escaped);
    g_free(escaped);
    json_object_object_add(
      body,
      "expectedEditedAt",
      app->selected_article_edited_at != NULL
        ? json_object_new_string(app->selected_article_edited_at)
        : json_object_new_null()
    );
  } else {
    path = g_strdup("/api/articles");
  }
  app->article_saving = TRUE;
  gtk_widget_set_sensitive(app->article_save_button, FALSE);
  gtk_label_set_text(GTK_LABEL(app->article_editor_status), "保存しています…");
  pine_api_request_async(
    app->api,
    method,
    path,
    json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN),
    NULL,
    article_saved,
    app
  );
  json_object_put(body);
  g_free(path);
  g_free(content);
  g_free(title);
}

static void article_insert_snippet(GtkWidget *widget, gpointer user_data) {
  PineApp *app = user_data;
  const gchar *snippet = g_object_get_data(G_OBJECT(widget), "markdown-snippet");
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(
    GTK_TEXT_VIEW(app->article_editor_content)
  );
  gtk_text_buffer_insert_at_cursor(buffer, snippet != NULL ? snippet : "", -1);
  gtk_widget_grab_focus(app->article_editor_content);
}

static GtkWidget *build_articles_view(PineApp *app) {
  GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *sidebar_header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 9);
  GtkWidget *brand = gtk_label_new("Pine2 記事");
  GtkWidget *tabs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *chat_tab = gtk_button_new_with_label("チャット");
  GtkWidget *article_tab = gtk_button_new_with_label("記事");
  GtkWidget *boom_tab = gtk_button_new_with_label("Boom");
  GtkWidget *list_heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
  GtkWidget *list_title = gtk_label_new("記事一覧");
  GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
  GtkWidget *new_article = gtk_button_new_with_label("＋");
  GtkWidget *list_scroller = gtk_scrolled_window_new(NULL, NULL);
  GtkWidget *sidebar_footer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *settings = gtk_button_new_with_label("設定");
  GtkWidget *empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *empty_title = gtk_label_new("記事を選択してください");
  GtkWidget *empty_note = gtk_label_new("左の一覧から記事を開くか、新しい記事を作成できます。");

  GtkWidget *detail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *detail_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *detail_labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *detail_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
  app->article_title_label = gtk_label_new("記事を読み込み中…");
  app->article_meta_label = gtk_label_new("");
  app->article_tags_label = gtk_label_new("");
  app->article_social_label = gtk_label_new("");
  app->article_edit_button = gtk_button_new_with_label("編集");
  GtkWidget *detail_scroller = gtk_scrolled_window_new(NULL, NULL);
  app->article_body_view = gtk_text_view_new();

  GtkWidget *editor = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *editor_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  app->article_editor_heading = gtk_label_new("新しい記事");
  GtkWidget *editor_cancel = gtk_button_new_with_label("キャンセル");
  app->article_save_button = gtk_button_new_with_label("保存");
  GtkWidget *editor_form = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *editor_title_label = gtk_label_new("タイトル");
  GtkWidget *editor_tags_label = gtk_label_new("タグ（カンマ区切り・最大10件）");
  GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  GtkWidget *markdown_note = gtk_label_new("Markdown / GFM");
  app->article_editor_title = gtk_entry_new();
  app->article_editor_tags = gtk_entry_new();
  app->article_editor_notebook = gtk_notebook_new();
  GtkWidget *editor_scroller = gtk_scrolled_window_new(NULL, NULL);
  GtkWidget *preview_scroller = gtk_scrolled_window_new(NULL, NULL);
  app->article_editor_content = gtk_text_view_new();
  app->article_editor_preview = gtk_text_view_new();
  app->article_editor_status = gtk_label_new("");

  gtk_style_context_add_class(gtk_widget_get_style_context(paned), "article-shell");
  gtk_widget_set_size_request(sidebar, 300, -1);
  gtk_style_context_add_class(gtk_widget_get_style_context(sidebar), "article-sidebar");
  gtk_widget_set_margin_start(sidebar_header, 15);
  gtk_widget_set_margin_end(sidebar_header, 15);
  gtk_widget_set_margin_top(sidebar_header, 14);
  gtk_widget_set_margin_bottom(sidebar_header, 12);
  gtk_label_set_xalign(GTK_LABEL(brand), 0.0f);
  gtk_style_context_add_class(gtk_widget_get_style_context(brand), "brand-small");
  app->articles_status = gtk_label_new("まだ読み込まれていません");
  gtk_label_set_xalign(GTK_LABEL(app->articles_status), 0.0f);
  gtk_style_context_add_class(gtk_widget_get_style_context(app->articles_status), "article-list-status");
  gtk_box_pack_start(GTK_BOX(sidebar_header), brand, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(sidebar_header), app->articles_status, FALSE, FALSE, 0);
  gtk_box_set_homogeneous(GTK_BOX(tabs), TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(chat_tab), "tab-inactive");
  gtk_style_context_add_class(gtk_widget_get_style_context(article_tab), "tab-active");
  gtk_style_context_add_class(gtk_widget_get_style_context(boom_tab), "tab-inactive");
  g_object_set_data(G_OBJECT(boom_tab), "web-path", "/boom");
  gtk_box_pack_start(GTK_BOX(tabs), chat_tab, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(tabs), article_tab, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(tabs), boom_tab, TRUE, TRUE, 0);

  gtk_widget_set_margin_start(list_heading, 13);
  gtk_widget_set_margin_end(list_heading, 9);
  gtk_widget_set_margin_top(list_heading, 11);
  gtk_widget_set_margin_bottom(list_heading, 7);
  gtk_label_set_xalign(GTK_LABEL(list_title), 0.0f);
  gtk_style_context_add_class(gtk_widget_get_style_context(list_title), "section-title");
  gtk_style_context_add_class(gtk_widget_get_style_context(refresh), "header-button");
  gtk_style_context_add_class(gtk_widget_get_style_context(new_article), "create-room");
  gtk_box_pack_start(GTK_BOX(list_heading), list_title, TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(list_heading), new_article, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(list_heading), refresh, FALSE, FALSE, 0);
  app->articles_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(app->articles_list), GTK_SELECTION_SINGLE);
  gtk_container_add(GTK_CONTAINER(list_scroller), app->articles_list);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(list_scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(list_scroller), GTK_SHADOW_NONE);
  gtk_widget_set_margin_start(sidebar_footer, 14);
  gtk_widget_set_margin_end(sidebar_footer, 14);
  gtk_widget_set_margin_top(sidebar_footer, 10);
  gtk_widget_set_margin_bottom(sidebar_footer, 12);
  gtk_style_context_add_class(gtk_widget_get_style_context(settings), "settings-button");
  gtk_box_pack_start(GTK_BOX(sidebar_footer), settings, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(sidebar), sidebar_header, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(sidebar), tabs, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(sidebar), list_heading, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(sidebar), list_scroller, TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(sidebar), sidebar_footer, FALSE, FALSE, 0);

  app->article_page_stack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(app->article_page_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_widget_set_halign(empty, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(empty, GTK_ALIGN_CENTER);
  gtk_style_context_add_class(gtk_widget_get_style_context(empty_title), "article-empty-title");
  gtk_style_context_add_class(gtk_widget_get_style_context(empty_note), "article-empty-note");
  gtk_box_pack_start(GTK_BOX(empty), empty_title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(empty), empty_note, FALSE, FALSE, 0);

  gtk_style_context_add_class(gtk_widget_get_style_context(detail_header), "article-header");
  gtk_label_set_xalign(GTK_LABEL(app->article_title_label), 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(app->article_title_label), TRUE);
  gtk_label_set_xalign(GTK_LABEL(app->article_meta_label), 0.0f);
  gtk_label_set_xalign(GTK_LABEL(app->article_tags_label), 0.0f);
  gtk_label_set_xalign(GTK_LABEL(app->article_social_label), 0.0f);
  gtk_style_context_add_class(gtk_widget_get_style_context(app->article_title_label), "article-title");
  gtk_style_context_add_class(gtk_widget_get_style_context(app->article_meta_label), "article-meta");
  gtk_style_context_add_class(gtk_widget_get_style_context(app->article_tags_label), "article-tags");
  gtk_style_context_add_class(gtk_widget_get_style_context(app->article_social_label), "article-social");
  gtk_box_pack_start(GTK_BOX(detail_labels), app->article_title_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(detail_labels), app->article_meta_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(detail_labels), app->article_tags_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(detail_labels), app->article_social_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(detail_actions), app->article_edit_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(detail_header), detail_labels, TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(detail_header), detail_actions, FALSE, FALSE, 0);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(app->article_body_view), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(app->article_body_view), FALSE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->article_body_view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(app->article_body_view), 28);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(app->article_body_view), 28);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(app->article_body_view), 22);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(app->article_body_view), 28);
  gtk_style_context_add_class(gtk_widget_get_style_context(app->article_body_view), "article-body");
  gtk_container_add(GTK_CONTAINER(detail_scroller), app->article_body_view);
  gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(detail_scroller), GTK_SHADOW_NONE);
  gtk_box_pack_start(GTK_BOX(detail), detail_header, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(detail), detail_scroller, TRUE, TRUE, 0);

  gtk_style_context_add_class(gtk_widget_get_style_context(editor_header), "article-header");
  gtk_label_set_xalign(GTK_LABEL(app->article_editor_heading), 0.0f);
  gtk_style_context_add_class(gtk_widget_get_style_context(app->article_editor_heading), "article-title");
  gtk_style_context_add_class(gtk_widget_get_style_context(app->article_save_button), "suggested-action");
  gtk_box_pack_start(GTK_BOX(editor_header), app->article_editor_heading, TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(editor_header), app->article_save_button, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(editor_header), editor_cancel, FALSE, FALSE, 0);
  gtk_widget_set_margin_start(editor_form, 18);
  gtk_widget_set_margin_end(editor_form, 18);
  gtk_widget_set_margin_top(editor_form, 14);
  gtk_widget_set_margin_bottom(editor_form, 14);
  gtk_label_set_xalign(GTK_LABEL(editor_title_label), 0.0f);
  gtk_label_set_xalign(GTK_LABEL(editor_tags_label), 0.0f);
  gtk_style_context_add_class(gtk_widget_get_style_context(editor_title_label), "settings-field-label");
  gtk_style_context_add_class(gtk_widget_get_style_context(editor_tags_label), "settings-field-label");
  gtk_entry_set_max_length(GTK_ENTRY(app->article_editor_title), 200);
  gtk_entry_set_placeholder_text(GTK_ENTRY(app->article_editor_tags), "linux, GTK3, Pine2");
  gtk_style_context_add_class(gtk_widget_get_style_context(markdown_note), "article-markdown-note");
  const gchar *toolbar_labels[] = {"見出し", "太字", "リンク", "コード", "Mermaid", NULL};
  const gchar *toolbar_snippets[] = {"# 見出し", "**太字**", "[表示名](https://)", "```\nコード\n```", "```mermaid\nflowchart TD\n  A[開始] --> B[完了]\n```", NULL};
  gtk_box_pack_start(GTK_BOX(toolbar), markdown_note, FALSE, FALSE, 4);
  for (gint i = 0; toolbar_labels[i] != NULL; i++) {
    GtkWidget *button = gtk_button_new_with_label(toolbar_labels[i]);
    g_object_set_data_full(
      G_OBJECT(button),
      "markdown-snippet",
      g_strdup(toolbar_snippets[i]),
      g_free
    );
    g_signal_connect(button, "clicked", G_CALLBACK(article_insert_snippet), app);
    gtk_box_pack_start(GTK_BOX(toolbar), button, FALSE, FALSE, 0);
  }
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->article_editor_content), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(app->article_editor_content), 12);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(app->article_editor_content), 12);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(app->article_editor_content), 10);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(app->article_editor_content), 10);
  gtk_style_context_add_class(gtk_widget_get_style_context(app->article_editor_content), "article-editor-text");
  gtk_container_add(GTK_CONTAINER(editor_scroller), app->article_editor_content);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(app->article_editor_preview), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(app->article_editor_preview), FALSE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->article_editor_preview), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(app->article_editor_preview), 22);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(app->article_editor_preview), 22);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(app->article_editor_preview), 18);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(app->article_editor_preview), 22);
  gtk_style_context_add_class(gtk_widget_get_style_context(app->article_editor_preview), "article-body");
  gtk_container_add(GTK_CONTAINER(preview_scroller), app->article_editor_preview);
  gtk_notebook_append_page(GTK_NOTEBOOK(app->article_editor_notebook), editor_scroller, gtk_label_new("編集"));
  gtk_notebook_append_page(GTK_NOTEBOOK(app->article_editor_notebook), preview_scroller, gtk_label_new("プレビュー"));
  gtk_widget_set_size_request(app->article_editor_notebook, -1, 300);
  gtk_label_set_xalign(GTK_LABEL(app->article_editor_status), 0.0f);
  gtk_style_context_add_class(gtk_widget_get_style_context(app->article_editor_status), "settings-status");
  gtk_box_pack_start(GTK_BOX(editor_form), editor_title_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(editor_form), app->article_editor_title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(editor_form), editor_tags_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(editor_form), app->article_editor_tags, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(editor_form), toolbar, FALSE, FALSE, 4);
  gtk_box_pack_start(GTK_BOX(editor_form), app->article_editor_notebook, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(editor_form), app->article_editor_status, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(editor), editor_header, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(editor), editor_form, TRUE, TRUE, 0);

  gtk_stack_add_named(GTK_STACK(app->article_page_stack), empty, "empty");
  gtk_stack_add_named(GTK_STACK(app->article_page_stack), detail, "detail");
  gtk_stack_add_named(GTK_STACK(app->article_page_stack), editor, "editor");
  gtk_stack_set_visible_child_name(GTK_STACK(app->article_page_stack), "empty");
  gtk_paned_pack1(GTK_PANED(paned), sidebar, FALSE, FALSE);
  gtk_paned_pack2(GTK_PANED(paned), app->article_page_stack, TRUE, FALSE);
  gtk_paned_set_position(GTK_PANED(paned), 300);

  g_signal_connect(app->articles_list, "row-selected", G_CALLBACK(article_row_selected), app);
  g_signal_connect(chat_tab, "clicked", G_CALLBACK(show_chat_page), app);
  g_signal_connect(boom_tab, "clicked", G_CALLBACK(open_web_section), app);
  g_signal_connect(refresh, "clicked", G_CALLBACK(refresh_articles_clicked), app);
  g_signal_connect(new_article, "clicked", G_CALLBACK(article_new_clicked), app);
  g_signal_connect(settings, "clicked", G_CALLBACK(show_settings_dialog), app);
  g_signal_connect(app->article_edit_button, "clicked", G_CALLBACK(article_edit_clicked), app);
  g_signal_connect(editor_cancel, "clicked", G_CALLBACK(article_editor_cancel), app);
  g_signal_connect(app->article_save_button, "clicked", G_CALLBACK(article_save), app);
  g_signal_connect(app->article_editor_notebook, "switch-page", G_CALLBACK(article_editor_preview), app);
  return paned;
}

static void toggle_sidebar(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  PineApp *app = user_data;
  gtk_widget_set_visible(app->sidebar, !gtk_widget_get_visible(app->sidebar));
}

static GtkWidget *build_login_view(PineApp *app) {
  GtkWidget *center = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget *username_label = gtk_label_new("ユーザー名");
  GtkWidget *username_help = gtk_label_new(
    "3〜50文字。このユーザー名がチャットでの表示名になります。"
  );
  GtkWidget *password_label = gtk_label_new("パスワード");
  GtkWidget *password_help = gtk_label_new("6文字以上で入力してください。");
  GtkWidget *existing_note = gtk_label_new("※Pine2の既存アカウントをそのまま利用できます。");
  GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

  gtk_widget_set_halign(center, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(center, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(card, 430, -1);
  gtk_widget_set_margin_start(card, 36);
  gtk_widget_set_margin_end(card, 36);
  gtk_widget_set_margin_top(card, 32);
  gtk_widget_set_margin_bottom(card, 32);
  gtk_style_context_add_class(gtk_widget_get_style_context(card), "login-card");

  app->login_title = gtk_label_new("Kan Accountでログイン");
  app->login_subtitle = gtk_label_new("アカウントでログインしてください");
  gtk_style_context_add_class(gtk_widget_get_style_context(app->login_title), "login-title");
  gtk_style_context_add_class(gtk_widget_get_style_context(app->login_subtitle), "login-subtitle");
  gtk_widget_set_margin_bottom(app->login_subtitle, 14);
  gtk_label_set_xalign(GTK_LABEL(username_label), 0.0f);
  gtk_label_set_xalign(GTK_LABEL(password_label), 0.0f);
  gtk_label_set_xalign(GTK_LABEL(username_help), 0.0f);
  gtk_label_set_xalign(GTK_LABEL(password_help), 0.0f);
  gtk_style_context_add_class(gtk_widget_get_style_context(username_label), "field-label");
  gtk_style_context_add_class(gtk_widget_get_style_context(password_label), "field-label");
  gtk_style_context_add_class(gtk_widget_get_style_context(username_help), "field-help");
  gtk_style_context_add_class(gtk_widget_get_style_context(password_help), "field-help");
  gtk_style_context_add_class(gtk_widget_get_style_context(existing_note), "login-note");

  app->username_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(app->username_entry), "ユーザー名を入力");
  gtk_entry_set_input_purpose(GTK_ENTRY(app->username_entry), GTK_INPUT_PURPOSE_NAME);
  app->password_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(app->password_entry), "パスワードを入力");
  gtk_entry_set_visibility(GTK_ENTRY(app->password_entry), FALSE);
  gtk_entry_set_activates_default(GTK_ENTRY(app->password_entry), TRUE);
  app->login_button = gtk_button_new_with_label("ログイン");
  gtk_style_context_add_class(gtk_widget_get_style_context(app->login_button), "suggested-action");
  app->auth_toggle_button = gtk_button_new_with_label(
    "まだアカウントをお持ちでない方はこちら"
  );
  gtk_style_context_add_class(
    gtk_widget_get_style_context(app->auth_toggle_button),
    "auth-toggle"
  );
  app->login_spinner = gtk_spinner_new();
  app->login_status = gtk_label_new("");
  gtk_label_set_line_wrap(GTK_LABEL(app->login_status), TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(app->login_status), "status");

  gtk_box_pack_start(GTK_BOX(button_box), app->login_spinner, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(button_box), app->login_button, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(card), app->login_title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(card), app->login_subtitle, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(card), username_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(card), app->username_entry, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(card), username_help, FALSE, FALSE, 0);
  gtk_widget_set_margin_top(password_label, 7);
  gtk_box_pack_start(GTK_BOX(card), password_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(card), app->password_entry, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(card), password_help, FALSE, FALSE, 0);
  gtk_widget_set_margin_top(button_box, 10);
  gtk_box_pack_start(GTK_BOX(card), button_box, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(card), app->login_status, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(card), app->auth_toggle_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(card), existing_note, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(center), card, FALSE, FALSE, 0);

  g_signal_connect(app->login_button, "clicked", G_CALLBACK(begin_login), app);
  g_signal_connect(app->password_entry, "activate", G_CALLBACK(begin_login), app);
  g_signal_connect(app->auth_toggle_button, "clicked", G_CALLBACK(toggle_auth_mode), app);
  return center;
}

static GtkWidget *build_chat_view(PineApp *app) {
  GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  GtkWidget *sidebar_header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
  GtkWidget *brand_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *brand = gtk_label_new("Pine2");
  GtkWidget *logout = gtk_button_new_with_label("ログアウト");
  GtkWidget *user_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *tabs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *chat_tab = gtk_button_new_with_label("チャット");
  GtkWidget *articles_tab = gtk_button_new_with_label("記事");
  GtkWidget *boom_tab = gtk_button_new_with_label("Boom");
  GtkWidget *room_area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *room_heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *sidebar_title = gtk_label_new("ルーム一覧");
  GtkWidget *create_room = gtk_button_new_with_label("＋");
  GtkWidget *rooms_scroller = gtk_scrolled_window_new(NULL, NULL);
  GtkWidget *sidebar_footer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *settings = gtk_button_new_with_label("設定");
  GtkWidget *chat = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *chat_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *menu = gtk_button_new_with_label("☰");
  GtkWidget *room_labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
  GtkWidget *header_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
  GtkWidget *composer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *composer_scroller = gtk_scrolled_window_new(NULL, NULL);

  gtk_style_context_add_class(gtk_widget_get_style_context(paned), "app-shell");
  app->sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request(app->sidebar, 286, -1);
  gtk_style_context_add_class(gtk_widget_get_style_context(app->sidebar), "sidebar");

  gtk_widget_set_margin_start(sidebar_header, 16);
  gtk_widget_set_margin_end(sidebar_header, 16);
  gtk_widget_set_margin_top(sidebar_header, 14);
  gtk_widget_set_margin_bottom(sidebar_header, 11);
  gtk_style_context_add_class(gtk_widget_get_style_context(sidebar_header), "sidebar-header");
  gtk_label_set_xalign(GTK_LABEL(brand), 0.0f);
  gtk_style_context_add_class(gtk_widget_get_style_context(brand), "brand-small");
  gtk_style_context_add_class(gtk_widget_get_style_context(logout), "logout-button");
  app->connection_label = gtk_label_new("● 接続中…");
  gtk_label_set_xalign(GTK_LABEL(app->connection_label), 1.0f);
  gtk_style_context_add_class(gtk_widget_get_style_context(app->connection_label), "connection-connecting");
  gtk_box_pack_start(GTK_BOX(brand_row), brand, TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(brand_row), logout, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(brand_row), app->connection_label, FALSE, FALSE, 0);

  app->user_avatar = avatar_widget_new(
    app,
    NULL,
    "?",
    26,
    "sidebar-avatar",
    &app->user_avatar_fallback
  );
  app->user_label = gtk_label_new("ユーザー");
  gtk_style_context_add_class(gtk_widget_get_style_context(app->user_label), "sidebar-user");
  gtk_label_set_xalign(GTK_LABEL(app->user_label), 0.0f);
  gtk_box_pack_start(GTK_BOX(user_row), app->user_avatar, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(user_row), app->user_label, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(sidebar_header), brand_row, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(sidebar_header), user_row, FALSE, FALSE, 0);

  gtk_box_set_homogeneous(GTK_BOX(tabs), TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(chat_tab), "tab-active");
  gtk_style_context_add_class(gtk_widget_get_style_context(articles_tab), "tab-inactive");
  gtk_style_context_add_class(gtk_widget_get_style_context(boom_tab), "tab-inactive");
  g_object_set_data(G_OBJECT(boom_tab), "web-path", "/boom");
  gtk_box_pack_start(GTK_BOX(tabs), chat_tab, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(tabs), articles_tab, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(tabs), boom_tab, TRUE, TRUE, 0);

  gtk_style_context_add_class(gtk_widget_get_style_context(room_area), "room-area");
  gtk_widget_set_margin_start(room_heading, 16);
  gtk_widget_set_margin_end(room_heading, 16);
  gtk_widget_set_margin_top(room_heading, 16);
  gtk_widget_set_margin_bottom(room_heading, 12);
  gtk_label_set_xalign(GTK_LABEL(sidebar_title), 0.0f);
  gtk_style_context_add_class(gtk_widget_get_style_context(sidebar_title), "section-title");
  gtk_style_context_add_class(gtk_widget_get_style_context(create_room), "create-room");
  gtk_box_pack_start(GTK_BOX(room_heading), sidebar_title, TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(room_heading), create_room, FALSE, FALSE, 0);
  app->rooms_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(app->rooms_list), GTK_SELECTION_SINGLE);
  gtk_container_add(GTK_CONTAINER(rooms_scroller), app->rooms_list);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(rooms_scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(rooms_scroller), GTK_SHADOW_NONE);
  gtk_box_pack_start(GTK_BOX(room_area), room_heading, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(room_area), rooms_scroller, TRUE, TRUE, 0);

  gtk_widget_set_margin_start(sidebar_footer, 16);
  gtk_widget_set_margin_end(sidebar_footer, 16);
  gtk_widget_set_margin_top(sidebar_footer, 12);
  gtk_widget_set_margin_bottom(sidebar_footer, 14);
  gtk_style_context_add_class(gtk_widget_get_style_context(sidebar_footer), "sidebar-footer");
  gtk_style_context_add_class(gtk_widget_get_style_context(settings), "settings-button");
  gtk_box_pack_start(GTK_BOX(sidebar_footer), settings, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(app->sidebar), sidebar_header, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(app->sidebar), tabs, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(app->sidebar), room_area, TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(app->sidebar), sidebar_footer, FALSE, FALSE, 0);

  gtk_style_context_add_class(gtk_widget_get_style_context(chat), "chat-shell");
  gtk_style_context_add_class(gtk_widget_get_style_context(chat_header), "chat-header");
  gtk_style_context_add_class(gtk_widget_get_style_context(menu), "menu-button");
  app->room_title = gtk_label_new("ルームを選択してください");
  app->room_subtitle = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(app->room_title), 0.0f);
  gtk_label_set_xalign(GTK_LABEL(app->room_subtitle), 0.0f);
  gtk_style_context_add_class(gtk_widget_get_style_context(app->room_title), "room-title");
  gtk_style_context_add_class(gtk_widget_get_style_context(app->room_subtitle), "room-subtitle");
  gtk_box_pack_start(GTK_BOX(room_labels), app->room_title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(room_labels), app->room_subtitle, FALSE, FALSE, 0);
  app->invite_button = gtk_button_new_with_label("招待");
  gtk_widget_set_sensitive(app->invite_button, FALSE);
  gtk_style_context_add_class(gtk_widget_get_style_context(app->invite_button), "invite-button");
  gtk_style_context_add_class(gtk_widget_get_style_context(refresh), "header-button");
  gtk_box_pack_start(GTK_BOX(header_actions), app->invite_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(header_actions), refresh, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(chat_header), menu, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(chat_header), room_labels, TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(chat_header), header_actions, FALSE, FALSE, 0);

  app->messages_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  app->messages_scroller = gtk_scrolled_window_new(NULL, NULL);
  gtk_style_context_add_class(gtk_widget_get_style_context(app->messages_scroller), "messages-area");
  gtk_style_context_add_class(gtk_widget_get_style_context(app->messages_list), "messages-list");
  gtk_container_add(GTK_CONTAINER(app->messages_scroller), app->messages_list);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(app->messages_scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(app->messages_scroller), GTK_SHADOW_NONE);

  gtk_style_context_add_class(gtk_widget_get_style_context(composer), "composer");
  app->composer_entry = gtk_text_view_new();
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->composer_entry), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(app->composer_entry), FALSE);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(app->composer_entry), 10);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(app->composer_entry), 10);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(app->composer_entry), 9);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(app->composer_entry), 9);
  gtk_widget_set_tooltip_text(app->composer_entry, "メッセージを入力…（Shift+Enterで改行）");
  gtk_widget_set_size_request(composer_scroller, -1, 50);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(composer_scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(composer_scroller), GTK_SHADOW_NONE);
  gtk_style_context_add_class(gtk_widget_get_style_context(composer_scroller), "composer-input");
  gtk_container_add(GTK_CONTAINER(composer_scroller), app->composer_entry);
  gtk_widget_set_sensitive(app->composer_entry, FALSE);
  app->send_button = gtk_button_new_with_label("送信");
  gtk_widget_set_sensitive(app->send_button, FALSE);
  gtk_style_context_add_class(gtk_widget_get_style_context(app->send_button), "suggested-action");
  gtk_box_pack_start(GTK_BOX(composer), composer_scroller, TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(composer), app->send_button, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(chat), chat_header, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(chat), app->messages_scroller, TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(chat), composer, FALSE, FALSE, 0);
  gtk_paned_pack1(GTK_PANED(paned), app->sidebar, FALSE, FALSE);
  gtk_paned_pack2(GTK_PANED(paned), chat, TRUE, FALSE);
  gtk_paned_set_position(GTK_PANED(paned), 286);

  g_signal_connect(app->rooms_list, "row-selected", G_CALLBACK(room_selected), app);
  g_signal_connect(app->send_button, "clicked", G_CALLBACK(send_message), app);
  g_signal_connect(app->composer_entry, "key-press-event", G_CALLBACK(composer_key_press), app);
  g_signal_connect_swapped(refresh, "clicked", G_CALLBACK(refresh_messages), app);
  g_signal_connect(create_room, "clicked", G_CALLBACK(show_create_room_dialog), app);
  g_signal_connect(app->invite_button, "clicked", G_CALLBACK(copy_invite_link), app);
  g_signal_connect(articles_tab, "clicked", G_CALLBACK(show_articles_page), app);
  g_signal_connect(boom_tab, "clicked", G_CALLBACK(open_web_section), app);
  g_signal_connect(settings, "clicked", G_CALLBACK(show_settings_dialog), app);
  g_signal_connect(menu, "clicked", G_CALLBACK(toggle_sidebar), app);
  g_signal_connect(logout, "clicked", G_CALLBACK(begin_logout), app);
  return paned;
}

static void apply_theme(PineApp *app) {
  static const gchar *light_palette =
    "@define-color pine #f97316;"
    "@define-color pine_hover #ea580c;"
    "@define-color pine_soft #ffedd5;"
    "@define-color pine_on #2b1505;"
    "@define-color window_bg #eef0f2;"
    "@define-color surface #ffffff;"
    "@define-color surface_alt #f7f7f8;"
    "@define-color sidebar_bg #f3f3f4;"
    "@define-color text_color #27272a;"
    "@define-color muted_color #71717a;"
    "@define-color border_color #d4d4d8;"
    "@define-color border_soft #e5e7eb;"
    "@define-color hover_bg #e8e8ea;"
    "@define-color selected_bg #ffedd5;"
    "@define-color input_bg #ffffff;"
    "@define-color other_message #ffffff;"
    "@define-color chrome_bg #f4f4f5;"
    "@define-color avatar_bg #737373;"
    "@define-color chrome_top #fbfbfc;"
    "@define-color chrome_bottom #ececef;"
    "@define-color button_top #ffffff;"
    "@define-color button_bottom #ececef;"
    "@define-color button_hover_top #ffffff;"
    "@define-color button_hover_bottom #e2e2e5;"
    "@define-color selected_top #fff4eb;"
    "@define-color selected_bottom #ffe2c7;"
    "@define-color input_bottom #fafafa;"
    "@define-color pine_top #fb923c;"
    "@define-color pine_bottom #f97316;"
    "@define-color pine_hover_top #fdba74;"
    "@define-color pine_hover_bottom #ea580c;"
    "@define-color highlight_color rgba(255,255,255,.88);"
    "@define-color shadow_color rgba(0,0,0,.18);"
    "@define-color inset_shadow rgba(0,0,0,.12);"
    "@define-color own_meta rgba(43,21,5,.68);"
    "@define-color selection_bg #fed7aa;";
  static const gchar *dark_palette =
    "@define-color pine #fb923c;"
    "@define-color pine_hover #f97316;"
    "@define-color pine_soft #4a2b18;"
    "@define-color pine_on #211205;"
    "@define-color window_bg #18181a;"
    "@define-color surface #242426;"
    "@define-color surface_alt #202022;"
    "@define-color sidebar_bg #202022;"
    "@define-color text_color #f0f0f1;"
    "@define-color muted_color #a1a1aa;"
    "@define-color border_color #414145;"
    "@define-color border_soft #323236;"
    "@define-color hover_bg #303034;"
    "@define-color selected_bg #4a2b18;"
    "@define-color input_bg #2a2a2d;"
    "@define-color other_message #29292c;"
    "@define-color chrome_bg #212124;"
    "@define-color avatar_bg #626267;"
    "@define-color chrome_top #2c2c30;"
    "@define-color chrome_bottom #202023;"
    "@define-color button_top #37373b;"
    "@define-color button_bottom #29292c;"
    "@define-color button_hover_top #414146;"
    "@define-color button_hover_bottom #313135;"
    "@define-color selected_top #573720;"
    "@define-color selected_bottom #432817;"
    "@define-color input_bottom #262629;"
    "@define-color pine_top #fdad6a;"
    "@define-color pine_bottom #f97316;"
    "@define-color pine_hover_top #fdba74;"
    "@define-color pine_hover_bottom #ea580c;"
    "@define-color highlight_color rgba(255,255,255,.11);"
    "@define-color shadow_color rgba(0,0,0,.55);"
    "@define-color inset_shadow rgba(0,0,0,.46);"
    "@define-color own_meta rgba(33,18,5,.72);"
    "@define-color selection_bg #7c3f18;";
  static const gchar *css_base =
    "window, dialog { background: @window_bg; color: @text_color; font-family: \"Noto Sans CJK JP\", \"Noto Sans JP\", Sans; }"
    "tooltip { background: @surface; color: @text_color; border: 1px solid @border_color; }"
    "headerbar.pine-titlebar, .pine-titlebar.titlebar { background-image: linear-gradient(to bottom, @chrome_top, @chrome_bottom); background-color: @chrome_bg; color: @text_color; border: 0; border-bottom: 1px solid @border_color; box-shadow: inset 0 1px @highlight_color, 0 1px 2px @shadow_color; min-height: 36px; }"
    "headerbar.pine-titlebar .title { color: @text_color; font-weight: 700; }"
    "headerbar.pine-titlebar button.titlebutton { background-image: none; background-color: transparent; color: @muted_color; border: 0; border-radius: 4px; box-shadow: none; outline-style: none; padding: 5px; }"
    "headerbar.pine-titlebar button.titlebutton:hover { background-image: linear-gradient(to bottom, @button_hover_top, @button_hover_bottom); background-color: @hover_bg; color: @text_color; box-shadow: inset 0 1px @highlight_color; }"
    "button { background-image: linear-gradient(to bottom, @button_top, @button_bottom); background-color: @surface_alt; color: @text_color; border: 1px solid @border_color; border-radius: 5px; padding: 6px 10px; outline-style: none; box-shadow: inset 0 1px @highlight_color, 0 1px 1px @shadow_color; }"
    "button:hover { background-image: linear-gradient(to bottom, @button_hover_top, @button_hover_bottom); background-color: @hover_bg; }"
    "button:active { background-image: linear-gradient(to bottom, @button_bottom, @button_top); background-color: @selected_bg; box-shadow: inset 0 1px 2px @inset_shadow; }"
    "button:focus { outline-style: none; border-color: @pine; box-shadow: inset 0 1px @highlight_color, 0 0 0 1px @pine; }"
    "button:disabled { color: @muted_color; background-image: linear-gradient(to bottom, @surface_alt, @button_bottom); background-color: @surface_alt; box-shadow: none; opacity: .65; }"
    "button.suggested-action { background-image: linear-gradient(to bottom, @pine_top, @pine_bottom); background-color: @pine; color: @pine_on; border-color: @pine_bottom; font-weight: 700; padding: 8px 20px; box-shadow: inset 0 1px rgba(255,255,255,.38), 0 1px 2px @shadow_color; }"
    "button.suggested-action:hover { background-image: linear-gradient(to bottom, @pine_hover_top, @pine_hover_bottom); background-color: @pine_hover; border-color: @pine_hover_bottom; }"
    "button.suggested-action:active { background-image: linear-gradient(to bottom, @pine_bottom, @pine_top); box-shadow: inset 0 1px 2px rgba(80,30,0,.38); }"
    "entry { background-image: linear-gradient(to bottom, @input_bg, @input_bottom); background-color: @input_bg; color: @text_color; border: 1px solid @border_color; border-radius: 5px; padding: 9px 11px; caret-color: @text_color; box-shadow: inset 0 1px 2px @inset_shadow; }"
    "entry:focus { border-color: @pine; box-shadow: 0 0 0 1px @pine; }"
    "entry selection, textview text selection { background-color: @selection_bg; color: @text_color; }"
    "scrollbar { background: transparent; }"
    "scrollbar slider { background: @border_color; border: 3px solid transparent; border-radius: 8px; min-width: 5px; min-height: 32px; }"
    "scrollbar slider:hover { background: @muted_color; }";
  static const gchar *css_login =
    ".login-card { background-image: linear-gradient(to bottom, @surface, @surface_alt); background-color: @surface; border: 1px solid @border_color; border-radius: 7px; box-shadow: inset 0 1px @highlight_color, 0 8px 24px rgba(0,0,0,.14); padding: 28px; }"
    ".login-title { color: @pine; font-size: 26px; font-weight: 800; }"
    ".login-subtitle { color: @muted_color; font-size: 13px; }"
    ".field-label { color: @text_color; font-size: 13px; font-weight: 700; }"
    ".field-help { color: @muted_color; font-size: 11px; }"
    ".status { color: #ef4444; font-size: 12px; }"
    ".login-note { color: @muted_color; font-size: 12px; }"
    "button.auth-toggle { background: transparent; color: @pine; border-color: transparent; font-size: 12px; }"
    "button.auth-toggle:hover { color: @pine_hover; background: transparent; }";
  static const gchar *css_navigation =
    ".app-shell { background: @window_bg; }"
    "paned > separator { background-image: none; background-color: @border_color; border: 0; box-shadow: none; min-width: 1px; min-height: 1px; }"
    "scrolledwindow, scrolledwindow viewport { border: 0; box-shadow: none; outline-style: none; }"
    ".sidebar, .sidebar-header, .sidebar-footer { background: @sidebar_bg; color: @text_color; }"
    ".sidebar-header { background-image: linear-gradient(to bottom, @chrome_top, @sidebar_bg); border-bottom: 1px solid @border_soft; box-shadow: inset 0 1px @highlight_color; }"
    ".brand-small { color: @pine; font-size: 19px; font-weight: 800; }"
    ".sidebar-user { color: @text_color; font-size: 12px; }"
    ".sidebar-avatar { background-image: linear-gradient(to bottom, @pine_top, @pine_bottom); background-color: @pine; color: @pine_on; border: 1px solid @pine_bottom; border-radius: 13px; box-shadow: inset 0 1px rgba(255,255,255,.35); font-size: 11px; font-weight: 800; }"
    "button.logout-button { background: transparent; color: @muted_color; border-color: transparent; padding: 3px 5px; font-size: 10px; }"
    "button.logout-button:hover { background: @hover_bg; color: @text_color; }"
    ".connection-online { color: #22c55e; font-size: 11px; }"
    ".connection-connecting { color: @pine; font-size: 11px; }"
    ".connection-offline { color: #ef4444; font-size: 11px; }"
    "button.tab-active, button.tab-inactive { background-image: none; border-radius: 0; border: 0; border-bottom: 2px solid transparent; padding: 9px 3px 8px; font-size: 12px; box-shadow: none; }"
    "button.tab-active { background-image: linear-gradient(to bottom, @surface, @surface_alt); background-color: @surface; color: @pine; border-bottom-color: @pine; box-shadow: inset 0 1px @highlight_color; font-weight: 700; }"
    "button.tab-inactive { background: @sidebar_bg; color: @muted_color; }"
    "button.tab-inactive:hover { color: @text_color; background: @hover_bg; }"
    ".room-area, .room-area viewport, .room-area list { background: @sidebar_bg; color: @text_color; }"
    ".section-title { color: @text_color; font-size: 13px; font-weight: 700; }"
    "button.create-room { background: transparent; color: @pine; border-color: transparent; border-radius: 4px; padding: 1px 7px; font-size: 17px; font-weight: 800; }"
    "button.create-room:hover { background: @pine_soft; color: @pine; }"
    ".room-area row { background: transparent; color: @text_color; border-left: 3px solid transparent; border-radius: 4px; margin: 2px 8px; }"
    ".room-area row:hover { background-image: linear-gradient(to bottom, @button_hover_top, @button_hover_bottom); background-color: @hover_bg; box-shadow: inset 0 1px @highlight_color; }"
    ".room-area row:selected { background-image: linear-gradient(to bottom, @selected_top, @selected_bottom); background-color: @selected_bg; color: @text_color; border-left-color: @pine; box-shadow: inset 0 1px @highlight_color, 0 1px 1px @shadow_color; }"
    ".room-name { color: @text_color; font-weight: 700; }"
    ".room-date { color: @muted_color; font-size: 10px; }"
    ".unread-badge { background-image: linear-gradient(to bottom, @pine_top, @pine_bottom); background-color: @pine; color: @pine_on; border: 1px solid @pine_bottom; border-radius: 10px; box-shadow: inset 0 1px rgba(255,255,255,.32); padding: 2px 7px; font-weight: 800; }"
    ".sidebar-footer { border-top: 1px solid @border_soft; }"
    "button.settings-button { background-image: linear-gradient(to bottom, @button_top, @button_bottom); background-color: @surface_alt; color: @text_color; border-color: @border_color; font-weight: 600; }";
  static const gchar *css_chat =
    ".chat-shell { background: @window_bg; color: @text_color; }"
    ".chat-header { background-image: linear-gradient(to bottom, @surface, @surface_alt); background-color: @surface; color: @text_color; border-bottom: 1px solid @border_color; box-shadow: inset 0 1px @highlight_color, 0 1px 2px @shadow_color; padding: 10px 14px; }"
    ".room-title { color: @text_color; font-size: 18px; font-weight: 700; }"
    ".room-subtitle { color: @muted_color; font-size: 11px; }"
    "button.menu-button, button.header-button { background: transparent; color: @pine; border-color: transparent; box-shadow: none; outline-style: none; padding: 5px; }"
    "button.menu-button:hover, button.header-button:hover { background: @pine_soft; }"
    "button.menu-button:focus, button.header-button:focus { background: @pine_soft; box-shadow: none; }"
    "button.invite-button { background-image: linear-gradient(to bottom, @button_top, @button_bottom); background-color: @surface; color: @pine; border-color: @pine; padding: 5px 10px; }"
    "button.invite-button:hover { background-image: linear-gradient(to bottom, @selected_top, @selected_bottom); background-color: @pine_soft; }"
    ".messages-area, .messages-area viewport, .messages-list { background: @window_bg; color: @text_color; }"
    ".messages-list { padding: 12px; }"
    ".message-own, .message-other { border-radius: 7px; padding: 8px 11px; }"
    ".message-own { background-image: linear-gradient(to bottom, @pine_top, @pine_bottom); background-color: @pine; color: @pine_on; border: 1px solid @pine_bottom; box-shadow: inset 0 1px rgba(255,255,255,.30), 0 1px 2px @shadow_color; }"
    ".message-other { background-image: linear-gradient(to bottom, @surface, @other_message); background-color: @other_message; color: @text_color; border: 1px solid @border_soft; box-shadow: inset 0 1px @highlight_color, 0 1px 2px @shadow_color; }"
    ".message-sender { color: @muted_color; font-size: 10px; }"
    ".message-time-own, .read-state { color: @own_meta; font-size: 9px; }"
    ".message-time-other { color: @muted_color; font-size: 9px; }"
    ".avatar-own, .avatar-other { color: #ffffff; border-radius: 16px; font-size: 11px; font-weight: 800; }"
    ".avatar-own { background-image: linear-gradient(to bottom, @pine_top, @pine_hover_bottom); background-color: @pine_hover; color: @pine_on; box-shadow: inset 0 1px rgba(255,255,255,.30); }"
    ".avatar-other { background: @avatar_bg; }"
    ".date-separator { background-image: linear-gradient(to bottom, @button_top, @button_bottom); background-color: @surface; color: @muted_color; border: 1px solid @border_soft; border-radius: 11px; box-shadow: inset 0 1px @highlight_color, 0 1px 1px @shadow_color; padding: 3px 10px; font-size: 10px; font-weight: 600; }"
    "button.image-link { background: @surface_alt; color: @text_color; border-color: @border_color; padding: 4px 7px; }"
    ".composer { background-image: linear-gradient(to bottom, @chrome_top, @chrome_bottom); background-color: @surface_alt; border-top: 1px solid @border_color; box-shadow: inset 0 1px @highlight_color; padding: 10px 12px; }"
    ".composer-input { background-image: linear-gradient(to bottom, @input_bg, @input_bottom); background-color: @input_bg; border: 1px solid @border_color; border-radius: 5px; box-shadow: inset 0 1px 2px @inset_shadow; }"
    ".composer-input:focus-within { border-color: @pine; }"
    ".composer-input textview, .composer-input textview text { background: @input_bg; color: @text_color; caret-color: @text_color; }";
  static const gchar *css_articles =
    ".article-shell { background: @surface; color: @text_color; }"
    ".article-sidebar, .article-sidebar viewport, .article-sidebar list { background: @sidebar_bg; color: @text_color; }"
    ".article-sidebar row { background: transparent; color: @text_color; border-left: 3px solid transparent; border-radius: 4px; margin: 2px 8px; }"
    ".article-sidebar row:hover { background-image: linear-gradient(to bottom, @button_hover_top, @button_hover_bottom); background-color: @hover_bg; box-shadow: inset 0 1px @highlight_color; }"
    ".article-sidebar row:selected { background-image: linear-gradient(to bottom, @selected_top, @selected_bottom); background-color: @selected_bg; border-left-color: @pine; box-shadow: inset 0 1px @highlight_color, 0 1px 1px @shadow_color; }"
    ".article-list-status, .article-row-meta { color: @muted_color; font-size: 10px; }"
    ".article-row-title { color: @text_color; font-weight: 700; }"
    ".article-row-tags, .article-tags, .article-markdown-note { color: @pine; font-size: 10px; font-weight: 700; }"
    ".article-header { background-image: linear-gradient(to bottom, @surface, @surface_alt); background-color: @surface; color: @text_color; border-bottom: 1px solid @border_color; box-shadow: inset 0 1px @highlight_color, 0 1px 2px @shadow_color; padding: 12px 16px; }"
    ".article-title { color: @text_color; font-size: 20px; font-weight: 700; }"
    ".article-meta, .article-social { color: @muted_color; font-size: 10px; }"
    ".article-body, .article-body text { background: @surface; color: @text_color; }"
    ".article-empty-title { color: @text_color; font-size: 18px; font-weight: 700; }"
    ".article-empty-note { color: @muted_color; font-size: 11px; }"
    ".article-editor-text, .article-editor-text text { background: @input_bg; color: @text_color; caret-color: @text_color; font-family: monospace; }"
    ".article-editor-text { border: 1px solid @border_color; box-shadow: inset 0 1px 2px @inset_shadow; }"
    ".article-shell notebook { background: @surface; color: @text_color; border: 1px solid @border_color; }"
    ".article-shell notebook > header { background-image: linear-gradient(to bottom, @chrome_top, @chrome_bottom); background-color: @surface_alt; border-bottom: 1px solid @border_color; }"
    ".article-shell notebook > header tab { color: @muted_color; padding: 7px 13px; border-bottom: 2px solid transparent; }"
    ".article-shell notebook > header tab:checked { color: @pine; border-bottom-color: @pine; font-weight: 700; }";
  static const gchar *css_settings =
    ".settings-window { background: @surface; color: @text_color; }"
    ".settings-navigation { background-image: linear-gradient(to right, @sidebar_bg, @surface_alt); background-color: @sidebar_bg; border-right: 1px solid @border_color; box-shadow: inset 1px 0 @highlight_color; }"
    ".settings-navigation-title { color: @pine; font-size: 16px; font-weight: 800; }"
    ".settings-categories, .settings-categories row { background: transparent; color: @text_color; border: 0; }"
    ".settings-categories row:hover { background-image: linear-gradient(to bottom, @button_hover_top, @button_hover_bottom); background-color: @hover_bg; }"
    ".settings-categories row:selected, .settings-category-active { background-image: linear-gradient(to bottom, @selected_top, @selected_bottom); background-color: @selected_bg; color: @text_color; border-left: 3px solid @pine; box-shadow: inset 0 1px @highlight_color; }"
    ".settings-page { background: @surface; color: @text_color; }"
    ".settings-heading { color: @text_color; font-size: 22px; font-weight: 700; }"
    ".settings-subtitle, .settings-note, .theme-detail { color: @muted_color; font-size: 11px; }"
    ".settings-section-title { color: @text_color; font-size: 13px; font-weight: 700; }"
    ".theme-option { background-image: linear-gradient(to bottom, @surface, @surface_alt); background-color: @surface_alt; border: 1px solid @border_color; border-radius: 6px; box-shadow: inset 0 1px @highlight_color, 0 2px 4px @shadow_color; padding: 10px; }"
    ".theme-radio { color: @text_color; font-weight: 700; }"
    ".theme-radio radio { background: @input_bg; border-color: @border_color; box-shadow: none; }"
    ".theme-radio radio:checked { background: @pine; border-color: @pine; box-shadow: inset 0 0 0 3px @input_bg; }"
    ".theme-preview { border: 1px solid @border_color; border-radius: 4px; }"
    ".theme-preview-light { background: #f3f4f6; }"
    ".theme-preview-light .preview-toolbar { background-image: linear-gradient(to bottom, #ffffff, #ececef); background-color: #ffffff; border-bottom: 1px solid #d4d4d8; }"
    ".theme-preview-light .preview-sidebar { background: #eeeeef; border-right: 1px solid #d4d4d8; }"
    ".theme-preview-light .preview-content { background: #f8f8f9; border-top: 3px solid #f97316; }"
    ".theme-preview-dark { background: #18181a; }"
    ".theme-preview-dark .preview-toolbar { background-image: linear-gradient(to bottom, #343438, #242426); background-color: #242426; border-bottom: 1px solid #414145; }"
    ".theme-preview-dark .preview-sidebar { background: #202022; border-right: 1px solid #414145; }"
    ".theme-preview-dark .preview-content { background: #29292c; border-top: 3px solid #fb923c; }";
  static const gchar *css_settings_details =
    ".settings-page, .settings-page viewport { background: @surface; color: @text_color; }"
    ".settings-card { background-image: linear-gradient(to bottom, @surface, @surface_alt); background-color: @surface_alt; border: 1px solid @border_color; border-radius: 6px; box-shadow: inset 0 1px @highlight_color, 0 2px 4px @shadow_color; padding: 14px; }"
    ".settings-field-label { color: @text_color; font-size: 11px; font-weight: 700; }"
    ".settings-text-input { background-image: linear-gradient(to bottom, @input_bg, @input_bottom); background-color: @input_bg; border: 1px solid @border_color; border-radius: 5px; box-shadow: inset 0 1px 2px @inset_shadow; }"
    ".settings-text-input textview, .settings-text-input textview text { background: transparent; color: @text_color; caret-color: @text_color; }"
    ".settings-status { color: @pine; font-size: 11px; }"
    ".settings-value { color: @text_color; font-family: monospace; }"
    ".settings-profile-avatar { background-image: linear-gradient(to bottom, @pine_top, @pine_bottom); background-color: @pine; color: @pine_on; border: 1px solid @pine_bottom; border-radius: 32px; box-shadow: inset 0 1px rgba(255,255,255,.35), 0 2px 4px @shadow_color; font-size: 22px; font-weight: 800; }"
    ".avatar-fallback { background: transparent; color: inherit; }"
    "button.destructive-action { background-image: linear-gradient(to bottom, #ef7777, #c83e3e); background-color: #c83e3e; color: #ffffff; border-color: #a92f2f; box-shadow: inset 0 1px rgba(255,255,255,.30), 0 1px 2px @shadow_color; }"
    "button.destructive-action:hover { background-image: linear-gradient(to bottom, #f28b8b, #d94b4b); background-color: #d94b4b; }";
  const gchar *palette = app->theme == PINE_THEME_DARK
    ? dark_palette
    : light_palette;
  gchar *css = g_strconcat(
    palette,
    css_base,
    css_login,
    css_navigation,
    css_chat,
    css_articles,
    css_settings,
    css_settings_details,
    NULL
  );
  GError *error = NULL;

  if (app->style_provider == NULL) {
    app->style_provider = gtk_css_provider_new();
    gtk_style_context_add_provider_for_screen(
      gdk_screen_get_default(),
      GTK_STYLE_PROVIDER(app->style_provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
  }
  if (!gtk_css_provider_load_from_data(app->style_provider, css, -1, &error)) {
    g_warning("テーマCSSを読み込めませんでした: %s", error->message);
    g_clear_error(&error);
  }

  GtkSettings *gtk_settings = gtk_settings_get_default();
  if (gtk_settings != NULL) {
    g_object_set(
      gtk_settings,
      "gtk-application-prefer-dark-theme",
      app->theme == PINE_THEME_DARK,
      NULL
    );
  }
  if (app->window != NULL) {
    gtk_widget_queue_resize(app->window);
    gtk_widget_queue_draw(app->window);
  }
  g_free(css);
}

static gboolean quit_smoke_test(gpointer user_data) {
  g_application_quit(G_APPLICATION(user_data));
  return G_SOURCE_REMOVE;
}

static void application_activate(GtkApplication *application, gpointer user_data) {
  PineApp *app = user_data;
  const gchar *server = g_getenv("PINE2_SERVER_URL");
  app->application = application;
  app->server_url = g_strdup(
    server != NULL && *server != '\0' ? server : DEFAULT_SERVER
  );
  app->avatar_cache = g_hash_table_new_full(
    g_str_hash,
    g_str_equal,
    g_free,
    (GDestroyNotify)avatar_entry_free
  );
  load_theme_setting(app);
  app->api = pine_api_new(app->server_url);
  app->window = gtk_application_window_new(application);
  gtk_window_set_title(GTK_WINDOW(app->window), "Pine2");
  gtk_window_set_default_size(GTK_WINDOW(app->window), 1080, 720);
  gtk_widget_set_size_request(app->window, 720, 480);
  apply_theme(app);

  GtkWidget *titlebar = gtk_header_bar_new();
  gtk_header_bar_set_title(GTK_HEADER_BAR(titlebar), "Pine2");
  gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(titlebar), TRUE);
  gtk_header_bar_set_decoration_layout(
    GTK_HEADER_BAR(titlebar),
    "menu:minimize,maximize,close"
  );
  gtk_style_context_add_class(
    gtk_widget_get_style_context(titlebar),
    "pine-titlebar"
  );
  gtk_window_set_titlebar(GTK_WINDOW(app->window), titlebar);

  app->root_stack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(app->root_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_add_named(GTK_STACK(app->root_stack), build_login_view(app), "login");
  gtk_stack_add_named(GTK_STACK(app->root_stack), build_chat_view(app), "chat");
  gtk_stack_add_named(GTK_STACK(app->root_stack), build_articles_view(app), "articles");
  gtk_container_add(GTK_CONTAINER(app->window), app->root_stack);
  gtk_widget_show_all(app->window);
  gtk_stack_set_visible_child_name(GTK_STACK(app->root_stack), "login");
  app->poll_source = g_timeout_add_seconds(MESSAGE_POLL_SECONDS, poll_messages, app);
  app->rooms_poll_source = g_timeout_add_seconds(15, poll_rooms, app);

  if (g_getenv("PINE2_GTK_SMOKE_TEST") != NULL) {
    pine_markdown_render(
      GTK_TEXT_VIEW(app->article_body_view),
      "# GTK記事テスト\n\n**強調** と [リンク](https://example.com)\n\n"
      "> 引用\n\n- 項目\n\n| 名前 | 状態 | 件数 |\n"
      "| :-- | :--: | --: |\n| 日本語 | 正常 | 12 |\n\n"
      "```c\nint main(void) { return 0; }\n```",
      GTK_WINDOW(app->window),
      app->theme == PINE_THEME_DARK
    );
    GtkTextBuffer *smoke_buffer = gtk_text_view_get_buffer(
      GTK_TEXT_VIEW(app->article_body_view)
    );
    GtkTextIter smoke_start;
    GtkTextIter smoke_end;
    gtk_text_buffer_get_bounds(smoke_buffer, &smoke_start, &smoke_end);
    gchar *smoke_text = gtk_text_buffer_get_text(
      smoke_buffer,
      &smoke_start,
      &smoke_end,
      FALSE
    );
    g_assert_nonnull(strstr(smoke_text, "┌"));
    g_assert_nonnull(strstr(smoke_text, "│ 名前"));
    g_assert_nonnull(strstr(smoke_text, "日本語"));
    g_assert_nonnull(strstr(smoke_text, "└"));
    g_free(smoke_text);
    g_timeout_add(150, quit_smoke_test, application);
  } else {
    set_login_busy(app, TRUE);
    gtk_label_set_text(GTK_LABEL(app->login_status), "保存済みセッションを確認しています…");
    pine_api_request_async(
      app->api,
      "GET",
      "/api/auth/check",
      NULL,
      NULL,
      auth_completed,
      app
    );
  }
}

static void pine_app_clear(PineApp *app) {
  if (app->poll_source != 0) {
    g_source_remove(app->poll_source);
  }
  if (app->rooms_poll_source != 0) {
    g_source_remove(app->rooms_poll_source);
  }
  pine_api_free(app->api);
  g_free(app->user_id);
  g_free(app->display_name);
  g_free(app->active_room_id);
  g_free(app->active_room_name);
  g_free(app->message_snapshot);
  g_free(app->server_url);
  g_free(app->settings_path);
  g_free(app->profile_text);
  g_free(app->icon_type);
  g_free(app->icon_value);
  g_free(app->icon_image_url);
  clear_selected_article(app);
  g_clear_pointer(&app->avatar_cache, g_hash_table_unref);
  if (app->style_provider != NULL && gdk_screen_get_default() != NULL) {
    gtk_style_context_remove_provider_for_screen(
      gdk_screen_get_default(),
      GTK_STYLE_PROVIDER(app->style_provider)
    );
  }
  g_clear_object(&app->style_provider);
}

int main(int argc, char **argv) {
  PineApp app = {0};
  GtkApplication *application;
  int status;

  curl_global_init(CURL_GLOBAL_DEFAULT);
  application = gtk_application_new("app.pine2.Pine2", (GApplicationFlags)0);
  g_signal_connect(application, "activate", G_CALLBACK(application_activate), &app);
  status = g_application_run(G_APPLICATION(application), argc, argv);
  pine_app_clear(&app);
  g_object_unref(application);
  /* Request workers retain their API state until completion. Process shutdown
   * releases libcurl's global state after those threads have disappeared. */
  return status;
}
