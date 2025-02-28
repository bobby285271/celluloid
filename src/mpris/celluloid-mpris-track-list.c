/*
 * Copyright (c) 2017-2019, 2021 gnome-mpv
 *
 * This file is part of Celluloid.
 *
 * Celluloid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Celluloid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Celluloid.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "celluloid-mpris-track-list.h"
#include "celluloid-mpris-gdbus.h"
#include "celluloid-mpris.h"
#include "celluloid-common.h"
#include "celluloid-def.h"

#include <glib/gprintf.h>

enum
{
	PROP_0,
	PROP_CONTROLLER,
	N_PROPERTIES
};

struct _CelluloidMprisTrackList
{
	CelluloidMprisModule parent;
	CelluloidController *controller;
	GHashTable *readonly_table;
	guint reg_id;
};

struct  _CelluloidMprisTrackListClass
{
	CelluloidMprisModuleClass parent_class;
};

static void
register_interface(CelluloidMprisModule *module);

static void
unregister_interface(CelluloidMprisModule *module);

static void
set_property(	GObject *object,
		guint property_id,
		const GValue *value,
		GParamSpec *pspec );

static void
get_property(	GObject *object,
		guint property_id,
		GValue *value,
		GParamSpec *pspec );

static void
method_handler(	GDBusConnection *connection,
		const gchar *sender,
		const gchar *object_path,
		const gchar *interface_name,
		const gchar *method_name,
		GVariant *parameters,
		GDBusMethodInvocation *invocation,
		gpointer data );

static GVariant *
get_prop_handler(	GDBusConnection *connection,
			const gchar *sender,
			const gchar *object_path,
			const gchar *interface_name,
			const gchar *property_name,
			GError **error,
			gpointer data );

static gboolean
set_prop_handler(	GDBusConnection *connection,
			const gchar *sender,
			const gchar *object_path,
			const gchar *interface_name,
			const gchar *property_name,
			GVariant *value,
			GError **error,
			gpointer data );

static void
playlist_handler(	GObject *object,
			GParamSpec *pspec,
			gpointer data );

static void
metadata_update_handler(CelluloidModel *model, gint64 pos, gpointer data);

static void
update_playlist(CelluloidMprisTrackList *track_list);

static gint64
track_id_to_index(const gchar *track_id);

static GVariant *
playlist_entry_to_variant(CelluloidPlaylistEntry *entry, gint64 index);

static GVariant *
get_tracks_metadata(GPtrArray *playlist, const gchar **track_ids);

static void
celluloid_mpris_track_list_class_init(CelluloidMprisTrackListClass *klass);

static void
celluloid_mpris_track_list_init(CelluloidMprisTrackList *track_list);

G_DEFINE_TYPE(CelluloidMprisTrackList, celluloid_mpris_track_list, CELLULOID_TYPE_MPRIS_MODULE);

static void
register_interface(CelluloidMprisModule *module)
{
	CelluloidMprisTrackList *track_list = CELLULOID_MPRIS_TRACK_LIST(module);
	CelluloidModel *model =	celluloid_controller_get_model
				(track_list->controller);
	GDBusConnection *conn;
	GDBusInterfaceInfo *iface;
	GDBusInterfaceVTable vtable;

	g_object_get(module, "conn", &conn, "iface", &iface, NULL);

	celluloid_mpris_module_connect_signal(	module,
						model,
						"notify::playlist",
						G_CALLBACK(playlist_handler),
						module );
	celluloid_mpris_module_connect_signal(	module,
						model,
						"metadata-update",
						G_CALLBACK(metadata_update_handler),
						module );

	celluloid_mpris_module_set_properties
		(	module,
			"Tracks", g_variant_new("ao", NULL),
			"CanEditTracks", g_variant_new_boolean(FALSE),
			NULL );

	vtable.method_call = (GDBusInterfaceMethodCallFunc)method_handler;
	vtable.get_property = (GDBusInterfaceGetPropertyFunc)get_prop_handler;
	vtable.set_property = (GDBusInterfaceSetPropertyFunc)set_prop_handler;

	track_list->reg_id = g_dbus_connection_register_object
				(	conn,
					MPRIS_OBJ_ROOT_PATH,
					iface,
					&vtable,
					module,
					NULL,
					NULL );

	update_playlist(CELLULOID_MPRIS_TRACK_LIST(module));
}

static void
unregister_interface(CelluloidMprisModule *module)
{
	CelluloidMprisTrackList *track_list = CELLULOID_MPRIS_TRACK_LIST(module);
	GDBusConnection *conn = NULL;

	g_object_get(module, "conn", &conn, NULL);
	g_dbus_connection_unregister_object(conn, track_list->reg_id);
}

static void
set_property(	GObject *object,
		guint property_id,
		const GValue *value,
		GParamSpec *pspec )
{
	CelluloidMprisTrackList *self = CELLULOID_MPRIS_TRACK_LIST(object);

	switch(property_id)
	{
		case PROP_CONTROLLER:
		self->controller = g_value_get_pointer(value);
		break;

		default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
		break;
	}
}

static void
get_property(	GObject *object,
		guint property_id,
		GValue *value,
		GParamSpec *pspec )
{
	CelluloidMprisTrackList *self = CELLULOID_MPRIS_TRACK_LIST(object);

	switch(property_id)
	{
		case PROP_CONTROLLER:
		g_value_set_pointer(value, self->controller);
		break;

		default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
		break;
	}
}

static void
method_handler(	GDBusConnection *connection,
		const gchar *sender,
		const gchar *object_path,
		const gchar *interface_name,
		const gchar *method_name,
		GVariant *parameters,
		GDBusMethodInvocation *invocation,
		gpointer data )
{
	CelluloidMprisTrackList *track_list = data;
	CelluloidModel *model =	celluloid_controller_get_model
				(track_list->controller);
	gboolean unknown_method = FALSE;
	GVariant *return_value = NULL;

	if(g_strcmp0(method_name, "GetTracksMetadata") == 0)
	{
		GPtrArray *playlist = NULL;
		const gchar **track_ids = NULL;

		g_object_get(model, "playlist", &playlist, NULL);
		g_variant_get(parameters, "(^a&o)", &track_ids);

		return_value = get_tracks_metadata(playlist, track_ids);

		g_free(track_ids);
	}
	else if(g_strcmp0(method_name, "GoTo") == 0)
	{
		const gchar *track_id;

		g_variant_get(parameters, "(o)", &track_id, NULL);

		if(g_str_has_prefix(track_id, MPRIS_TRACK_ID_PREFIX))
		{
			const gsize prefix_len = sizeof(MPRIS_TRACK_ID_PREFIX)-1;
			gint64 playlist_pos =	g_ascii_strtoll
						(track_id+prefix_len, NULL, 10);

			g_object_set(model, "playlist-pos", playlist_pos, NULL);
		}
		else
		{
			g_warning(	"The GoTo MPRIS method was called with "
					"invalid track ID: %s",
					track_id );
		}

		return_value = g_variant_new("()", NULL);
	}
	else if(g_strcmp0(method_name, "AddTrack") == 0)
	{
		/* Not implemented */
		g_warning(	"The %s MPRIS method was called even though "
				"CanEditTracks property is FALSE",
				method_name );

		return_value = g_variant_new("()", NULL);
	}
	else if(g_strcmp0(method_name, "RemoveTrack") == 0)
	{
		/* Not implemented */
		g_warning(	"The %s MPRIS method was called even though "
				"CanEditTracks property is FALSE",
				method_name );

		return_value = g_variant_new("()", NULL);
	}
	else
	{
		unknown_method = TRUE;
	}

	if(unknown_method)
	{
		g_dbus_method_invocation_return_error
			(	invocation,
				CELLULOID_MPRIS_ERROR,
				CELLULOID_MPRIS_ERROR_UNKNOWN_METHOD,
				"Attempted to call unknown method \"%s\"",
				method_name );
	}
	else
	{
		g_dbus_method_invocation_return_value(invocation, return_value);
	}
}

