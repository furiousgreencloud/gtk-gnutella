/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi, Richard Eckart
 *
 * GUI filtering functions.
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

#include "gui.h"

#ifdef USE_GTK2

#include "gtk-missing.h"
#include "gtkcolumnchooser.h"
#include "nodes_gui_common.h"
#include "nodes_gui.h"
#include "interface-glade2.h"
#include "override.h"		/* Must be the last header included */

RCSID("$Id$");

#define UPDATE_MIN	300		/* Update screen every 5 minutes at least */

/* 
 * These hash tables record which information about which nodes has
 * changed. By using this the number of updates to the gui can be
 * significantly reduced.
 */
static GHashTable *ht_node_info_changed = NULL;
static GHashTable *ht_node_flags_changed = NULL;

static GtkTreeView *treeview_nodes = NULL;
static GtkListStore *nodes_model = NULL;
static GtkCellRenderer *nodes_gui_cell_renderer = NULL;

/* hash table for fast handle -> GtkTreeIter mapping */
static GHashTable *nodes_handles = NULL;
/* list of all node handles */

/***
 *** Private functions
 ***/

static void nodes_gui_node_removed(gnet_node_t);
static void nodes_gui_node_added(gnet_node_t, const gchar *);
static void nodes_gui_node_info_changed(gnet_node_t);
static void nodes_gui_node_flags_changed(gnet_node_t);

void on_popup_nodes_config_cols_activate(
	GtkMenuItem *menuitem, gpointer user_data)
{
    GtkWidget *cc;

    cc = gtk_column_chooser_new(GTK_WIDGET(treeview_nodes));
    gtk_menu_popup(GTK_MENU(cc), NULL, NULL, NULL, NULL, 1, 0);
}

/*
 * add_column
 *
 * Create a column, associating the attribute ``attr'' (usually "text") of the
 * cell_renderer to the first column of the model. Also associate the
 * foreground color with the c_gnet_fg column, so that we can set
 * the foreground color for the whole row.
 */
static void add_column(
	GtkTreeView *tree, gint column_id, const gchar *title, const gchar *attr)
{
    GtkTreeViewColumn *column;

   	column = gtk_tree_view_column_new_with_attributes(
		title, nodes_gui_cell_renderer,
		attr, column_id, 
		"foreground-gdk", c_gnet_fg,
		NULL);
	g_object_set(G_OBJECT(column),
		"fixed-width", 1,
		"min-width", 1,
		"reorderable", TRUE,
		"resizable", TRUE,
		"sizing", GTK_TREE_VIEW_COLUMN_FIXED,
		NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW (tree), column);
}

/*
 * nodes_gui_set_column_width
 *
 * Sets the specified column, from the specified tree to the
 * specified width.
 */
static void set_column(GtkTreeView *tree, gint id, gint width, gboolean visible)
{
	GtkTreeViewColumn *column;

	column = gtk_tree_view_get_column(tree, id);
	gtk_tree_view_column_set_visible(column, visible);
	gtk_tree_view_column_set_fixed_width(column, MAX(1, width));
}

/*
 * nodes_gui_create_treeview_nodes
 *
 * Sets up the treeview_nodes object for use by
 * settings_gui. (Uses a default width of one; actual
 * widths are set during nodes_gui_init. This
 * component must be able to be initialized before
 * width settings are initialized.)
 */
