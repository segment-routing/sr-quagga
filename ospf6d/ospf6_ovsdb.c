
#include <log.h>
#include "hashmap.h"
#include "memory.h"
#include "memtypes.h"

#include "ospf6_ovsdb.h"
#include "ospf6_top.h"


static int
zlog_srdb (const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vzlog(NULL, LOG_ERR, fmt, args);
	va_end(args);
	return 0;
}

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
	if (node) {
		free(node->db_entry.prefix);
		XFREE(MTYPE_OSPF6_OVSDB_NODE, node);
	}
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

	struct ospf6_ovsdb_node *node = XCALLOC(MTYPE_OSPF6_OVSDB_NODE, sizeof (struct ospf6_ovsdb_node));
	if (!node) {
		zlog_debug("  %s: Allocation problem", __func__);
		return -1;
	}
	strncpy(node->db_entry.name, mapping_entry->routerName, SLEN + 1);
	strncpy(node->db_entry.addr, mapping_entry->addr, SLEN + 1);
	node->db_entry.prefix = strndup(mapping_entry->prefix, 1024); // FIXME Use a define
	strncpy(node->db_entry.pbsid, mapping_entry->pbsid, SLEN + 1);
	node->ospf_id = (int64_t) mapping_entry->routerId;

	if (hmap_set(ospf6->ovsdb->nodes_map, &node->ospf_id, node)) {
		zlog_debug("  %s: Cannot insert the node %ld in the hmap", __func__, node->ospf_id);
		free_node(node);
		return -1;
	}

	return 0;
}

static int
available_link_read (struct srdb_entry *entry)
{
	struct srdb_availlink_entry *mapping_entry = (struct srdb_availlink_entry *) entry;

	struct ospf6_ovsdb_link *link = XCALLOC(MTYPE_OSPF6_OVSDB_LINK, sizeof (struct ospf6_ovsdb_link));
	if (!link) {
		zlog_debug("  %s: Allocation problem", __func__);
		return -1;
	}

	strncpy(link->db_entry.name1, mapping_entry->name1, SLEN + 1);
	strncpy(link->db_entry.name2, mapping_entry->name2, SLEN + 1);
	strncpy(link->db_entry.addr1, mapping_entry->addr1, SLEN + 1);
	strncpy(link->db_entry.addr2, mapping_entry->addr2, SLEN + 1);

	link->db_entry.metric = mapping_entry->metric;
	link->db_entry.bw = mapping_entry->bw;
	link->db_entry.ava_bw = mapping_entry->ava_bw;
	link->db_entry.delay = mapping_entry->delay;

	link->ospf_id_1 = mapping_entry->routerId1;
	link->ospf_id_2 = mapping_entry->routerId2;
	link->ospf_id_1 = mapping_entry->routerId1 > mapping_entry->routerId2
	                     ? mapping_entry->routerId1 : mapping_entry->routerId2;
	link->ospf_id_2 = link->ospf_id_1 != mapping_entry->routerId1
	                      ? mapping_entry->routerId1 : mapping_entry->routerId2;
	link->link_id = ((int64_t) link->ospf_id_1 << 32) + ((int64_t) link->ospf_id_2);

	if (hmap_set(ospf6->ovsdb->links_map, &link->link_id, link)) {
		zlog_debug("  %s: Cannot insert the link '%d' <-> '%d' in the hmap", __func__,
		           link->ospf_id_1, link->ospf_id_2);
		free_link(link);
		return -1;
	}

	return 0;
}

static int
launch_srdb (struct srdb *srdb)
{
	unsigned int mon_flags;

	mon_flags = MON_INITIAL | MON_INSERT;

	if (srdb_monitor(srdb, "NameIdMapping", mon_flags, mapping_read,
	                 NULL, NULL, false, false) < 0) {
		zlog_err("failed to monitor the Name-RouterId mapping.");
		return -1;
	}

	if (srdb_monitor(srdb, "AvailableLink", mon_flags, available_link_read,
	                 NULL, NULL, false, false) < 0) {
		zlog_err("failed to monitor the AvailableLink table.");
		return -1;
	}

	return 0;
}

struct ospf6_ovsdb*
ospf6_ovsdb_create (const char *proto, const char *ip6, const char *port, const char *database)
{
	struct ospf6_ovsdb *ovsdb;

	ovsdb = XCALLOC (MTYPE_OSPF6_OVSDB, sizeof (struct ospf6_ovsdb));
	if (!ovsdb)
		return NULL;

	strncpy(ovsdb->ovsdb_conf.ovsdb_client, "ovsdb-client", SLEN + 1);
	snprintf(ovsdb->ovsdb_conf.ovsdb_server, SLEN + 1, "%s:[%s]:%s", proto, ip6, port);
	strncpy(ovsdb->ovsdb_conf.ovsdb_database, database, SLEN + 1);
	ovsdb->ovsdb_conf.ntransacts = 1;

	ovsdb->srdb = srdb_new(&ovsdb->ovsdb_conf, zlog_srdb);
	if (!ovsdb->srdb)
		goto out_free_ovsdb;

	/* The key is the router id */
	ovsdb->nodes_map = hmap_new(hash_long, compare_long);
	if (!ovsdb->nodes_map)
		goto free_srdb;

	/* The key is the juxtaposition of both router id forming the link */
	ovsdb->links_map = hmap_new(hash_long, compare_long);
	if (!ovsdb->links_map)
		goto free_nodes_map;

	/* Start monitoring the mapping between router ids and names */
	if (launch_srdb(ovsdb->srdb))
		goto free_links_map;

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
}

