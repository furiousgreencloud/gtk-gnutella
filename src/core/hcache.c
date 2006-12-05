/*
 * $Id$
 *
 * Copyright (c) 2002-2003, Raphael Manfredi, Richard Eckart
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

/**
 * @ingroup core
 * @file
 *
 * Host cache management.
 *
 * @author Raphael Manfredi
 * @author Richard Eckart
 * @date 2002-2003
 *
 * @todo
 * TODO:
 *
 *	- finer grained stats:
 *		-# hits/misses while adding,
 *		-# hits/misses while bad checking
 *		-# how many hosts were tried to connect to?
 *	- display stats about gwcache usage:
 *		-# how often
 *		-# how many hosts got
 *	- move unstable servant code from nodes.c to hcache.c
 *	- make sure hosts we are currently connected too are also saved
 *		to disk on exit!
 *	- save more metadata if we can make use of it.
 *
 */

#include "common.h"

RCSID("$Id$")

#include "hosts.h"
#include "hcache.h"
#include "pcache.h"
#include "nodes.h"
#include "settings.h"
#include "bogons.h"
#include "hostiles.h"

#include "lib/bg.h"
#include "lib/file.h"
#include "lib/hashlist.h"
#include "lib/tm.h"
#include "lib/walloc.h"

#include "if/gnet_property.h"
#include "if/gnet_property_priv.h"

#include "lib/override.h"			/* Must be the last header included */

#define MIN_RESERVE_SIZE	1024	/**< we'd like that many pongs in reserve */

/**
 * An entry within the hostcache.
 *
 * We don't really store the IP/port, as those are stored in the key of
 * hash table recording all known hosts.  Rather, we store "metadata" about
 * the host.
 */
typedef struct hostcache_entry {
    hcache_type_t type;				/**< Hostcache which contains this host */
    time_t        time_added;		/**< Time when entry was added */
#if 0
	guint32       avg_uptime;		/**< Reported average uptime (seconds) */
	gchar *       vendor;			/**< Latest known vendor name (atom) */
#endif
} hostcache_entry_t;

/** No metadata for host */
#define NO_METADATA			GINT_TO_POINTER(1)

/**
 * A hostcache table.
 */
typedef struct hostcache {
	const gchar		*name;		        /**< Name of the cache */
	hcache_type_t   type;				/**< Cache type */

    gboolean        addr_only;          /**< Use IP only, port always 0 */
    gboolean        dirty;            	/**< If updated since last disk flush */
    hash_list_t *   hostlist;           /**< Host list: IP/Port  */

    guint           hits;               /**< Hits to the cache */
    guint           misses;             /**< Misses to the cache */

    guint           host_count;			/**< Amount of hosts in cache */
	gnet_property_t reading;			/**< Property to signal reading */
    gnet_property_t hosts_in_catcher;   /**< Property to update host count */
    gint            mass_update;        /**< If a mass update is in progess */
} hostcache_t;

static hostcache_t *caches[HCACHE_MAX];

static gboolean hcache_close_running = FALSE;

/**
 * Names of the host caches.
 *
 * @note
 * Has to be in the same order as in the hcache_type_t definition
 * in gnet_nodes.h.
 */
static const gchar * const names[HCACHE_MAX] = {
    "fresh regular",
    "valid regular",
    "fresh ultra",
    "valid ultra",
    "timeout",
    "busy",
    "unstable",
    "none",
};

static const gchar * const host_type_names[HOST_MAX] = {
    "any",
    "ultra",
};


static gpointer bg_reader[HCACHE_MAX];

enum {
    HCACHE_ALREADY_CONNECTED,
    HCACHE_INVALID_HOST,
    HCACHE_LOCAL_INSTANCE,
    HCACHE_STATS_MAX
};

static guint stats[HCACHE_STATS_MAX];

/**
 * Initiate mass update of host cache. While mass updates are in
 * progress, the hosts_in_catcher property will not be updated.
 */
static void
start_mass_update(hostcache_t *hc)
{
    hc->mass_update++;
}

/**
 * End mass update of host cache. If a hostcache has already been freed
 * when this function is called, it will not tigger a property update
 * for that hostcache and all of its group (ANY, ULTRA, BAD).
 */
static void
stop_mass_update(hostcache_t *hc)
{
    g_assert(hc->mass_update > 0);
    hc->mass_update--;

    if (hc->mass_update == 0) {
        switch (hc->type) {
        case HCACHE_FRESH_ANY:
        case HCACHE_VALID_ANY: {
            gnet_prop_set_guint32_val(hc->hosts_in_catcher,
                caches[HCACHE_FRESH_ANY]->host_count +
                caches[HCACHE_VALID_ANY]->host_count);
            break;
        }
        case HCACHE_FRESH_ULTRA:
        case HCACHE_VALID_ULTRA:
            gnet_prop_set_guint32_val(hc->hosts_in_catcher,
                caches[HCACHE_FRESH_ULTRA]->host_count +
                caches[HCACHE_VALID_ULTRA]->host_count);
            break;
        case HCACHE_TIMEOUT:
        case HCACHE_UNSTABLE:
        case HCACHE_BUSY:
            gnet_prop_set_guint32_val(hc->hosts_in_catcher,
                caches[HCACHE_TIMEOUT]->host_count +
                caches[HCACHE_UNSTABLE]->host_count +
                caches[HCACHE_BUSY]->host_count);
            break;
        default:
            g_error("stop_mass_update: unknown cache type: %d", hc->type);
        }
    }
}

/**
 * Hashtable: IP/Port -> Metadata
 */
static GHashTable * ht_known_hosts = NULL;

static void
hcache_update_low_on_pongs(void)
{
    host_low_on_pongs = (guint) (
            caches[HCACHE_FRESH_ANY]->host_count +
            caches[HCACHE_VALID_ANY]->host_count
        ) < (max_hosts_cached >> 3);
}

/***
 *** Metadata allocation.
 ***/

static hostcache_entry_t *
hce_alloc(void)
{
	return walloc0(sizeof(struct hostcache_entry));
}

