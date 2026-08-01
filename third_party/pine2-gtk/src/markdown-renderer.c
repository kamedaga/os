#include "markdown-renderer.h"

#include <cmark-gfm-core-extensions.h>
#include <cmark-gfm.h>
#include <string.h>

typedef struct {
  GtkTextBuffer *buffer;
  GtkWindow *parent_window;
  GtkTextTag *heading[3];
  GtkTextTag *paragraph;
  GtkTextTag *strong;
  GtkTextTag *emphasis;
  GtkTextTag *strike;
  GtkTextTag *inline_code;
  GtkTextTag *code_block;
  GtkTextTag *quote;
  GtkTextTag *link;
  GtkTextTag *rule;
  GtkTextTag *table;
  GtkTextTag *table_header;
} MarkdownContext;

static gboolean link_event(
  GtkTextTag *tag,
  GObject *object,
  GdkEvent *event,
  GtkTextIter *iter,
  gpointer user_data
) {
  (void)object;
  (void)iter;
  if (event->type != GDK_BUTTON_RELEASE ||
      ((GdkEventButton *)event)->button != GDK_BUTTON_PRIMARY) {
    return FALSE;
  }
  const gchar *url = g_object_get_data(G_OBJECT(tag), "pine-url");
  if (url == NULL || *url == '\0') {
    return FALSE;
  }
  GError *error = NULL;
  gtk_show_uri_on_window(
    GTK_WINDOW(user_data),
    url,
    ((GdkEventButton *)event)->time,
    &error
  );
  if (error != NULL) {
    g_warning("Markdownリンクを開けませんでした: %s", error->message);
    g_error_free(error);
  }
  return TRUE;
}

static void buffer_insert(MarkdownContext *context, const gchar *text) {
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(context->buffer, &end);
  gtk_text_buffer_insert(context->buffer, &end, text != NULL ? text : "", -1);
}

static gint buffer_offset(MarkdownContext *context) {
  GtkTextIter end;
  gtk_text_buffer_get_end_iter(context->buffer, &end);
  return gtk_text_iter_get_offset(&end);
}

static void apply_tag(
  MarkdownContext *context,
  GtkTextTag *tag,
  gint start_offset,
  gint end_offset
) {
  if (tag == NULL || end_offset <= start_offset) {
    return;
  }
  GtkTextIter start;
  GtkTextIter end;
  gtk_text_buffer_get_iter_at_offset(context->buffer, &start, start_offset);
  gtk_text_buffer_get_iter_at_offset(context->buffer, &end, end_offset);
  gtk_text_buffer_apply_tag(context->buffer, tag, &start, &end);
}

static GtkTextTag *create_link_tag(
  MarkdownContext *context,
  const gchar *url
) {
  GtkTextTag *tag = gtk_text_buffer_create_tag(
    context->buffer,
    NULL,
    "foreground",
    "#e66a16",
    "underline",
    PANGO_UNDERLINE_SINGLE,
    NULL
  );
  g_object_set_data_full(G_OBJECT(tag), "pine-url", g_strdup(url), g_free);
  g_signal_connect(tag, "event", G_CALLBACK(link_event), context->parent_window);
  return tag;
}

static void render_inline(MarkdownContext *context, cmark_node *node);

static void render_inline_children(MarkdownContext *context, cmark_node *node) {
  for (cmark_node *child = cmark_node_first_child(node);
       child != NULL;
       child = cmark_node_next(child)) {
    render_inline(context, child);
  }
}

