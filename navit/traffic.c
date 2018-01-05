/**
 * Navit, a modular navigation system.
 * Copyright (C) 2005-2017 Navit Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

/** @file
 * @brief Contains code related to processing traffic messages into map items.
 *
 * Currently the only map items supported are traffic distortions. More may be added in the future.
 *
 * Traffic distortions are used by Navit to route around traffic problems.
 */

#include <string.h>
#include <time.h>

#ifdef _POSIX_C_SOURCE
#include <sys/types.h>
#endif
#include "glib_slice.h"
#include "config.h"
#include "navit.h"
#include "util.h"
#include "coord.h"
#include "item.h"
#include "map.h"
#include "mapset.h"
#include "route_protected.h"
#include "route.h"
#include "transform.h"
#include "xmlconfig.h"
#include "traffic.h"
#include "plugin.h"
#include "fib.h"
#include "event.h"
#include "callback.h"
#include "debug.h"

/** The penalty applied to an off-road link */
#define PENALTY_OFFROAD 2

/**
 * @brief Private data shared between all traffic instances.
 */
struct traffic_shared_priv {
	GList * messages;           /**< Currently active messages */
	// TODO messages by ID?                 In a later phase…
};

/**
 * @brief A traffic plugin instance.
 *
 * If multiple traffic plugins are loaded, each will have its own `struct traffic` instance.
 */
struct traffic {
	NAVIT_OBJECT
	struct navit *navit;         /**< The navit instance */
	struct traffic_shared_priv *shared; /**< Private data shared between all instances */
	struct traffic_priv *priv;   /**< Private data used by the plugin */
	struct traffic_methods meth; /**< Methods implemented by the plugin */
	struct callback * callback;  /**< The callback function for the idle loop */
	struct event_timeout * timeout; /**< The timeout event that triggers the loop function */
	struct mapset *ms;           /**< The mapset used for routing */
	struct route *rt;            /**< The route to notify of traffic changes */
	struct map *map;             /**< The traffic map, in which traffic distortions are stored */
};

struct traffic_message_priv {
	struct item **items;        /**< The items for this message in the traffic map */
};

/**
 * @brief Private data for the traffic map.
 *
 * If multiple traffic plugins are loaded, the map is shared between all of them.
 */
struct map_priv {
	GList * items;              /**< The map items */
	// TODO items by start/end coordinates? In a later phase…
};

/**
 * @brief Implementation-specific map rect data
 */
struct map_rect_priv {
	struct map_priv *mpriv;     /**< The map to which this map rect refers */
	struct item *item;          /**< The current item, i.e. the last item returned by the `map_rect_get_item` method */
	GList * next_item;          /**< `GList` entry for the next item to be returned by `map_rect_get_item` */
};

/**
 * @brief Implementation-specific item data for traffic map items
 */
struct item_priv {
	struct attr **attrs;        /**< The attributes for the item, `NULL`-terminated */
	struct coord *coords;       /**< The coordinates for the item */
	int coord_count;            /**< The number of elements in `coords` */
	int refcount;               /**< How many references to this item exist */
	char ** message_ids;        /**< Message IDs for the messages associated with this item, `NULL`-terminated */
	struct attr **next_attr;    /**< The next attribute of `item` to be returned by the `item_attr_get` method */
	unsigned int next_coord;    /**< The index of the next coordinate of `item` to be returned by the `item_coord_get` method */
};

/**
 * @brief Data for segments affected by a traffic message.
 *
 * Speed can be specified in three different ways:
 * \li `speed` replaces the maximum speed of the segment, if lower
 * \li `speed_penalty` subtracts the specified amount from the maximum speed of the segment
 * \li `speed_factor` is the percentage of the maximum speed of the segment to be assumed
 *
 * Where more than one of these values is set, the lowest speed applies.
 */
struct seg_data {
	enum item_type type;         /**< The item type; currently only `type_traffic_distortion` is supported */
	int speed;                   /**< The expected speed in km/h (`INT_MAX` for unlimited, 0 indicates
	                              *   that the road is closed) */
	int speed_penalty;           /**< Difference between expected speed and the posted speed limit of
	                              *   the segment (0 for none); the resulting maximum speed is never
	                              *   less than 5 km/h */
	int speed_factor;            /**< Expected speed expressed as a percentage of the posted limit (100
	                              *   for full speed) */
	int delay;                   /**< Expected delay for all segments combined, in 1/10 s */
	struct attr ** attrs;        /**< Additional attributes to add to the segments */
};

static struct seg_data * seg_data_new(void);
static struct item * tm_add_item(struct map *map, enum item_type type, int id_hi, int id_lo,
		struct attr **attrs, struct coord *c, int count, char * id);
static void tm_dump_item(struct item * item);
static void tm_destroy(struct map_priv *priv);
static void tm_coord_rewind(void *priv_data);
static void tm_item_destroy(struct item * item);
static struct item * tm_item_ref(struct item * item);
static struct item * tm_item_unref(struct item * item);
static int tm_coord_get(void *priv_data, struct coord *c, int count);
static void tm_attr_rewind(void *priv_data);
static int tm_attr_get(void *priv_data, enum attr_type attr_type, struct attr *attr);
static struct route_graph * traffic_location_get_route_graph(struct traffic_location * this_,
		struct mapset * ms);
static int traffic_location_match_attributes(struct traffic_location * this_, struct item *item);
static int traffic_message_add_segments(struct traffic_message * this_, struct mapset * ms,
		struct seg_data * data, struct map *map);
static void traffic_location_populate_route_graph(struct traffic_location * this_, struct route_graph * rg,
		struct mapset * ms, int mode);
static void traffic_loop(struct traffic * this_);
static struct traffic * traffic_new(struct attr *parent, struct attr **attrs);
static void traffic_message_dump(struct traffic_message * this_);
static struct seg_data * traffic_message_parse_events(struct traffic_message * this_);
static struct route_graph_point * traffic_route_flood_graph(struct route_graph * rg,
		struct traffic_point * from, struct traffic_point * to);

static struct item_methods methods_traffic_item = {
	tm_coord_rewind,
	tm_coord_get,
	tm_attr_rewind,
	tm_attr_get,
};

/**
 * @brief Creates a new `struct seg_data` and initializes it with default values.
 */
static struct seg_data * seg_data_new(void) {
	struct seg_data * ret = g_new0(struct seg_data, 1);
	ret->type = type_traffic_distortion;
	ret->speed = INT_MAX;
	ret->speed_factor = 100;
	return ret;
}

/**
 * @brief Destroys a traffic map item.
 *
 * This function should never be called directly. Instead, be sure to obtain all references by calling
 * `tm_item_ref()` and destroying them by calling `tm_item_unref()`.
 *
 * @param item The item (its `priv_data` member must point to a `struct item_priv`)
 */
static void tm_item_destroy(struct item * item) {
	int i = 0;
	struct item_priv * priv_data = item->priv_data;

	attr_list_free(priv_data->attrs);
	g_free(priv_data->coords);

	while (priv_data->message_ids && priv_data->message_ids[i]) {
		g_free(priv_data->message_ids[i++]);
	}
	g_free(priv_data->message_ids);

	g_free(item->priv_data);
	g_free(item);
}

/**
 * @brief References a traffic map item.
 *
 * Storing a reference to a traffic map item should always be done by calling this function, passing the
 * item as its argument. This will return the item and increase its reference count by one.
 *
 * Never store a pointer to a traffic item not obtained via this function. Doing so may have undesired
 * side effects as the item will not be aware of the reference to it, and the reference may unexpectedly
 * become invalid, leading to a segmentation fault.
 *
 * @param item The item (its `priv_data` member must point to a `struct item_priv`)
 *
 * @return The item. `NULL` will be returned if the argument is `NULL` or points to an item whose
 * `priv_data` member is `NULL`.
 */
static struct item * tm_item_ref(struct item * item) {
	if (!item)
		return NULL;
	if (!item->priv_data)
		return NULL;
	((struct item_priv *) item->priv_data)->refcount++;
	return item;
}

/**
 * @brief Unreferences a traffic map item.
 *
 * This must be called when destroying a reference to a traffic map item. It will decrease the reference
 * count of the item by one, and destroy the item if the last reference to is is removed.
 *
 * If the unreference operation is successful, this function returns `NULL`. This allows one-line
 * operations such as:
 *
 * {@code some_item = tm_item_unref(some_item);}
 *
 * @param item The item (its `priv_data` member must point to a `struct item_priv`)
 *
 * @return `NULL` if the item was unreferenced successfully, `item` if it points to an item whose
 * `priv_data` member is `NULL`.
 */
/* TODO use weak references in map, remove from map when unreferencing (g_list_remove_all) */
static struct item * tm_item_unref(struct item * item) {
	if (!item)
		return item;
	if (!item->priv_data)
		return item;
	((struct item_priv *) item->priv_data)->refcount--;
	if (((struct item_priv *) item->priv_data)->refcount <= 0) {
		tm_item_destroy(item);
	}
	return NULL;
}