static void
hce_free(struct hostcache_entry *hce)
{
	g_assert(hce && hce != NO_METADATA);
	wfree(hce, sizeof(*hce));
}

/**
 * Output contents information about a hostcache.
 */
static void
hcache_dump_info(const struct hostcache *hc, const gchar *what)
{
    g_message("[%s|%s] %u (%d) hosts (%u hits, %u misses)",
        hc->name, what, hc->host_count,
        hash_list_length(hc->hostlist),
        hc->hits, hc->misses);
}

/***
 *** Hostcache access.
 ***/

/**
 * Get information about the host entry, both the host and the metadata.
 *
 * @param addr	the address of the host
 * @param port	the port used by the host
 * @param h		filled with the host entry in the table
 * @param e		filled with the meta data of the host, as held in table
 *
 * @return FALSE if entry was not found in the cache.
 */
static gboolean
hcache_ht_get(const host_addr_t addr, guint16 port,
	gnet_host_t **h, hostcache_entry_t **e)
{
	gnet_host_t host;
	gpointer k, v;
	gboolean found;

	gnet_host_set(&host, addr, port);

	found = g_hash_table_lookup_extended(ht_known_hosts, &host, &k, &v);
	if (found) {
		*h = k;
		*e = v;
	}

	return found;
}

/**
 * Add host to the hash table host cache.
 *
 * Also creates a metadata struct unless the host was added to HL_CAUGHT
 * in which case we cannot know anything about the host. Yet we cannot
 * assert that HL_CAUGHT never contains a host with metadata because when
 * HL_CAUGHT becomes empty, move all hosts from HL_VALID to HL_CAUGHT. We
 * can however assert that any host which does not have metadata is in
 * HL_CAUGHT.
 *
 * @return Pointer to metadata struct for the added host or NO_METADATA
 *         if no metadata was added.
 */
static hostcache_entry_t *
hcache_ht_add(hcache_type_t type, gnet_host_t *host)
{
    hostcache_entry_t *hce;

    hce = hce_alloc();
    hce->type = type;
    hce->time_added = tm_time();

	g_hash_table_insert(ht_known_hosts, host, hce);

    return hce;
}

/**
 * Remove host from the hash table host cache.
 */
static void
hcache_ht_remove(gnet_host_t *host)
{
	union {
		hostcache_entry_t *hce;
		gpointer ptr;
	} entry;
	gpointer key;
	gboolean found;

	found = g_hash_table_lookup_extended(ht_known_hosts,
		(gconstpointer) host, &key, &entry.ptr);

	if (!found) {
		g_warning("hcache_ht_remove: attempt to remove unknown host: %s",
			  gnet_host_to_string(host));
		return;
	}

	g_hash_table_remove(ht_known_hosts, host);

	if (entry.hce != NO_METADATA)
		hce_free(entry.hce);
}

/**
 * Get metadata for host.
 *
 * @return NULL if host was not found, NO_METADATA if no metadata was stored
 *         or a pointer to a hostcache_entry struct which hold the metadata.
 */
static hostcache_entry_t *
hcache_get_metadata(gnet_host_t *host)
{
    return g_hash_table_lookup(ht_known_hosts, (gconstpointer) host);
}

/**
 * @return TRUE if the host is in one of the "bad hosts" caches.
 */
gboolean
hcache_node_is_bad(const host_addr_t addr)
{
    hostcache_entry_t *hce;
    gnet_host_t h;

	/*
	 * When we're low on pongs, we cannot afford the luxury of discarding
	 * any IP address, or we'll end up contacting web caches for more.
	 */

	if (host_low_on_pongs)
		return FALSE;

	gnet_host_set(&h, addr, 0);
    hce = hcache_get_metadata(&h);

    if ((hce == NULL) || (hce == NO_METADATA))
        return FALSE;

    caches[hce->type]->hits++;

    switch (hce->type) {
    case HCACHE_FRESH_ANY:
    case HCACHE_VALID_ANY:
    case HCACHE_FRESH_ULTRA:
    case HCACHE_VALID_ULTRA:
        return FALSE;
    default:
        return TRUE;
    }
}

/**
 * Move entries from one hostcache to another. This only works if the
 * target hostcache is empty.
 */
static void
hcache_move_entries(hostcache_t *to, hostcache_t *from)
{
	hash_list_iter_t *iter;
	gpointer item;

    g_assert(hash_list_length(to->hostlist) == 0);
    g_assert(to->host_count == 0);

	hash_list_free(to->hostlist);
    to->hostlist = from->hostlist;
    to->host_count = from->host_count;
    from->hostlist = hash_list_new(NULL, NULL);
    from->host_count = 0;

    /*
     * Make sure that after switching hce->list points to the new
     * list HL_CAUGHT
     */

	iter = hash_list_iterator(to->hostlist);

	while (NULL != (item = hash_list_next(iter))) {
        hostcache_entry_t *hce;

        hce = hcache_get_metadata((gnet_host_t *) item);
        if (hce == NULL || hce == NO_METADATA)
            continue;
        hce->type = to->type;
    }

	hash_list_release(iter);
}

/**
 * Make sure we have some host available in HCACHE_FRESH_ANY
 * and HCACHE_FRESH_ULTRA.
 *
 * If one of the both is empty, all hosts from HCACHE_VALID_XXX
 * are moved to HCACHE_VALID_XXX. When caught on other hcaches than
 * FRESH_ANY and FRESH_ULTRA nothing happens.
 *
 * @return TRUE if host are available in hc after the call.
 */
static gboolean
hcache_require_caught(hostcache_t *hc)
{
    g_assert(NULL != hc);

    switch (hc->type) {
    case HCACHE_FRESH_ANY:
    case HCACHE_VALID_ANY:
        if (caches[HCACHE_FRESH_ANY]->host_count == 0) {
            hcache_move_entries(
                caches[HCACHE_FRESH_ANY], caches[HCACHE_VALID_ANY]);
        }
        return caches[HCACHE_FRESH_ANY]->host_count != 0;
    case HCACHE_FRESH_ULTRA:
    case HCACHE_VALID_ULTRA:
        if (caches[HCACHE_FRESH_ULTRA]->host_count == 0) {
            hcache_move_entries(
                caches[hc->type], caches[HCACHE_VALID_ULTRA]);
        }
        return caches[HCACHE_FRESH_ULTRA]->host_count != 0;
    default:
        return hc->host_count != 0;
    }
}

