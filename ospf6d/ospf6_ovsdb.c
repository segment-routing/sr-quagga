
#include <log.h>
#include "hashmap.h"
#include "memory.h"
#include "memtypes.h"

#include "ospf6_ovsdb.h"
#include "ospf6_top.h"


static inline int
compare_long (void *k1, void *k2)
{
	return *(uint64_t *)k1 != *(uint64_t *)k2;
}

static inline unsigned int
hash_long (void *key)
{
	uint32_t *ptr = key;
	return hashint(hashint(ptr[0]) ^ hashint(ptr[1]));
}

static void
free_node (struct ospf6_ovsdb_node *node)
{
	if (node)
		XFREE(MTYPE_OSPF6_OVSDB_NODE, node);
}

static void
free_link (struct ospf6_ovsdb_link *link)
{
	if (link)
		XFREE(MTYPE_OSPF6_OVSDB_LINK, link);
}

static int
mapping_read (struct srdb_entry *entry)
{
	struct srdb_namemap_entry *mapping_entry = (struct srdb_namemap_entry *) entry;
	zlog_debug("  %s: start", __func__);

	struct ospf6_ovsdb_node *node = XCALLOC(MTYPE_OSPF6_OVSDB_NODE, sizeof (struct ospf6_ovsdb_node));
	if (!node) {
		zlog_debug("  %s: Allocation problem", __func__);
		return -1;
	}
	strncpy(node->db_entry.name, mapping_entry->routerName, SLEN + 1);
	strncpy(node->db_entry.name, mapping_entry->routerName, SLEN + 1);
	node->ospf_id = mapping_entry->routerId;

	if (hmap_set(ospf6->ovsdb->nodes_map, (void *)(intptr_t) node->ospf_id, node)) {
		zlog_debug("  %s: Cannot insert it in the hmap", __func__);
		return -1;
	}
	zlog_debug("  %s: successful end for router id %d", __func__, node->ospf_id);

	return 0;
}

static int
launch_srdb (struct srdb *srdb)
{
	unsigned int mon_flags;

	mon_flags = MON_INITIAL | MON_INSERT;

	fprintf(stderr, "TEST sdfdsfgdf\n");

	if (srdb_monitor(srdb, "NameIdMapping", mon_flags, mapping_read,
	                 NULL, NULL, false, false) < 0) {
		zlog_err("failed to monitor the Name-RouterId mapping.");
		return -1;
	}

	return 0;
}

struct ospf6_ovsdb*
ospf6_ovsdb_create (const char *proto, const char *ip6, const char *port, const char *database)
{
	struct ospf6_ovsdb *ovsdb;

	zlog_debug("  %s: Attempt to create an ovsdb object", __func__);

	ovsdb = XCALLOC (MTYPE_OSPF6_OVSDB, sizeof (struct ospf6_ovsdb));
	if (!ovsdb)
		return NULL;

	strncpy(ovsdb->ovsdb_conf.ovsdb_client, "ovsdb-client", SLEN + 1);
	snprintf(ovsdb->ovsdb_conf.ovsdb_server, SLEN + 1, "%s:[%s]:%s", proto, ip6, port);
	strncpy(ovsdb->ovsdb_conf.ovsdb_database, database, SLEN + 1);
	ovsdb->ovsdb_conf.ntransacts = 1;

	zlog_debug("  %s: Attempt to create an ovsdb object 2", __func__);

	ovsdb->srdb = srdb_new(&ovsdb->ovsdb_conf);
	if (!ovsdb->srdb)
		goto out_free_ovsdb;

	zlog_debug("  %s: Attempt to create an ovsdb object 3", __func__);

	/* The key is the router id */
	ovsdb->nodes_map = hmap_new(hash_int, compare_int);
	if (!ovsdb->nodes_map)
		goto free_srdb;

	zlog_debug("  %s: Attempt to create an ovsdb object 4", __func__);

	/* The key is the juxtaposition of both router id forming the link */
	ovsdb->links_map = hmap_new(hash_long, compare_long);
	if (!ovsdb->links_map)
		goto free_nodes_map;

	zlog_debug("  %s: Attempt to create an ovsdb object 5", __func__);

	/* Start monitoring the mapping between router ids and names */
	if (launch_srdb(ovsdb->srdb))
		goto free_links_map;

	zlog_debug("  %s: Object created", __func__);

	return ovsdb;

free_links_map:
	hmap_destroy(ovsdb->links_map);
free_nodes_map:
	hmap_destroy(ovsdb->nodes_map);
free_srdb:
	srdb_destroy(ovsdb->srdb);
out_free_ovsdb:
	zlog_debug("  %s: Object NOT created", __func__);
	XFREE(MTYPE_OSPF6_OVSDB, ovsdb);
	return NULL;
}

