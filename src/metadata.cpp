#include "metadata.h"

#include <algorithm>

namespace
{

enum Column
{
    KEY,
    VALUE,
    WEIGHT,
    SEPARATOR,
    URL,
    COUNT
};

gboolean is_separator(GtkTreeModel* model, GtkTreeIter* iter, gpointer)
{
    gboolean sep = FALSE;
    gtk_tree_model_get(model, iter, SEPARATOR, &sep, -1);
    return sep;
}

// Clicking a row that carries a URL opens it; everything else is inert.
gboolean on_click(GtkWidget* tree, GdkEventButton* e, gpointer)
{
    if (e->button != 1 || e->type != GDK_BUTTON_PRESS)
        return FALSE;
    GtkTreePath* path = nullptr;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(tree), int(e->x), int(e->y), &path, nullptr, nullptr, nullptr))
        return FALSE;
    GtkTreeModel* model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree));
    GtkTreeIter iter;
    gchar* url = nullptr;
    if (gtk_tree_model_get_iter(model, &iter, path))
        gtk_tree_model_get(model, &iter, URL, &url, -1);
    gtk_tree_path_free(path);
    if (url && *url)
        gtk_show_uri_on_window(GTK_WINDOW(gtk_widget_get_toplevel(tree)), url, GDK_CURRENT_TIME, nullptr);
    g_free(url);
    return FALSE;
}

}  // namespace

MetadataPane::MetadataPane()
{
    pane_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(pane_, 240, -1);  // minimum; the paned divider sets the actual width
    gtk_widget_set_no_show_all(pane_, TRUE);
    title_ = gtk_label_new("Metadata");
    gtk_widget_set_margin_top(title_, 6);
    gtk_widget_set_margin_bottom(title_, 6);
    gtk_label_set_ellipsize(GTK_LABEL(title_), PANGO_ELLIPSIZE_MIDDLE);
    PangoAttrList* attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(title_), attrs);
    pango_attr_list_unref(attrs);
    gtk_box_pack_start(GTK_BOX(pane_), title_, FALSE, FALSE, 0);

    store_ = gtk_list_store_new(COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_BOOLEAN, G_TYPE_STRING);
    tree_ = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store_));
    gtk_tree_view_set_row_separator_func(GTK_TREE_VIEW(tree_), is_separator, nullptr, nullptr);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree_)), GTK_SELECTION_NONE);
    g_signal_connect(tree_, "button-press-event", G_CALLBACK(on_click), nullptr);

    GtkCellRenderer* key = gtk_cell_renderer_text_new();
    g_object_set(key, "xpad", 8, nullptr);
    g_object_set(key, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    GtkTreeViewColumn* key_col = gtk_tree_view_column_new_with_attributes("Tag", key, "text", KEY, "weight", WEIGHT, nullptr);
    gtk_tree_view_column_set_resizable(key_col, TRUE);
    gtk_tree_view_column_set_sizing(key_col, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(key_col, 150);  // the long libexif titles would otherwise squeeze the values out
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_), key_col);

    GtkCellRenderer* value = gtk_cell_renderer_text_new();
    g_object_set(value, "wrap-mode", PANGO_WRAP_WORD_CHAR, "wrap-width", 200, "xpad", 8, nullptr);
    GtkTreeViewColumn* value_col = gtk_tree_view_column_new_with_attributes("Value", value, "markup", VALUE, nullptr);
    gtk_tree_view_column_set_expand(value_col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_), value_col);
    // Long values wrap to whatever width the pane has, so nothing hides off the right.
    struct Wrap
    {
        GtkCellRenderer* renderer;
        GtkTreeViewColumn* column;
        int last = 0;
    };
    auto* wrap = new Wrap{value, value_col, 0};
    g_object_set_data_full(G_OBJECT(tree_), "wrap", wrap, +[](gpointer p) { delete static_cast<Wrap*>(p); });

    // EXTERNAL: the scroller doesn't adopt the tree's minimum width (which the
    // wrap width would otherwise pin), so the divider can move freely both
    // ways; the wrap follows the scroller's width instead.
    GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_EXTERNAL, GTK_POLICY_AUTOMATIC);
    g_signal_connect(scroller, "size-allocate",
                     G_CALLBACK(+[](GtkWidget*, GdkRectangle* alloc, gpointer p)
                                {
                                    auto* w = static_cast<Wrap*>(p);
                                    const int width = std::max(80, alloc->width - 150 - 24);  // minus the Tag column and padding
                                    if (width == w->last)
                                        return;
                                    w->last = width;
                                    g_object_set(w->renderer, "wrap-width", width, nullptr);
                                    gtk_tree_view_column_queue_resize(w->column);
                                }),
                     wrap);
    gtk_container_add(GTK_CONTAINER(scroller), tree_);
    gtk_box_pack_start(GTK_BOX(pane_), scroller, TRUE, TRUE, 0);
    gtk_widget_show_all(title_);
    gtk_widget_show_all(scroller);
}

bool MetadataPane::visible() const
{
    return gtk_widget_get_visible(pane_);
}

void MetadataPane::set_width(int w)
{
    width_ = std::max(240, w);
}

void MetadataPane::toggle()
{
    gtk_widget_set_visible(pane_, !visible());
}

void MetadataPane::set(const std::string& title, const std::vector<MetadataGroup>& groups, const Image* image, const std::string& map_url)
{
    gtk_label_set_text(GTK_LABEL(title_), title.empty() ? "Metadata" : title.c_str());
    gtk_list_store_clear(store_);
    GtkTreeIter iter;
    bool first = true;
    auto heading = [&](const std::string& name)
    {
        if (!first)
        {
            gtk_list_store_append(store_, &iter);
            gtk_list_store_set(store_, &iter, SEPARATOR, TRUE, -1);
        }
        first = false;
        gtk_list_store_append(store_, &iter);
        gtk_list_store_set(store_, &iter, KEY, name.c_str(), VALUE, "", WEIGHT, PANGO_WEIGHT_BOLD, -1);
    };
    for (const MetadataGroup& g : groups)
    {
        heading(g.name);
        if (g.name == "GPS" && image && image->has_location)
        {
            gchar* markup = g_markup_printf_escaped("<span foreground=\"#1a5fb4\" underline=\"single\">%s</span>", image->location_text().c_str());
            gtk_list_store_append(store_, &iter);
            gtk_list_store_set(store_, &iter, KEY, "Location", VALUE, markup, WEIGHT, PANGO_WEIGHT_NORMAL, URL, map_url.c_str(), -1);
            g_free(markup);
        }
        for (const MetadataItem& item : g.items)
        {
            gchar* escaped = g_markup_escape_text(item.value.c_str(), -1);
            gtk_list_store_append(store_, &iter);
            gtk_list_store_set(store_, &iter, KEY, item.key.c_str(), VALUE, escaped, WEIGHT, PANGO_WEIGHT_NORMAL, -1);
            g_free(escaped);
        }
    }
}