static GVariant *
get_prop_handler(	GDBusConnection *connection,
			const gchar *sender,
			const gchar *object_path,
			const gchar *interface_name,
			const gchar *property_name,
			GError **error,
			gpointer data )
{
	CelluloidMprisTrackList *track_list = CELLULOID_MPRIS_TRACK_LIST(data);
	CelluloidMprisModule *module = CELLULOID_MPRIS_MODULE(data);
	GVariant *value = NULL;

	if(!g_hash_table_contains(track_list->readonly_table, property_name))
	{
		g_set_error
			(	error,
				CELLULOID_MPRIS_ERROR,
				CELLULOID_MPRIS_ERROR_UNKNOWN_PROPERTY,
				"Failed to get value of unknown property \"%s\"",
				property_name );
	}
	else
	{
		celluloid_mpris_module_get_properties
			(module, property_name, &value, NULL);
	}

	return value?g_variant_ref(value):NULL;
}

static gboolean
set_prop_handler(	GDBusConnection *connection,
			const gchar *sender,
			const gchar *object_path,
			const gchar *interface_name,
			const gchar *property_name,
			GVariant *value,
			GError **error,
			gpointer data )
{
	CelluloidMprisTrackList *track_list = CELLULOID_MPRIS_TRACK_LIST(data);

	if(!g_hash_table_contains(track_list->readonly_table, property_name))
	{
		g_set_error
			(	error,
				CELLULOID_MPRIS_ERROR,
				CELLULOID_MPRIS_ERROR_UNKNOWN_PROPERTY,
				"Failed to set value of unknown property \"%s\"",
				property_name );
	}
	else if(GPOINTER_TO_INT(g_hash_table_lookup(track_list->readonly_table, property_name)))
	{
		g_set_error
			(	error,
				CELLULOID_MPRIS_ERROR,
				CELLULOID_MPRIS_ERROR_SET_READONLY,
				"Attempted to set value of readonly property \"%s\"",
				property_name );
	}

	/* Always fail since the interface only has read-only properties */
	return FALSE;
}