void
ospf6_ovsdb_delete (struct ospf6_ovsdb *ovsdb)
{
	zlog_debug("  %s: Start object deletion", __func__);
	if (!ovsdb)
		return;

	struct hmap_entry *he;

	while (!llist_empty(&ovsdb->links_map->keys)) {
		he = llist_first_entry(&ovsdb->links_map->keys, struct hmap_entry, key_head);
		llist_remove(&he->key_head);
		llist_remove(&he->map_head);
		ovsdb->links_map->elems--;
		free_link(he->elem);
		free(he);
	}
	hmap_destroy(ovsdb->links_map);

	while (!llist_empty(&ovsdb->nodes_map->keys)) {
		he = llist_first_entry(&ovsdb->nodes_map->keys, struct hmap_entry, key_head);
		llist_remove(&he->key_head);
		llist_remove(&he->map_head);
		ovsdb->nodes_map->elems--;
		free_node(he->elem);
		free(he);
	}
	hmap_destroy(ovsdb->nodes_map);

	srdb_destroy(ovsdb->srdb);
	zlog_debug("  %s: Object deleted", __func__);
}

static int
ospf6_ovsdb_insert_nodeState (struct ospf6_ovsdb *ovsdb, struct ospf6_ovsdb_node *node)
{
	struct srdb_table *tbl;
	int ret;
	zlog_debug("  %s: start", __func__);

	tbl = srdb_table_by_name(ovsdb->srdb->tables, "NodeState");
	if (!tbl) {
		zlog_err("Cannot find NodeState table");
		return -1;
	}

	ret = srdb_insert_sync(ovsdb->srdb, tbl,
	                       (struct srdb_entry *) &node->db_entry, NULL);
	if (ret) {
		zlog_err("Cannot push NodeState name='%s' addr='%s' prefix='%s' pbsid='%s'",
		         node->db_entry.name, node->db_entry.addr, node->db_entry.prefix, node->db_entry.pbsid);
		return -1;
	}
	zlog_debug("  %s: sucessful end", __func__);

	return 0;
}

int
ospf6_ovsdb_set_active_router (struct ospf6_ovsdb *ovsdb, int routerid)
{
	zlog_debug("  %s: start", __func__);
	char buf[128];
	struct ospf6_ovsdb_node *node = hmap_get(ovsdb->nodes_map, (void *)(intptr_t) routerid);
	if (!node) {
		inet_ntop(AF_INET, &routerid, buf, sizeof(buf));
		zlog_err("Router id '%d' => '%s' cannot be mapped to a router name", routerid, buf);
		return -1;
	}

	/* The node is known as active */
	node->last_spf = 1;

	/* We send it to ovsdb if the prefix is known and it was not inserted yet
	 * (because not active on last SPF)
	 */
	// TODO Additional condition when router and link sub-lsa are implemented if (*(node->db_entry.prefix) != '\0')
	if (!node->ovsdb_inserted) {
		node->ovsdb_inserted = !ospf6_ovsdb_insert_nodeState(ovsdb, node);
		zlog_debug("  %s: Just inserted ? %d", __func__, node->ovsdb_inserted);
		return node->ovsdb_inserted;
	}
	zlog_debug("  %s: Already inserted", __func__);

	return 0;
}

static int
ospf6_ovsdb_insert_linkState (struct ospf6_ovsdb *ovsdb, struct ospf6_ovsdb_link *link)
{
	struct srdb_table *tbl;
	int ret;
	zlog_debug("  %s: start", __func__);

	tbl = srdb_table_by_name(ovsdb->srdb->tables, "LinkState");
	if (!tbl) {
		zlog_err("Cannot find NodeState table");
		return -1;
	}

	ret = srdb_insert_sync(ovsdb->srdb, tbl,
	                       (struct srdb_entry *) &link->db_entry, NULL);
	if (ret) {
		zlog_err("Cannot push LinkState name1='%s' addr1='%s' name2='%s' addr2='%s'",
		         link->db_entry.name1, link->db_entry.addr1, link->db_entry.name2, link->db_entry.addr2);
		return -1;
	}
	zlog_debug("  %s: sucessful end", __func__);

	return 0;
}