static void render_inline(MarkdownContext *context, cmark_node *node) {
  const cmark_node_type type = cmark_node_get_type(node);
  const gchar *type_name = cmark_node_get_type_string(node);
  const gint start = buffer_offset(context);

  if (g_strcmp0(type_name, "strikethrough") == 0) {
    render_inline_children(context, node);
    apply_tag(context, context->strike, start, buffer_offset(context));
    return;
  }

  switch (type) {
    case CMARK_NODE_TEXT:
      buffer_insert(context, cmark_node_get_literal(node));
      break;
    case CMARK_NODE_SOFTBREAK:
      buffer_insert(context, " ");
      break;
    case CMARK_NODE_LINEBREAK:
      buffer_insert(context, "\n");
      break;
    case CMARK_NODE_CODE:
      buffer_insert(context, cmark_node_get_literal(node));
      apply_tag(context, context->inline_code, start, buffer_offset(context));
      break;
    case CMARK_NODE_EMPH:
      render_inline_children(context, node);
      apply_tag(context, context->emphasis, start, buffer_offset(context));
      break;
    case CMARK_NODE_STRONG:
      render_inline_children(context, node);
      apply_tag(context, context->strong, start, buffer_offset(context));
      break;
    case CMARK_NODE_LINK: {
      render_inline_children(context, node);
      const gchar *url = cmark_node_get_url(node);
      if (buffer_offset(context) == start) {
        buffer_insert(context, url);
      }
      GtkTextTag *tag = create_link_tag(context, url);
      apply_tag(context, tag, start, buffer_offset(context));
      break;
    }
    case CMARK_NODE_IMAGE: {
      buffer_insert(context, "画像: ");
      render_inline_children(context, node);
      const gchar *url = cmark_node_get_url(node);
      buffer_insert(context, " (");
      const gint url_start = buffer_offset(context);
      buffer_insert(context, url);
      GtkTextTag *tag = create_link_tag(context, url);
      apply_tag(context, tag, url_start, buffer_offset(context));
      buffer_insert(context, ")");
      break;
    }
    case CMARK_NODE_HTML_INLINE:
      break;
    default:
      render_inline_children(context, node);
      break;
  }
}

static void render_block(MarkdownContext *context, cmark_node *node, gint depth);

static void table_cell_text(GString *text, cmark_node *node) {
  switch (cmark_node_get_type(node)) {
    case CMARK_NODE_TEXT:
    case CMARK_NODE_CODE:
      g_string_append(text, cmark_node_get_literal(node));
      return;
    case CMARK_NODE_SOFTBREAK:
    case CMARK_NODE_LINEBREAK:
      g_string_append_c(text, ' ');
      return;
    case CMARK_NODE_HTML_INLINE:
    case CMARK_NODE_HTML_BLOCK:
      return;
    default:
      break;
  }
  for (cmark_node *child = cmark_node_first_child(node);
       child != NULL;
       child = cmark_node_next(child)) {
    table_cell_text(text, child);
  }
}

static gchar *normalized_table_cell(cmark_node *cell) {
  GString *raw = g_string_new("");
  GString *normalized = g_string_new("");
  gboolean pending_space = FALSE;
  table_cell_text(raw, cell);
  for (const gchar *cursor = raw->str; *cursor != '\0';) {
    const gunichar character = g_utf8_get_char(cursor);
    if (g_unichar_isspace(character)) {
      pending_space = normalized->len > 0;
    } else {
      if (pending_space) {
        g_string_append_c(normalized, ' ');
      }
      g_string_append_unichar(normalized, character);
      pending_space = FALSE;
    }
    cursor = g_utf8_next_char(cursor);
  }
  g_string_free(raw, TRUE);
  return g_string_free(normalized, FALSE);
}

static gint table_text_width(const gchar *text) {
  gint width = 0;
  for (const gchar *cursor = text; cursor != NULL && *cursor != '\0';) {
    const gunichar character = g_utf8_get_char(cursor);
    if (g_unichar_combining_class(character) == 0) {
      width += g_unichar_iswide(character) ? 2 : 1;
    }
    cursor = g_utf8_next_char(cursor);
  }
  return width;
}

static gchar *truncate_table_cell(const gchar *text, gint maximum_width) {
  if (table_text_width(text) <= maximum_width) {
    return g_strdup(text);
  }
  GString *truncated = g_string_new("");
  gint width = 0;
  const gint content_width = MAX(1, maximum_width - 1);
  for (const gchar *cursor = text; *cursor != '\0';) {
    const gunichar character = g_utf8_get_char(cursor);
    const gint character_width = g_unichar_combining_class(character) != 0
      ? 0
      : (g_unichar_iswide(character) ? 2 : 1);
    if (width + character_width > content_width) {
      break;
    }
    g_string_append_unichar(truncated, character);
    width += character_width;
    cursor = g_utf8_next_char(cursor);
  }
  g_string_append_unichar(truncated, 0x2026);
  return g_string_free(truncated, FALSE);
}

static void table_repeat(GString *output, const gchar *character, gint count) {
  for (gint i = 0; i < count; i++) {
    g_string_append(output, character);
  }
}