static void
playlist_handler(	GObject *object,
			GParamSpec *pspec,
			gpointer data )
{
	update_playlist(data);
}

static void
metadata_update_handler(CelluloidModel *model, gint64 pos, gpointer data)
{
	GDBusConnection *conn = NULL;
	GDBusInterfaceInfo *iface = NULL;
	gchar *track_id = NULL;
	GVariant *signal_params = NULL;
	GVariant *metadata = NULL;
	GPtrArray *playlist = NULL;

	g_object_get(	G_OBJECT(data),
			"conn", &conn,
			"iface", &iface,
			NULL );
	g_object_get(model, "playlist", &playlist, NULL);

	track_id = g_strdup_printf(	MPRIS_TRACK_ID_PREFIX
					"%" G_GINT64_FORMAT,
					pos );
	metadata = playlist_entry_to_variant(	g_ptr_array_index(playlist, pos),
						pos );
	signal_params = g_variant_new("(o@a{sv})", track_id, metadata);

	g_dbus_connection_emit_signal
		(	conn,
			NULL,
			MPRIS_OBJ_ROOT_PATH,
			iface->name,
			"TrackMetadataChanged",
			signal_params,
			NULL );

	g_free(track_id);
}

static void
update_playlist(CelluloidMprisTrackList *track_list)
{
	CelluloidModel *model =	celluloid_controller_get_model
				(track_list->controller);
	gint64 playlist_pos = -1;
	guint playlist_count = 0;
	GPtrArray *playlist = NULL;
	GDBusConnection *conn = NULL;
	GDBusInterfaceInfo *iface = NULL;
	gchar *current_track = NULL;
	GVariant *tracks = NULL;
	GVariant *signal_params = NULL;
	GVariantBuilder builder;

	g_object_get(	G_OBJECT(model),
			"playlist-pos", &playlist_pos,
			"playlist", &playlist,
			NULL );
	g_object_get(	G_OBJECT(track_list),
			"conn", &conn,
			"iface", &iface,
			NULL );

	playlist_count = playlist?playlist->len:0;

	g_variant_builder_init(&builder, G_VARIANT_TYPE("ao"));

	if(playlist_count <= 0
	|| playlist_pos < 0
	|| playlist_pos > playlist_count-1)
	{
		current_track = g_strdup(MPRIS_TRACK_ID_NO_TRACK);
	}

	for(	gint64 i = MAX(0, playlist_pos-MPRIS_TRACK_LIST_BEFORE);
		i < MIN(playlist_count, playlist_pos+MPRIS_TRACK_LIST_AFTER);
		i++ )
	{
		gchar *path = g_strdup_printf(	MPRIS_TRACK_ID_PREFIX
						"%" G_GINT64_FORMAT,
						i );

		if(i == playlist_pos)
		{
			current_track = g_strdup(path);
		}

		g_variant_builder_add_value
			(&builder, g_variant_new_object_path(path));

		g_free(path);
	}

	tracks = g_variant_new("ao", (playlist_count > 0)?&builder:NULL);
	signal_params = g_variant_new("(@aoo)", tracks, current_track);

	celluloid_mpris_module_set_properties_full
		(	CELLULOID_MPRIS_MODULE(track_list),
			FALSE,
			"Tracks", tracks,
			NULL );

	/* Only emit TrackListReplaced signal. TrackAdded, TrackRemoved, and
	 * TrackMetadataChanged are currently unused.
	 */
	g_dbus_connection_emit_signal
		(	conn,
			NULL,
			MPRIS_OBJ_ROOT_PATH,
			iface->name,
			"TrackListReplaced",
			signal_params,
			NULL );

	g_free(current_track);
}