// TODO func to remove item from message (but keep it in the map if referenced elsewhere)

/**
 * @brief Returns an item from the map which matches the supplied data.
 *
 * For now only the item type, start coordinates and end coordinates are compared; attributes are
 * ignored. Inverted coordinates are not considered a match for now.
 *
 * @param mr A map rectangle in the traffic map
 * @param type Type of the item
 * @param attrs The attributes for the item
 * @param c Points to an array of coordinates for the item
 * @param count Number of items in `c`
 */
/*
 * TODO
 * Comparison criteria need to be revisited to properly handle different reports for the same segment.
 * Differences may affect quantifiers (explicit vs. implied speed) or maxspeed vs. delay.
 */
static struct item * tm_find_item(struct map_rect *mr, enum item_type type, struct attr **attrs,
		struct coord *c, int count) {
	struct item * ret = NULL;
	struct item * curr;
	struct item_priv * curr_priv;

	while ((curr = map_rect_get_item(mr)) && !ret) {
		if (curr->type != type)
			continue;
		curr_priv = curr->priv_data;
		if (curr_priv->coords[0].x == c[0].x && curr_priv->coords[0].y == c[0].y
				&& curr_priv->coords[curr_priv->coord_count-1].x == c[count-1].x
				&& curr_priv->coords[curr_priv->coord_count-1].y == c[count-1].y)
			ret = curr;
	}
	return ret;
}

/**
 * @brief Dumps an item to a textfile map.
 *
 * This method writes the item to a textfile map named `distortion.txt` in the default data folder.
 * This map can be added to the active mapset in order for the distortions to be rendered on the map and
 * considered for routing.
 *
 * All data passed to this method is safe to free after the method returns, and doing so is the
 * responsibility of the caller.
 *
 * @param item The item
 */
static void tm_dump_item(struct item * item) {
	struct item_priv * ip = (struct item_priv *) item->priv_data;
	struct attr **attrs = ip->attrs;
	struct coord *c = ip->coords;
	int i;
	char * attr_text;

	/* add the configuration directory to the name of the file to use */
	char *dist_filename = g_strjoin(NULL, navit_get_user_data_directory(TRUE),
									"/distortion.txt", NULL);
	if (dist_filename) {
		FILE *map = fopen(dist_filename,"a");
		if (map) {
			fprintf(map, "type=%s", item_to_name(item->type));
			while (*attrs) {
				attr_text = attr_to_text(*attrs, NULL, 0);
				/* FIXME this may not work properly for all attribute types */
				fprintf(map, " %s=%s", attr_to_name((*attrs)->type), attr_text);
				g_free(attr_text);
				attrs++;
			}
			fprintf(map, "\n");

			for (i = 0; i < ip->coord_count; i++) {
				fprintf(map,"0x%x 0x%x\n", c[i].x, c[i].y);
			}
			fclose(map);
		} else {
			dbg(lvl_error,"could not open file for distortions !!");

		} /* else - if (map) */
		g_free(dist_filename);			/* free the file name */
	} /* if (dist_filename) */
}

/**
 * @brief Dumps the traffic map to a textfile map.
 *
 * This method writes all items to a textfile map named `distortion.txt` in the default data folder.
 * This map can be added to the active mapset in order for the distortions to be rendered on the map and
 * considered for routing.
 *
 * @param map The traffic map
 */
static void tm_dump(struct map * map) {
	/* external method, verifies the public API as well as internal structure */
	struct map_rect * mr;
	struct item * item;

	mr = map_rect_new(map, NULL);
	while ((item = map_rect_get_item(mr)))
		tm_dump_item(item);
	map_rect_destroy(mr);
}

/**
 * @brief Adds an item to the map.
 *
 * If a matching item is already in the map, that item will be returned.
 *
 * All data passed to this method is safe to free after the method returns, and doing so is the
 * responsibility of the caller.
 *
 * @param map The traffic map
 * @param type Type of the item
 * @param id_hi First part of the ID of the item (item IDs have two parts)
 * @param id_lo Second part of the ID of the item
 * @param attrs The attributes for the item
 * @param c Points to an array of coordinates for the item
 * @param count Number of items in `c`
 * @param id Message ID for the associated message
 *
 * @return The map item
 */
static struct item * tm_add_item(struct map *map, enum item_type type, int id_hi, int id_lo,
		struct attr **attrs, struct coord *c, int count, char * id) {
	struct item * ret = NULL;
	struct item_priv * priv_data;
	struct map_rect * mr;

	mr = map_rect_new(map, NULL);
	ret = tm_find_item(mr, type, attrs, c, count);
	/* TODO if (ret), check and (if needed) change attributes for the existing item */
	if (!ret) {
		ret = map_rect_create_item(mr, type);
		ret->id_hi = id_hi;
		ret->id_lo = id_lo;
		ret->map = map;
		ret->meth = &methods_traffic_item;
		priv_data = (struct item_priv *) ret->priv_data;
		priv_data->attrs = attr_list_dup(attrs);
		priv_data->coords = g_memdup(c, sizeof(struct coord) * count);
		priv_data->coord_count = count;
		priv_data->message_ids = g_new0(char *, 2);
		priv_data->message_ids[0] = g_strdup(id);
		priv_data->next_attr = attrs;
		priv_data->next_coord = 0;
		/* TODO use weak references in map, remove from map when unreferencing (g_list_remove_all) */
		ret = tm_item_ref(ret);
	}
	map_rect_destroy(mr);
	//tm_dump_item(ret);
	return ret;
}

/**
 * @brief Destroys (closes) the traffic map.
 *
 * @param priv The private data for the traffic map instance
 */
static void tm_destroy(struct map_priv *priv) {
	g_free(priv);
}

/**
 * @brief Opens a new map rectangle on the traffic map.
 *
 * This function opens a new map rectangle on the route graph's map.
 *
 * @param priv The traffic graph map's private data
 * @param sel The map selection (to restrict search to a rectangle, order and/or item types)
 * @return A new map rect's private data
 */
static struct map_rect_priv * tm_rect_new(struct map_priv *priv, struct map_selection *sel) {
	struct map_rect_priv * mr;
	dbg(lvl_debug,"enter\n");
	mr=g_new0(struct map_rect_priv, 1);
	mr->mpriv = priv;
	mr->next_item = priv->items;
	/* all other pointers are initially NULL */
	return mr;
}

/**
 * @brief Destroys a map rectangle on the traffic map.
 */
static void tm_rect_destroy(struct map_rect_priv *mr) {
	/* just free the map_rect_priv, all its members are pointers to data "owned" by others */
	g_free(mr);
}

/**
 * @brief Returns the next item from the traffic map
 *
 * @param mr The map rect to search for items
 *
 * @return The next item, or `NULL` if the last item has already been retrieved.
 */
static struct item * tm_get_item(struct map_rect_priv *mr) {
	struct item * ret = NULL;
	if (mr->next_item) {
		ret = (struct item *) mr->next_item->data;
		mr->item = ret;
		tm_attr_rewind(ret->priv_data);
		tm_coord_rewind(ret->priv_data);
		mr->next_item = g_list_next(mr->next_item);
	}

	return ret;
}

/**
 * @brief Returns the next item with the supplied ID from the traffic map
 *
 * @param mr The map rect to search for items
 * @param id_hi The high-order portion of the ID
 * @param id_lo The low-order portion of the ID
 *
 * @return The next item matching the ID; `NULL` if there are no matching items or the last matching
 * item has already been retrieved.
 */
static struct item * tm_get_item_byid(struct map_rect_priv *mr, int id_hi, int id_lo) {
	struct item *ret = NULL;
	do {
		ret = tm_get_item(mr);
	} while (ret && (ret->id_lo != id_lo || ret->id_hi != id_hi));
	return ret;
}

/**
 * @brief Creates a new item of the specified type and inserts it into the map.
 *
 * @param mr The map rect in which to create the item
 * @param type The type of item to create
 *
 * @return The new item. The item is of type `type` and has an allocated `priv_data` member; all other
 * members of both structs are `NULL`.
 */
static struct item * tm_rect_create_item(struct map_rect_priv *mr, enum item_type type) {
	struct map_priv * map_priv = mr->mpriv;
	struct item * ret = NULL;
	struct item_priv * priv_data;

	dbg(lvl_error, "enter\n");

	priv_data = g_new0(struct item_priv, 1);

	dbg(lvl_error, "priv_data allocated\n");

	ret = g_new0(struct item, 1);
	ret->type = type;
	ret->priv_data = priv_data;
	map_priv->items = g_list_append(map_priv->items, ret);

	dbg(lvl_error, "return\n");

	return ret;
}

