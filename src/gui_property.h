/*
 * Copyright (c) 2001-2002, Richard Eckart
 *
 * THIS FILE IS AUTOGENERATED! DO NOT EDIT!
 * This file is generated from gui_props.ag using autogen.
 * Autogen is available at http://autogen.sourceforge.net/.
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

#ifndef _gui_property_h_
#define _gui_property_h_

#include "prop.h"

#define GUI_PROPERTY_MIN (1000)
#define GUI_PROPERTY_MAX (1000+GUI_PROPERTY_END-1)
#define GUI_PROPERTY_NUM (GUI_PROPERTY_END-1000)
 
typedef enum {    
    PROP_MONITOR_ENABLED=1000,    
    PROP_MONITOR_MAX_ITEMS,    
    PROP_QUEUE_REGEX_CASE,    
    PROP_SEARCH_AUTOSELECT,    
    PROP_NODES_COL_WIDTHS,    
    PROP_DL_ACTIVE_COL_WIDTHS,    
    PROP_DL_QUEUED_COL_WIDTHS,    
    PROP_SEARCH_RESULTS_COL_VISIBLE,    
    PROP_SEARCH_LIST_COL_WIDTHS,    
    PROP_SEARCH_RESULTS_COL_WIDTHS,    
    PROP_SEARCH_STATS_COL_WIDTHS,    
    PROP_UL_STATS_COL_WIDTHS,    
    PROP_UPLOADS_COL_WIDTHS,    
    PROP_FILTER_RULES_COL_WIDTHS,    
    PROP_FILTER_FILTERS_COL_WIDTHS,    
    PROP_GNET_STATS_MSG_COL_WIDTHS,    
    PROP_GNET_STATS_FC_TTL_COL_WIDTHS,    
    PROP_GNET_STATS_FC_HOPS_COL_WIDTHS,    
    PROP_GNET_STATS_FC_COL_WIDTHS,    
    PROP_GNET_STATS_DROP_REASONS_COL_WIDTHS,    
    PROP_GNET_STATS_RECV_COL_WIDTHS,    
    PROP_WINDOW_COORDS,    
    PROP_FILTER_DLG_COORDS,    
    PROP_DOWNLOADS_DIVIDER_POS,    
    PROP_MAIN_DIVIDER_POS,    
    PROP_GNET_STATS_DIVIDER_POS,    
    PROP_SIDE_DIVIDER_POS,    
    PROP_SEARCH_MAX_RESULTS,    
    PROP_GUI_DEBUG,    
    PROP_FILTER_MAIN_DIVIDER_POS,    
    PROP_SEARCH_RESULTS_SHOW_TABS,    
    PROP_TOOLBAR_VISIBLE,    
    PROP_STATUSBAR_VISIBLE,    
    PROP_PROGRESSBAR_UPLOADS_VISIBLE,    
    PROP_PROGRESSBAR_DOWNLOADS_VISIBLE,    
    PROP_PROGRESSBAR_CONNECTIONS_VISIBLE,    
    PROP_PROGRESSBAR_BWS_IN_VISIBLE,    
    PROP_PROGRESSBAR_BWS_OUT_VISIBLE,    
    PROP_PROGRESSBAR_BWS_GIN_VISIBLE,    
    PROP_PROGRESSBAR_BWS_GOUT_VISIBLE,    
    PROP_PROGRESSBAR_BWS_IN_AVG,    
    PROP_PROGRESSBAR_BWS_OUT_AVG,    
    PROP_PROGRESSBAR_BWS_GIN_AVG,    
    PROP_PROGRESSBAR_BWS_GOUT_AVG,    
    PROP_SEARCH_AUTOSELECT_IDENT,    
    PROP_JUMP_TO_DOWNLOADS,    
    PROP_SHOW_SEARCH_RESULTS_SETTINGS,    
    PROP_SEARCH_AUTOSELECT_FUZZY,    
    PROP_DEFAULT_MINIMUM_SPEED,    
    PROP_SEARCH_STATS_MODE,    
    PROP_SEARCH_STATS_UPDATE_INTERVAL,    
    PROP_SEARCH_STATS_DELCOEF,    
    PROP_CONFIRM_QUIT,    
    PROP_SHOW_TOOLTIPS,    
    PROP_EXPERT_MODE,    
    PROP_GNET_STATS_PERC,    
    PROP_GNET_STATS_BYTES,    
    PROP_GNET_STATS_HOPS,    
    PROP_GNET_STATS_WITH_HEADERS,    
    PROP_GNET_STATS_DROP_PERC,    
    PROP_GNET_STATS_GENERAL_COL_WIDTHS,    
    PROP_AUTOCLEAR_UPLOADS,    
    PROP_TREEMENU_NODES_EXPANDED,    
    PROP_GNET_STATS_PKG_COL_WIDTHS,    
    PROP_GNET_STATS_BYTE_COL_WIDTHS,    
    PROP_CONFIG_TOOLBAR_STYLE,
    GUI_PROPERTY_END
} gui_property_t;

/*
 * Property set stub
 */
prop_set_stub_t *gui_prop_get_stub(void);

/*
 * Property definition
 */
prop_def_t *gui_prop_get_def(property_t);

/*
 * Property-change listeners
 */
void gui_prop_add_prop_changed_listener(
    property_t, prop_changed_listener_t, gboolean);
void gui_prop_remove_prop_changed_listener(
    property_t, prop_changed_listener_t);

/*
 * get/set functions
 *
 * The *_val macros are shortcuts for single scalar properties.
 */
void gui_prop_set_boolean(
    property_t, const gboolean *, gsize, gsize);
gboolean *gui_prop_get_boolean(
    property_t, gboolean *, gsize, gsize);

#define gui_prop_set_boolean_val(p, v) do { \
	gboolean value = v; \
	gui_prop_set_boolean(p, &value, 0, 1); \
} while (0)

#define gui_prop_get_boolean_val(p, v) do { \
	gui_prop_get_boolean(p, v, 0, 1); \
} while (0)


void gui_prop_set_string(property_t, const gchar *);
gchar *gui_prop_get_string(property_t, gchar *, gsize);

void gui_prop_set_guint32(
    property_t, const guint32 *, gsize, gsize);
guint32 *gui_prop_get_guint32(
    property_t, guint32 *, gsize, gsize);

#define gui_prop_set_guint32_val(p, v) do { \
	guint32 value = v; \
	gui_prop_set_guint32(p, &value, 0, 1); \
} while (0)

#define gui_prop_get_guint32_val(p, v) do { \
	gui_prop_get_guint32(p, v, 0, 1); \
} while (0)

void gui_prop_set_storage(property_t, const guint8 *, gsize);
guint8 *gui_prop_get_storage(property_t, guint8 *, gsize);


#endif /* _gui_property_h_ */

