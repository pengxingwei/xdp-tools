/* SPDX-License-Identifier: GPL-2.0 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>

#include <locale.h>
#include <unistd.h>
#include <time.h>

#include <bpf/bpf.h>
#include "libbpf.h"
#include <arpa/inet.h>

#include <net/if.h>
#include <linux/if_ether.h>
#include <linux/if_link.h> /* depend on kernel-headers installed */

#include "params.h"
#include "logging.h"
#include "util.h"
#include "common_kern_user.h"
#include "prog_features.h"

#define NEED_RLIMIT (20*1024*1024) /* 10 Mbyte */
#define BPFFS_DIR "xdp-filter"

struct flag_val map_flags_all[] = {
	{"src", MAP_FLAG_SRC},
	{"dst", MAP_FLAG_DST},
	{"tcp", MAP_FLAG_TCP},
	{"udp", MAP_FLAG_UDP},
	{}
};

struct flag_val map_flags_srcdst[] = {
	{"src", MAP_FLAG_SRC},
	{"dst", MAP_FLAG_DST},
	{}
};

struct flag_val map_flags_tcpudp[] = {
	{"tcp", MAP_FLAG_TCP},
	{"udp", MAP_FLAG_UDP},
	{}
};

static char *find_progname(__u32 features)
{
	struct prog_feature *feat;

	if (!features)
		return NULL;

	for (feat = prog_features; feat->prog_name; feat++) {
		if ((ntohl(feat->features) & features) == features)
			return feat->prog_name;
	}
	return NULL;
}

int map_get_counter_flags(int fd, void *key, __u64 *counter, __u8 *flags)
{
	/* For percpu maps, userspace gets a value per possible CPU */
	unsigned int nr_cpus = libbpf_num_possible_cpus();
	__u64 values[nr_cpus];
	__u64 sum_ctr = 0;
	int i;

	if ((bpf_map_lookup_elem(fd, key, values)) != 0)
		return -ENOENT;

	/* Sum values from each CPU */
	for (i = 0; i < nr_cpus; i++) {
		__u8 flg = values[i] & MAP_FLAGS;

		if (!flg)
			return -ENOENT; /* not set */
		*flags = flg;
		sum_ctr += values[i] >> COUNTER_SHIFT;
	}
	*counter = sum_ctr;

	return 0;
}

int map_set_flags(int fd, void *key, __u8 flags)
{
	/* For percpu maps, userspace gets a value per possible CPU */
	unsigned int nr_cpus = libbpf_num_possible_cpus();
	__u64 values[nr_cpus];
	int i;

	if ((bpf_map_lookup_elem(fd, key, values)) != 0)
		memset(values, 0, sizeof(values));

	for (i = 0; i < nr_cpus; i++)
		values[i]  = flags ? (values[i] & ~MAP_FLAGS) | (flags & MAP_FLAGS) : 0;

	pr_debug("Setting new map value %llu from flags %u\n", values[0], flags);

	return bpf_map_update_elem(fd, key, &values, 0);
}

struct loadopt {
	bool help;
	struct iface iface;
	int features;
	bool force;
	bool skb_mode;
};

struct flag_val load_features[] = {
	{"tcp", FEAT_TCP},
	{"udp", FEAT_UDP},
	{"ipv6", FEAT_IPV6},
	{"ipv4", FEAT_IPV4},
	{"ethernet", FEAT_ETHERNET},
	{"all", FEAT_ALL},
	{}
};

static struct option_wrapper load_options[] = {
	DEFINE_OPTION('h', "help", no_argument, false, OPT_HELP, NULL,
		      "Show help", "",
		      struct loadopt, help),
	DEFINE_OPTION('v', "verbose", no_argument, false, OPT_VERBOSE, NULL,
		      "Enable verbose logging", "",
		      struct loadopt, help),
	DEFINE_OPTION('F', "force", no_argument, false, OPT_BOOL, NULL,
		      "Force loading of XDP program", "",
		      struct loadopt, force),
	DEFINE_OPTION('s', "skb-mode", no_argument, false, OPT_BOOL, NULL,
		      "Load XDP program in SKB (generic) mode", "",
		      struct loadopt, skb_mode),
	DEFINE_OPTION('d', "dev", required_argument, true, OPT_IFNAME, NULL,
		      "Load on device <ifname>", "<ifname>",
		      struct loadopt, iface),
	DEFINE_OPTION('f', "features", optional_argument, true,
		      OPT_FLAGS, load_features,
		      "Enable features <feats>", "<feats>",
		      struct loadopt, features),
	END_OPTIONS
};

