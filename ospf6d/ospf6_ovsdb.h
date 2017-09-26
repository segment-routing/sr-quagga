
#include <srdb.h>

#ifndef OSPF6_OVSDB_H


struct ospf6_ovsdb_node {
	int ospf_id;
	struct srdb_nodestate_entry db_entry;
	/* Last time this node was up */
	int last_spf;
	int current_spf;
	int ovsdb_inserted;
};

struct ospf6_ovsdb_link {
	int ospf_id_1;
	int ospf_id_2;
	struct srdb_linkstate_entry db_entry;
	/* Last time this node was up */
	int last_spf;
	int current_spf;
	int ovsdb_inserted;
};

/* OVSDB structure */
struct ospf6_ovsdb
{
	struct ovsdb_config ovsdb_conf;
	struct srdb *srdb;

	struct hashmap *nodes_map;
	struct hashmap *links_map;
};


struct ospf6_ovsdb*
ospf6_ovsdb_create (const char *proto, const char *ip6, const char *port, const char *database);

void
ospf6_ovsdb_delete (struct ospf6_ovsdb *ovsdb);

int
ospf6_ovsdb_set_active_router (struct ospf6_ovsdb *ovsdb, int routerid);
int
ospf6_ovsdb_set_active_link (struct ospf6_ovsdb *ovsdb, int routerid_1, int routerid_2);

void
ospf6_ovsdb_delete_down_element (struct ospf6_ovsdb *ovsdb);

#define OSPF6_OVSDB_H

#endif /* OSPF6_OVSDB_H */