int
ospf6_ovsdb_set_active_link (struct ospf6_ovsdb *ovsdb, int routerid_1, int routerid_2)
{
	char buf1[128];
	char buf2[128];
	int64_t link_id = 0;
	zlog_debug("  %s: start", __func__);

	if (routerid_1 == routerid_2)
		return -1;

	int first_routerid = routerid_1 > routerid_2 ? routerid_1 : routerid_2;
	int second_routerid = first_routerid != routerid_1 ? routerid_1 : routerid_2;

	link_id = ((int64_t) first_routerid << 32) + ((int64_t) second_routerid);
	struct ospf6_ovsdb_link *link = hmap_get(ovsdb->links_map, &link_id);
	if (!link) {
		/* New link */
		link = XCALLOC(MTYPE_OSPF6_OVSDB_LINK, sizeof (struct ospf6_ovsdb_link));
		if (!link)
			return -1;
		link->ospf_id_1 = first_routerid;
		link->ospf_id_2 = second_routerid;

		struct ospf6_ovsdb_node *node1 = hmap_get(ovsdb->nodes_map, (void *)(intptr_t) first_routerid);
		if (!node1) {
			inet_ntop(AF_INET, &first_routerid, buf1, sizeof(buf1));
			zlog_err("Link to non-existent router '%s'", buf1);
			free_link(link);
			return -1;
		}
		struct ospf6_ovsdb_node *node2 = hmap_get(ovsdb->nodes_map, (void *)(intptr_t) second_routerid);
		if (!node2) {
			inet_ntop(AF_INET, &second_routerid, buf1, sizeof(buf1));
			zlog_err("Link to non-existent router '%s'", buf1);
			free_link(link);
			return -1;
		}

		strncpy(link->db_entry.name1, node1->db_entry.name, SLEN + 1);
		strncpy(link->db_entry.name2, node2->db_entry.name, SLEN + 1);

		if (hmap_set(ovsdb->links_map, &link_id, link)) {
			inet_ntop(AF_INET, &first_routerid, buf1, sizeof(buf1));
			inet_ntop(AF_INET, &second_routerid, buf2, sizeof(buf2));
			zlog_err("Link '%s' <-> '%s' cannot be added", buf1, buf2);
			free_link(link);
			return -1;
		}
	}

	/* The node is known as active */
	link->last_spf = 1;

	/* We send it to ovsdb if the prefix is known and it was not inserted yet
	 * (because not active on last SPF)
	 */
	// TODO Additional condition when router and link sub-lsa are implemented
	// if (*(link->db_entry.addr1) != '\0' && *(link->db_entry.addr2) != '\0')
	if (!link->ovsdb_inserted) {
		link->ovsdb_inserted = !ospf6_ovsdb_insert_linkState(ovsdb, link);
		zlog_debug("  %s: Just inserted ? %d", __func__, link->ovsdb_inserted);
		return link->ovsdb_inserted;
	}
	zlog_debug("  %s: Already inserted", __func__);

	return 0;
}

static int
ospf6_ovsdb_delete_node (struct ospf6_ovsdb *ovsdb, struct ospf6_ovsdb_node *node)
{
	// TODO Add delete primitives to srdb library
	return 0;
}

static int
ospf6_ovsdb_delete_link (struct ospf6_ovsdb *ovsdb, struct ospf6_ovsdb_link *link)
{
	// TODO Add delete primitives to srdb library
	return 0;
}

void
ospf6_ovsdb_delete_down_element (struct ospf6_ovsdb *ovsdb)
{
	struct hmap_entry *he;
	struct ospf6_ovsdb_node *node;
	struct ospf6_ovsdb_link *link;
	zlog_debug("  %s: start", __func__);

	/* For consistency reasons, links are deleted before nodes */

	hmap_foreach(ovsdb->links_map, he) {
		link = he->elem;
		if (!link->last_spf && link->ovsdb_inserted) {
			link->ovsdb_inserted = ospf6_ovsdb_delete_link(ovsdb, link);
		} else if (link->last_spf && link->ovsdb_inserted) {
			link->last_spf = 0; /* For the next spf */
		}
	}

	hmap_foreach(ovsdb->nodes_map, he) {
		node = he->elem;
		if (!node->last_spf && node->ovsdb_inserted) {
			node->ovsdb_inserted = ospf6_ovsdb_delete_node(ovsdb, node);
		} else if (node->last_spf && node->ovsdb_inserted) {
			node->last_spf = 0; /* For the next spf */
		}
	}
	zlog_debug("  %s: end", __func__);
}