int do_load(int argc, char **argv)
{
	char *progname, pin_root_path[PATH_MAX];
	struct bpf_object *obj = NULL;
	struct loadopt opt = {};
	struct bpf_program *prog;
	int err = EXIT_SUCCESS;
	DECLARE_LIBBPF_OPTS(bpf_object_open_opts, opts,
			    .pin_root_path = pin_root_path);

	parse_cmdline_args(argc, argv, load_options, &opt,
			   "xdp-filter load",
			   "Load xdp-filter on an interface");

	progname = find_progname(opt.features);
	if (!progname) {
		pr_warn("Couldn't find an eBPF program with the requested feature set!\n");
		return EXIT_FAILURE;
	}

	pr_debug("Found prog '%s' matching feature set to be loaded on interface '%s'.\n",
		 progname, opt.iface.ifname);

	err = check_bpf_environ(NEED_RLIMIT);
	if (err)
		goto out;

	err = get_bpf_root_dir(pin_root_path, sizeof(pin_root_path), BPFFS_DIR);
	if (err)
		goto out;

	obj = bpf_object__open_file(progname, &opts);
	err = libbpf_get_error(obj);
	if (err) {
		obj = NULL;
		goto out;
	}

	err = bpf_object__load(obj);
	if (err)
		goto out;

	prog = bpf_program__next(NULL, obj);
	if (!prog) {
		pr_warn("Couldn't find an eBPF program to attach. This is a bug!\n");
		goto out;
	}

	err = load_xdp_program(prog, opt.iface.ifindex, opt.force, opt.skb_mode);
	if (err) {
		pr_warn("Couldn't attach XDP program on iface '%s'\n",
			opt.iface.ifname);
		goto out;
	}

out:
	if (obj)
		bpf_object__close(obj);
	return err;
}

int print_ports(int map_fd)
{
	__u32 map_key = -1, next_key = 0;
	int err;

	printf("Filtered ports:\n");
	printf("  Port   Type             Hit counter\n");
	FOR_EACH_MAP_KEY(err, map_fd, map_key, next_key)
	{
		char buf[100];
		__u64 counter;
		__u8 flags;

		err = map_get_counter_flags(map_fd, &map_key, &counter, &flags);
		if (err == -ENOENT)
			continue;
		else if (err)
			return err;

		print_flags(buf, sizeof(buf), map_flags_all, flags);
		printf("  %-6u %-15s  %llu\n", ntohs(map_key), buf, counter);
	}
	return 0;
}

struct portopt {
	bool help;
	unsigned int mode;
	unsigned int proto;
	__u16 port;
	bool print_status;
	bool remove;
};

static struct option_wrapper port_options[] = {
	DEFINE_OPTION('r', "remove", no_argument, false, OPT_BOOL, NULL,
		      "Remove port instead of adding", "",
		      struct portopt, remove),
	DEFINE_OPTION('s', "status", no_argument, false, OPT_BOOL, NULL,
		      "Print status of filtered ports after changing", "",
		      struct portopt, print_status),
	DEFINE_OPTION('m', "mode", required_argument, false,
		      OPT_FLAGS, map_flags_srcdst,
		      "Filter mode; default dst", "<mode>",
		      struct portopt, mode),
	DEFINE_OPTION('P', "proto", required_argument, false,
		      OPT_FLAGS, map_flags_tcpudp,
		      "Protocol to filter; default tcp,udp", "<proto>",
		      struct portopt, proto),
	DEFINE_OPTION('p', "port", required_argument, true,
		      OPT_U16, NULL,
		      "Port to add or remove", "<port>",
		      struct portopt, port),
	DEFINE_OPTION('v', "verbose", no_argument, false, OPT_VERBOSE, NULL,
		      "Enable verbose logging", "",
		      struct portopt, help),
	DEFINE_OPTION('h', "help", no_argument, false, OPT_HELP, NULL,
		      "Show help", "",
		      struct portopt, help),
	END_OPTIONS
};