/**
 * @brief Rewinds the coordinates of the currently selected item.
 *
 * After rewinding, the next call to the `tm_coord_get()` will return the first coordinate of the
 * current item.
 *
 * @param priv_data The item's private data
 */
static void tm_coord_rewind(void *priv_data) {
	struct item_priv * ip = priv_data;

	ip->next_coord = 0;
}

/**
 * @brief Returns the coordinates of a traffic item.
 *
 * @param priv_data The item's private data
 * @param c Pointer to a `struct coord` array where coordinates will be stored
 * @param count The maximum number of coordinates to retrieve (must be less than or equal to the number
 * of items `c` can hold)
 * @return The number of coordinates retrieved
 */
static int tm_coord_get(void *priv_data, struct coord *c, int count) {
	struct item_priv * ip = priv_data;
	int ret = count;

	if (!ip)
		return 0;
	if (ip->next_coord >= ip->coord_count)
		return 0;
	if (ip->next_coord + count > ip->coord_count)
		ret = ip->coord_count - ip->next_coord;
	memcpy(c, &ip->coords[ip->next_coord], ret * sizeof(struct coord));
	ip->next_coord += ret;
	return ret;
}

/**
 * @brief Rewinds the attributes of the currently selected item.
 *
 * After rewinding, the next call to `tm_attr_get()` will return the first attribute.
 *
 * @param priv_data The item's private data
 */
static void tm_attr_rewind(void *priv_data) {
	struct item_priv * ip = priv_data;

	ip->next_attr = ip->attrs;
}

/**
 * @brief Returns the next attribute of a traffic item which matches the specified type.
 *
 * @param priv_data The item's private data
 * @param attr_type The attribute type to retrieve, or `attr_any` to retrieve the next attribute,
 * regardless of type
 * @param attr Receives the attribute
 *
 * @return True on success, false on failure
 */
static int tm_attr_get(void *priv_data, enum attr_type attr_type, struct attr *attr) {
	struct item_priv * ip = priv_data;
	int ret = 0;

	if (!ip->next_attr)
		return 0;
	while (*(ip->next_attr) && !ret) {
		ret = (attr_type == attr_any) || (attr_type == (*(ip->next_attr))->type);
		if (ret)
			attr_dup_content(*(ip->next_attr), attr);
		ip->next_attr++;
	}
	return ret;
}

static struct map_methods traffic_map_meth = {
	projection_mg,    /* pro: The projection used for that type of map */
	"utf-8",          /* charset: The charset this map uses. */
	tm_destroy,       /* map_destroy: Destroy ("close") a map. */
	tm_rect_new,      /* map_rect_new: Create a new map rect on the map. */
	tm_rect_destroy,  /* map_rect_destroy: Destroy a map rect */
	tm_get_item,      /* map_rect_get_item: Return the next item from a map rect */
	tm_get_item_byid, /* map_rect_get_item_byid: Get an item with a specific ID from a map rect, can be NULL */
	NULL,             /* map_search_new: Start a new search on the map, can be NULL */
	NULL,             /* map_search_destroy: Destroy a map search struct, ignored if `map_search_new` is NULL */
	NULL,             /* map_search_get_item: Get the next item of a search on the map */
	tm_rect_create_item, /* map_rect_create_item: Create a new item in the map */
	NULL,             /* map_get_attr */
	NULL,             /* map_set_attr */
};

/**
 * @brief Determines the degree to which the attributes of a location and a map item match.
 *
 * The result of this method is used to match a location to a map item. Its result is a score—the higher
 * the score, the better the match.
 *
 * To calculate the score, all supplied attributes are examined. An exact match adds 4 to the score, a
 * partial match adds 2. Values of 1 are subtracted where additional granularity is needed. Undefined
 * attributes are not considered a match.
 *
 * @param this_ The location
 * @param item The map item
 *
 * @return The score
 */
static int traffic_location_match_attributes(struct traffic_location * this_, struct item *item) {
	int ret = 0;
	struct attr attr;

	/* road type */
	if ((this_->road_type != type_line_unspecified)) {
		if (item->type == this_->road_type)
			ret +=4;
		else
			switch (this_->road_type) {
			/* motorway */
			case type_highway_land:
				if (item->type == type_highway_city)
					ret += 3;
				else if (item->type == type_street_n_lanes)
					ret += 2;
				break;
			case type_highway_city:
				if (item->type == type_highway_land)
					ret += 3;
				else if (item->type == type_street_n_lanes)
					ret += 2;
				break;
			/* trunk */
			case type_street_n_lanes:
				if ((item->type == type_highway_land) || (item->type == type_highway_city)
						|| (item->type == type_street_4_land)
						|| (item->type == type_street_4_city))
					ret += 2;
				break;
			/* primary */
			case type_street_4_land:
				if (item->type == type_street_4_city)
					ret += 3;
				else if ((item->type == type_street_n_lanes)
						|| (item->type == type_street_3_land))
					ret += 2;
				else if (item->type == type_street_3_city)
					ret += 1;
				break;
			case type_street_4_city:
				if (item->type == type_street_4_land)
					ret += 3;
				else if ((item->type == type_street_n_lanes)
						|| (item->type == type_street_3_city))
					ret += 2;
				else if (item->type == type_street_3_land)
					ret += 1;
				break;
			/* secondary */
			case type_street_3_land:
				if (item->type == type_street_3_city)
					ret += 3;
				else if ((item->type == type_street_4_land)
						|| (item->type == type_street_2_land))
					ret += 2;
				else if ((item->type == type_street_4_city)
						|| (item->type == type_street_2_city))
					ret += 1;
				break;
			case type_street_3_city:
				if (item->type == type_street_3_land)
					ret += 3;
				else if ((item->type == type_street_4_city)
						|| (item->type == type_street_2_city))
					ret += 2;
				else if ((item->type == type_street_4_land)
						|| (item->type == type_street_2_land))
					ret += 1;
				break;
			/* tertiary */
			case type_street_2_land:
				if (item->type == type_street_2_city)
					ret += 3;
				else if ((item->type == type_street_3_land))
					ret += 2;
				else if ((item->type == type_street_3_city))
					ret += 1;
				break;
			case type_street_2_city:
				if (item->type == type_street_2_land)
					ret += 3;
				else if ((item->type == type_street_3_city))
					ret += 2;
				else if ((item->type == type_street_3_land))
					ret += 1;
				break;
			default:
				break;
			}
	}

	/* road_ref */
	if (this_->road_ref && item_attr_get(item, attr_street_name_systematic, &attr)) {
		// TODO give partial score for partial matches
		if (!compare_name_systematic(this_->road_ref, attr.u.str))
			ret += 4;
	}

	/* road_name */
	if (this_->road_name && item_attr_get(item, attr_street_name, &attr)) {
		// TODO crude comparison in need of refinement
		if (!strcmp(this_->road_name, attr.u.str))
			ret += 4;
	}

	// TODO point->junction_ref
	// TODO point->junction_name

	// TODO direction
	// TODO destination

	// TODO tmc_table, point->tmc_id

	// TODO ramps

	return ret;
}

/**
 * @brief Returns the cost of the segment in the given direction.
 *
 * Currently the cost of a segment (for the purpose of matching traffic locations) is simply its length,
 * as only the best matching roads are used. Future versions may change this by considering all roads
 * and factoring match quality into the cost calculation.
 *
 * @param over The segment
 * @param dir The direction (positive numbers indicate positive direction)
 *
 * @return The cost of the segment, or `INT_MAX` if the segment is impassable in direction `dir`
 */
static int traffic_route_get_seg_cost(struct route_graph_segment *over, int dir) {
	if (over->data.flags & (dir >= 0 ? AF_ONEWAYREV : AF_ONEWAY))
		return INT_MAX;

	return over->data.len;
}

/**
 * @brief Populates a route graph.
 *
 * This method can operate in two modes: In “initial” mode the route graph is populated with the best
 * matching segments, which may not include any ramps. In “add ramps” mode, all ramps within the
 * enclosing rectangle are added, which can be done even after flooding the route graph.
 *
 * @param rg The route graph
 * @param ms The mapset to read the ramps from
 * @param mode 0 to initially populate the route graph, 1 to add ramps
 */