/**
 * Remove host from a hostcache.
 */
static void
hcache_unlink_host(hostcache_t *hc, gnet_host_t *host)
{
	g_assert(hc->host_count > 0 && hc->hostlist != NULL);
	g_assert(hash_list_contains(hc->hostlist, host, NULL));

	hash_list_remove(hc->hostlist, host);
    hc->host_count--;

    if (hc->mass_update == 0) {
        guint32 cur;
        gnet_prop_get_guint32_val(hc->hosts_in_catcher, &cur);
        gnet_prop_set_guint32_val(hc->hosts_in_catcher, cur - 1);
    }

	hc->dirty = TRUE;
	hcache_ht_remove(host);
	wfree(host, sizeof(*host));

	if (!hcache_close_running) {
		/* This must not be called during a close sequence as it
		 * would refill some caches and cause an assertion failure */
    	hcache_require_caught(hc);
	}
}

/**
 * Convert host cache type to string.
 */
const gchar *
hcache_type_to_string(hcache_type_t type)
{
	g_assert((guint) type < HCACHE_MAX);

	return names[type];
}

/**
 * Convert host type to string.
 */
const gchar *
host_type_to_string(host_type_t type)
{
	g_assert((guint) type < HOST_MAX);

	return host_type_names[type];
}


/**
 * @return the number of slots which can be added to the given type.
 *
 * @note
 * Several types share common pools. Adding a host of one type may
 * affect the number of slots left on other types.
 */
static gint32
hcache_slots_left(hcache_type_t type)
{
    g_assert((guint) type < HCACHE_MAX);

    switch (type) {
    case HCACHE_FRESH_ANY:
    case HCACHE_VALID_ANY:
        return max_hosts_cached -
            caches[HCACHE_FRESH_ANY]->host_count -
            caches[HCACHE_VALID_ANY]->host_count;
    case HCACHE_FRESH_ULTRA:
    case HCACHE_VALID_ULTRA:
        return max_ultra_hosts_cached -
            caches[HCACHE_FRESH_ULTRA]->host_count -
            caches[HCACHE_VALID_ULTRA]->host_count;
	case HCACHE_NONE:
		g_assert_not_reached();
    default:
        return max_bad_hosts_cached - caches[type]->host_count;
    }
}

/**
 * Register a host.
 *
 * If a host is already on the known hosts hashtable, it will not be
 * registered. Otherwise it will be added to the hashtable of known hosts
 * and added to one of the host lists as indicated by the "list" parameter.
 * Sanity checks are only applied when the host is added to HL_CAUGHT, since
 * when a host is added to any of the other lists it must have been in
 * HL_CAUGHT or in the pong-cache before.
 *
 * @return TRUE when IP/port passed sanity checks, regardless of whether it
 *         was added to the cache. (See above)
 */
gboolean
hcache_add(hcache_type_t type, const host_addr_t addr, guint16 port,
	const gchar *what)
{
	gnet_host_t *host;
	hostcache_t *hc;
	hostcache_entry_t *hce;

	g_assert((guint) type < HCACHE_MAX && type != HCACHE_NONE);

	/*
	 * Don't add anything to the "unstable" cache if they don't want to
	 * monitor unstable servents or when we're low on pongs (thereby
	 * automatically disabling this monitoring).  The aim is to avoid
	 * the host discarding the last few IP addresses it has, forcing it
	 * to contact the web caches...
	 */

    if (type == HCACHE_UNSTABLE) {
    	if (!node_monitor_unstable_ip || host_low_on_pongs)
			return FALSE;
	}

	if (is_my_address(addr, port)) {
        stats[HCACHE_LOCAL_INSTANCE]++;
		return FALSE;
    }

	if (node_host_is_connected(addr, port)) {
        stats[HCACHE_ALREADY_CONNECTED]++;
		return FALSE;			/* Connected to that host? */
    }

	hc = caches[type];
    g_assert(hc->type == type);

    if (
		!host_addr_is_routable(addr) &&
		(!hc->addr_only || !port_is_valid(port))
	) {
        stats[HCACHE_INVALID_HOST]++;
		return FALSE;			/* Is host valid? */
    }

    if (bogons_check(addr) || hostiles_check(addr)) {
        stats[HCACHE_INVALID_HOST]++;
		return FALSE;			/* Is host valid? */
    }

    /*
	 * If host is already known, check whether we could not simply move the
	 * entry from one cache to another.
	 */

	if (hcache_ht_get(addr, port, &host, &hce)) {
        g_assert(hce != NULL);

        hc->hits++;

		switch (type) {
		case HCACHE_TIMEOUT:
		case HCACHE_BUSY:
		case HCACHE_UNSTABLE:
			/*
			 * Move host to the proper cache, if not already in one of the
			 * "bad" caches.
			 */

			switch (hce->type) {
			case HCACHE_TIMEOUT:
			case HCACHE_BUSY:
			case HCACHE_UNSTABLE:
				return TRUE;
			default:
				break;				/* Move it */
			}
			break;

		case HCACHE_VALID_ULTRA:
		case HCACHE_FRESH_ULTRA:
			/*
			 * Move the host to the "ultra" cache if it's in the "any" ones.
			 */

			switch (hce->type) {
			case HCACHE_VALID_ANY:
			case HCACHE_FRESH_ANY:
				break;				/* Move it */
			default:
				return TRUE;
			}
			break;

		default:
			return TRUE;
		}

		/*
		 * OK, we can move it from the `hce->type' cache to the `type' one.
		 */

		g_assert(hash_list_contains(caches[hce->type]->hostlist, host, NULL));

		hash_list_remove(caches[hce->type]->hostlist, host);
		caches[hce->type]->host_count--;
		hash_list_prepend(hc->hostlist, host);
		hc->host_count++;
		caches[hce->type]->dirty = hc->dirty = TRUE;

		hce->type = type;
		hce->time_added = tm_time();

		return TRUE;
    }

	/* Okay, we got a new host */
	host = walloc(sizeof *host);

	gnet_host_set(host, addr, port);

	hcache_ht_add(type, host);

    switch (type) {
    case HCACHE_FRESH_ANY:
    case HCACHE_FRESH_ULTRA:
        hash_list_append(hc->hostlist, host);
        break;

    case HCACHE_VALID_ANY:
    case HCACHE_VALID_ULTRA:
        /*
         * We prepend to the list instead of appending because the day
         * we switch it as HCACHE_FRESH_XXX, we'll start reading from there,
         * in effect using the most recent hosts we know about.
         */
        hash_list_prepend(hc->hostlist, host);
        break;

    default:
        /*
         * hcache_expire() depends on the fact that new entries are
         * added to the beginning of the list
         */
        hash_list_prepend(hc->hostlist, host);
        break;
    }

    hc->misses++;
	hc->host_count++;
	hc->dirty = TRUE;

    if (hc->mass_update == 0) {
        guint32 cur;
        gnet_prop_get_guint32_val(hc->hosts_in_catcher, &cur);
        gnet_prop_set_guint32_val(hc->hosts_in_catcher, cur + 1);
    }

    hcache_prune(hc->type);
    hcache_update_low_on_pongs();

    if (dbg > 8) {
        printf("Added %s %s (%s)\n", what, gnet_host_to_string(host),
            (type == HCACHE_FRESH_ANY || type == HCACHE_VALID_ANY) ?
                (host_low_on_pongs ? "LOW" : "OK") : "");
    }

	return TRUE;
}