int do_port(int argc, char **argv)
{
	int map_fd = -1, err = EXIT_SUCCESS;
	char pin_root_path[PATH_MAX];
	char modestr[100], protostr[100];
	__u8 flags = 0;
	__u64 counter;
	__u32 map_key;
	struct portopt opt = {
		.mode = MAP_FLAG_DST,
		.proto = MAP_FLAG_TCP | MAP_FLAG_UDP,
	};

	parse_cmdline_args(argc, argv, port_options, &opt,
			   "xdp-filter port",
			   "Add or remove ports from xdp-filter");

	print_flags(modestr, sizeof(modestr), map_flags_srcdst, opt.mode);
	print_flags(protostr, sizeof(protostr), map_flags_tcpudp, opt.proto);
	pr_debug("%s %s port %u mode %s\n", opt.remove ? "Removing" : "Adding",
		 protostr, opt.port, modestr);

	err = check_bpf_environ(NEED_RLIMIT);
	if (err)
		goto out;

	err = get_bpf_root_dir(pin_root_path, sizeof(pin_root_path), BPFFS_DIR);
	if (err)
		goto out;

	map_fd = get_pinned_map_fd(pin_root_path, textify(MAP_NAME_PORTS));
	if (map_fd < 0) {
		pr_warn("Couldn't find port filter map; is xdp-filter loaded "
			"with the right features (udp and/or tcp)?\n");
		err = EXIT_FAILURE;
		goto out;
	}

	map_key = htons(opt.port);

	err = map_get_counter_flags(map_fd, &map_key, &counter, &flags);
	if (err && err != -ENOENT)
		goto out;

	if (opt.remove)
		flags &= ~(opt.mode | opt.proto);
	else
		flags |= opt.mode | opt.proto;

	if (!(flags & (MAP_FLAG_DST | MAP_FLAG_SRC)) ||
	    !(flags & (MAP_FLAG_TCP | MAP_FLAG_UDP)))
		flags = 0;

	err = map_set_flags(map_fd, &map_key, flags);
	if (err)
		goto out;

	if (opt.print_status) {
		err = print_ports(map_fd);
		if (err)
			goto out;
	}

out:
	if (map_fd >= 0)
		close(map_fd);
	return err;
}

int __print_ips(int map_fd, int af)
{
	struct ip_addr map_key = {.af = af}, next_key = {};
	int err;

	FOR_EACH_MAP_KEY(err, map_fd, map_key.addr, next_key.addr)
	{
		char flagbuf[100], addrbuf[100];
		__u64 counter;
		__u8 flags;

		err = map_get_counter_flags(map_fd, &map_key.addr, &counter, &flags);
		if (err == -ENOENT)
			continue;
		else if (err)
			return err;

		print_flags(flagbuf, sizeof(flagbuf), map_flags_srcdst, flags);
		print_addr(addrbuf, sizeof(addrbuf), &map_key);
		printf("  %-40s %-8s  %llu\n", addrbuf, flagbuf, counter);
	}

	return 0;
}

int print_ips()
{
	char pin_root_path[PATH_MAX];
	int map_fd4, map_fd6;
	int err = 0;

	err = get_bpf_root_dir(pin_root_path, sizeof(pin_root_path), BPFFS_DIR);
	if (err)
		goto out;

	map_fd6 = get_pinned_map_fd(pin_root_path, textify(MAP_NAME_IPV6));
	map_fd4 = get_pinned_map_fd(pin_root_path, textify(MAP_NAME_IPV4));
	if (map_fd4 < 0 && map_fd6 < 0) {
		err = -ENOENT;
		goto out;
	}

	printf("Filtered IP addresses:\n");
	printf("  %-40s Type      Hit counter\n", "Address");

	if (map_fd6 >= 0) {
		err = __print_ips(map_fd6, AF_INET6);
		if (err)
			goto out;
	}

	if (map_fd4 >= 0)
		err = __print_ips(map_fd4, AF_INET);

out:
	if (map_fd4 >= 0)
		close(map_fd4);
	if (map_fd6 >= 0)
		close(map_fd6);
	return err;
}


static int __do_address(const char *map_name, const char *feat_name,
			void *map_key, bool remove, int mode)
{
	char pin_root_path[PATH_MAX];
	int map_fd = -1, err = 0;
	__u8 flags = 0;
	__u64 counter;
	err = check_bpf_environ(NEED_RLIMIT);
	if (err)
		goto out;

	err = get_bpf_root_dir(pin_root_path, sizeof(pin_root_path), BPFFS_DIR);
	if (err)
		goto out;

	map_fd = get_pinned_map_fd(pin_root_path, map_name);
	if (map_fd < 0) {
		pr_warn("Couldn't find filter map; is xdp-filter loaded "
			"with the %s feature?\n", feat_name);
		err = -ENOENT;
		goto out;
	}

	err = map_get_counter_flags(map_fd, map_key, &counter, &flags);
	if (err && err != -ENOENT)
		goto out;

	if (remove)
		flags &= ~mode;
	else
		flags |= mode;

	err = map_set_flags(map_fd, map_key, flags);
	if (err)
		goto out;

out:
	return err ?: map_fd;
}