static int
ospf6_ovsdb_insert_nodeState (struct ospf6_ovsdb *ovsdb, struct ospf6_ovsdb_node *node)
{
	struct srdb_table *tbl;
	int ret;

	tbl = srdb_table_by_name(ovsdb->srdb->tables, "NodeState");
	if (!tbl) {
		zlog_err("Cannot find NodeState table");
		return -1;
	}

	ret = srdb_insert_sync(ovsdb->srdb, tbl,
	                       (struct srdb_entry *) &node->db_entry,
			       (char *) &node->db_entry._row);
	if (ret) {
		zlog_err("Cannot push NodeState name='%s' addr='%s' prefix='%s' pbsid='%s'",
		         node->db_entry.name, node->db_entry.addr, node->db_entry.prefix, node->db_entry.pbsid);
		return -1;
	}

	return 0;
}

int
ospf6_ovsdb_set_active_router (struct ospf6_ovsdb *ovsdb, int routerid)
{
	char buf[128];
	int64_t tmp = (int64_t) routerid;
	struct ospf6_ovsdb_node *node = hmap_get(ovsdb->nodes_map, &tmp);
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

	return 0;
}

static int
ospf6_ovsdb_insert_linkState (struct ospf6_ovsdb *ovsdb, struct ospf6_ovsdb_link *link)
{
	struct srdb_table *tbl;
	int ret;

	tbl = srdb_table_by_name(ovsdb->srdb->tables, "LinkState");
	if (!tbl) {
		zlog_err("Cannot find LinkState table");
		return -1;
	}

	ret = srdb_insert_sync(ovsdb->srdb, tbl,
	                       (struct srdb_entry *) &link->db_entry,
			       (char *) &link->db_entry._row);
	if (ret) {
		zlog_err("Cannot push LinkState name1='%s' addr1='%s' name2='%s' addr2='%s'",
		         link->db_entry.name1, link->db_entry.addr1, link->db_entry.name2, link->db_entry.addr2);
		return -1;
	}

	return 0;
}

int
ospf6_ovsdb_set_active_link (struct ospf6_ovsdb *ovsdb, int routerid_1, int routerid_2)
{
	char buf1[128];
	char buf2[128];
	int64_t link_id = 0;

	if (routerid_1 == routerid_2)
		return -1;

	int first_routerid = routerid_1 > routerid_2 ? routerid_1 : routerid_2;
	int second_routerid = first_routerid != routerid_1 ? routerid_1 : routerid_2;

	link_id = ((int64_t) first_routerid << 32) + ((int64_t) second_routerid);
	struct ospf6_ovsdb_link *link = hmap_get(ovsdb->links_map, &link_id);
	if (!link) {
		inet_ntop(AF_INET, &first_routerid, buf1, sizeof(buf1));
		inet_ntop(AF_INET, &second_routerid, buf2, sizeof(buf2));
		zlog_err("Link '%s' <-> '%s' cannot be mapped to an available link", buf1, buf2);
		return -1;
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
		return link->ovsdb_inserted;
	}

	return 0;
}

static int
ospf6_ovsdb_delete_node (struct ospf6_ovsdb *ovsdb, struct ospf6_ovsdb_node *node)
{
	struct srdb_table *tbl;
	int ret;

	tbl = srdb_table_by_name(ovsdb->srdb->tables, "NodeState");
	if (!tbl) {
		zlog_err("Cannot find NodeState table");
		return -1;
	}

	ret = srdb_delete_sync(ovsdb->srdb, tbl,
			       (struct srdb_entry *) &node->db_entry, NULL);
	if (ret) {
		zlog_err("Cannot delete NodeState name='%s' addr='%s' prefix='%s' pbsid='%s'",
		         node->db_entry.name, node->db_entry.addr, node->db_entry.prefix, node->db_entry.pbsid);
		return -1;
	}

	return 0;
}

static int
ospf6_ovsdb_delete_link (struct ospf6_ovsdb *ovsdb, struct ospf6_ovsdb_link *link)
{
	struct srdb_table *tbl;
	int ret;

	tbl = srdb_table_by_name(ovsdb->srdb->tables, "LinkState");
	if (!tbl) {
		zlog_err("Cannot find LinkState table");
		return -1;
	}

	ret = srdb_delete_sync(ovsdb->srdb, tbl,
	                       (struct srdb_entry *) &link->db_entry, NULL);
	if (ret) {
		zlog_err("Cannot delete LinkState name1='%s' addr1='%s' name2='%s' addr2='%s'",
		         link->db_entry.name1, link->db_entry.addr1, link->db_entry.name2, link->db_entry.addr2);
		return -1;
	}

	return 0;
}

void
ospf6_ovsdb_delete_down_element (struct ospf6_ovsdb *ovsdb)
{
	struct hmap_entry *he;
	struct ospf6_ovsdb_node *node;
	struct ospf6_ovsdb_link *link;

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
}

