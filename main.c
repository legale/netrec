#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>

#include "netlink.h"
#include "state.h"
#include "verify.h"
#include "yaml.h"

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s [-a|--apply] -c config.yaml\n", prog);
}

static int apply_lock(void)
{
	int fd;

	fd = open("/tmp/netrec.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (fd < 0) {
		fprintf(stderr, "apply: lock open failed: %s\n", strerror(errno));
		return -errno;
	}

	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
		fprintf(stderr, "apply: another netrec apply is running\n");
		close(fd);
		return -EAGAIN;
	}

	return fd;
}

static int load_real(struct real_state *rs)
{
	int rc;

	rc = nl_load_state(rs);
	if (rc) {
		fprintf(stderr, "netlink: failed: %s\n", nl_errstr(rc));
		rs_free(rs);
		return rc;
	}

	return 0;
}

static int run_verify(const struct desired_state *ds, int apply, int *act_fail)
{
	struct real_state rs;
	int rc;

	rc = load_real(&rs);
	if (rc) {
		return rc;
	}

	*act_fail = 0;
	rc = verify_state(ds, &rs, apply, act_fail);
	rs_free(&rs);
	return rc;
}

int main(int argc, char **argv)
{
	struct desired_state ds;
	const char *cfg = NULL;
	int act_fail = 0;
	int lock_fd = -1;
	int apply = 0;
	int diff;
	int i;
	int rc;

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

	rc = yaml_load_desired(cfg, &ds);
	if (rc) {
		return 1;
	}

	if (apply) {
		lock_fd = apply_lock();
		if (lock_fd < 0) {
			return 1;
		}
	}

	diff = run_verify(&ds, apply, &act_fail);
	if (!apply) {
		return diff ? 1 : 0;
	}
	if (diff < 0 || act_fail) {
		return 1;
	}
	if (!diff) {
		return 0;
	}

	printf("POST_VERIFY\n");
	diff = run_verify(&ds, 0, &act_fail);
	if (diff || act_fail) {
		return 1;
	}

	(void)lock_fd;
	return 0;
}