static void traffic_location_populate_route_graph(struct traffic_location * this_, struct route_graph * rg,
		struct mapset * ms, int mode) {
	/* Corners of the enclosing rectangle, in Mercator coordinates */
	struct coord c1, c2;

	/* buffer zone around the rectangle */
	int max_dist = 1000;

	/* The item being processed */
	struct item *item;

	/* Mercator coordinates of current and previous point */
	struct coord c, l;

	/* The attribute matching score (current and maximum) */
	int score, maxscore = 0;

	/* Data for the route graph segment */
	struct route_graph_segment_data data;

	/* The length of the current segment */
#ifdef AVOID_FLOAT
	int len;
#else
	double len;
#endif

	/* Whether the current item is segmented */
	int segmented;

	/* Holds an attribute retrieved from the current item */
	struct attr attr;

	/* Start and end point of the current way or segment */
	struct route_graph_point *s_pnt, *e_pnt;

	if (!(this_->sw && this_->ne))
		return;

	rg->h = mapset_open(ms);

	while ((rg->m = mapset_next(rg->h, 2))) {
		transform_from_geo(map_projection(rg->m), this_->sw, &c1);
		transform_from_geo(map_projection(rg->m), this_->ne, &c2);

		rg->sel = route_rect(18, &c1, &c2, 0, max_dist);

		if (!rg->sel)
			continue;
		rg->mr = map_rect_new(rg->m, rg->sel);
		if (!rg->mr) {
			map_selection_destroy(rg->sel);
			rg->sel = NULL;
			continue;
		}
		while ((item = map_rect_get_item(rg->mr))) {
			/* TODO we might need turn restrictions in mode 1 as well */
			if ((mode == 1) && (item->type != type_ramp))
				continue;
			/* TODO are there any non-routable line types which we can exclude? */
			if ((item->type < type_line) || (item->type >= type_area))
				continue;
			if (item_get_default_flags(item->type)) {

				if (item_coord_get(item, &l, 1)) {
					if (mode == 0) {
						score = traffic_location_match_attributes(this_, item);

						if (score < maxscore)
							continue;
						if (score > maxscore) {
							/* we have found a better match, drop the previous route graph */
							route_graph_free_points(rg);
							route_graph_free_segments(rg);
							maxscore = score;
						}
					}

					data.flags=0;
					data.offset=1;
					data.maxspeed=-1;
					data.item=item;
					len = 0;
					segmented = 0;

					int default_flags_value = AF_ALL;
					int *default_flags = item_get_default_flags(item->type);
					if (!default_flags)
						default_flags = &default_flags_value;
					if (item_attr_get(item, attr_flags, &attr)) {
						data.flags = attr.u.num;
						if (data.flags & AF_SEGMENTED)
							segmented = 1;
					} else
						data.flags = *default_flags;

					if (data.flags & AF_SPEED_LIMIT) {
						if (item_attr_get(item, attr_maxspeed, &attr))
							data.maxspeed = attr.u.num;
					}

					/* clear flags we're not copying here */
					data.flags &= ~(AF_DANGEROUS_GOODS | AF_SIZE_OR_WEIGHT_LIMIT);

					s_pnt = route_graph_add_point(rg, &l);

					if (!segmented) {
						while (item_coord_get(item, &c, 1)) {
							len += transform_distance(map_projection(item->map), &l, &c);
							l = c;
						}
						e_pnt = route_graph_add_point(rg, &l);
						dbg_assert(len >= 0);
						data.len=len;
						if (!route_graph_segment_is_duplicate(s_pnt, &data))
							route_graph_add_segment(rg, s_pnt, e_pnt, &data);
					} else {
						int isseg, rc;
						int sc = 0;
						do {
							isseg = item_coord_is_node(item);
							rc = item_coord_get(item, &c, 1);
							if (rc) {
								len += transform_distance(map_projection(item->map), &l, &c);
								l = c;
								if (isseg) {
									e_pnt = route_graph_add_point(rg, &l);
									data.len = len;
									if (!route_graph_segment_is_duplicate(s_pnt, &data))
										route_graph_add_segment(rg, s_pnt, e_pnt, &data);
									data.offset++;
									s_pnt = route_graph_add_point(rg, &l);
									len = 0;
								}
							}
						} while(rc);
						e_pnt = route_graph_add_point(rg, &l);
						dbg_assert(len >= 0);
						sc++;
						data.len = len;
						if (!route_graph_segment_is_duplicate(s_pnt, &data))
							route_graph_add_segment(rg, s_pnt, e_pnt, &data);
					}
				}
			}
		}
		map_rect_destroy(rg->mr);
		rg->mr = NULL;
	}
	route_graph_build_done(rg, 1);
}

/**
 * @brief Builds a new route graph for traffic location matching.
 *
 * Traffic location matching is done by using a modified routing algorithm to identify the segments
 * affected by a traffic message.
 *
 * @param this_ The location to match to the map
 * @param ms The mapset to use for the route graph
 *
 * @return A route graph. The caller is responsible for destroying the route graph and all related data
 * when it is no longer needed.
 */
static struct route_graph * traffic_location_get_route_graph(struct traffic_location * this_,
		struct mapset * ms) {
	struct route_graph *rg;

	if (!(this_->sw && this_->ne))
		return NULL;

	rg = g_new0(struct route_graph, 1);

	rg->done_cb = NULL;
	rg->busy = 1;

	/* build the route graph */
	traffic_location_populate_route_graph(this_, rg, ms, 0);

	return rg;
}

/**
 * @brief Determines the path between two reference points in a route graph.
 *
 * The reference points `from` and `to` are the beginning and end of the path and do not necessarily
 * coincide with the `from` and `to` members of the location. For a point location with an auxiliary
 * point, one will instead be the `at` member of the location; when examining the opposite direction of
 * a bidirectional location, `from` and `to` will be swapped with respect to the location.
 *
 * The coordinates contained in the reference points are typically approximate, i.e. they do not
 * precisely coincide with a point in the route graph.
 *
 * When this function returns, the route graph will be flooded, i.e. every point will have a cost
 * assigned to it and the `seg` member for each point will be set, indicating the next segment on which
 * to proceed in order to reach the destination. For the last point in the graph, `seg` will be `NULL`.
 * Unlike in common routing, the last point will have a nonzero cost if `to` does not coincide with a
 * point in the route graph.
 *
 * The cost of each node represents the cost to reach `to`. Currently distance is used for cost, with a
 * penalty applied to the offroad connection from the last point in the graph to `to`. Future versions
 * may calculate segment cost differently.
 *
 * To obtain the path, start with the return value. Its `seg` member points to the next segment. Either
 * the `start` or the `end` value of that segment will coincide with the point currently being examined;
 * the other of the two is the point at the other end. Repeat this until you reach a point whose `seg`
 * member is `NULL`.
 *
 * This function can be run multiple times against the same route graph but with different reference
 * points. It is safe to call with `NULL` passed for one or both reference points, in which case `NULL`
 * will be returned.
 *
 * The caller is responsible for freeing up the data structures passed to this function when they are no
 * longer needed.
 *
 * @param rg The route graph
 * @param from Start location
 * @param to Destination location
 *
 * @return The point in the route graph at which the path begins, or `NULL` if no path was found.
 */
static struct route_graph_point * traffic_route_flood_graph(struct route_graph * rg,
		struct traffic_point * from, struct traffic_point * to) {
	struct route_graph_point * ret;

	/* Projected coordinates of start and destination point */
	struct coord c_start, c_dst;

	int i;

	/* This heap will hold all points with "temporarily" calculated costs */
	struct fibheap *heap;

	/* Cost of the start position */
	int start_value;

	/* The point currently being examined */
	struct route_graph_point *p;

	/* Cost of point being examined, other end of segment being examined, segment */
	int min, new, val;

	/* The segment currently being examined */
	struct route_graph_segment *s = NULL;

	if (!from || !to)
		return NULL;

	/* transform coordinates */
	transform_from_geo(projection_mg, &to->coord, &c_dst);
	transform_from_geo(projection_mg, &from->coord, &c_start);

	/* prime the route graph */
	heap = fh_makekeyheap();

	start_value = PENALTY_OFFROAD * transform_distance(projection_mg, &c_start, &c_dst);
	ret = NULL;

	dbg(lvl_debug, "start flooding route graph, start_value=%d\n", start_value);

	for (i = 0; i < HASH_SIZE; i++) {
		p = rg->hash[i];
		while (p) {
			p->value = PENALTY_OFFROAD * transform_distance(projection_mg, &p->c, &c_dst);
			p->el = fh_insertkey(heap, p->value, p);
			p->seg = NULL;
			p = p->hash_next;
		}
	}

	/* flood the route graph */
	for (;;) {
		p = fh_extractmin(heap); /* Starting Dijkstra by selecting the point with the minimum costs on the heap */
		if (!p) /* There are no more points with temporarily calculated costs, Dijkstra has finished */
			break;

		dbg(lvl_debug, "p=0x%x, value=%d\n", p, p->value);

		min = p->value;
		p->el = NULL; /* This point is permanently calculated now, we've taken it out of the heap */
		s = p->start;
		while (s) { /* Iterating all the segments leading away from our point to update the points at their ends */
			val = traffic_route_get_seg_cost(s, -1);

			dbg(lvl_debug, "  negative segment, val=%d\n", val);

			if (val != INT_MAX) {
				new = min + val;
				if (new < s->end->value) { /* We've found a less costly way to reach the end of s, update it */
					s->end->value = new;
					s->end->seg = s;
					if (!s->end->el) {
						s->end->el = fh_insertkey(heap, new, s->end);
					} else {
						fh_replacekey(heap, s->end->el, new);
					}
					new += PENALTY_OFFROAD * transform_distance(projection_mg, &s->end->c, &c_start);
					if (new < start_value) { /* We've found a less costly way from the start point, update */
						start_value = new;
						ret = s->end;
					}
				}
			}
			s = s->start_next;
		}
		s = p->end;
		while (s) { /* Doing the same as above with the segments leading towards our point */
			val = traffic_route_get_seg_cost(s, 1);

			dbg(lvl_debug, "  positive segment, val=%d\n", val);

			if (val != INT_MAX) {
				new = min + val;
				if (new < s->start->value) {
					s->start->value = new;
					s->start->seg = s;
					if (!s->start->el) {
						s->start->el = fh_insertkey(heap, new, s->start);
					} else {
						fh_replacekey(heap, s->start->el, new);
					}
					new += PENALTY_OFFROAD * transform_distance(projection_mg, &s->start->c, &c_start);
					if (new < start_value) {
						start_value = new;
						ret = s->start;
					}
				}
			}
			s = s->end_next;
		}
	}

	fh_deleteheap(heap);
	return ret;
}