struct ipopt {
	bool help;
	unsigned int mode;
	struct ip_addr addr;
	bool print_status;
	bool remove;
};

static struct option_wrapper ip_options[] = {
	DEFINE_OPTION('r', "remove", no_argument, false, OPT_BOOL, NULL,
		      "Remove port instead of adding", "",
		      struct ipopt, remove),
	DEFINE_OPTION('s', "status", no_argument, false, OPT_BOOL, NULL,
		      "Print status of filtered ports after changing", "",
		      struct ipopt, print_status),
	DEFINE_OPTION('m', "mode", required_argument, false,
		      OPT_FLAGS, map_flags_srcdst,
		      "Filter mode; default dst", "<mode>",
		      struct ipopt, mode),
	DEFINE_OPTION('a', "addr", required_argument, true,
		      OPT_IPADDR, NULL,
		      "Address to add or remove", "<addr>",
		      struct ipopt, addr),
	DEFINE_OPTION('v', "verbose", no_argument, false, OPT_VERBOSE, NULL,
		      "Enable verbose logging", "",
		      struct ipopt, help),
	DEFINE_OPTION('h', "help", no_argument, false, OPT_HELP, NULL,
		      "Show help", "",
		      struct ipopt, help),
	END_OPTIONS
};

static int do_ip(int argc, char **argv)
{
	int map_fd = -1, err = EXIT_SUCCESS;
	char modestr[100], addrstr[100];
	bool v6;
	struct ipopt opt = {
		.mode = MAP_FLAG_DST,
	};

	parse_cmdline_args(argc, argv, ip_options, &opt,
			   "xdp-filter ip",
			   "Add or remove IP addresses from xdp-filter");

	print_flags(modestr, sizeof(modestr), map_flags_srcdst, opt.mode);
	print_addr(addrstr, sizeof(addrstr), &opt.addr);
	pr_debug("%s addr %s mode %s\n", opt.remove ? "Removing" : "Adding",
		 addrstr, modestr);

	v6 = (opt.addr.af == AF_INET6);

	map_fd = __do_address(v6 ? textify(MAP_NAME_IPV6) : textify(MAP_NAME_IPV4),
			      v6 ? "ipv6" : "ipv4",
			      &opt.addr.addr, opt.remove, opt.mode);
	if (map_fd < 0) {
		err = map_fd;
		goto out;
	}

	if (opt.print_status) {
		err = print_ips();
		if (err)
			goto out;
	}

out:
	if (map_fd >= 0)
		close(map_fd);
	return err;
}

int print_ethers(int map_fd)
{
	struct mac_addr map_key = {}, next_key = {};
	int err;

	printf("Filtered MAC addresses:\n");
	printf("  %-18s Type             Hit counter\n", "Address");
	FOR_EACH_MAP_KEY(err, map_fd, map_key, next_key)
	{
		char modebuf[100], addrbuf[100];
		__u64 counter;
		__u8 flags;

		err = map_get_counter_flags(map_fd, &map_key, &counter, &flags);
		if (err == -ENOENT)
			continue;
		else if (err)
			return err;

		print_flags(modebuf, sizeof(modebuf), map_flags_srcdst, flags);
		print_macaddr(addrbuf, sizeof(addrbuf), &map_key);
		printf("  %-18s %-15s  %llu\n", addrbuf, modebuf, counter);
	}
	return 0;
}

struct etheropt {
	bool help;
	unsigned int mode;
	struct mac_addr addr;
	bool print_status;
	bool remove;
};