static void table_border(
  GString *output,
  const gint *widths,
  guint columns,
  const gchar *left,
  const gchar *junction,
  const gchar *right
) {
  g_string_append(output, left);
  for (guint column = 0; column < columns; column++) {
    table_repeat(output, "─", widths[column] + 2);
    g_string_append(output, column + 1 < columns ? junction : right);
  }
  g_string_append_c(output, '\n');
}

static void render_table(MarkdownContext *context, cmark_node *table, gint depth) {
  (void)depth;
  guint columns = cmark_gfm_extensions_get_table_columns(table);
  if (columns == 0) {
    return;
  }
  GPtrArray *rows = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
  GArray *headers = g_array_new(FALSE, FALSE, sizeof(gboolean));
  for (cmark_node *row = cmark_node_first_child(table);
       row != NULL;
       row = cmark_node_next(row)) {
    GPtrArray *cells = g_ptr_array_new_with_free_func(g_free);
    for (cmark_node *cell = cmark_node_first_child(row);
         cell != NULL;
         cell = cmark_node_next(cell)) {
      g_ptr_array_add(cells, normalized_table_cell(cell));
    }
    while (cells->len < columns) {
      g_ptr_array_add(cells, g_strdup(""));
    }
    g_ptr_array_add(rows, cells);
    gboolean header = cmark_gfm_extensions_get_table_row_is_header(row) != 0;
    g_array_append_val(headers, header);
  }

  const gint column_cap = CLAMP(64 / (gint)columns, 8, 32);
  gint *widths = g_new0(gint, columns);
  for (guint column = 0; column < columns; column++) {
    widths[column] = 3;
    for (guint row = 0; row < rows->len; row++) {
      GPtrArray *cells = g_ptr_array_index(rows, row);
      const gchar *cell = column < cells->len ? g_ptr_array_index(cells, column) : "";
      widths[column] = MAX(widths[column], MIN(table_text_width(cell), column_cap));
    }
  }

  const guint8 *alignments = cmark_gfm_extensions_get_table_alignments(table);
  GString *output = g_string_new("");
  GArray *header_ranges = g_array_new(FALSE, FALSE, sizeof(gint));
  table_border(output, widths, columns, "┌", "┬", "┐");
  for (guint row = 0; row < rows->len; row++) {
    GPtrArray *cells = g_ptr_array_index(rows, row);
    const gboolean header = g_array_index(headers, gboolean, row);
    const gint header_start = (gint)g_utf8_strlen(output->str, -1);
    g_string_append(output, "│");
    for (guint column = 0; column < columns; column++) {
      const gchar *raw = column < cells->len ? g_ptr_array_index(cells, column) : "";
      gchar *cell = truncate_table_cell(raw, widths[column]);
      const gint padding = widths[column] - table_text_width(cell);
      gint left_padding = 0;
      gint right_padding = padding;
      const guint8 alignment = alignments != NULL ? alignments[column] : 0;
      if (alignment == 'r') {
        left_padding = padding;
        right_padding = 0;
      } else if (alignment == 'c') {
        left_padding = padding / 2;
        right_padding = padding - left_padding;
      }
      g_string_append_c(output, ' ');
      table_repeat(output, " ", left_padding);
      g_string_append(output, cell);
      table_repeat(output, " ", right_padding);
      g_string_append(output, " │");
      g_free(cell);
    }
    g_string_append_c(output, '\n');
    if (header) {
      const gint header_end = (gint)g_utf8_strlen(output->str, -1);
      g_array_append_val(header_ranges, header_start);
      g_array_append_val(header_ranges, header_end);
      table_border(output, widths, columns, "├", "┼", "┤");
    }
  }
  table_border(output, widths, columns, "└", "┴", "┘");
  g_string_append_c(output, '\n');

  const gint table_start = buffer_offset(context);
  buffer_insert(context, output->str);
  apply_tag(context, context->table, table_start, buffer_offset(context));
  for (guint index = 0; index + 1 < header_ranges->len; index += 2) {
    apply_tag(
      context,
      context->table_header,
      table_start + g_array_index(header_ranges, gint, index),
      table_start + g_array_index(header_ranges, gint, index + 1)
    );
  }
  g_array_unref(header_ranges);
  g_string_free(output, TRUE);
  g_free(widths);
  g_array_unref(headers);
  g_ptr_array_unref(rows);
}