/**
 * @brief Generates segments affected by a traffic message.
 *
 * This translates the approximate coordinates in the `from`, `at` and `to` members of the location to
 * one or more map segments, using both the raw coordinates and the auxiliary information contained in
 * the location. Each segment is stored in the map, if not already present, and a link is stored with
 * the message.
 *
 * @param this_ The traffic message
 * @param ms The mapset to use for matching
 * @param data Data for the segments added to the map
 * @param map The traffic map
 *
 * @return `true` if the locations were matched successfully, `false` if there was a failure.
 */
static int traffic_message_add_segments(struct traffic_message * this_, struct mapset * ms,
		struct seg_data * data, struct map *map) {
	int i;

	/* Corners of the enclosing rectangle, in WGS84 coordinates */
	struct coord_geo * sw;
	struct coord_geo * ne;

	struct coord_geo * coords[] = {
			&this_->location->from->coord,
			&this_->location->at->coord,
			&this_->location->to->coord
	};

	/* The direction (positive or negative) */
	int dir = 1;

	/* Next point after start position */
	struct route_graph_point * start_next;

	struct route_graph_segment *s = NULL;

	/* point at which the next segment starts, i.e. up to which the path is complete */
	struct route_graph_point *start;

	/* route graph for simplified routing */
	struct route_graph *rg;

	/* Coordinate count for matched segment */
	int ccnt;

	/* Coordinates of matched segment and pointer into it, order as read from map */
	struct coord *c, ca[2048];

	/* Coordinates of matched segment, sorted */
	struct coord *cd, *cs;

	/* Attributes for traffic distortion */
	struct attr **attrs;

	/* Number of attributes */
	int attr_count;

	/* Speed calculated in various ways */
	int maxspeed, speed, penalized_speed, factor_speed;

	/* Number of segments */
	int count;

	/* Length of location */
	int len;

	/* The next item in the message's list of items */
	struct item ** next_item;

	/* The last item added */
	struct item * item;

	if (!data) {
		dbg(lvl_error, "no data for segments, aborting\n");
		return 0;
	}

	/* calculate enclosing rectangle, if not yet present */
	if (!this_->location->sw) {
		sw = g_new0(struct coord_geo, 1);
		sw->lat = INT_MAX;
		sw->lng = INT_MAX;
		for (i = 0; i < 3; i++)
			if (coords[i]) {
				if (coords[i]->lat < sw->lat)
					sw->lat = coords[i]->lat;
				if (coords[i]->lng < sw->lng)
					sw->lng = coords[i]->lng;
			}
		this_->location->sw = sw;
	}

	if (!this_->location->ne) {
		ne = g_new0(struct coord_geo, 1);
		ne->lat = -INT_MAX;
		ne->lng = -INT_MAX;
		for (i = 0; i < 3; i++)
			if (coords[i]) {
				if (coords[i]->lat > ne->lat)
					ne->lat = coords[i]->lat;
				if (coords[i]->lng > ne->lng)
					ne->lng = coords[i]->lng;
			}
		this_->location->ne = ne;
	}

	if (this_->location->at)
		/* TODO Point location, not supported yet */
		return 0;

	if (this_->location->ramps != location_ramps_none)
		/* TODO Ramps, not supported yet */
		return 0;

	/* Line location, main carriageway */

	rg = traffic_location_get_route_graph(this_->location, ms);

	/* determine segments, once for each direction */
	while (1) {
		if (dir > 0)
			start_next = traffic_route_flood_graph(rg,
					this_->location->from ? this_->location->from : this_->location->at,
					this_->location->to ? this_->location->to : this_->location->at);
		else
			start_next = traffic_route_flood_graph(rg,
					this_->location->to ? this_->location->to : this_->location->at,
					this_->location->from ? this_->location->from : this_->location->at);

		/* calculate route */
		s = start_next ? start_next->seg : NULL;
		start = start_next;

		if (!s)
			dbg(lvl_error, "no segments\n");

		/* count segments and calculate length */
		count = 0;
		len = 0;
		while (s) {
			count++;
			len += s->data.len;
			if (s->start == start)
				start = s->end;
			else
				start = s->start;
			s = start->seg;
		}

		s = start_next ? start_next->seg : NULL;
		start = start_next;

		if (this_->priv->items) {
			dbg(lvl_error, "internal error: message should not yet have any linked items at this point\n");
		}

		this_->priv->items = g_new0(struct item *, count);
		next_item = this_->priv->items;

		while (s) {
			ccnt = item_coord_get_within_range(&s->data.item, ca, 2047, &s->start->c, &s->end->c);
			c = ca;
			cs = g_new0(struct coord, ccnt);
			cd = cs;

			attr_count = 1;
			if ((data->speed != INT_MAX) || data->speed_penalty || (data->speed_factor != 100))
				attr_count++;
			if (data->delay)
				attr_count++;
			if (data->attrs)
				for (attrs = data->attrs; *attrs; attrs++)
					attr_count++;

			attrs = g_new0(struct attr*, attr_count);

			if (data->attrs)
				for (i = 0; data->attrs[i]; i++)
					attrs[i] = data->attrs[i];
			else
				i = 0;

			if ((data->speed != INT_MAX) || data->speed_penalty || (data->speed_factor != 100)) {
				if (s->data.flags & AF_SPEED_LIMIT) {
					maxspeed = RSD_MAXSPEED(&s->data);
				} else {
					switch (s->data.item.type) {
					case type_highway_land:
					case type_street_n_lanes:
						maxspeed = 100;
						break;
					case type_highway_city:
					case type_street_4_land:
						maxspeed = 80;
						break;
					case type_street_3_land:
						maxspeed = 70;
						break;
					case type_street_2_land:
						maxspeed = 65;
						break;
					case type_street_1_land:
						maxspeed = 60;
						break;
					case type_street_4_city:
						maxspeed = 50;
						break;
					case type_ramp:
					case type_street_3_city:
					case type_street_unkn:
						maxspeed = 40;
						break;
					case type_street_2_city:
					case type_track_paved:
						maxspeed = 30;
						break;
					case type_track:
					case type_cycleway:
						maxspeed = 20;
						break;
					case type_roundabout:
					case type_street_1_city:
					case type_street_0:
					case type_living_street:
					case type_street_service:
					case type_street_parking_lane:
					case type_path:
					case type_track_ground:
					case type_track_gravelled:
					case type_track_unpaved:
					case type_track_grass:
					case type_bridleway:
						maxspeed = 10;
						break;
					case type_street_pedestrian:
					case type_footway:
					case type_steps:
						maxspeed = 5;
						break;
					default:
						maxspeed = 50;
					}
				}
				speed = data->speed;
				penalized_speed = maxspeed - data->speed_penalty;
				if (penalized_speed < 5)
					penalized_speed = 5;
				factor_speed = maxspeed * data->speed_factor / 100;
				if (speed > penalized_speed)
					speed = penalized_speed;
				if (speed > factor_speed)
					speed = factor_speed;
				attrs[i] = g_new0(struct attr, 1);
				attrs[i]->type = attr_maxspeed;
				attrs[i]->u.num = speed;
				i++;
			}

			if (data->delay) {
				attrs[i] = g_new0(struct attr, 1);
				attrs[i]->type = attr_delay;
				attrs[i]->u.num = data->delay * s->data.len / len;
			}

			if (s->start == start) {
				/* forward direction, maintain order of coordinates */
				for (i = 0; i < ccnt; i++) {
					*cd++ = *c++;
				}
				start = s->end;
			} else {
				/* backward direction, reverse order of coordinates */
				c += ccnt-1;
				for (i = 0; i < ccnt; i++) {
					*cd++ = *c--;
				}
				start = s->start;
			}

			item = tm_add_item(map, type_traffic_distortion, s->data.item.id_hi, s->data.item.id_lo, attrs, cs, ccnt, this_->id);

			if (((data->speed != INT_MAX) || data->speed_penalty || (data->speed_factor != 100)) && (data->delay))
				g_free(attrs[attr_count - 2]);
			if ((data->speed != INT_MAX) || data->speed_penalty || (data->speed_factor != 100) || data->delay)
				g_free(attrs[attr_count - 1]);
			g_free(attrs);
			g_free(cs);

			*next_item = tm_item_ref(item);
			next_item++;

			s = start->seg;
		}

		/* TODO tweak ends (find the point where the ramp touches the main road) */

		if ((this_->location->directionality == location_dir_one) || (dir < 0))
			break;

		dir = -1;
	}

	route_graph_free_points(rg);
	route_graph_free_segments(rg);
	g_free(rg);

	return 1;
}

