#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

void pine_markdown_render(
  GtkTextView *view,
  const gchar *markdown,
  GtkWindow *parent_window,
  gboolean dark_theme
);

G_END_DECLS