static struct option_wrapper ether_options[] = {
	DEFINE_OPTION('r', "remove", no_argument, false, OPT_BOOL, NULL,
		      "Remove port instead of adding", "",
		      struct etheropt, remove),
	DEFINE_OPTION('s', "status", no_argument, false, OPT_BOOL, NULL,
		      "Print status of filtered ports after changing", "",
		      struct etheropt, print_status),
	DEFINE_OPTION('m', "mode", required_argument, false,
		      OPT_FLAGS, map_flags_srcdst,
		      "Filter mode; default dst", "<mode>",
		      struct etheropt, mode),
	DEFINE_OPTION('a', "addr", required_argument, true,
		      OPT_MACADDR, NULL,
		      "Address to add or remove", "<addr>",
		      struct etheropt, addr),
	DEFINE_OPTION('v', "verbose", no_argument, false, OPT_VERBOSE, NULL,
		      "Enable verbose logging", "",
		      struct etheropt, help),
	DEFINE_OPTION('h', "help", no_argument, false, OPT_HELP, NULL,
		      "Show help", "",
		      struct etheropt, help),
	END_OPTIONS
};

static int do_ether(int argc, char **argv)
{
	int err = EXIT_SUCCESS, map_fd = -1;
	char modestr[100], addrstr[100];
	struct etheropt opt = {
		.mode = MAP_FLAG_DST,
	};

	parse_cmdline_args(argc, argv, ether_options, &opt,
			   "xdp-filter ether",
			   "Add or remove Ethernet addresses from xdp-filter");

	print_flags(modestr, sizeof(modestr), map_flags_srcdst, opt.mode);
	print_macaddr(addrstr, sizeof(addrstr), &opt.addr);
	pr_debug("%s addr %s mode %s\n", opt.remove ? "Removing" : "Adding",
		 addrstr, modestr);

	map_fd = __do_address(textify(MAP_NAME_ETHERNET),
			      "ethernet", &opt.addr.addr, opt.remove, opt.mode);
	if (map_fd < 0) {
		err = map_fd;
		goto out;
	}

	if (opt.print_status) {
		err = print_ethers(map_fd);
		if (err)
			goto out;
	}

out:
	if (map_fd >= 0)
		close(map_fd);
	return err;
}

int do_status(int argc, char **argv)
{
	char pin_root_path[PATH_MAX];
	int err = EXIT_SUCCESS, map_fd = -1;

	err = check_bpf_environ(NEED_RLIMIT);
	if (err)
		goto out;

	err = get_bpf_root_dir(pin_root_path, sizeof(pin_root_path), BPFFS_DIR);
	if (err)
		goto out;

	map_fd = get_pinned_map_fd(pin_root_path, textify(MAP_NAME_PORTS));
	if (map_fd >= 0) {
		err = print_ports(map_fd);
		if (err)
			goto out;
		printf("\n");
	}
	close(map_fd);
	map_fd = -1;

	err = print_ips(pin_root_path);
	if (err)
		goto out;

	printf("\n");

	map_fd = get_pinned_map_fd(pin_root_path, textify(MAP_NAME_ETHERNET));
	if (map_fd >= 0) {
		err = print_ethers(map_fd);
		if (err)
			goto out;
	}


out:
	if (map_fd >= 0)
		close(map_fd);
	return err;
}

int do_help(int argc, char **argv)
{
	fprintf(stderr,
		"Usage: xdp-filter COMMAND [options]\n"
		"\n"
		"COMMAND can be one of:\n"
		"       load        - load xdp-filter on an interface\n"
		"       port        - add a port to the blacklist\n"
		"       ip          - add an IP address to the blacklist\n"
		"       ether       - add an Ethernet MAC address to the blacklist\n"
		"       status      - show current xdp-filter status\n"
		"       help        - show this help message\n"
		"\n"
		"Use 'xdp-filter COMMAND --help' to see options for each command\n");
	exit(-1);
}


static const struct cmd {
	const char *cmd;
	int (*func)(int argc, char **argv);
} cmds[] = {
	{ "load",	do_load },
	{ "port",	do_port },
	{ "ip",	do_ip },
	{ "ether",	do_ether },
	{ "status",	do_status },
	{ "help",	do_help },
	{ 0 }
};

static int do_cmd(const char *argv0, int argc, char **argv)
{
	const struct cmd *c;

	for (c = cmds; c->cmd; ++c) {
		if (is_prefix(argv0, c->cmd))
			return -(c->func(argc, argv));
	}

	fprintf(stderr, "Object \"%s\" is unknown, try \"xdp-filt help\".\n", argv0);
	return EXIT_FAILURE;
}

const char *pin_basedir =  "/sys/fs/bpf";

int main(int argc, char **argv)
{
	init_logging();

	if (argc > 1)
		return do_cmd(argv[1], argc-1, argv+1);

	do_help(argc, argv);
	return EXIT_FAILURE;
}