/**
 * @brief Prints a dump of a message to debug output.
 *
 * @param this_ The message to dump
 */
static void traffic_message_dump(struct traffic_message * this_) {
	int i, j;
	char * point_names[3] = {"From", "At", "To"};
	struct traffic_point * points[3];

	if (!this_) {
		dbg(lvl_debug, "(null)\n");
		return;
	}

	points[0] = this_->location->from;
	points[1] = this_->location->at;
	points[2] = this_->location->to;

	dbg(lvl_debug, "id='%s', is_cancellation=%d, is_forecast=%d\n",
			this_->id, this_->is_cancellation, this_->is_forecast);
	/* TODO timestamps */

	/* dump replaced message IDs */
	dbg(lvl_debug, "  replaced_count=%d\n",
			this_->replaced_count);
	for (i = 0; i < this_->replaced_count; i++) {
		dbg(lvl_debug, "  Replaces: '%s'\n", this_->replaces[i]);
	}

	/* dump location */
	dbg(lvl_debug, "  Location: road_type='%s', road_ref='%s', road_name='%s'\n",
			item_to_name(this_->location->road_type), this_->location->road_ref,
			this_->location->road_name);
	dbg(lvl_debug, "    directionality=%d, destination='%s', direction='%s'\n",
			this_->location->directionality, this_->location->destination, this_->location->direction);
	dbg(lvl_debug, "    fuzziness=%d, ramps=%d, tmc_table='%s', tmc_direction=%+d\n",
			this_->location->fuzziness, this_->location->ramps, this_->location->tmc_table,
			this_->location->tmc_direction);
	for (i = 0; i < 3; i++) {
		if (points[i]) {
			dbg(lvl_debug, "    %s: lat=%.5f, lng=%.5f\n",
					point_names[i], points[i]->coord.lat, points[i]->coord.lng);
			dbg(lvl_debug, "      junction_name='%s', junction_ref='%s', tmc_id='%s'\n",
					points[i]->junction_name, points[i]->junction_ref, points[i]->tmc_id);

			if (points[i]->map_coord) {
				dbg(lvl_debug, "      Map-matched: pro=%d, x=%x, y=%x\n",
						points[i]->map_coord->pro, points[i]->map_coord->x, points[i]->map_coord->y);
			} else {
				dbg(lvl_debug, "      Map-matched: (null)\n");
			}

			if (points[i]->map_coord_backward) {
				dbg(lvl_debug, "      Map-matched backward: pro=%d, x=%x, y=%x\n",
						points[i]->map_coord_backward->pro, points[i]->map_coord_backward->x,
						points[i]->map_coord_backward->y);
			} else {
				dbg(lvl_debug, "      Map-matched backward: (null)\n");
			}
		} else {
			dbg(lvl_debug, "    %s: (null)\n",
					point_names[i]);
		}
	}

	/* dump events */
	dbg(lvl_debug, "  event_count=%d\n",
			this_->event_count);
	for (i = 0; i < this_->event_count; i++) {
		dbg(lvl_debug, "  Event: event_class=%d, type=%d, length=%d m, speed=%d km/h\n",
				this_->events[i]->event_class, this_->events[i]->type, this_->events[i]->length,
				this_->events[i]->speed);
		/* TODO quantifier */

		/* dump supplementary information */
		dbg(lvl_debug, "    si_count=%d\n",
				this_->events[i]->si_count);
		for (j = 0; j < this_->events[i]->si_count; j++) {
			dbg(lvl_debug, "    Supplementary Information: si_class=%d, type=%d\n",
					this_->events[i]->si[j]->si_class, this_->events[i]->si[j]->type);
			/* TODO quantifier */
		}
	}
}

/**
 * @brief Parses the events of a traffic message.
 *
 * @param message The message to parse
 *
 * @return A `struct seg_data`, or `NULL` if the message contains no usable information
 */
static struct seg_data * traffic_message_parse_events(struct traffic_message * this_) {
	struct seg_data * ret = NULL;

	int i;

	/* Default assumptions, used only if no explicit values are given */
	int speed = INT_MAX;
	int speed_penalty = 0;
	int speed_factor = 100;
	int delay = 0;

	for (i = 0; i < this_->event_count; i++) {
		if (this_->events[i]->speed != INT_MAX) {
			if (!ret)
				ret = seg_data_new();
			if (ret->speed > this_->events[i]->speed)
				ret->speed = this_->events[i]->speed;
		}
		if (this_->events[i]->event_class == event_class_congestion) {
			switch (this_->events[i]->type) {
			case event_congestion_heavy_traffic:
			case event_congestion_traffic_building_up:
			case event_congestion_traffic_heavier_than_normal:
			case event_congestion_traffic_much_heavier_than_normal:
				/* Heavy traffic: assume 10 km/h below the posted limit, unless explicitly specified */
				if ((this_->events[i]->speed == INT_MAX) && (speed_penalty < 10))
					speed_penalty = 10;
				break;
			case event_congestion_slow_traffic:
			case event_congestion_traffic_congestion:
			case event_congestion_traffic_problem:
				/* Slow traffic or unspecified congestion: assume half the posted limit, unless explicitly specified */
				if ((this_->events[i]->speed == INT_MAX) && (speed_factor > 50))
					speed_factor = 50;
				break;
			case event_congestion_queue:
				/* Queuing traffic: assume 20 km/h, unless explicitly specified */
				if ((this_->events[i]->speed == INT_MAX) && (speed > 20))
					speed = 20;
				break;
			case event_congestion_stationary_traffic:
			case event_congestion_long_queue:
				/* Stationary traffic or long queues: assume 5 km/h, unless explicitly specified */
				if ((this_->events[i]->speed == INT_MAX) && (speed > 5))
					speed = 5;
				break;
			default:
				break;
			}
		} else if (this_->events[i]->event_class == event_class_delay) {
			switch (this_->events[i]->type) {
			case event_delay_delay:
			case event_delay_long_delay:
				/* Delay or long delay: assume 30 minutes, unless explicitly specified */
				if (this_->events[i]->quantifier) {
					if (!ret)
						ret = seg_data_new();
					if (ret->delay < this_->events[i]->quantifier->u.q_duration)
						ret->delay = this_->events[i]->quantifier->u.q_duration;
				} else if (delay < 18000)
					delay = 18000;
				break;
			case event_delay_very_long_delay:
				/* Very long delay: assume 1 hour, unless explicitly specified */
				if (this_->events[i]->quantifier) {
					if (!ret)
						ret = seg_data_new();
					if (ret->delay < this_->events[i]->quantifier->u.q_duration)
						ret->delay = this_->events[i]->quantifier->u.q_duration;
				} else if (delay < 36000)
					delay = 36000;
				break;
			case event_delay_several_hours:
				/* Delay of several hours: assume 3 hours */
				if (delay < 108000)
					delay = 108000;
				break;
			case event_delay_uncertain_duration:
				/* TODO */
				break;
			default:
				break;
			}
		} else if (this_->events[i]->event_class == event_class_restriction) {
			switch (this_->events[i]->type) {
			case event_restriction_blocked:
			case event_restriction_blocked_ahead:
			case event_restriction_carriageway_blocked:
			case event_restriction_carriageway_closed:
			case event_restriction_closed:
			case event_restriction_closed_ahead:
				if (!ret)
					ret = seg_data_new();
				if (ret->speed > 0)
					ret->speed = 0;
				break;
			case event_restriction_intermittent_closures:
			case event_restriction_batch_service:
			case event_restriction_single_alternate_line_traffic:
				/* Assume 30% of the posted limit for all of these cases */
				if (speed_factor > 30)
					speed_factor = 30;
				break;
			default:
				break;
			}
		}
	}