static void nodes_gui_create_treeview_nodes(void)
{
	static const struct {
		const gchar * const title;
		const gint id;
		const gchar * const attr;
	} columns[] = {
		{ N_("Host"),		c_gnet_host,		"text" },
		{ N_("Flags"),		c_gnet_flags,		"markup" },
		{ N_("User-Agent"), c_gnet_user_agent,	"text" },
		{ N_("Ver"),		c_gnet_version,		"text" },
		{ N_("Connected"),	c_gnet_connected,	"text" },
		{ N_("Uptime"),		c_gnet_uptime,		"text" },
		{ N_("Info"),		c_gnet_info,		"text" }
	};
	GtkTreeView *tree;
	guint i;

	STATIC_ASSERT(NODES_VISIBLE_COLUMNS == G_N_ELEMENTS(columns));

    /*
     * Create a model.  We are using the store model for now, though we
     * could use any other GtkTreeModel
     */
    nodes_model = gtk_list_store_new(c_gnet_num,
        G_TYPE_STRING,   /* c_gnet_host */
        G_TYPE_STRING,   /* c_gnet_flags */
        G_TYPE_STRING,   /* c_gnet_user_agent */
        G_TYPE_STRING,   /* c_gnet_version */
        G_TYPE_STRING,   /* c_gnet_connected */
        G_TYPE_STRING,   /* c_gnet_uptime */
        G_TYPE_STRING,   /* c_gnet_info */
        G_TYPE_UINT,     /* c_gnet_handle */
        GDK_TYPE_COLOR	 /* c_gnet_fg */
     );

    /*
     * Get the monitor widget
     */
	tree = GTK_TREE_VIEW(lookup_widget(main_window, "treeview_nodes"));
	treeview_nodes = tree;
	gtk_tree_view_set_model(tree, GTK_TREE_MODEL(nodes_model));
	gtk_tree_selection_set_mode(gtk_tree_view_get_selection(tree),
		GTK_SELECTION_MULTIPLE);

	nodes_gui_cell_renderer = gtk_cell_renderer_text_new();
	gtk_cell_renderer_text_set_fixed_height_from_font(
		GTK_CELL_RENDERER_TEXT(nodes_gui_cell_renderer), 1);
	g_object_set(G_OBJECT(nodes_gui_cell_renderer),
	     "foreground-set", TRUE,
	     "xpad", GUI_CELL_RENDERER_XPAD,
	     "ypad", GUI_CELL_RENDERER_YPAD,
	     NULL);

	for (i = 0; i < G_N_ELEMENTS(columns); i++) {
		add_column(tree, columns[i].id, _(columns[i].title), columns[i].attr);
	}
}

static inline void nodes_gui_remove_selected_helper(
	GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
	GSList **list = data;
	guint handle;

	gtk_tree_model_get(model, iter, c_gnet_handle, &handle, (-1));
	*list = g_slist_prepend(*list, GUINT_TO_POINTER(handle));
}

/*
 * find_node:
 *
 * Fetches the GtkTreeIter that points to the row which holds the
 * data about the given node.
 */
static inline GtkTreeIter *find_node(gnet_node_t n)
{
	GtkTreeIter *iter = NULL;

	g_hash_table_lookup_extended(nodes_handles, GUINT_TO_POINTER(n),
				NULL, (gpointer) &iter);
	return iter;
}

/*
 * nodes_gui_update_node_info:
 *
 * Updates vendor, version and info column 
 */
static inline void nodes_gui_update_node_info(
    gnet_node_info_t *n, GtkTreeIter *iter)
{
    time_t now = time((time_t *) NULL);
	static gchar version[32];
    gnet_node_status_t status;

    g_assert(n != NULL);

    if (iter == NULL)
        iter = find_node(n->node_handle);

	g_assert(NULL != iter);

    node_get_status(n->node_handle, &status);
    gm_snprintf(version, sizeof(version), "%d.%d",
		n->proto_major, n->proto_minor);

	gtk_list_store_set(nodes_model, iter, 
		c_gnet_user_agent, n->vendor ? lazy_locale_to_utf8(n->vendor, 0) : NULL,
		c_gnet_version, version,
		c_gnet_info, nodes_gui_common_status_str(&status, now),
		(-1));
}

/*
 * nodes_gui_update_node_flags
 *  
 */