/**
 * Add a caught (fresh) host to the right list depending on the host type.
 */
gboolean
hcache_add_caught(host_type_t type, const host_addr_t addr, guint16 port,
	const gchar *what)
{
    switch (type) {
    case HOST_ANY:
    	return hcache_add(HCACHE_FRESH_ANY, addr, port, what);
    case HOST_ULTRA:
    	return hcache_add(HCACHE_FRESH_ULTRA, addr, port, what);
    case HOST_MAX:
		g_assert_not_reached();
    }

    g_error("hcache_add_caught: unknown host type: %d", type);
    return FALSE;
}

/**
 * Add a valid host to the right list depending on the host type.
 */
gboolean
hcache_add_valid(host_type_t type, const host_addr_t addr, guint16 port,
	const gchar *what)
{
    switch (type) {
    case HOST_ANY:
    	return hcache_add(HCACHE_VALID_ANY, addr, port, what);
    case HOST_ULTRA:
    	return hcache_add(HCACHE_VALID_ULTRA, addr, port, what);
    case HOST_MAX:
		g_assert_not_reached();
    }

    g_error("hcache_add_valid: unknown host type: %d", type);
    return FALSE;
}

/**
 * Remove host from cache.
 *
 * After removing hcache_require_caught is called.
 */
static void
hcache_remove(gnet_host_t *h)
{
    hostcache_entry_t *hce;
    hostcache_t *hc;

    hce = hcache_get_metadata(h);
    if (hce == NULL) {
		g_warning("hcache_remove: attempt to remove unknown host: %s",
			gnet_host_to_string(h));
        return; /* Host is not in hashtable */
    }

    hc = caches[hce->type];

    hcache_unlink_host(hc, h);
}

/**
 * Do we have less that our mimumum amount of hosts in the cache?
 */
gboolean
hcache_is_low(host_type_t type)
{
    switch (type) {
    case HOST_ANY:
        return (caches[HCACHE_FRESH_ANY]->host_count +
            caches[HCACHE_VALID_ANY]->host_count) < MIN_RESERVE_SIZE;
    case HOST_ULTRA:
        return (caches[HCACHE_FRESH_ULTRA]->host_count +
            caches[HCACHE_VALID_ULTRA]->host_count) < MIN_RESERVE_SIZE;
    case HOST_MAX:
		g_assert_not_reached();
    }
    g_error("hcache_is_low: unknown host type: %d", type);
    return FALSE; /* Only here to make -Wall happy */
}

/**
 * Remove all entries from hostcache.
 */
static void
hcache_remove_all(hostcache_t *hc)
{
	gnet_host_t *h;

    if (hc->host_count == 0) {
		g_assert(hash_list_length(hc->hostlist) == 0);
        return;
	}

    /* FIXME: may be possible to do this faster */

    start_mass_update(hc);

    while (NULL != (h = hash_list_first(hc->hostlist)))
        hcache_remove(h);

    g_assert(hash_list_length(hc->hostlist) == 0);
    g_assert(hc->host_count == 0);

    stop_mass_update(hc);
    g_assert(hc->host_count == 0);
}

/**
 * Clear the whole host cache for a host type and the pong cache of
 * the same type. Use this to clear the "ultra" and "any" host caches.
 */
void
hcache_clear_host_type(host_type_t type)
{
	gboolean valid = FALSE;

    switch (type) {
    case HOST_ANY:
        hcache_remove_all(caches[HCACHE_FRESH_ANY]);
        hcache_remove_all(caches[HCACHE_VALID_ANY]);
		valid = TRUE;
        break;
    case HOST_ULTRA:
        hcache_remove_all(caches[HCACHE_FRESH_ULTRA]);
        hcache_remove_all(caches[HCACHE_VALID_ULTRA]);
		valid = TRUE;
        break;
    case HOST_MAX:
		g_assert_not_reached();
    }

	if (!valid)
        g_error("hcache_clear_host_type: unknown host type: %d", type);

	pcache_clear_recent(type);
}

/**
 * Clear the whole host cache but does not clear the pong caches. Use
 * this to clear the "bad" host caches.
 */
void
hcache_clear(hcache_type_t type)
{
    g_assert((guint) type < HCACHE_MAX);

    hcache_remove_all(caches[type]);
}

/**
 * @return the amount of hosts in the cache.
 */
