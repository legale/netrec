#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "netlink.h"
#include "state.h"
#include "verify.h"
#include "yaml.h"

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s [-a|--apply] -c config.yaml\n", prog);
}

int main(int argc, char **argv)
{
	/* Поток простой: YAML -> netlink snapshot -> verify -> optional apply. */
	struct desired_state ds;
	struct real_state rs;
	const char *cfg = NULL;
	int i;
	int rc;
	int apply = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-a") || !strcmp(argv[i], "--apply")) {
			apply = 1;
			continue;
		}

		if (!strcmp(argv[i], "-c") && i + 1 < argc) {
			cfg = argv[++i];
			continue;
		}

		usage(argv[0]);
		return 2;
	}

	if (!cfg) {
		usage(argv[0]);
		return 2;
	}

	/* desired_state - только то, что пользователь описал в YAML. */
	rc = yaml_load_desired(cfg, &ds);
	if (rc) {
		return 1;
	}

	/* real_state - снимок ядра через rtnetlink, без изменения системы. */
	rc = nl_load_state(&rs);
	if (rc) {
		fprintf(stderr, "netlink: failed: %s\n", nl_errstr(rc));
		rs_free(&rs);
		return 1;
	}

	/* verifier печатает OK/MISS/ACT; apply выполняет только ACT. */
	rc = verify_state(&ds, &rs, apply) ? 1 : 0;
	rs_free(&rs);
	return rc;
}
