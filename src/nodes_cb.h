/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi, Richard Eckart
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

#ifndef _nodes_cb_h_
#define _nodes_cb_h_

#include <gtk/gtk.h>

/***
 *** nodes panel
 ***/

gboolean on_clist_nodes_button_press_event (GtkWidget *widget, GdkEventButton *event, gpointer user_data);
void on_clist_nodes_resize_column (GtkCList *clist, gint column, gint width, gpointer user_data);
void on_clist_nodes_select_row (GtkCList *clist, gint row, gint column, GdkEvent *event, gpointer user_data);
void on_clist_nodes_unselect_row (GtkCList *clist, gint row, gint column, GdkEvent *event, gpointer user_data);
void on_button_nodes_add_clicked (GtkButton *button, gpointer user_data); 
void on_button_nodes_remove_clicked (GtkButton *button, gpointer user_data);
void on_entry_host_activate (GtkEditable *editable, gpointer user_data); 
void on_entry_host_changed (GtkEditable *editable, gpointer user_data);

/***
 *** popup-nodes
 ***/
void on_popup_nodes_remove_activate (GtkMenuItem *menuitem, gpointer user_data); 

#endif /* _nodes_cb_h_ */