static void render_list(MarkdownContext *context, cmark_node *list, gint depth) {
  const gboolean ordered = cmark_node_get_list_type(list) == CMARK_ORDERED_LIST;
  gint index = (gint)cmark_node_get_list_start(list);
  for (cmark_node *item = cmark_node_first_child(list);
       item != NULL;
       item = cmark_node_next(item), index++) {
    for (gint indent = 0; indent < depth; indent++) {
      buffer_insert(context, "    ");
    }
    if (ordered) {
      gchar *marker = g_strdup_printf("%d. ", index);
      buffer_insert(context, marker);
      g_free(marker);
    } else {
      buffer_insert(context, "• ");
    }
    for (cmark_node *child = cmark_node_first_child(item);
         child != NULL;
         child = cmark_node_next(child)) {
      if (cmark_node_get_type(child) == CMARK_NODE_PARAGRAPH) {
        render_inline_children(context, child);
      } else if (cmark_node_get_type(child) == CMARK_NODE_LIST) {
        buffer_insert(context, "\n");
        render_list(context, child, depth + 1);
      } else {
        render_block(context, child, depth + 1);
      }
    }
    buffer_insert(context, "\n");
  }
  if (depth == 0) {
    buffer_insert(context, "\n");
  }
}

static void render_block(MarkdownContext *context, cmark_node *node, gint depth) {
  const cmark_node_type type = cmark_node_get_type(node);
  const gchar *type_name = cmark_node_get_type_string(node);
  if (g_strcmp0(type_name, "table") == 0) {
    render_table(context, node, depth);
    return;
  }

  switch (type) {
    case CMARK_NODE_DOCUMENT:
      for (cmark_node *child = cmark_node_first_child(node);
           child != NULL;
           child = cmark_node_next(child)) {
        render_block(context, child, depth);
      }
      break;
    case CMARK_NODE_HEADING: {
      const gint start = buffer_offset(context);
      render_inline_children(context, node);
      const gint level = CLAMP(cmark_node_get_heading_level(node), 1, 3);
      apply_tag(
        context,
        context->heading[level - 1],
        start,
        buffer_offset(context)
      );
      buffer_insert(context, "\n\n");
      break;
    }
    case CMARK_NODE_PARAGRAPH: {
      const gint start = buffer_offset(context);
      render_inline_children(context, node);
      apply_tag(context, context->paragraph, start, buffer_offset(context));
      buffer_insert(context, "\n\n");
      break;
    }
    case CMARK_NODE_BLOCK_QUOTE: {
      const gint start = buffer_offset(context);
      for (cmark_node *child = cmark_node_first_child(node);
           child != NULL;
           child = cmark_node_next(child)) {
        render_block(context, child, depth + 1);
      }
      apply_tag(context, context->quote, start, buffer_offset(context));
      break;
    }
    case CMARK_NODE_LIST:
      render_list(context, node, depth);
      break;
    case CMARK_NODE_CODE_BLOCK: {
      const gchar *info = cmark_node_get_fence_info(node);
      const gint start = buffer_offset(context);
      if (info != NULL && g_ascii_strcasecmp(info, "mermaid") == 0) {
        buffer_insert(context, "Mermaid図（GTK版ではソース表示）\n");
      }
      buffer_insert(context, cmark_node_get_literal(node));
      if (buffer_offset(context) > start) {
        buffer_insert(context, "\n");
      }
      apply_tag(context, context->code_block, start, buffer_offset(context));
      buffer_insert(context, "\n");
      break;
    }
    case CMARK_NODE_THEMATIC_BREAK: {
      const gint start = buffer_offset(context);
      buffer_insert(context, "────────────────────────\n\n");
      apply_tag(context, context->rule, start, buffer_offset(context));
      break;
    }
    case CMARK_NODE_HTML_BLOCK:
      break;
    default:
      for (cmark_node *child = cmark_node_first_child(node);
           child != NULL;
           child = cmark_node_next(child)) {
        render_block(context, child, depth);
      }
      break;
  }
}