static inline void nodes_gui_update_node_flags(
	gnet_node_t n, gnet_node_flags_t *flags, GtkTreeIter *iter)
{
	gchar buf[256];

    if (iter == NULL)
        iter = find_node(n);

	g_assert(NULL != iter);
	gm_snprintf(buf, sizeof buf, "<tt>%s</tt>",
		nodes_gui_common_flags_str(flags));
	gtk_list_store_set(nodes_model, iter, c_gnet_flags, buf, (-1));
	if (NODE_P_LEAF == flags->peermode || NODE_P_NORMAL == flags->peermode) {
	    GdkColor *color = &(gtk_widget_get_style(GTK_WIDGET(treeview_nodes))
				->fg[GTK_STATE_INSENSITIVE]);
	    gtk_list_store_set(nodes_model, iter, c_gnet_fg, color, (-1));
	}
}

/***
 *** Public functions
 ***/


/*
 * nodes_gui_early_init:
 *
 * Initialize the widgets. Include creation of the actual treeview for
 * other init functions that manipulate it, notably settings_gui_init.
 */
void nodes_gui_early_init(void)
{
    popup_nodes = create_popup_nodes();
    nodes_gui_create_treeview_nodes();
}

/*
 * nodes_gui_init:
 *
 * Initialize the nodes controller. Register callbacks in the backend.
 */
void nodes_gui_init(void) 
{
    GtkTreeView *tree;
	guint32 width[NODES_VISIBLE_COLUMNS];
	gboolean visible[NODES_VISIBLE_COLUMNS];
	guint i;

	treeview_nodes = GTK_TREE_VIEW(lookup_widget(
		main_window, "treeview_nodes"));
	tree = treeview_nodes;

    g_object_set(tree, "fixed_height_mode", TRUE, NULL);

	gui_prop_get_guint32(PROP_NODES_COL_WIDTHS, width, 0, G_N_ELEMENTS(width));
	gui_prop_get_boolean(PROP_NODES_COL_VISIBLE, visible, 0,
		G_N_ELEMENTS(visible));

	for (i = 0; i < G_N_ELEMENTS(width); i++) {
    	set_column(tree, i, width[i], visible[i]);
	}

	nodes_handles = g_hash_table_new_full(
		NULL, NULL, NULL, (GDestroyNotify) w_tree_iter_free);

    ht_node_info_changed = g_hash_table_new(g_direct_hash, g_direct_equal);
    ht_node_flags_changed = g_hash_table_new(g_direct_hash, g_direct_equal);

    node_add_node_added_listener(nodes_gui_node_added);
    node_add_node_removed_listener(nodes_gui_node_removed);
    node_add_node_info_changed_listener(nodes_gui_node_info_changed);
    node_add_node_flags_changed_listener(nodes_gui_node_flags_changed);
}

/*
 * nodes_gui_shutdown:
 *
 * Unregister callbacks in the backend and clean up.
 */
void nodes_gui_shutdown(void)
{
	tree_view_save_widths(treeview_nodes, PROP_NODES_COL_WIDTHS);
	tree_view_save_visibility(treeview_nodes, PROP_NODES_COL_VISIBLE);

    node_remove_node_added_listener(nodes_gui_node_added);
    node_remove_node_removed_listener(nodes_gui_node_removed);
    node_remove_node_info_changed_listener(nodes_gui_node_info_changed);
    node_remove_node_flags_changed_listener(nodes_gui_node_flags_changed);

	gtk_list_store_clear(nodes_model);
	g_object_unref(G_OBJECT(nodes_model));
	nodes_model = NULL;
	gtk_tree_view_set_model(treeview_nodes, NULL);

	g_hash_table_destroy(nodes_handles);
	nodes_handles = NULL;

    g_hash_table_destroy(ht_node_info_changed);
    g_hash_table_destroy(ht_node_flags_changed);

    ht_node_info_changed = NULL;
    ht_node_flags_changed = NULL;
}

/*
 * nodes_gui_remove_node:
 *
 * Removes all references to the given node handle in the gui.
 */
void nodes_gui_remove_node(gnet_node_t n)
{
    GtkTreeIter *iter;

    /* 
     * Make sure node is remove from the "changed" hash tables so
     * we don't try an update later. 
     */
    g_hash_table_remove(ht_node_info_changed, GUINT_TO_POINTER(n));
    g_hash_table_remove(ht_node_flags_changed, GUINT_TO_POINTER(n));

	iter = find_node(n);
	g_assert(NULL != iter);
	gtk_list_store_remove(nodes_model, iter);
	g_hash_table_remove(nodes_handles, GUINT_TO_POINTER(n));
}