static gint64
track_id_to_index(const gchar *track_id)
{
	gint64 index = -1;
	gchar *endptr = NULL;

	if(g_str_has_prefix(track_id, MPRIS_TRACK_ID_PREFIX))
	{
		const gsize prefix_len = sizeof(MPRIS_TRACK_ID_PREFIX)-1;

		index = g_ascii_strtoll(track_id+prefix_len, &endptr, 10);
	}

	if(endptr && *endptr)
	{
		g_warning("Failed to parse track ID: %s", track_id);
	}

	return index;
}

static GVariant *
playlist_entry_to_variant(CelluloidPlaylistEntry *entry, gint64 index)
{
	GVariantBuilder builder;
	gchar *track_id = NULL;
	gchar *title = NULL;
	gchar *sanitized_title = NULL;
	gchar *uri = NULL;
	GVariant *elem_value = NULL;

	g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));

	track_id = g_strdup_printf(	"%s%" G_GINT64_FORMAT,
					MPRIS_TRACK_ID_PREFIX,
					index );
	elem_value =	g_variant_new
			(	"{sv}",
				"mpris:trackid",
				g_variant_new_string(track_id) );
	g_variant_builder_add_value(&builder, elem_value);

	title =	entry->title?
		g_strdup(entry->title):
		get_name_from_path(entry->filename);
	sanitized_title = sanitize_utf8(title, TRUE);
	elem_value =	g_variant_new
			(	"{sv}",
				"xesam:title",
				g_variant_new_string(sanitized_title) );
	g_variant_builder_add_value(&builder, elem_value);

	uri =	g_filename_to_uri(entry->filename, NULL, NULL)?:
		g_strdup(entry->filename);
	elem_value =	g_variant_new
			(	"{sv}",
				"xesam:uri",
				g_variant_new_string(uri) );
	g_variant_builder_add_value(&builder, elem_value);

	g_free(track_id);
	g_free(title);
	g_free(sanitized_title);
	g_free(uri);

	return g_variant_new("a{sv}", &builder);
}

static GVariant *
get_tracks_metadata(GPtrArray *playlist, const gchar **track_ids)
{
	GVariantBuilder builder;

	g_assert(playlist);
	g_assert(track_ids);

	g_variant_builder_init(&builder, G_VARIANT_TYPE("aa{sv}"));

	for(const gchar **iter = track_ids; *iter; iter++)
	{
		gint64 index = track_id_to_index(*iter);

		if(index >= 0 && index < playlist->len)
		{
			CelluloidPlaylistEntry *entry;
			GVariant *elem;

			entry = g_ptr_array_index(playlist, index);
			elem = playlist_entry_to_variant(entry, index);

			g_variant_builder_add_value(&builder, elem);
		}
		else
		{
			g_warning(	"Attempted to retrieve metadata of "
					"non-existent track ID: %s",
					*iter );
		}
	}

	return g_variant_new("(aa{sv})", &builder);
}

static void
celluloid_mpris_track_list_class_init(CelluloidMprisTrackListClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	CelluloidMprisModuleClass *module_class =
		CELLULOID_MPRIS_MODULE_CLASS(klass);
	GParamSpec *pspec = NULL;

	object_class->set_property = set_property;
	object_class->get_property = get_property;
	module_class->register_interface = register_interface;
	module_class->unregister_interface = unregister_interface;

	pspec = g_param_spec_pointer
		(	"controller",
			"Controller",
			"The CelluloidController to use",
			G_PARAM_CONSTRUCT_ONLY|G_PARAM_READWRITE );
	g_object_class_install_property(object_class, PROP_CONTROLLER, pspec);
}

static void
celluloid_mpris_track_list_init(CelluloidMprisTrackList *track_list)
{
	const struct
	{
		const gchar *name;
		gboolean readonly;
	}
	properties[] =
	{
		{"Tracks", TRUE},
		{"Fullscreen", TRUE},
		{NULL, FALSE}
	};

	track_list->controller =
		NULL;
	track_list->readonly_table =
		g_hash_table_new_full(g_str_hash, g_int_equal, g_free, NULL);
	track_list->reg_id =
		0;

	for(gint i = 0; properties[i].name; i++)
	{
		g_hash_table_replace
			(	track_list->readonly_table,
				g_strdup(properties[i].name),
				GINT_TO_POINTER(properties[i].readonly) );
	}
}

CelluloidMprisModule *
celluloid_mpris_track_list_new(	CelluloidController *controller,
				GDBusConnection *conn )
{
	GDBusInterfaceInfo *iface;
	GObject *object;

	iface = celluloid_mpris_org_mpris_media_player2_track_list_interface_info();
	object = g_object_new(	celluloid_mpris_track_list_get_type(),
				"controller", controller,
				"conn", conn,
				"iface", iface,
				NULL );

	return CELLULOID_MPRIS_MODULE(object);
}