static cmark_node *parse_markdown(const gchar *markdown) {
  cmark_gfm_core_extensions_ensure_registered();
  cmark_parser *parser = cmark_parser_new(CMARK_OPT_DEFAULT);
  const gchar *extensions[] = {
    "autolink",
    "strikethrough",
    "table",
    "tasklist",
    NULL
  };
  for (gint i = 0; extensions[i] != NULL; i++) {
    cmark_syntax_extension *extension = cmark_find_syntax_extension(extensions[i]);
    if (extension != NULL) {
      cmark_parser_attach_syntax_extension(parser, extension);
    }
  }
  cmark_parser_feed(parser, markdown, strlen(markdown));
  cmark_node *document = cmark_parser_finish(parser);
  cmark_parser_free(parser);
  return document;
}

void pine_markdown_render(
  GtkTextView *view,
  const gchar *markdown,
  GtkWindow *parent_window,
  gboolean dark_theme
) {
  GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
  MarkdownContext context = {
    .buffer = buffer,
    .parent_window = parent_window,
  };
  context.heading[0] = gtk_text_buffer_create_tag(
    buffer, "heading-1", "scale", 1.75, "weight", PANGO_WEIGHT_BOLD,
    "pixels-above-lines", 14, "pixels-below-lines", 5, NULL
  );
  context.heading[1] = gtk_text_buffer_create_tag(
    buffer, "heading-2", "scale", 1.42, "weight", PANGO_WEIGHT_BOLD,
    "pixels-above-lines", 11, "pixels-below-lines", 4, NULL
  );
  context.heading[2] = gtk_text_buffer_create_tag(
    buffer, "heading-3", "scale", 1.18, "weight", PANGO_WEIGHT_BOLD,
    "pixels-above-lines", 8, "pixels-below-lines", 3, NULL
  );
  context.paragraph = gtk_text_buffer_create_tag(
    buffer, "paragraph", "pixels-below-lines", 2, NULL
  );
  context.strong = gtk_text_buffer_create_tag(
    buffer, "strong", "weight", PANGO_WEIGHT_BOLD, NULL
  );
  context.emphasis = gtk_text_buffer_create_tag(
    buffer, "emphasis", "style", PANGO_STYLE_ITALIC, NULL
  );
  context.strike = gtk_text_buffer_create_tag(
    buffer, "strike", "strikethrough", TRUE, NULL
  );
  context.inline_code = gtk_text_buffer_create_tag(
    buffer, "inline-code", "family", "monospace", "background",
    dark_theme ? "#3a312b" : "#fff1e6", "foreground",
    dark_theme ? "#ffb275" : "#b94b00", NULL
  );
  context.code_block = gtk_text_buffer_create_tag(
    buffer, "code-block", "family", "monospace", "paragraph-background",
    dark_theme ? "#252528" : "#f1f1f3", "foreground",
    dark_theme ? "#f4f4f5" : "#27272a", "left-margin", 14, "right-margin", 14,
    "pixels-above-lines", 8, "pixels-below-lines", 8, NULL
  );
  context.quote = gtk_text_buffer_create_tag(
    buffer, "quote", "left-margin", 18, "right-margin", 8,
    "foreground", dark_theme ? "#a1a1aa" : "#64646d",
    "style", PANGO_STYLE_ITALIC, "paragraph-background",
    dark_theme ? "#2b2928" : "#fff7ed", NULL
  );
  context.link = gtk_text_buffer_create_tag(
    buffer, "link", "foreground", "#e66a16", "underline",
    PANGO_UNDERLINE_SINGLE, NULL
  );
  context.rule = gtk_text_buffer_create_tag(
    buffer, "rule", "foreground", dark_theme ? "#77777d" : "#a1a1aa", NULL
  );
  context.table = gtk_text_buffer_create_tag(
    buffer, "table", "family", "monospace", "paragraph-background",
    dark_theme ? "#252528" : "#f6f6f7", "foreground",
    dark_theme ? "#e4e4e7" : "#3f3f46", "left-margin", 8,
    "right-margin", 8, "pixels-above-lines", 4, "pixels-below-lines", 4,
    NULL
  );
  context.table_header = gtk_text_buffer_create_tag(
    buffer, "table-header", "weight", PANGO_WEIGHT_BOLD, "foreground",
    dark_theme ? "#ffb275" : "#b94b00", NULL
  );

  cmark_node *document = parse_markdown(markdown != NULL ? markdown : "");
  if (document != NULL) {
    render_block(&context, document, 0);
    cmark_node_free(document);
  }
  gtk_text_view_set_buffer(view, buffer);
  g_object_unref(buffer);
}