	/* use implicit values if no explicit ones are given */
	if ((speed != INT_MAX) || speed_penalty || (speed_factor != 100) || delay) {
		if (!ret)
			ret = seg_data_new();
		if (ret->speed == INT_MAX) {
			ret->speed = speed;
			ret->speed_penalty = speed_penalty;
			ret->speed_factor = speed_factor;
		}
		if (!ret->delay)
			ret->delay = delay;
	}

	return ret;
}

/**
 * @brief Ensures the traffic instance points to valid shared data.
 *
 * This method first examines all registered traffic instances to see if one of them has the `shared`
 * member set. If that is the case, the current instance copies the `shared` pointer of the other
 * instance. Otherwise a new `struct traffic_shared_priv` is created and its address stored in `shared`.
 *
 * Calling this method on a traffic instance with a non-NULL `shared` member has no effect.
 *
 * @param this_ The traffic instance
 */
static void traffic_set_shared(struct traffic *this_) {
	struct attr_iter *iter;
	struct attr attr;
	struct traffic * traffic;

	dbg(lvl_error, "enter\n");

	if (!this_->shared) {
		iter = navit_attr_iter_new();
		while (navit_get_attr(this_->navit, attr_traffic, &attr, iter)) {
			traffic = (struct traffic *) attr.u.navit_object;
			if (traffic->shared)
				this_->shared = traffic->shared;
		}
		navit_attr_iter_destroy(iter);
	}

	if (!this_->shared) {
		this_->shared = g_new0(struct traffic_shared_priv, 1);
	}
}

/**
 * @brief The loop function for the traffic module.
 *
 * This function polls backends for new messages and processes them by inserting, removing or modifying
 * traffic distortions and triggering route recalculations as needed.
 */
static void traffic_loop(struct traffic * this_) {
	int i;
	struct traffic_message ** messages;

	/* Attributes for traffic distortions generated from the current traffic message */
	struct seg_data * data;

	messages = this_->meth.get_messages(this_->priv);
	if (!messages)
		return;

	for (i = 0; messages[i] != NULL; i++) {
		/* TODO identify existing messages replaced by this one */
		/* TODO handle cancellation messages */

		data = traffic_message_parse_events(messages[i]);

		/* TODO ensure we have a map the first time this runs */
		traffic_message_add_segments(messages[i], this_->ms, data, this_->map);

		g_free(data);

		/* store message */
		/* TODO handle replacements */
		this_->shared->messages = g_list_append(this_->shared->messages, messages[i]);

		traffic_message_dump(messages[i]);
	}
	if (i)
		tm_dump(this_->map);
	/* TODO trigger redraw if segments have changed */
	dbg(lvl_debug, "received %d message(s)\n", i);
}

/**
 * @brief Instantiates the traffic plugin
 *
 * At a minimum, `attrs` must contain a `type` attribute matching one of the available traffic plugins.
 *
 * @param parent The parent, usually the Navit instance
 * @param attrs The attributes for the plugin
 *
 * @return A `traffic` instance.
 */
static struct traffic * traffic_new(struct attr *parent, struct attr **attrs) {
	struct traffic *this_;
	struct traffic_priv *(*traffic_new)(struct navit *nav, struct traffic_methods *meth,
			struct attr **attrs, struct callback_list *cbl);
	struct attr *attr;

	attr = attr_search(attrs, NULL, attr_type);
	if (!attr) {
		dbg(lvl_error, "type missing\n");
		return NULL;
	}
	dbg(lvl_debug, "type='%s'\n", attr->u.str);
	traffic_new = plugin_get_category_traffic(attr->u.str);
	dbg(lvl_debug, "new=%p\n", traffic_new);
	if (!traffic_new) {
		dbg(lvl_error, "wrong type '%s'\n", attr->u.str);
		return NULL;
	}
	this_ = (struct traffic *) navit_object_new(attrs, &traffic_func, sizeof(struct traffic));
	if (parent->type == attr_navit)
		this_->navit = parent->u.navit;
	else {
		dbg(lvl_error, "wrong parent type '%s', only navit is permitted\n", attr_to_name(parent->type));
		navit_object_destroy((struct navit_object *) this_);
		return NULL;
	}

	this_->priv = traffic_new(parent->u.navit, &this_->meth, this_->attrs, NULL);
	dbg(lvl_debug, "get_messages=%p\n", this_->meth.get_messages);
	dbg(lvl_debug, "priv=%p\n", this_->priv);
	if (!this_->priv) {
		dbg(lvl_error, "plugin initialization failed\n");
		navit_object_destroy((struct navit_object *) this_);
		return NULL;
	}
	navit_object_ref((struct navit_object *) this_);
	dbg(lvl_debug,"return %p\n", this_);

	// TODO do this once and cycle through all plugins
	this_->callback = callback_new_1(callback_cast(traffic_loop), this_);
	this_->timeout = event_add_timeout(1000, 1, this_->callback); // TODO make interval configurable

	this_->map = NULL;

	if (!this_->shared)
		traffic_set_shared(this_);

	return this_;
}

struct traffic_point * traffic_point_new(float lon, float lat, char * junction_name, char * junction_ref,
		char * tmc_id) {
	struct traffic_point * ret;

	ret = g_new0(struct traffic_point, 1);
	ret->coord.lat = lat;
	ret->coord.lng = lon;
	ret->map_coord = NULL;
	ret->map_coord_backward = NULL;
	ret->junction_name = junction_name ? g_strdup(junction_name) : NULL;
	ret->junction_ref = junction_ref ? g_strdup(junction_ref) : NULL;
	ret->tmc_id = tmc_id ? g_strdup(tmc_id) : NULL;
	return ret;
}

struct traffic_point * traffic_point_new_short(float lon, float lat) {
	return traffic_point_new(lon, lat, NULL, NULL, NULL);
}

void traffic_point_destroy(struct traffic_point * this_) {
	if (this_->map_coord)
		g_free(this_->map_coord);
	if (this_->map_coord_backward)
		g_free(this_->map_coord_backward);
	if (this_->junction_name)
		g_free(this_->junction_name);
	if (this_->junction_ref)
		g_free(this_->junction_ref);
	if (this_->tmc_id)
		g_free(this_->tmc_id);
	g_free(this_);
}

// TODO split CID/LTN?
struct traffic_location * traffic_location_new(struct traffic_point * at, struct traffic_point * from,
		struct traffic_point * to, char * destination, char * direction, enum location_dir directionality,
		enum location_fuzziness fuzziness, enum location_ramps ramps, enum item_type road_type,
		char * road_name, char * road_ref, char * tmc_table, int tmc_direction) {
	struct traffic_location * ret;

	ret = g_new0(struct traffic_location, 1);
	ret->at = at;
	ret->from = from;
	ret->to = to;
	ret->destination = destination ? g_strdup(destination) : NULL;
	ret->direction = direction ? g_strdup(direction) : NULL;
	ret->directionality = directionality;
	ret->fuzziness = fuzziness;
	ret->ramps = ramps;
	ret->road_type = road_type;
	ret->road_name = road_name ? g_strdup(road_name) : NULL;
	ret->road_ref = road_ref ? g_strdup(road_ref) : NULL;
	ret->tmc_table = tmc_table ? g_strdup(tmc_table) : NULL;
	ret->tmc_direction = tmc_direction;
	ret->sw = NULL;
	ret->ne = NULL;
	return ret;
}

struct traffic_location * traffic_location_new_short(struct traffic_point * at, struct traffic_point * from,
		struct traffic_point * to, enum location_dir directionality, enum location_fuzziness fuzziness) {
	return traffic_location_new(at, from, to, NULL, NULL, directionality, fuzziness, location_ramps_none,
			type_line_unspecified, NULL, NULL, NULL, 0);
}

void traffic_location_destroy(struct traffic_location * this_) {
	if (this_->at)
		traffic_point_destroy(this_->at);
	if (this_->from)
		traffic_point_destroy(this_->from);
	if (this_->to)
		traffic_point_destroy(this_->to);
	if (this_->destination)
		g_free(this_->destination);
	if (this_->direction)
		g_free(this_->direction);
	if (this_->road_name)
		g_free(this_->road_name);
	if (this_->road_ref)
		g_free(this_->road_ref);
	if (this_->tmc_table)
		g_free(this_->tmc_table);
	if (this_->sw)
		g_free(this_->sw);
	if (this_->ne)
		g_free(this_->ne);
	g_free(this_);
}

struct traffic_suppl_info * traffic_suppl_info_new(enum si_class si_class, enum si_type type,
		struct quantifier * quantifier) {
	struct traffic_suppl_info * ret;
	ret = g_new0(struct traffic_suppl_info, 1);
	ret->si_class = si_class;
	ret->type = type;
	ret->quantifier = quantifier ? g_memdup(quantifier, sizeof(struct quantifier)) : NULL;
	return ret;
}