gint
hcache_size(host_type_t type)
{
    switch (type) {
    case HOST_ANY:
        return (caches[HCACHE_FRESH_ANY]->host_count +
            caches[HCACHE_VALID_ANY]->host_count);
    case HOST_ULTRA:
        return (caches[HCACHE_FRESH_ULTRA]->host_count +
            caches[HCACHE_VALID_ULTRA]->host_count);
    case HOST_MAX:
		g_assert_not_reached();
    }
    g_error("hcache_is_low: unknown host type: %d", type);
    return -1; /* Only here to make -Wall happy */
}

/**
 * Expire hosts from a single hostlist in a hostcache. Also removes
 * it from the host hashtable.
 *
 * @return total number of expired entries
 */
static guint32
hcache_expire_cache(hostcache_t *hc, time_t now)
{
    gint32 secs_to_keep = 60 * 30; /* 30 minutes */
    guint32 expire_count = 0;
	gnet_host_t *h;

    /*
	 * Prune all the expired ones from the list until the list is empty
     * or we find one which is not expired, in which case we know that
     * all the following are also not expired, because the list is
     * sorted by time_added
	 */

    while (NULL != (h = hash_list_last(hc->hostlist))) {
        hostcache_entry_t *hce = hcache_get_metadata(h);

        g_assert(hce != NULL);

        if (delta_time(now, hce->time_added) > secs_to_keep) {
            hcache_remove(h);
            expire_count++;
        } else {
            /* Found one which has not expired. Stopping */
            break;
        }
    }

    return expire_count;
}


/**
 * Expire hosts from the HL_BUSY, HL_TIMEOUT and HL_UNSTABLE lists
 * and remove them from the hosts hashtable too.
 *
 * @return total number of expired entries
 */
static guint32
hcache_expire_all(time_t now)
{
    guint32 expire_count = 0;

    expire_count += hcache_expire_cache(caches[HCACHE_TIMEOUT], now);
    expire_count += hcache_expire_cache(caches[HCACHE_BUSY], now);
    expire_count += hcache_expire_cache(caches[HCACHE_UNSTABLE], now);

    return expire_count;
}

/**
 * Remove hosts that exceed our maximum.
 *
 * This can be called on HCACHE_FRESH_ANY and on HCACHE_FRESH_ULTRA.
 *
 * If too many hosts are in the cache, then it will prune the HCACHE_FRESH_XXX
 * list. Only after HCACHE_FRESH_XXX is empty HCACHE_VALID_XXX will be moved
 * to HCACHE_FRESH_XXX and then it is purged.
 */
void
hcache_prune(hcache_type_t type)
{
	hostcache_t *hc;
    gint extra;

    g_assert((guint) type < HCACHE_MAX);

	hc = caches[type];

#define HALF_PRUNE(x) do {		\
	if (hc->host_count < caches[x]->host_count) \
		hc = caches[x];			\
} while (0)

    switch (type) {
    case HCACHE_VALID_ANY:
		HALF_PRUNE(HCACHE_FRESH_ANY);
        break;
    case HCACHE_VALID_ULTRA:
		HALF_PRUNE(HCACHE_FRESH_ULTRA);
        break;
    case HCACHE_FRESH_ANY:
		HALF_PRUNE(HCACHE_VALID_ANY);
		break;
    case HCACHE_FRESH_ULTRA:
		HALF_PRUNE(HCACHE_VALID_ULTRA);
		break;
    default:
        break;
    }

#undef HALF_PRUNE

    extra = -hcache_slots_left(hc->type);

    if (extra <= 0)
        return;

    start_mass_update(hc);

    hcache_require_caught(hc);
	while (extra > 0) {
		gnet_host_t *h = hash_list_first(hc->hostlist);
		if (NULL == h) {
			g_warning("BUG: asked to remove hosts, "
                "but hostcache list is empty: %s", hc->name);
			break;
		}
		hcache_remove(h);
        extra--;
	}

    stop_mass_update(hc);
}

/**
 * Fill `hosts', an array of `hcount' hosts already allocated with at most
 * `hcount' hosts from out caught list, without removing those hosts from
 * the list.
 *
 * @return amount of hosts filled
 */
gint
hcache_fill_caught_array(host_type_t type, gnet_host_t *hosts, gint hcount)
{
	gint i;
	hostcache_t *hc = NULL;
	GHashTable *seen_host = g_hash_table_new(host_hash, host_eq);
	hash_list_iter_t *iter;
	gnet_host_t *h;

    switch (type) {
    case HOST_ANY:
        hc = caches[HCACHE_FRESH_ANY];
        break;
    case HOST_ULTRA:
        hc = caches[HCACHE_FRESH_ULTRA];
        break;
    case HOST_MAX:
		g_assert_not_reached();
    }

	if (!hc)
        g_error("hcache_get_caught: unknown host type: %d", type);

	/*
	 * First try to fill from our recent pongs, as they are more fresh
	 * and therefore more likely to be connectible.
	 */

	for (i = 0; i < hcount; i++) {
		gnet_host_t host;
		host_addr_t addr;
		guint16 port;

		if (!pcache_get_recent(type, &addr, &port))
			break;

		gnet_host_set(&host, addr, port);
		if (g_hash_table_lookup(seen_host, &host))
			break;

		hosts[i] = host;		/* struct copy */

		g_hash_table_insert(seen_host, &hosts[i], GUINT_TO_POINTER(1));
	}

	if (i == hcount)
		goto done;

	/*
	 * Not enough fresh pongs, get some from our reserve.
	 */

	iter = hash_list_iterator_last(hc->hostlist);

	for (
        h = hash_list_previous(iter);
        i < hcount && h != NULL;
        i++, h = hash_list_previous(iter)
    ) {
		if (g_hash_table_lookup(seen_host, h))
			continue;

		hosts[i] = *h;			/* struct copy */

		g_hash_table_insert(seen_host, &hosts[i], GUINT_TO_POINTER(1));
	}

	hash_list_release(iter);

done:
	g_hash_table_destroy(seen_host);	/* Keys point directly into vector */

	return i;				/* Amount of hosts we filled */
}

