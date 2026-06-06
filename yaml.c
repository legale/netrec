#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yaml.h>

#include "yaml.h"

static void cpy(char *dst, size_t sz, const char *src)
{
	if (!sz) {
		return;
	}

	if (!src) {
		dst[0] = '\0';
		return;
	}

	snprintf(dst, sz, "%s", src);
}

static yaml_node_t *map_get(yaml_document_t *doc, yaml_node_t *map,
				    const char *key)
{
	/* YAML поддерживается как простая map->map->scalar структура. */
	yaml_node_pair_t *p;
	yaml_node_t *k;

	if (!map || map->type != YAML_MAPPING_NODE) {
		return NULL;
	}

	for (p = map->data.mapping.pairs.start;
	     p < map->data.mapping.pairs.top; p++) {
		k = yaml_document_get_node(doc, p->key);
		if (!k || k->type != YAML_SCALAR_NODE) {
			continue;
		}

		if (!strcmp((char *)k->data.scalar.value, key)) {
			return yaml_document_get_node(doc, p->value);
		}
	}

	return NULL;
}

static const char *scalar(yaml_node_t *node)
{
	if (!node || node->type != YAML_SCALAR_NODE) {
		return NULL;
	}

	return (char *)node->data.scalar.value;
}

static int get_str(yaml_document_t *doc, yaml_node_t *map, const char *key,
		   char *dst, size_t sz, int req)
{
	const char *s;

	s = scalar(map_get(doc, map, key));
	if (!s) {
		if (!req) {
			dst[0] = '\0';
			return 0;
		}

		fprintf(stderr, "yaml: missing %s\n", key);
		return -EINVAL;
	}

	cpy(dst, sz, s);
	return 0;
}


static int get_str_seq(yaml_document_t *doc, yaml_node_t *map, const char *key,
		       char dst[][IFNAMSIZ], int *nr, int max)
{
	/* bridge.ports - единственный список в YAML MVP. */
	yaml_node_item_t *it;
	yaml_node_t *seq;
	yaml_node_t *node;
	const char *v;
	int n = 0;

	seq = map_get(doc, map, key);
	*nr = 0;
	if (!seq) {
		return 0;
	}
	if (seq->type != YAML_SEQUENCE_NODE) {
		fprintf(stderr, "yaml: %s must be sequence\n", key);
		return -EINVAL;
	}

	for (it = seq->data.sequence.items.start;
	     it < seq->data.sequence.items.top; it++) {
		if (n >= max) {
			fprintf(stderr, "yaml: too many %s, max=%d\n", key, max);
			return -EINVAL;
		}

		node = yaml_document_get_node(doc, *it);
		v = scalar(node);
		if (!v || !*v) {
			fprintf(stderr, "yaml: bad %s item\n", key);
			return -EINVAL;
		}

		cpy(dst[n], IFNAMSIZ, v);
		n++;
	}

	*nr = n;
	return 0;
}

static int get_u32(yaml_document_t *doc, yaml_node_t *map, const char *key,
		   uint32_t *dst)
{
	const char *s;
	char *end;
	unsigned long v;

	s = scalar(map_get(doc, map, key));
	if (!s) {
		fprintf(stderr, "yaml: missing %s\n", key);
		return -EINVAL;
	}

	errno = 0;
	v = strtoul(s, &end, 0);
	if (errno || *end || v > UINT32_MAX) {
		fprintf(stderr, "yaml: bad %s=%s\n", key, s);
		return -EINVAL;
	}

	*dst = (uint32_t)v;
	return 0;
}

static int is_scn(const struct desired_state *ds, const char *a, const char *b)
{
	if (!strcmp(ds->scenario, a)) {
		return 1;
	}
	if (b && !strcmp(ds->scenario, b)) {
		return 1;
	}

	return 0;
}

static int load_bridge(yaml_document_t *doc, yaml_node_t *root,
		       struct desired_state *ds)
{
	yaml_node_t *br;
	int rc;

	br = map_get(doc, root, "bridge");
	if (!br) {
		fprintf(stderr, "yaml: bridge required\n");
		return -EINVAL;
	}

	rc = get_str(doc, br, "name", ds->br_name, sizeof(ds->br_name), 1);
	if (rc) {
		return rc;
	}

	rc = get_str(doc, br, "addr", ds->br_addr, sizeof(ds->br_addr), 0);
	if (rc) {
		return rc;
	}

	return get_str_seq(doc, br, "ports", ds->br_ports,
			   &ds->n_br_ports, NR_BR_PORT_MAX);
}

static int load_uplink(yaml_document_t *doc, yaml_node_t *root,
		       struct desired_state *ds)
{
	yaml_node_t *up;

	up = map_get(doc, root, "uplink");
	if (!up) {
		fprintf(stderr, "yaml: uplink required\n");
		return -EINVAL;
	}

	return get_str(doc, up, "ifname", ds->up_ifname,
		       sizeof(ds->up_ifname), 1);
}

static int load_vlan(yaml_document_t *doc, yaml_node_t *root,
		     struct desired_state *ds)
{
	yaml_node_t *vl;
	int rc;

	vl = map_get(doc, root, "vlan");
	if (!vl) {
		fprintf(stderr, "yaml: vlan required\n");
		return -EINVAL;
	}

	rc = get_str(doc, vl, "ifname", ds->vlan_ifname,
		     sizeof(ds->vlan_ifname), 1);
	if (rc) {
		return rc;
	}
	rc = get_u32(doc, vl, "id", &ds->vlan_id);
	if (rc) {
		return rc;
	}
	rc = get_str(doc, vl, "link", ds->vlan_link,
		     sizeof(ds->vlan_link), 0);
	if (rc) {
		return rc;
	}
	rc = get_str(doc, vl, "bridge", ds->vlan_bridge,
		     sizeof(ds->vlan_bridge), 0);
	if (rc) {
		return rc;
	}