void traffic_suppl_info_destroy(struct traffic_suppl_info * this_) {
	if (this_->quantifier)
		g_free(this_->quantifier);
	g_free(this_);
}

struct traffic_event * traffic_event_new(enum event_class event_class, enum event_type type,
		int length, int speed, struct quantifier * quantifier, int si_count, struct traffic_suppl_info ** si) {
	struct traffic_event * ret;

	ret = g_new0(struct traffic_event, 1);
	ret->event_class = event_class;
	ret->type = type;
	ret->length = length;
	ret->speed = speed;
	ret->quantifier = quantifier ? g_memdup(quantifier, sizeof(struct quantifier)) : NULL;
	if (si_count && si) {
		ret->si_count = si_count;
		ret->si = g_memdup(si, sizeof(struct traffic_suppl_info *) * si_count);
	} else {
		ret->si_count = 0;
		ret->si = NULL;
	}
	return ret;
}

struct traffic_event * traffic_event_new_short(enum event_class event_class, enum event_type type) {
	return traffic_event_new(event_class, type, -1, INT_MAX, NULL, 0, NULL);
}

void traffic_event_destroy(struct traffic_event * this_) {
	int i;

	if (this_->quantifier)
		g_free(this_->quantifier);
	if (this_->si) {
		for (i = 0; i < this_->si_count; i++)
			traffic_suppl_info_destroy(this_->si[i]);
		g_free(this_->si);
	}
	g_free(this_);
}

void traffic_event_add_suppl_info(struct traffic_event * this_, struct traffic_suppl_info * si) {
	struct traffic_suppl_info ** si_new;

	if (this_->si_count && this_->si) {
		si_new = g_new0(struct traffic_suppl_info *, this_->si_count + 1);
		memcpy(si_new, this_->si, sizeof(struct traffic_suppl_info *) * this_->si_count);
		si_new[this_->si_count] = si;
		g_free(this_->si);
		this_->si = si_new;
		this_->si_count++;
	} else {
		this_->si = g_new0(struct traffic_suppl_info *, 1);
		this_->si[0] = si;
		this_->si_count = 1;
	}
}

struct traffic_suppl_info * traffic_event_get_suppl_info(struct traffic_event * this_, int index) {
	if (this_->si && (index < this_->si_count))
		return this_->si[index];
	else
		return NULL;
}

struct traffic_message * traffic_message_new(char * id, time_t receive_time, time_t update_time,
		time_t expiration_time, time_t start_time, time_t end_time, int is_cancellation, int is_forecast,
		int replaced_count, char ** replaces, struct traffic_location * location, int event_count,
		struct traffic_event ** events) {
	struct traffic_message * ret;

	ret = g_new0(struct traffic_message, 1);
	ret->id = g_strdup(id);
	ret->receive_time = receive_time;
	ret->update_time = update_time;
	ret->expiration_time = expiration_time;
	ret->start_time = start_time;
	ret->end_time = end_time;
	ret->is_cancellation = is_cancellation;
	ret->is_forecast = is_forecast;
	if (replaced_count && replaces) {
		ret->replaced_count = replaced_count;
		ret->replaces = g_memdup(replaces, sizeof(char *) * replaced_count);
	} else {
		ret->replaced_count = 0;
		ret->replaces = NULL;
	}
	ret->location = location;
	ret->event_count = event_count;
	ret->events = g_memdup(events, sizeof(struct traffic_event *) * event_count);
	ret->priv = g_new0(struct traffic_message_priv, 1);
	ret->priv->items = NULL;
	return ret;
}

struct traffic_message * traffic_message_new_short(char * id, time_t receive_time, time_t update_time,
		time_t expiration_time, int is_cancellation, int is_forecast, struct traffic_location * location,
		int event_count, struct traffic_event ** events) {
	return traffic_message_new(id, receive_time, update_time, expiration_time, 0, 0, is_cancellation,
			is_forecast, 0, NULL, location, event_count, events);
}

struct traffic_message * traffic_message_new_single_event(char * id, time_t receive_time, time_t update_time,
		time_t expiration_time, int is_cancellation, int is_forecast, struct traffic_location * location,
		enum event_class event_class, enum event_type type) {
	struct traffic_event * event;
	struct traffic_event ** events;

	event = traffic_event_new_short(event_class, type);
	events = g_new0(struct traffic_event *, 1);
	events[0] = event;
	return traffic_message_new_short(id, receive_time, update_time, expiration_time, is_cancellation,
			is_forecast, location, 1, events);
	g_free(events);
}

void traffic_message_destroy(struct traffic_message * this_) {
	int i;

	g_free(this_->id);
	if (this_->replaces) {
		for (i = 0; i < this_->replaced_count; i++)
			g_free(this_->replaces[i]);
		g_free(this_->replaces);
	}
	traffic_location_destroy(this_->location);
	for (i = 0; i < this_->event_count; i++)
		traffic_event_destroy(this_->events[i]);
	g_free(this_->events);
	g_free(this_->priv->items);
	g_free(this_->priv);
}

void traffic_message_add_event(struct traffic_message * this_, struct traffic_event * event) {
	struct traffic_event ** events_new;

	events_new = g_new0(struct traffic_event *, this_->event_count + 1);
	memcpy(events_new, this_->events, sizeof(struct traffic_event *) * this_->event_count);
	events_new[this_->event_count] = event;
	g_free(this_->events);
	this_->events = events_new;
	this_->event_count++;
}

struct traffic_event * traffic_message_get_event(struct traffic_message * this_, int index) {
	if (this_->events && (index < this_->event_count))
		return this_->events[index];
	else
		return NULL;
}

/**
 * @brief Registers a new traffic map plugin
 *
 * @param meth Receives the map methods
 * @param attrs The attributes for the map
 * @param cbl
 *
 * @return A pointer to a {@code map_priv} structure for the map
 */
// FIXME make sure we use the return value of this function instead of creating another
static struct map_priv * traffic_map_new(struct map_methods *meth, struct attr **attrs, struct callback_list *cbl) {
	struct map_priv *ret;

	dbg(lvl_error, "enter\n");

	ret = g_new0(struct map_priv, 1);
	*meth = traffic_map_meth;

	dbg(lvl_error, "***** map_priv=0x%x\n", ret);
	return ret; // TODO
}

void traffic_init(void) {
	dbg(lvl_error, "enter\n");
	plugin_register_category_map("traffic", traffic_map_new);
}

/* FIXME
 * Revisit the whole logic of this.
 * map_new() calls through to traffic_map_new, instantiating a struct map_priv, though the instance
 * is only kept internally and not disclosed to anyone. map_new() returns a struct map instance,
 * whose members are not exposed via headers.
 *
 * Creating another map_priv around the map is incorrect usage.
 *
 * We want one map for any number of plugins.
 *
 * We need some kind of data structure which holds the list of items for the map and which is shared
 * by all traffic instances, in addition to being accessible from the map.
 */
struct map * traffic_get_map(struct traffic *this_) {
	struct attr_iter *iter;
	struct attr *attr;
	struct traffic * traffic;

	dbg(lvl_error, "enter\n");

	if (!this_->map) {
		attr = g_new0(struct attr, 1);
		iter = navit_attr_iter_new();
		while (navit_get_attr(this_->navit, attr_traffic, attr, iter)) {
			traffic = (struct traffic *) attr->u.navit_object;
			if (traffic->map)
				this_->map = traffic->map;
		}
		navit_attr_iter_destroy(iter);
		g_free(attr);
	}

	if (!this_->map) {
		struct attr *attrs[4];
		struct attr a_type,data,a_description;
		a_type.type = attr_type;
		a_type.u.str = "traffic";
		data.type = attr_data;
		data.u.str = "";
		a_description.type = attr_description;
		a_description.u.str = "Traffic";

		attrs[0] = &a_type;
		attrs[1] = &data;
		attrs[2] = &a_description;
		attrs[3] = NULL;

		this_->map = map_new(NULL, attrs);
		navit_object_ref((struct navit_object *) this_->map);
	}
	return this_->map;
}

void traffic_set_mapset(struct traffic *this_, struct mapset *ms) {
	this_->ms = ms;
}

void traffic_set_route(struct traffic *this_, struct route *rt) {
	this_->rt = rt;
}

struct object_func traffic_func = {
	attr_traffic,
	(object_func_new)traffic_new,
	(object_func_get_attr)navit_object_get_attr,
	(object_func_iter_new)navit_object_attr_iter_new,
	(object_func_iter_destroy)navit_object_attr_iter_destroy,
	(object_func_set_attr)navit_object_set_attr,
	(object_func_add_attr)navit_object_add_attr,
	(object_func_remove_attr)navit_object_remove_attr,
	(object_func_init)NULL,
	(object_func_destroy)navit_object_destroy,
	(object_func_dup)NULL,
	(object_func_ref)navit_object_ref,
	(object_func_unref)navit_object_unref,
};