/**
 * Finds a host in either the pong_cache or the host_cache that is in
 * one of the local networks.
 *
 * @return TRUE if host is found
 */
gboolean
hcache_find_nearby(host_type_t type, host_addr_t *addr, guint16 *port)
{
	gnet_host_t *h;
	static int alternate = 0;
	host_addr_t first_addr;
	guint16 first_port;
	gboolean got_recent;
	hostcache_t *hc = NULL;
	gboolean reading;
    gboolean result = FALSE;
	hash_list_iter_t *iter;

    switch (type) {
    case HOST_ANY:
        hc = caches[HCACHE_FRESH_ANY];
		break;
    case HOST_ULTRA:
        hc = caches[HCACHE_FRESH_ULTRA];
		break;
    case HOST_MAX:
		g_assert_not_reached();
    }

	if (!hc)
        g_error("hcache_get_caught: unknown host type: %d", type);

    gnet_prop_get_boolean_val(hc->reading, &reading);

	if (alternate++ & 1) {
		/* Iterate through all recent pongs */

		*addr = zero_host_addr;
		got_recent = pcache_get_recent(type, &first_addr, &first_port);

		while (got_recent) {
			if (host_addr_equal(*addr, first_addr) && *port == first_port)
				break;

			if (host_is_nearby(*addr))
				return TRUE;

			got_recent = pcache_get_recent(type, addr, port);
		}
	}

	/* iterate through whole list */

	iter = reading ? hash_list_iterator(hc->hostlist) :
		hash_list_iterator_last(hc->hostlist);

	for (h = hash_list_follower(iter); h; h = hash_list_follower(iter)) {

		if (host_is_nearby(gnet_host_get_addr(h))) {
            *addr = gnet_host_get_addr(h);
            *port = gnet_host_get_port(h);

            result = TRUE;
			break;
		}
	}

	hash_list_release(iter);

	if (result)
		hcache_unlink_host(hc, h);

	return result;
}

/**
 * Get host IP/port information from our caught host list, or from the
 * recent pong cache, in alternance.
 */
void
hcache_get_caught(host_type_t type, host_addr_t *addr, guint16 *port)
{
	static guint alternate = 0;
	hostcache_t *hc = NULL;
	gboolean reading;
	extern guint32 number_local_networks;
	gnet_host_t *h;
	gboolean available;

    switch(type) {
    case HOST_ANY:
        hc = caches[HCACHE_FRESH_ANY];
        break;
    case HOST_ULTRA:
        hc = caches[HCACHE_FRESH_ULTRA];
        break;
    case HOST_MAX:
		g_assert_not_reached();
    }

	if (!hc)
        g_error("hcache_get_caught: unknown host type: %d", type);

    gnet_prop_get_boolean_val(hc->reading, &reading);
    available = hcache_require_caught(hc);

    g_assert(available);

    hcache_update_low_on_pongs();

	/*
	 * First, try to find a local host
	 */

	if (
		use_netmasks && number_local_networks &&
		hcache_find_nearby(type, addr, port)
	)
		return;

	/*
	 * Try the recent pong cache when `alternate' is odd.
	 */

	if (alternate++ & 0x1 && pcache_get_recent(type, addr, port))
		return;

	/*
	 * If we're done reading from the host file, get latest host, at the
	 * tail of the list.  Otherwise, get the first host in that list.
	 */

	h = reading ? hash_list_first(hc->hostlist) : hash_list_last(hc->hostlist);

	*addr = gnet_host_get_addr(h);
	*port = gnet_host_get_port(h);

    hcache_unlink_host(hc, h);
}

/***
 *** Hostcache management.
 ***/

/**
 * Allocate hostcache of type `type'.
 *
 * The `incache' property is what needs to be updated so that the GUI can
 * display the proper amount of hosts we currently hold.
 *
 * The `maxhosts' variable is the pointer to the variable giving the maximum
 * amount of hosts we can store.
 *
 * The `reading' variable is the property to update to signal whether we're
 * reading the persisted file.
 */
static hostcache_t *
hcache_alloc(hcache_type_t type, gnet_property_t reading,
	gnet_property_t catcher, const gchar *name)
{
	struct hostcache *hc;

	g_assert((guint) type < HCACHE_MAX);

	hc = g_malloc0(sizeof *hc);

	hc->hostlist = hash_list_new(NULL, NULL);
	hc->name = name;
	hc->type = type;
	hc->reading = reading;
    hc->hosts_in_catcher = catcher;
    hc->addr_only = FALSE;

	return hc;
}

/**
 * Dispose of the hostcache.
 */
static void
hcache_free(hostcache_t *hc)
{
    g_assert(hc != NULL);
    g_assert(hc->host_count == 0);
    g_assert(hash_list_length(hc->hostlist) == 0);

	hash_list_free(hc->hostlist);
	G_FREE_NULL(hc);
}

/***
 *** Hosts text files
 ***/

/*
 * Host reading context.
 */

#define READ_MAGIC		0x3d00003d
#define HOST_READ_CNT	20			/**< Amount of hosts to read each tick */

struct read_ctx {
	gint magic;						/**< Magic number */
	FILE *fd;						/**< File descriptor to read from */
	hostcache_t *hc;				/**< Hostcache to fill */
};

/**
 * Dispose of the read context.
 */
static void
read_ctx_free(gpointer u)
{
	struct read_ctx *rctx = u;

	g_assert(rctx->magic == READ_MAGIC);

	if (rctx->fd != NULL)
		fclose(rctx->fd);

	wfree(rctx, sizeof(*rctx));
}

/**
 * Read is finished.
 */
static void
read_done(hostcache_t *hc)
{
    /*
     * Order is important so the GUI can update properly. First we say
     * that loading has finished, then we tell the GUI the number of
     * hosts in the catcher.
     *      -- Richard, 6/8/2002
     */

    gnet_prop_set_boolean_val(hc->reading, FALSE);
}

/**
 * One reading step.
 */