/*
 * nodes_gui_add_node:
 *
 * Adds the given node to the gui.
 */
void nodes_gui_add_node(gnet_node_info_t *n, const gchar *type)
{
    GtkTreeIter *iter = w_tree_iter_new();
	static gchar proto_tmp[32];

    g_assert(n != NULL);

   	gm_snprintf(proto_tmp, sizeof(proto_tmp), "%d.%d",
		n->proto_major, n->proto_minor);
    gtk_list_store_append(nodes_model, iter);
    gtk_list_store_set(nodes_model, iter, 
        c_gnet_host,    ip_port_to_gchar(n->ip, n->port),
        c_gnet_flags,    NULL,
        c_gnet_user_agent, n->vendor ? lazy_locale_to_utf8(n->vendor, 0) : NULL,
        c_gnet_version, proto_tmp,
        c_gnet_connected, NULL,
        c_gnet_uptime,  NULL,
        c_gnet_info,    NULL,
        c_gnet_handle,  n->node_handle,
        (-1));
	g_hash_table_insert(nodes_handles,
		GUINT_TO_POINTER(n->node_handle), iter);
}


static inline void update_row(gpointer key, gpointer value, gpointer user_data)
{
	static gchar timestr[SIZE_FIELD_MAX];
	GtkTreeIter *iter = value;
	gnet_node_t n = (gnet_node_t) GPOINTER_TO_UINT(key);
	time_t now = *(time_t *) user_data;
	gnet_node_status_t status;

	g_assert(NULL != iter);
	node_get_status(n, &status);

    /* 
     * Update additional info too if it has recorded changes.
     */
    if (g_hash_table_lookup(ht_node_info_changed, GUINT_TO_POINTER(n))) {
        gnet_node_info_t info;

        g_hash_table_remove(ht_node_info_changed, GUINT_TO_POINTER(n));
        node_fill_info(n, &info);
        nodes_gui_update_node_info(&info, iter);
        node_clear_info(&info);
    }

    if (g_hash_table_lookup(ht_node_flags_changed, GUINT_TO_POINTER(n))) {
        gnet_node_flags_t flags;

        g_hash_table_remove(ht_node_flags_changed, GUINT_TO_POINTER(n));
        node_fill_flags(n, &flags);
        nodes_gui_update_node_flags(n, &flags, iter);
    }

	if (status.connect_date) {
		g_strlcpy(timestr, short_uptime(delta_time(now, status.connect_date)),
			sizeof(timestr));
		gtk_list_store_set(nodes_model, iter, 
			c_gnet_connected, timestr,
			c_gnet_uptime, status.up_date
				? short_uptime(delta_time(now, status.up_date)) : NULL,
			c_gnet_info, nodes_gui_common_status_str(&status, now),
			(-1));
	} else {
		gtk_list_store_set(nodes_model, iter,
			c_gnet_uptime, status.up_date
				? short_uptime(delta_time(now, status.up_date)) : NULL,
			c_gnet_info, nodes_gui_common_status_str(&status, now),
			(-1));
	}
}

/*
 * gui_update_nodes_display
 *
 * Update all the nodes at the same time.
 */

/* FIXME: we should remember for every node when it was last
 *       updated and only refresh every node at most once every
 *       second. This information should be kept in a struct pointed
 *       to by the row user_data and should be automatically freed
 *       when removing the row (see upload stats code).
 */

