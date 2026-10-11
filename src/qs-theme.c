#include "qs-theme.h"

/* Shared visual language: one radius, theme-aware colors, type scale.
 * Every screen uses only these classes, so all pages share one token. */
void
qs_theme_load (void)
{
  static gboolean  css_loaded = FALSE;
  GtkCssProvider  *provider;

  if (css_loaded)
    return;
  provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_data (provider,
    ".qs-popup { background-color: @theme_bg_color; }"
    ".qs-section { font-weight: bold; font-size: smaller;"
    " color: alpha(@theme_fg_color, 0.6); }"
    ".qs-page-title { font-weight: bold; font-size: larger; }"
    ".qs-title { font-weight: bold; }"
    ".qs-mini-title { font-weight: bold; font-size: smaller; }"
    ".qs-sub, .qs-status { color: alpha(@theme_fg_color, 0.6);"
    " font-size: smaller; }"
    ".qs-value { font-weight: bold; font-size: larger; }"
    ".qs-sys-num { font-weight: bold; font-size: 0.85em; }"
    ".qs-back { min-width: 32px; min-height: 32px; padding: 0;"
    " border-radius: 999px; }"
    ".qs-tile, .qs-btn { border-radius: 10px; }"
    ".qs-tile { padding: 6px 8px; }"
    ".qs-compact { padding: 2px 8px; min-height: 0; }"
    ".qs-btn { min-height: 40px; }"
    ".qs-tile-active, button.qs-tile:checked {"
    " background-color: alpha(@theme_selected_bg_color, 0.22);"
    " border-color: alpha(@theme_selected_bg_color, 0.6);"
    " color: @theme_fg_color; }"
    ".qs-card { background-color: @theme_base_color;"
    " border: 1px solid @borders; border-radius: 10px; }"
    "list.qs-list { background-color: transparent; border: none; }"
    "list.qs-list row { background-image: none; background-color: transparent;"
    " border: none; box-shadow: none; padding: 0; }"
    "list.qs-list row.qs-card { background-color: @theme_base_color;"
    " border: 1px solid @borders; border-radius: 10px; }"
    "list.qs-list row:selected { background-color: transparent;"
    " color: @theme_fg_color; }"
    "scale.qs-slider trough { min-height: 6px; border-radius: 3px; }"
    "scale.qs-slider highlight { min-height: 6px; border-radius: 3px;"
    " background-color: @theme_selected_bg_color; border: none; }"
    "scale.qs-slider slider { min-width: 16px; min-height: 16px;"
    " border-radius: 999px; }"
    ".qs-combo { min-height: 40px; padding-left: 12px; }"
    "separator.qs-sep { color: alpha(@theme_fg_color, 0.15);"
    " background-color: alpha(@theme_fg_color, 0.15);"
    " background-image: none; min-height: 1px; }", -1, NULL);
  gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
    GTK_STYLE_PROVIDER (provider),
    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (provider);
  css_loaded = TRUE;
}