static bgret_t
read_step(gpointer unused_h, gpointer u, gint ticks)
{
	struct read_ctx *rctx = u;
	hostcache_t *hc;
	gint max_read;
	gint count;
	gint i;
	static gchar h_tmp[1024];

	(void) unused_h;
	g_assert(READ_MAGIC == rctx->magic);
	g_assert(rctx->fd);

	hc = rctx->hc;

	max_read = hcache_slots_left(hc->type);
	count = ticks * HOST_READ_CNT;
	count = MIN(max_read, count);

	if (dbg > 9)
		printf("read_step(%s): ticks=%d, count=%d\n", hc->name, ticks, count);

	for (i = 0; i < count; i++) {
		if (fgets(h_tmp, sizeof(h_tmp) - 1, rctx->fd)) { /* NUL appended */
			host_addr_t addr;
			guint16 port;

			if (string_to_host_addr_port(h_tmp, NULL, &addr, &port))
                hcache_add(hc->type, addr, port, "on-disk cache");
		} else
			goto done;
	}

	if (count < max_read)
		return BGR_MORE;		/* Host cache not full, need to read more */

	/* Fall through */

done:
	fclose(rctx->fd);
	rctx->fd = NULL;

	read_done(hc);

	return BGR_DONE;
}

/**
 * Invoked when the task is completed.
 */
static void
bg_reader_done(gpointer unused_h, gpointer ctx,
		bgstatus_t unused_status, gpointer unused_arg)
{
	struct read_ctx *rctx = ctx;
	hostcache_t *hc;

	(void) unused_h;
	(void) unused_status;
	(void) unused_arg;
	g_assert(rctx->magic == READ_MAGIC);

	hc = rctx->hc;
	bg_reader[hc->type] = NULL;
}

/**
 * Loads caught hosts from text file.
 */
static void
hcache_retrieve(hostcache_t *hc, const gchar *filename)
{
	struct read_ctx *rctx;
	FILE *fd;
	bgstep_cb_t step = read_step;

    {
		file_path_t fp;

		file_path_set(&fp, settings_config_dir(), filename);
		fd = file_config_open_read("hosts", &fp, 1);
	}

	if (!fd)
		return;

	rctx = walloc(sizeof(*rctx));
	rctx->magic = READ_MAGIC;
	rctx->fd = fd;
	rctx->hc = hc;

    gnet_prop_set_boolean_val(hc->reading, TRUE);

	bg_reader[hc->type] = bg_task_create(
		hc->type == HCACHE_FRESH_ANY ?
			"Hostcache reading" : "Ultracache reading",
		&step, 1, rctx, read_ctx_free, bg_reader_done, NULL);
}

/**
 * Write all data from cache to supplied file.
 */
static void
hcache_write(FILE *f, hostcache_t *hc)
{
	hash_list_iter_t *iter;
	gnet_host_t *h;

	iter = hash_list_iterator(hc->hostlist);

	for (h = hash_list_next(iter); h != NULL; h = hash_list_next(iter)) {
		fprintf(f, "%s\n", gnet_host_to_string(h));
	}
	hash_list_release(iter);
}

/**
 * Persist hostcache to disk.
 * If `extra' is not HCACHE_NONE, it is appended after the dump of `type'.
 */
static void
hcache_store(hcache_type_t type, const gchar *filename, hcache_type_t extra)
{
	FILE *f;
	file_path_t fp;

	g_assert((guint) type < HCACHE_MAX && type != HCACHE_NONE);
	g_assert((guint) extra < HCACHE_MAX);
	g_assert(caches[type] != NULL);
	g_assert(extra == HCACHE_NONE || caches[extra] != NULL);

	file_path_set(&fp, settings_config_dir(), filename);
	f = file_config_open_write(filename, &fp);

	if (!f)
		return;

	hcache_write(f, caches[type]);

	if (extra != HCACHE_NONE)
		hcache_write(f, caches[extra]);

	file_config_close(f, &fp);
}

/**
 * Get statistical information about the caches.
 *
 * @param s must point to an hcache_stats_t[HCACHE_MAX] array.
 */
void
hcache_get_stats(hcache_stats_t *s)
{
    guint n;

    for (n = 0; n < HCACHE_MAX; n++) {
		if (n == HCACHE_NONE)
			continue;
        s[n].host_count = caches[n]->host_count;
        s[n].hits       = caches[n]->hits;
        s[n].misses     = caches[n]->misses;
        s[n].reading    = FALSE;
    }
}

/**
 * Host cache timer.
 */
void
hcache_timer(time_t now)
{
    hcache_expire_all(now);

    if (dbg >= 15) {
        hcache_dump_info(caches[HCACHE_FRESH_ANY],   "timer");
        hcache_dump_info(caches[HCACHE_VALID_ANY],   "timer");

        hcache_dump_info(caches[HCACHE_FRESH_ULTRA], "timer");
        hcache_dump_info(caches[HCACHE_VALID_ULTRA], "timer");

        hcache_dump_info(caches[HCACHE_TIMEOUT],  "timer");
        hcache_dump_info(caches[HCACHE_BUSY],     "timer");
        hcache_dump_info(caches[HCACHE_UNSTABLE], "timer");

        g_message("Hcache global: local %u   alrdy connected %u   invalid %u",
            stats[HCACHE_LOCAL_INSTANCE], stats[HCACHE_ALREADY_CONNECTED],
            stats[HCACHE_INVALID_HOST]);
    }
}

/**
 * Initialize host caches.
 */