void nodes_gui_update_nodes_display(time_t now)
{
#define DO_FREEZE FALSE
	static const gboolean do_freeze = DO_FREEZE;
    static time_t last_update = 0;
	gint current_page;
	static GtkNotebook *notebook = NULL;
    GtkTreeModel *model;

	if (gui_debug > 0) {
    	g_message("recorded changed: flags: %d info: %d",
        	g_hash_table_size(ht_node_flags_changed),
        	g_hash_table_size(ht_node_info_changed));
	}

    if (delta_time(now, last_update) < 2)
        return;

	/*
	 * Usually don't perform updates if nobody is watching.  However,
	 * we do need to perform periodic cleanup of dead entries or the
	 * memory usage will grow.  Perform an update every UPDATE_MIN minutes
	 * at least.
	 *		--RAM, 28/12/2003
	 */

	if (notebook == NULL)
		notebook = GTK_NOTEBOOK(lookup_widget(main_window, "notebook_main"));

    current_page = gtk_notebook_get_current_page(notebook);
	if (
		current_page != nb_main_page_gnet &&
		delta_time(now, last_update) < UPDATE_MIN
	) {
		return;
	}

    last_update = now;

	if (do_freeze) {
    	/* "Freeze" view */
    	model = gtk_tree_view_get_model(treeview_nodes);
    	g_object_ref(model);
    	gtk_tree_view_set_model(treeview_nodes, NULL);
	}

	g_hash_table_foreach(nodes_handles, update_row, &now);

	if (do_freeze) {
    	/* "Thaw" view */
    	gtk_tree_view_set_model(treeview_nodes, model);
    	g_object_unref(model);
	} else {
    	gtk_widget_queue_draw(GTK_WIDGET(treeview_nodes));
	}
}

/***
 *** Callbacks
 ***/

/*
 * nodes_gui_node_removed:
 *
 * Callback: called when a node is removed from the backend.
 *
 * Removes all references to the node from the frontend.
 */
static void nodes_gui_node_removed(gnet_node_t n)
{
    if (gui_debug >= 5)
        g_warning("nodes_gui_node_removed(%u)\n", n);

    nodes_gui_remove_node(n);
}

/*
 * nodes_gui_node_added:
 *
 * Callback: called when a node is added from the backend.
 *
 * Adds the node to the gui.
 */
static void nodes_gui_node_added(gnet_node_t n, const gchar *type)
{
    gnet_node_info_t *info;

    if (gui_debug >= 5)
        g_warning("nodes_gui_node_added(%u, %s)\n", n, type);

    info = node_get_info(n);
    nodes_gui_add_node(info, type);
    node_free_info(info);
}

/*
 * nodes_gui_node_info_changed:
 *
 * Callback: called when node information was changed by the backend.
 *
 * This updates the node information in the gui. 
 */
static void nodes_gui_node_info_changed(gnet_node_t n)
{
    g_hash_table_insert(ht_node_info_changed, 
        GUINT_TO_POINTER(n), GUINT_TO_POINTER(1));
#if 0
    gnet_node_info_t info;

    node_fill_info(n, &info);
    nodes_gui_update_node_info(&info, NULL);
    node_clear_info(&info);
#endif
}

/*
 * nodes_gui_node_flags_changed
 *
 * Callback invoked when the node's user-visible flags are changed.
 */
static void nodes_gui_node_flags_changed(gnet_node_t n)
{
    g_hash_table_insert(ht_node_flags_changed, 
        GUINT_TO_POINTER(n), GUINT_TO_POINTER(1));

#if 0
    gnet_node_flags_t flags;

    node_fill_flags(n, &flags);
    nodes_gui_update_node_flags(n, &flags, NULL);
#endif
}

/*
 * nodes_gui_remove_selected
 *
 * Removes all selected nodes from the treeview and disconnects them 
 */
void nodes_gui_remove_selected(void)
{
	GtkTreeView *treeview;
	GtkTreeSelection *selection;
	GSList *node_list = NULL;

	treeview = GTK_TREE_VIEW(lookup_widget(main_window, "treeview_nodes"));
	selection = gtk_tree_view_get_selection(treeview);
	gtk_tree_selection_selected_foreach(selection,
		(gpointer) &nodes_gui_remove_selected_helper, &node_list);
	node_remove_nodes_by_handle(node_list);
	g_slist_free(node_list);
}

/* vi: set ts=4: */
#endif	/* USE_GTK2 */