	if (!ds->vlan_link[0]) {
		cpy(ds->vlan_link, sizeof(ds->vlan_link), ds->up_ifname);
	}
	if (!ds->vlan_bridge[0]) {
		cpy(ds->vlan_bridge, sizeof(ds->vlan_bridge), ds->br_name);
	}

	if (strcmp(ds->vlan_link, ds->up_ifname)) {
		fprintf(stderr, "yaml: vlan.link must equal uplink.ifname\n");
		return -EINVAL;
	}
	if (strcmp(ds->vlan_bridge, ds->br_name)) {
		fprintf(stderr, "yaml: vlan.bridge must equal bridge.name\n");
		return -EINVAL;
	}

	return 0;
}

static int load_wg_vxlan(yaml_document_t *doc, yaml_node_t *root,
			 struct desired_state *ds)
{
	yaml_node_t *wg;
	yaml_node_t *vx;
	int rc;

	wg = map_get(doc, root, "wg");
	vx = map_get(doc, root, "vxlan");
	if (!wg || !vx) {
		fprintf(stderr, "yaml: wg/vxlan required\n");
		return -EINVAL;
	}

	rc = get_str(doc, wg, "ifname", ds->wg_ifname,
		     sizeof(ds->wg_ifname), 1);
	if (rc) {
		return rc;
	}
	rc = get_str(doc, wg, "peer_ip", ds->wg_peer_ip,
		     sizeof(ds->wg_peer_ip), 1);
	if (rc) {
		return rc;
	}
	rc = get_str(doc, wg, "route_dev", ds->wg_route_dev,
		     sizeof(ds->wg_route_dev), 1);
	if (rc) {
		return rc;
	}

	rc = get_str(doc, vx, "ifname", ds->vx_ifname,
		     sizeof(ds->vx_ifname), 1);
	if (rc) {
		return rc;
	}
	rc = get_u32(doc, vx, "vni", &ds->vx_vni);
	if (rc) {
		return rc;
	}
	rc = get_str(doc, vx, "remote", ds->vx_remote,
		     sizeof(ds->vx_remote), 1);
	if (rc) {
		return rc;
	}
	rc = get_str(doc, vx, "dev", ds->vx_dev, sizeof(ds->vx_dev), 1);
	if (rc) {
		return rc;
	}
	rc = get_str(doc, vx, "bridge", ds->vx_bridge,
		     sizeof(ds->vx_bridge), 1);
	if (rc) {
		return rc;
	}

	if (strcmp(ds->wg_route_dev, ds->br_name)) {
		fprintf(stderr, "yaml: wg.route_dev must equal bridge.name\n");
		return -EINVAL;
	}
	if (strcmp(ds->vx_dev, ds->wg_ifname)) {
		fprintf(stderr, "yaml: vxlan.dev must equal wg.ifname\n");
		return -EINVAL;
	}
	if (strcmp(ds->vx_bridge, ds->br_name)) {
		fprintf(stderr, "yaml: vxlan.bridge must equal bridge.name\n");
		return -EINVAL;
	}

	return 0;
}

static int load_doc(yaml_document_t *doc, struct desired_state *ds)
{
	/* Сначала scenario, потом только нужные секции этого scenario. */
	yaml_node_t *root;
	int rc;

	root = yaml_document_get_root_node(doc);
	if (!root || root->type != YAML_MAPPING_NODE) {
		fprintf(stderr, "yaml: root must be mapping\n");
		return -EINVAL;
	}

	rc = get_str(doc, root, "scenario", ds->scenario,
		     sizeof(ds->scenario), 1);
	if (rc) {
		return rc;
	}

	if (is_scn(ds, "only_uplink", "only_uplink_iface")) {
		return load_uplink(doc, root, ds);
	}
	if (is_scn(ds, "uplink_bridge", "uplink_in_bridge")) {
		rc = load_bridge(doc, root, ds);
		if (rc) {
			return rc;
		}
		return load_uplink(doc, root, ds);
	}
	if (is_scn(ds, "vlan_bridge", "vlan_in_bridge")) {
		rc = load_bridge(doc, root, ds);
		if (rc) {
			return rc;
		}
		rc = load_uplink(doc, root, ds);
		if (rc) {
			return rc;
		}
		return load_vlan(doc, root, ds);
	}
	if (!strcmp(ds->scenario, "wg_vxlan_bridge")) {
		rc = load_bridge(doc, root, ds);
		if (rc) {
			return rc;
		}
		rc = load_uplink(doc, root, ds);
		if (rc) {
			return rc;
		}
		return load_wg_vxlan(doc, root, ds);
	}

	fprintf(stderr, "yaml: unsupported scenario=%s\n", ds->scenario);
	return -EINVAL;
}

int yaml_load_desired(const char *path, struct desired_state *ds)
{
	/* libyaml нужен только для чтения config.yaml в desired_state. */
	yaml_parser_t parser;
	yaml_document_t doc;
	FILE *f;
	int rc;

	memset(ds, 0, sizeof(*ds));

	f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return -errno;
	}

	if (!yaml_parser_initialize(&parser)) {
		fclose(f);
		return -ENOMEM;
	}

	yaml_parser_set_input_file(&parser, f);
	if (!yaml_parser_load(&parser, &doc)) {
		fprintf(stderr, "yaml: parse failed at line %lu\n",
			(unsigned long)parser.problem_mark.line + 1);
		yaml_parser_delete(&parser);
		fclose(f);
		return -EINVAL;
	}

	rc = load_doc(&doc, ds);
	yaml_document_delete(&doc);
	yaml_parser_delete(&parser);
	fclose(f);

	return rc;
}