void
hcache_init(void)
{
    memset(bg_reader, 0, sizeof(bg_reader));
    memset(stats, 0, sizeof(stats));

	ht_known_hosts = g_hash_table_new(host_hash, host_eq);

    caches[HCACHE_FRESH_ANY] = hcache_alloc(
        HCACHE_FRESH_ANY, PROP_READING_HOSTFILE,
        PROP_HOSTS_IN_CATCHER,
        "hosts.fresh.any");

    caches[HCACHE_FRESH_ULTRA] = hcache_alloc(
        HCACHE_FRESH_ULTRA, PROP_READING_ULTRAFILE,
        PROP_HOSTS_IN_ULTRA_CATCHER,
        "hosts.fresh.ultra");

    caches[HCACHE_VALID_ANY] = hcache_alloc(
        HCACHE_VALID_ANY, PROP_READING_HOSTFILE,
        PROP_HOSTS_IN_CATCHER,
        "hosts.valid.any");

    caches[HCACHE_VALID_ULTRA] = hcache_alloc(
        HCACHE_VALID_ULTRA, PROP_READING_ULTRAFILE,
        PROP_HOSTS_IN_ULTRA_CATCHER,
        "hosts.valid.ultra");

    caches[HCACHE_TIMEOUT] = hcache_alloc(
        HCACHE_TIMEOUT, PROP_READING_HOSTFILE,
        PROP_HOSTS_IN_BAD_CATCHER,
        "hosts.timeout");
    caches[HCACHE_TIMEOUT]->addr_only = TRUE;

    caches[HCACHE_BUSY] = hcache_alloc(
        HCACHE_BUSY, PROP_READING_HOSTFILE,
        PROP_HOSTS_IN_BAD_CATCHER,
        "hosts.busy");
    caches[HCACHE_BUSY]->addr_only = TRUE;

    caches[HCACHE_UNSTABLE] = hcache_alloc(
        HCACHE_UNSTABLE, PROP_READING_HOSTFILE,
        PROP_HOSTS_IN_BAD_CATCHER,
        "hosts.unstable");
    caches[HCACHE_UNSTABLE]->addr_only = TRUE;
}

/**
 * Load hostcache data from disk.
 */
void
hcache_retrieve_all(void)
{
	hcache_retrieve(caches[HCACHE_FRESH_ANY], "hosts");
	hcache_retrieve(caches[HCACHE_FRESH_ULTRA], "ultras");
}

/**
 * Save hostcache data to disk, for the relevant host type.
 */
void
hcache_store_if_dirty(host_type_t type)
{
	gnet_property_t prop;
	gboolean reading;
	hcache_type_t first, second;
	const gchar *file;

	switch (type) {
    case HOST_ANY:
		prop = PROP_READING_HOSTFILE;
		first = HCACHE_VALID_ANY;
		second = HCACHE_FRESH_ANY;
		file = "hosts";
        break;
    case HOST_ULTRA:
		prop = PROP_READING_ULTRAFILE;
		first = HCACHE_VALID_ULTRA;
		second = HCACHE_FRESH_ULTRA;
		file = "ultras";
		break;
	default:
		g_error("can't store cache for host type %d", type);
		return;
	}

	gnet_prop_get_boolean_val(prop, &reading);

	if (reading)
		return;

	if (!caches[first]->dirty && !caches[second]->dirty)
		return;

	hcache_store(first, file, second);

	caches[first]->dirty = caches[second]->dirty = FALSE;
}

/**
 * Check whether the host caches are completely read. This should be
 * used before contacting a GWebCache at start-up time.
 *
 * @returns TRUE if the host caches have been completely loaded and FALSE
 *			if the loading is still in process.
 */
gboolean
hcache_read_finished(void)
{
	gboolean reading;

	gnet_prop_get_boolean_val(PROP_READING_HOSTFILE, &reading);
	if (reading)
		return FALSE;

	gnet_prop_get_boolean_val(PROP_READING_ULTRAFILE, &reading);
	if (reading)
		return FALSE;

	return TRUE;
}

/**
 * Shutdown host caches.
 */
void
hcache_shutdown(void)
{
	gboolean reading;

	/*
	 * Write "valid" hosts first.  Next time we are launched, we'll first
	 * start reading from the head first.  And once the whole cache has
	 * been read in memory, we'll begin using the tail of the list, i.e.
	 * possibly older hosts, which will help ensure we don't always connect
	 * to the same set of hosts.
	 */

	/* Save the caught hosts */

	gnet_prop_get_boolean_val(PROP_READING_HOSTFILE, &reading);

	if (reading)
		g_warning("exit() while still reading the hosts file, "
			"caught hosts not saved!");
	else
		hcache_store(HCACHE_VALID_ANY, "hosts", HCACHE_FRESH_ANY);

	/* Save the caught ultra hosts */

	gnet_prop_get_boolean_val(PROP_READING_ULTRAFILE, &reading);

	if (reading)
		g_warning("exit() while still reading the ultrahosts file, "
			"caught hosts not saved !");
	else
		hcache_store(HCACHE_VALID_ULTRA, "ultras", HCACHE_FRESH_ULTRA);
}

/**
 * Destroy all host caches.
 */
void hcache_close(void)
{
	static const hcache_type_t types[] = {
        HCACHE_FRESH_ANY,
        HCACHE_VALID_ANY,
        HCACHE_FRESH_ULTRA,
        HCACHE_VALID_ULTRA,
        HCACHE_TIMEOUT,
        HCACHE_BUSY,
        HCACHE_UNSTABLE
    };
	guint i;

	g_assert(!hcache_close_running);
	hcache_close_running = TRUE;

    /*
     * First we stop all background processes and remove all hosts,
     * only then we free the hcaches. This is important because
     * hcache_require_caught will crash if we free certain hostcaches.
     */

	for (i = 0; i < G_N_ELEMENTS(types); i++) {
		guint j;
		hcache_type_t type = types[i];

		if (bg_reader[type] != NULL)
			bg_task_cancel(bg_reader[type]);

		/* Make sure all previous caches have been cleared */
		for (j = 0; j < type; j++)
    		g_assert(caches[j]->host_count == 0);

        hcache_remove_all(caches[type]);

		/* Make sure no caches have been refilled */
		for (j = 0; j <= type; j++)
    		g_assert(caches[j]->host_count == 0);
	}

	for (i = 0; i < G_N_ELEMENTS(types); i++) {
		hcache_type_t type = types[i];

		hcache_free(caches[type]);
        caches[type] = NULL;
	}

    g_assert(g_hash_table_size(ht_known_hosts) == 0);

	g_hash_table_destroy(ht_known_hosts);
    ht_known_hosts = NULL;
}

/* vi: set ts=4 sw=4 cindent: */
