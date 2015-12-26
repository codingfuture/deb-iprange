/*
 * Copyright (C) 2003-2015 Costa Tsaousis <costa@tsaousis.gr>
 * Copyright (C) 2012-2015 Phil Whineray <phil@sanewall.org>
 * Copyright (C) 2003 Gabriel L. Somlo
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#if defined(HAVE_INTTYPES_H)
#include <inttypes.h>
#elif defined(HAVE_STDINT_H)
#include <stdint.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/types.h>
#include <netdb.h>
#include <pthread.h>
#include <unistd.h>

/*
 * the maximum line element to read in input files
 * normally the elements are IP, IP/MASK, HOSTNAME
 */
#define MAX_INPUT_ELEMENT 255

#define BINARY_HEADER_V10 "iprange binary format v1.0\n"
static uint32_t endianess = 0x1A2B3C4D;

static char *PROG;
static int debug;
static int cidr_use_network = 1;
static int default_prefix = 32;

static char *print_prefix_ips = "";
static char *print_prefix_nets = "";
static char *print_suffix_ips = "";
static char *print_suffix_nets = "";

/*---------------------------------------------------------------------*/
/* network address type: one field for the net address, one for prefix */
/*---------------------------------------------------------------------*/
typedef struct network_addr {
	in_addr_t addr;
	in_addr_t broadcast;
} network_addr_t;

/*------------------------------------------------------------------*/
/* Set a bit to a given value (0 or 1); MSB is bit 1, LSB is bit 32 */
/*------------------------------------------------------------------*/
static inline in_addr_t set_bit(in_addr_t addr, int bitno, int val)
{

	if (val)
		return (addr | (1 << (32 - bitno)));
	else
		return (addr & ~(1 << (32 - bitno)));

}				/* set_bit() */

/*--------------------------------------*/
/* Compute netmask address given prefix */
/*--------------------------------------*/
static inline in_addr_t netmask(int prefix)
{

	if (prefix == 0)
		return (~((in_addr_t) - 1));
	else
		return (~((1 << (32 - prefix)) - 1));

}				/* netmask() */

/*----------------------------------------------------*/
/* Compute broadcast address given address and prefix */
/*----------------------------------------------------*/
static inline in_addr_t broadcast(in_addr_t addr, int prefix)
{

	return (addr | ~netmask(prefix));

}				/* broadcast() */

/*--------------------------------------------------*/
/* Compute network address given address and prefix */
/*--------------------------------------------------*/
static inline in_addr_t network(in_addr_t addr, int prefix)
{

	return (addr & netmask(prefix));

}				/* network() */

/*------------------------------------------------*/
/* Print out a 32-bit address in A.B.C.D/M format */
/*------------------------------------------------*/

static int prefix_counters[33];
static int prefix_enabled[] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
static int split_range_disable_printing;


#ifdef SYSTEM_IP2STR
/*
 * #warning "Using system functions for ip2str()"
 * the default system ip2str()
 */

static inline char *ip2str(in_addr_t IP) {
	struct in_addr in;
	in.s_addr = htonl(IP);
	return inet_ntoa(in);
}

#else /* ! SYSTEM_IP2STR */
/* #warning "Using iprange internal functions for ip2str()" */

/*
 * very fast implementation of IP address printing
 * this is 30% faster than the system default (inet_ntoa() based)
 * http://stackoverflow.com/questions/1680365/integer-to-ip-address-c
 */

static inline char *ip2str(in_addr_t IP) {
	static char buf[10];

	int i, k = 0;
	char c0, c1;

	for(i = 0; i < 4; i++) {
		c0 = ((((IP & (0xff << ((3 - i) * 8))) >> ((3 - i) * 8))) / 100) + 0x30;
		if(c0 != '0') *(buf + k++) = c0;

		c1 = (((((IP & (0xff << ((3 - i) * 8))) >> ((3 - i) * 8))) % 100) / 10) + 0x30;
		if(!(c1 == '0' && c0 == '0')) *(buf + k++) = c1;

		*(buf + k) = (((((IP & (0xff << ((3 - i) * 8)))) >> ((3 - i) * 8))) % 10) + 0x30;
		k++;

		if(i < 3) *(buf + k++) = '.';
	}
	*(buf + k) = 0;

	return buf;
}
#endif /* ! SYSTEM_IP2STR */

static inline void print_addr(in_addr_t addr, int prefix)
{

	if(likely(prefix >= 0 && prefix <= 32))
		prefix_counters[prefix]++;

	if(unlikely(split_range_disable_printing)) return;

	if (prefix < 32)
		printf("%s%s/%d%s\n", print_prefix_nets, ip2str(addr), prefix, print_suffix_nets);
	else
		printf("%s%s%s\n", print_prefix_ips, ip2str(addr), print_suffix_ips);

}				/* print_addr() */

/*------------------------------------------------------------*/
/* Recursively compute network addresses to cover range lo-hi */
/*------------------------------------------------------------*/
/* Note: Worst case scenario is when lo=0.0.0.1 and hi=255.255.255.254
 *       We then have 62 CIDR bloks to cover this interval, and 125
 *       calls to split_range();
 *       The maximum possible recursion depth is 32.
 */

static inline int split_range(in_addr_t addr, int prefix, in_addr_t lo, in_addr_t hi)
{
	in_addr_t bc, lower_half, upper_half;

	if(unlikely(lo > hi)) {
		/*
		 * it should never happen
		 * give a log for the user to see
		 */
		in_addr_t t = hi;
		fprintf(stderr, "%s: WARNING: invalid range reversed start=%s", PROG, ip2str(lo));
		fprintf(stderr, " end=%s\n", ip2str(hi));
		hi = lo;
		lo = t;
	}

	if (unlikely((prefix < 0) || (prefix > 32))) {
		fprintf(stderr, "%s: Invalid netmask %d!\n", PROG, prefix);
		return 0;
	}

	bc = broadcast(addr, prefix);

	if (unlikely((lo < addr) || (hi > bc))) {
		fprintf(stderr, "%s: Out of range limits: %x, %x for "
			"network %x/%d, broadcast: %x!\n", PROG, lo, hi, addr, prefix, bc);
		return 0;
	}

	if ((lo == addr) && (hi == bc) && prefix_enabled[prefix]) {
		print_addr(addr, prefix);
		return 1;
	}

	prefix++;
	lower_half = addr;
	upper_half = set_bit(addr, prefix, 1);

	if (hi < upper_half) {
		return split_range(lower_half, prefix, lo, hi);
	} else if (lo >= upper_half) {
		return split_range(upper_half, prefix, lo, hi);
	}

	return (
		split_range(lower_half, prefix, lo, broadcast(lower_half, prefix)) +
		split_range(upper_half, prefix, upper_half, hi)
	);

}				/* split_range() */

/*-----------------------------------------------------------*/
/* Convert an A.B.C.D address into a 32-bit host-order value */
/*-----------------------------------------------------------*/
static inline in_addr_t a_to_hl(char *ipstr, int *err) {
	struct in_addr in;

	if (unlikely(!inet_aton(ipstr, &in))) {
		fprintf(stderr, "%s: Invalid address %s.\n", PROG, ipstr);
		in.s_addr = 0;
		if(err) (*err)++;
		return (ntohl(in.s_addr));
	}

	return (ntohl(in.s_addr));

}				/* a_to_hl() */

/*-----------------------------------------------------------------*/
/* convert a network address char string into a host-order network */
/* address and an integer prefix value                             */
/*-----------------------------------------------------------------*/
static inline network_addr_t str_to_netaddr(char *ipstr, int *err) {

	long int prefix = default_prefix;
	char *prefixstr;
	network_addr_t netaddr;

	if ((prefixstr = strchr(ipstr, '/'))) {
		*prefixstr = '\0';
		prefixstr++;
		errno = 0;
		prefix = strtol(prefixstr, (char **)NULL, 10);
		if (unlikely(errno || (*prefixstr == '\0') || (prefix < 0) || (prefix > 32))) {
			/* try the netmask format */
			in_addr_t mask = ~a_to_hl(prefixstr, err);
			/*fprintf(stderr, "mask is %u (0x%08x)\n", mask, mask);*/
			prefix = 32;
			while((likely(mask & 0x00000001))) {
				mask >>= 1;
				prefix--;
			}

			if(unlikely(mask)) {
				if(err) (*err)++;
				fprintf(stderr, "%s: Invalid netmask %s\n", PROG, prefixstr);
				netaddr.addr = 0;
				netaddr.broadcast = 0;
				return (netaddr);
			}
		}
	}

	if(likely(cidr_use_network))
		netaddr.addr = network(a_to_hl(ipstr, err), prefix);
	else
		netaddr.addr = a_to_hl(ipstr, err);

	netaddr.broadcast = broadcast(netaddr.addr, prefix);

	return (netaddr);

}				/* str_to_netaddr() */

/*----------------------------------------------------------*/
/* compare two network_addr_t structures; used with qsort() */
/* sort in increasing order by address, then by prefix.     */
/*----------------------------------------------------------*/
static int compar_netaddr(const void *p1, const void *p2)
{

	network_addr_t *na1 = (network_addr_t *) p1, *na2 = (network_addr_t *) p2;

	if (na1->addr < na2->addr)
		return (-1);
	if (na1->addr > na2->addr)
		return (1);
	if (na1->broadcast > na2->broadcast)
		return (-1);
	if (na1->broadcast < na2->broadcast)
		return (1);

	return (0);

}				/* compar_netaddr() */


/*------------------------------------------------------*/
/* Print out an address range in a.b.c.d-A.B.C.D format */
/*------------------------------------------------------*/
static inline void print_addr_range(in_addr_t lo, in_addr_t hi)
{

	if(unlikely(lo > hi)) {
		/*
		 * it should never happen
		 * give a log for the user to see
		 */
		in_addr_t t = hi;
		fprintf(stderr, "%s: WARNING: invalid range reversed start=%s", PROG, ip2str(lo));
		fprintf(stderr, " end=%s\n", ip2str(hi));
		hi = lo;
		lo = t;
	}

	if(lo == hi) {
		printf("%s%s-", print_prefix_ips, ip2str(lo));
		printf("%s%s\n", ip2str(hi), print_suffix_ips);
	}
	else {
		printf("%s%s-", print_prefix_nets, ip2str(lo));
		printf("%s%s\n", ip2str(hi), print_suffix_nets);
	}

}				/* print_addr_range() */

static inline void print_addr_single(in_addr_t x)
{

	printf("%s%s%s\n", print_prefix_ips, ip2str(x), print_suffix_ips);

}				/* print_addr_single() */


/* ---------------------------------------------------------------------------- */

#define NETADDR_INC 1024
#define MAX_LINE 1024

#define IPSET_FLAG_OPTIMIZED 	0x00000001

typedef struct ipset {
	char filename[FILENAME_MAX+1];
	/* char name[FILENAME_MAX+1]; */

	unsigned long int lines;
	unsigned long int entries;
	unsigned long int entries_max;
	unsigned long int unique_ips;		/* this is updated only after calling ipset_optimize() */

	uint32_t flags;

	struct ipset *next;
	struct ipset *prev;

	network_addr_t *netaddrs;
} ipset;


/* ----------------------------------------------------------------------------
 * ipset_create()
 *
 * create an empty ipset with the given name and free entries in its array
 *
 */

static inline ipset *ipset_create(const char *filename, int entries) {
	ipset *ips = malloc(sizeof(ipset));
	if(unlikely(!ips)) return NULL;

	if(entries < NETADDR_INC) entries = NETADDR_INC;

	ips->netaddrs = malloc(entries * sizeof(network_addr_t));
	if(unlikely(!ips->netaddrs)) {
		free(ips);
		return NULL;
	}

	ips->lines = 0;
	ips->entries = 0;
	ips->entries_max = entries;
	ips->unique_ips = 0;
	ips->next = NULL;
	ips->prev = NULL;
	ips->flags = 0;

	strncpy(ips->filename, (filename && *filename)?filename:"stdin", FILENAME_MAX);
	ips->filename[FILENAME_MAX] = '\0';

	/* strcpy(ips->name, ips->filename); */

	return ips;
}


/* ----------------------------------------------------------------------------
 * ipset_free()
 *
 * release the memory of an ipset and re-link its siblings so that lingage will
 * be consistent
 *
 */

static inline void ipset_free(ipset *ips) {
	if(ips->next) ips->next->prev = ips->prev;
	if(ips->prev) ips->prev->next = ips->next;

	free(ips->netaddrs);
	free(ips);
}


/* ----------------------------------------------------------------------------
 * ipset_free_all()
 *
 * release all the memory occupied by all ipsets linked together (prev, next)
 *
 */

static inline void ipset_free_all(ipset *ips) {
	if(ips->prev) {
		ips->prev->next = NULL;
		ipset_free_all(ips->prev);
	}

	if(ips->next) {
		ips->next->prev = NULL;
		ipset_free_all(ips->next);
	}

	ipset_free(ips);
}


/* ----------------------------------------------------------------------------
 * ipset_expand()
 *
 * exprand the ipset so that it will have at least the given number of free
 * entries in its internal array
 *
 */

static inline void ipset_expand(ipset *ips, unsigned long int free_entries_needed) {
	if(unlikely(!free_entries_needed)) free_entries_needed = 1;

	if(unlikely(ips && (ips->entries_max - ips->entries) < free_entries_needed)) {
		network_addr_t *n;

		ips->entries_max += (free_entries_needed < NETADDR_INC)?NETADDR_INC:free_entries_needed;

		n = realloc(ips->netaddrs, ips->entries_max * sizeof(network_addr_t));
		if(unlikely(!n)) {
			fprintf(stderr, "%s: Cannot re-allocate memory (%ld bytes)\n", PROG, ips->entries_max * sizeof(network_addr_t));
			exit(1);
		}
		ips->netaddrs = n;
	}
}

static inline void ipset_added_entry(ipset *ips) {
	register unsigned long entries = ips->entries;

	ips->lines++;
	ips->unique_ips += ips->netaddrs[entries].broadcast - ips->netaddrs[entries].addr + 1;

	if(likely(ips->flags & IPSET_FLAG_OPTIMIZED && entries > 0)) {
		/* the new is just next to the last */
		if(unlikely(ips->netaddrs[entries].addr == (ips->netaddrs[entries - 1].broadcast + 1))) {
			ips->netaddrs[entries - 1].broadcast = ips->netaddrs[entries].broadcast;
			return;
		}

		/* the new is after the end of the last */
		if(likely(ips->netaddrs[entries].addr > ips->netaddrs[entries - 1].broadcast)) {
			ips->entries++;
			return;
		}

		/* the new is before the beginning of the last */
		ips->flags ^= IPSET_FLAG_OPTIMIZED;

		if(unlikely(debug)) {
			in_addr_t new_from = ips->netaddrs[ips->entries].addr;
			in_addr_t new_to = ips->netaddrs[ips->entries].broadcast;

			in_addr_t last_from = ips->netaddrs[ips->entries - 1].addr;
			in_addr_t last_to = ips->netaddrs[ips->entries - 1].broadcast;

			fprintf(stderr, "%s: NON-OPTIMIZED %s at line %lu, entry %lu, last was %s (%u) - ", PROG, ips->filename, ips->lines, ips->entries, ip2str(last_from), last_from);
			fprintf(stderr, "%s (%u), new is ", ip2str(last_to), last_to);
			fprintf(stderr, "%s (%u) - ", ip2str(new_from), new_from);
			fprintf(stderr, "%s (%u)\n", ip2str(new_to), new_to);
		}
	}

	ips->entries++;
}

/* ----------------------------------------------------------------------------
 * ipset_add_ipstr()
 *
 * add a single IP entry to an ipset, by parsing the given IP string
 *
 */

static inline int ipset_add_ipstr(ipset *ips, char *ipstr) {
	int err = 0;

	ipset_expand(ips, 1);

	ips->netaddrs[ips->entries] = str_to_netaddr(ipstr, &err);
	if(!err) ipset_added_entry(ips);
	return !err;

}


/* ----------------------------------------------------------------------------
 * ipset_add()
 *
 * add an IP entry (from - to) to the ipset given
 *
 */

static inline void ipset_add(ipset *ips, in_addr_t from, in_addr_t to) {
	ipset_expand(ips, 1);

	ips->netaddrs[ips->entries].addr = from;
	ips->netaddrs[ips->entries].broadcast = to;
	ipset_added_entry(ips);

}


/* ----------------------------------------------------------------------------
 * ipset_optimize()
 *
 * takes an ipset with any number of entries (lo-hi pairs) in any order and
 * it optimizes it in place
 * after this optimization, all entries in the ipset are sorted (ascending)
 * and non-overlapping (it returns less or equal number of entries)
 *
 */

static inline void ipset_optimize(ipset *ips) {
	network_addr_t *naddrs;
	int i, n = ips->entries, lines = ips->lines;
	network_addr_t *oaddrs = ips->netaddrs;
	in_addr_t lo, hi;

	if(unlikely(ips->flags & IPSET_FLAG_OPTIMIZED)) {
		fprintf(stderr, "%s: Is already optimized %s\n", PROG, ips->filename);
		return;
	}

	if(unlikely(debug)) fprintf(stderr, "%s: Optimizing %s\n", PROG, ips->filename);

	/* sort it */
	qsort((void *)ips->netaddrs, ips->entries, sizeof(network_addr_t), compar_netaddr);

	/* optimize it in a new space */
	naddrs = malloc(ips->entries * sizeof(network_addr_t));
	if(unlikely(!naddrs)) {
		ipset_free(ips);
		fprintf(stderr, "%s: Cannot allocate memory (%ld bytes)\n", PROG, ips->entries * sizeof(network_addr_t));
		exit(1);
	}

	ips->netaddrs = naddrs;
	ips->entries = 0;
	ips->unique_ips = 0;
	ips->lines = 0;

	if(!n) return;

	lo = oaddrs[0].addr;
	hi = oaddrs[0].broadcast;
	for (i = 1; i < n; i++) {
		/*
		 * if the broadcast of this
		 * is before the broadcast of the last
		 * then skip it = it fits entirely inside the current
		 */
		if (oaddrs[i].broadcast <= hi)
			continue;

		/*
		 * if the network addr of this
		 * overlaps or is adjustent to the last
		 * then merge it = extent the broadcast of the last
		 */
		if (oaddrs[i].addr <= hi + 1) {
			hi = oaddrs[i].broadcast;
			continue;
		}

		/*
		 * at this point we are sure the old lo, hi
		 * do not overlap and are not adjustent to the current
		 * so, add the last to the new set
		 */
		ipset_add(ips, lo, hi);

		/* prepare for the next loop */
		lo = oaddrs[i].addr;
		hi = oaddrs[i].broadcast;
	}
	ipset_add(ips, lo, hi);
	ips->lines = lines;

	ips->flags |= IPSET_FLAG_OPTIMIZED;

	free(oaddrs);
}

static unsigned long int ipset_unique_ips(ipset *ips) {
	if(unlikely(!(ips->flags & IPSET_FLAG_OPTIMIZED)))
		ipset_optimize(ips);

	return(ips->unique_ips);
}

/* ----------------------------------------------------------------------------
 * ipset_optimize_all()
 *
 * it calls ipset_optimize() for all ipsets linked to 'next' to the given
 *
 */

static inline void ipset_optimize_all(ipset *root) {
	ipset *ips;

	for(ips = root; ips ;ips = ips->next)
		ipset_optimize(ips);
}


/* ----------------------------------------------------------------------------
 * ipset_diff()
 *
 * it takes 2 ipsets
 * it returns 1 new ipset having all the IPs that do not exist in either
 *
 * the result is optimized
 */

static inline ipset *ipset_diff(ipset *ips1, ipset *ips2) {
	ipset *ips;
	unsigned long int n1, n2, i1 = 0, i2 = 0;
	in_addr_t lo1, lo2, hi1, hi2;

	if(unlikely(!(ips1->flags & IPSET_FLAG_OPTIMIZED)))
		ipset_optimize(ips1);

	if(unlikely(!(ips2->flags & IPSET_FLAG_OPTIMIZED)))
		ipset_optimize(ips2);

	if(unlikely(debug)) fprintf(stderr, "%s: Finding diff IPs in %s and %s\n", PROG, ips1->filename, ips2->filename);

	ips = ipset_create("diff", 0);
	if(unlikely(!ips)) return NULL;

	n1 = ips1->entries;
	n2 = ips2->entries;

	lo1 = ips1->netaddrs[0].addr;
	lo2 = ips2->netaddrs[0].addr;
	hi1 = ips1->netaddrs[0].broadcast;
	hi2 = ips2->netaddrs[0].broadcast;

	while(i1 < n1 && i2 < n2) {
		if(lo1 > hi2) {
			ipset_add(ips, lo2, hi2);
			i2++;
			if(i2 < n2) {
				lo2 = ips2->netaddrs[i2].addr;
				hi2 = ips2->netaddrs[i2].broadcast;
			}
			continue;
		}
		if(lo2 > hi1) {
			ipset_add(ips, lo1, hi1);
			i1++;
			if(i1 < n1) {
				lo1 = ips1->netaddrs[i1].addr;
				hi1 = ips1->netaddrs[i1].broadcast;
			}
			continue;
		}

		/* they overlap */

		/* add the first part */
		if(lo1 > lo2) {
			ipset_add(ips, lo2, lo1 - 1);
		}
		else if(lo2 > lo1) {
			ipset_add(ips, lo1, lo2 - 1);
		}

		/* find the second part */
		if(hi1 > hi2) {
			lo1 = hi2 + 1;
			i2++;
			if(i2 < n2) {
				lo2 = ips2->netaddrs[i2].addr;
				hi2 = ips2->netaddrs[i2].broadcast;
			}
			continue;
		}

		else if(hi2 > hi1) {
			lo2 = hi1 + 1;
			i1++;
			if(i1 < n1) {
				lo1 = ips1->netaddrs[i1].addr;
				hi1 = ips1->netaddrs[i1].broadcast;
			}
			continue;
		}

		else { /* if(h1 == h2) */
			i1++;
			if(i1 < n1) {
				lo1 = ips1->netaddrs[i1].addr;
				hi1 = ips1->netaddrs[i1].broadcast;
			}

			i2++;
			if(i2 < n2) {
				lo2 = ips2->netaddrs[i2].addr;
				hi2 = ips2->netaddrs[i2].broadcast;
			}
		}
	}
	while(i1 < n1) {
		ipset_add(ips, lo1, hi1);
		i1++;
		if(i1 < n1) {
			lo1 = ips1->netaddrs[i1].addr;
			hi1 = ips1->netaddrs[i1].broadcast;
		}
	}
	while(i2 < n2) {
		ipset_add(ips, lo2, hi2);
		i2++;
		if(i2 < n2) {
			lo2 = ips2->netaddrs[i2].addr;
			hi2 = ips2->netaddrs[i2].broadcast;
		}
	}

	ips->lines = ips1->lines + ips2->lines;
	ips->flags |= IPSET_FLAG_OPTIMIZED;

	return ips;
}

/* ----------------------------------------------------------------------------
 * ipset_common()
 *
 * it takes 2 ipsets
 * it returns 1 new ipset having all the IPs common to both ipset given
 *
 * the result is optimized
 */

static inline ipset *ipset_common(ipset *ips1, ipset *ips2) {
	ipset *ips;
	unsigned long int n1, n2, i1 = 0, i2 = 0;
	in_addr_t lo1, lo2, hi1, hi2, lo, hi;

	if(unlikely(!(ips1->flags & IPSET_FLAG_OPTIMIZED)))
		ipset_optimize(ips1);

	if(unlikely(!(ips2->flags & IPSET_FLAG_OPTIMIZED)))
		ipset_optimize(ips2);

	if(unlikely(debug)) fprintf(stderr, "%s: Finding common IPs in %s and %s\n", PROG, ips1->filename, ips2->filename);

	ips = ipset_create("common", 0);
	if(unlikely(!ips)) return NULL;

	n1 = ips1->entries;
	n2 = ips2->entries;

	lo1 = ips1->netaddrs[0].addr;
	lo2 = ips2->netaddrs[0].addr;
	hi1 = ips1->netaddrs[0].broadcast;
	hi2 = ips2->netaddrs[0].broadcast;

	while(i1 < n1 && i2 < n2) {
		if(lo1 > hi2) {
			i2++;
			if(i2 < n2) {
				lo2 = ips2->netaddrs[i2].addr;
				hi2 = ips2->netaddrs[i2].broadcast;
			}
			continue;
		}

		if(lo2 > hi1) {
			i1++;
			if(i1 < n1) {
				lo1 = ips1->netaddrs[i1].addr;
				hi1 = ips1->netaddrs[i1].broadcast;
			}
			continue;
		}

		/* they overlap */

		if(lo1 > lo2) lo = lo1;
		else lo = lo2;

		if(hi1 < hi2) {
			hi = hi1;
			i1++;
			if(i1 < n1) {
				lo1 = ips1->netaddrs[i1].addr;
				hi1 = ips1->netaddrs[i1].broadcast;
			}
		}
		else {
			hi = hi2;
			i2++;
			if(i2 < n2) {
				lo2 = ips2->netaddrs[i2].addr;
				hi2 = ips2->netaddrs[i2].broadcast;
			}
		}

		ipset_add(ips, lo, hi);
	}

	ips->lines = ips1->lines + ips2->lines;
	ips->flags |= IPSET_FLAG_OPTIMIZED;

	return ips;
}


/* ----------------------------------------------------------------------------
 * ipset_exclude()
 *
 * it takes 2 ipsets (ips1, ips2)
 * it returns 1 new ipset having all the IPs of ips1, excluding the IPs of ips2
 *
 * the result is optimized
 */

static inline ipset *ipset_exclude(ipset *ips1, ipset *ips2) {
	ipset *ips;
	unsigned long int n1, n2, i1 = 0, i2 = 0;
	in_addr_t lo1, lo2, hi1, hi2;

	if(unlikely(!(ips1->flags & IPSET_FLAG_OPTIMIZED)))
		ipset_optimize(ips1);

	if(unlikely(!(ips2->flags & IPSET_FLAG_OPTIMIZED)))
		ipset_optimize(ips2);

	if(unlikely(debug)) fprintf(stderr, "%s: Removing IPs in %s from %s\n", PROG, ips2->filename, ips1->filename);

	ips = ipset_create(ips1->filename, 0);
	if(unlikely(!ips)) return NULL;

	n1 = ips1->entries;
	n2 = ips2->entries;

	lo1 = ips1->netaddrs[0].addr;
	lo2 = ips2->netaddrs[0].addr;
	hi1 = ips1->netaddrs[0].broadcast;
	hi2 = ips2->netaddrs[0].broadcast;

	while(i1 < n1 && i2 < n2) {
		if(lo1 > hi2) {
			i2++;
			if(i2 < n2) {
				lo2 = ips2->netaddrs[i2].addr;
				hi2 = ips2->netaddrs[i2].broadcast;
			}
			continue;
		}

		if(lo2 > hi1) {
			ipset_add(ips, lo1, hi1);

			i1++;
			if(i1 < n1) {
				lo1 = ips1->netaddrs[i1].addr;
				hi1 = ips1->netaddrs[i1].broadcast;
			}
			continue;
		}

		/* they overlap */

		if(lo1 < lo2) {
			ipset_add(ips, lo1, lo2-1);
			lo1 = lo2;
		}

		if(hi1 == hi2) {
			i1++;
			if(i1 < n1) {
				lo1 = ips1->netaddrs[i1].addr;
				hi1 = ips1->netaddrs[i1].broadcast;
			}

			i2++;
			if(i2 < n2) {
				lo2 = ips2->netaddrs[i2].addr;
				hi2 = ips2->netaddrs[i2].broadcast;
			}
		}
		else if(hi1 < hi2) {
			i1++;
			if(i1 < n1) {
				lo1 = ips1->netaddrs[i1].addr;
				hi1 = ips1->netaddrs[i1].broadcast;
			}
		}
		else if(hi1 > hi2) {
			lo1 = hi2 + 1;
			i2++;
			if(i2 < n2) {
				lo2 = ips2->netaddrs[i2].addr;
				hi2 = ips2->netaddrs[i2].broadcast;
			}
		}
	}

	if(i1 < n1) {
		ipset_add(ips, lo1, hi1);
		i1++;

		/* if there are entries left in ips1, copy them */
		while(i1 < n1) {
			ipset_add(ips, ips1->netaddrs[i1].addr, ips1->netaddrs[i1].broadcast);
			i1++;
		}
	}

	ips->lines = ips1->lines + ips2->lines;
	ips->flags |= IPSET_FLAG_OPTIMIZED;
	return ips;
}


/* ----------------------------------------------------------------------------
 * parse_line()
 *
 * it parses a single line of input
 * returns
 * 		-1 = cannot parse line
 * 		 0 = skip line - nothing useful here
 * 		 1 = parsed 1 ip address
 * 		 2 = parsed 2 ip addresses
 *       3 = parsed 1 hostname
 *
 */

#define LINE_IS_INVALID -1
#define LINE_IS_EMPTY 0
#define LINE_HAS_1_IP 1
#define LINE_HAS_2_IPS 2
#define LINE_HAS_1_HOSTNAME 3

static inline int parse_hostname(char *line, int lineid, char *ipstr, char *ipstr2, int len) {
	char *s = line;
	int i = 0;

	if(ipstr2) { ; }

	/* skip all spaces */
	while(unlikely(*s == ' ' || *s == '\t')) s++;

	while(likely(i < len && (
		   (*s >= '0' && *s <= '9')
		|| (*s >= 'a' && *s <= 'z')
		|| (*s >= 'A' && *s <= 'Z')
		|| *s == '_'
		|| *s == '-'
		|| *s == '.'
		))) ipstr[i++] = *s++;

	/* terminate ipstr */
	ipstr[i] = '\0';

	/* skip all spaces */
	while(unlikely(*s == ' ' || *s == '\t')) s++;

	/* the rest is comment */
	if(unlikely(*s == '#' || *s == ';')) return LINE_HAS_1_HOSTNAME;

	/* if we reached the end of line */
	if(likely(*s == '\r' || *s == '\n' || *s == '\0')) return LINE_HAS_1_HOSTNAME;

	fprintf(stderr, "%s: Ignoring text after hostname '%s' on line %d: '%s'\n", PROG, ipstr, lineid, s);

	return LINE_HAS_1_HOSTNAME;
}

static inline int parse_line(char *line, int lineid, char *ipstr, char *ipstr2, int len) {
	char *s = line;
	int i = 0;

	/* skip all spaces */
	while(unlikely(*s == ' ' || *s == '\t')) s++;

	/* skip a line of comment */
	if(unlikely(*s == '#' || *s == ';')) return LINE_IS_EMPTY;

	/* if we reached the end of line */
	if(unlikely(*s == '\r' || *s == '\n' || *s == '\0')) return LINE_IS_EMPTY;

	/* get the ip address */
	while(likely(i < len && ((*s >= '0' && *s <= '9') || *s == '.' || *s == '/')))
		ipstr[i++] = *s++;

	if(unlikely(!i)) return parse_hostname(line, lineid, ipstr, ipstr2, len);

	/* terminate ipstr */
	ipstr[i] = '\0';

	/* skip all spaces */
	while(unlikely(*s == ' ' || *s == '\t')) s++;

	/* the rest is comment */
	if(unlikely(*s == '#' || *s == ';')) return LINE_HAS_1_IP;

	/* if we reached the end of line */
	if(likely(*s == '\r' || *s == '\n' || *s == '\0')) return LINE_HAS_1_IP;

	if(unlikely(*s != '-')) {
		/*fprintf(stderr, "%s: Ignoring text on line %d, expected a - after %s, but found '%s'\n", PROG, lineid, ipstr, s);*/
		/*return LINE_HAS_1_IP;*/
		return parse_hostname(line, lineid, ipstr, ipstr2, len);
	}

	/* skip the - */
	s++;

	/* skip all spaces */
	while(unlikely(*s == ' ' || *s == '\t')) s++;

	/* the rest is comment */
	if(unlikely(*s == '#' || *s == ';')) {
		fprintf(stderr, "%s: Ignoring text on line %d, expected an ip address after -, but found '%s'\n", PROG, lineid, s);
		return LINE_HAS_1_IP;
	}

	/* if we reached the end of line */
	if(unlikely(*s == '\r' || *s == '\n' || *s == '\0')) {
		fprintf(stderr, "%s: Incomplete range on line %d, expected an ip address after -, but line ended\n", PROG, lineid);
		return LINE_HAS_1_IP;
	}

	/* get the ip 2nd address */
	i = 0;
	while(likely(i < len && ((*s >= '0' && *s <= '9') || *s == '.' || *s == '/')))
		ipstr2[i++] = *s++;

	if(unlikely(!i)) {
		/*fprintf(stderr, "%s: Incomplete range on line %d, expected an ip address after -, but line ended\n", PROG, lineid); */
		/*return LINE_HAS_1_IP; */
		return parse_hostname(line, lineid, ipstr, ipstr2, len);
	}

	/* terminate ipstr */
	ipstr2[i] = '\0';

	/* skip all spaces */
	while(unlikely(*s == ' ' || *s == '\t')) s++;

	/* the rest is comment */
	if(unlikely(*s == '#' || *s == ';')) return LINE_HAS_2_IPS;

	/* if we reached the end of line */
	if(likely(*s == '\r' || *s == '\n' || *s == '\0')) return LINE_HAS_2_IPS;

	/*fprintf(stderr, "%s: Ignoring text on line %d, after the second ip address: '%s'\n", PROG, lineid, s); */
	/*return LINE_HAS_2_IPS; */
	return parse_hostname(line, lineid, ipstr, ipstr2, len);
}

/* ----------------------------------------------------------------------------
 * binary files v1.0
 *
 */

static int ipset_load_binary_v10(FILE *fp, ipset *ips, int first_line_missing) {
	char buffer[MAX_LINE + 1], *s;
	unsigned long entries, bytes, lines, unique_ips;
	uint32_t endian;
	size_t loaded;

	if(!first_line_missing) {
		s = fgets(buffer, MAX_LINE, fp);
		buffer[MAX_LINE] = '\0';
		if(!s || strcmp(s, BINARY_HEADER_V10)) {
			fprintf(stderr, "%s: %s expecting binary header but found '%s'.\n", PROG, ips->filename, s?s:"");
			return 1;
		}
	}

	s = fgets(buffer, MAX_LINE, fp);
	buffer[MAX_LINE] = '\0';
	if(!s || ( strcmp(s, "optimized\n") && strcmp(s, "non-optimized\n") )) {
		fprintf(stderr, "%s: %s 2nd line should be the optimized flag, but found '%s'.\n", PROG, ips->filename, s?s:"");
		return 1;
	}
	if(!strcmp(s, "optimized\n")) ips->flags |= IPSET_FLAG_OPTIMIZED;
	else if(ips->flags & IPSET_FLAG_OPTIMIZED) ips->flags ^= IPSET_FLAG_OPTIMIZED;

	s = fgets(buffer, MAX_LINE, fp);
	buffer[MAX_LINE] = '\0';
	if(!s || strncmp(s, "record size ", 12)) {
		fprintf(stderr, "%s: %s 3rd line should be the record size, but found '%s'.\n", PROG, ips->filename, s?s:"");
		return 1;
	}
	if(atol(&s[12]) != sizeof(network_addr_t)) {
		fprintf(stderr, "%s: %s: invalid record size %lu (expected %lu)\n", PROG, ips->filename, atol(&s[12]), (unsigned long)sizeof(network_addr_t));
		return 1;
	}

	s = fgets(buffer, MAX_LINE, fp);
	buffer[MAX_LINE] = '\0';
	if(!s || strncmp(s, "records ", 8)) {
		fprintf(stderr, "%s: %s 4th line should be the number of records, but found '%s'.\n", PROG, ips->filename, s?s:"");
		return 1;
	}
	entries = strtoul(&s[8], NULL, 10);

	s = fgets(buffer, MAX_LINE, fp);
	buffer[MAX_LINE] = '\0';
	if(!s || strncmp(s, "bytes ", 6)) {
		fprintf(stderr, "%s: %s 5th line should be the number of bytes, but found '%s'.\n", PROG, ips->filename, s?s:"");
		return 1;
	}
	bytes = strtoul(&s[6], NULL, 10);

	s = fgets(buffer, MAX_LINE, fp);
	buffer[MAX_LINE] = '\0';
	if(!s || strncmp(s, "lines ", 6)) {
		fprintf(stderr, "%s: %s 6th line should be the number of lines read, but found '%s'.\n", PROG, ips->filename, s?s:"");
		return 1;
	}
	lines = strtoul(&s[6], NULL, 10);

	s = fgets(buffer, MAX_LINE, fp);
	buffer[MAX_LINE] = '\0';
	if(!s || strncmp(s, "unique ips ", 11)) {
		fprintf(stderr, "%s: %s 7th line should be the number of unique IPs, but found '%s'.\n", PROG, ips->filename, s?s:"");
		return 1;
	}
	unique_ips = strtoul(&s[11], NULL, 10);

	if(bytes != ((sizeof(network_addr_t) * entries) + sizeof(uint32_t))) {
		fprintf(stderr, "%s: %s invalid number of bytes, found %lu, expected %lu.\n", PROG, ips->filename, bytes, ((sizeof(network_addr_t) * entries) + sizeof(uint32_t)));
		return 1;
	}

	loaded = fread(&endian, sizeof(uint32_t), 1, fp);
	if(endian != endianess) {
		fprintf(stderr, "%s: %s: incompatible endianess\n", PROG, ips->filename);
		return 1;
	}

	if(unique_ips < entries) {
		fprintf(stderr, "%s: %s: unique IPs (%lu) cannot be less than entries (%lu)\n", PROG, ips->filename, unique_ips, entries);
		return 1;
	}

	if(lines < entries) {
		fprintf(stderr, "%s: %s: lines (%lu) cannot be less than entries (%lu)\n", PROG, ips->filename, lines, entries);
		return 1;
	}

	ipset_expand(ips, entries);

	loaded = fread(&ips->netaddrs[ips->entries], sizeof(network_addr_t), entries, fp);

	if(loaded != entries) {
		fprintf(stderr, "%s: %s: expected to load %lu entries, loaded %zd\n", PROG, ips->filename, entries, loaded);
		return 1;
	}

	ips->entries += loaded;
	ips->lines += lines;
	ips->unique_ips += unique_ips;

	return 0;
}

static void ipset_save_binary_v10(ipset *ips) {
	if(!ips->entries) return;

	fprintf(stdout, BINARY_HEADER_V10);
	if(ips->flags & IPSET_FLAG_OPTIMIZED) fprintf(stdout, "optimized\n");
	else fprintf(stdout, "non-optimized\n");
	fprintf(stdout, "record size %lu\n", (unsigned long)sizeof(network_addr_t));
	fprintf(stdout, "records %lu\n", ips->entries);
	fprintf(stdout, "bytes %lu\n", (sizeof(network_addr_t) * ips->entries) + sizeof(uint32_t));
	fprintf(stdout, "lines %lu\n", ips->entries);
	fprintf(stdout, "unique ips %lu\n", ips->unique_ips);
	fwrite(&endianess, sizeof(uint32_t), 1, stdout);
	fwrite(ips->netaddrs, sizeof(network_addr_t), ips->entries, stdout);
}


/* ----------------------------------------------------------------------------
 * hostname resolution
 */

typedef struct dnsreq {
	struct dnsreq *next;
	char tries;
	char hostname[];
} DNSREQ;

typedef struct dnsrep {
	in_addr_t ip;
	struct dnsrep *next;
} DNSREP;

static DNSREQ *dns_requests;
static DNSREP *dns_replies;
static int dns_threads;
static int dns_threads_max = 5;
static int dns_silent;
static int dns_progress;
static unsigned long dns_requests_pending;
static unsigned long dns_requests_made;
static unsigned long dns_requests_finished;
static unsigned long dns_requests_retries;
static unsigned long dns_replies_found;
static unsigned long dns_replies_failed;

static pthread_mutex_t dns_mut = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dns_cond = PTHREAD_COND_INITIALIZER;
static pthread_rwlock_t dns_requests_rwlock = PTHREAD_RWLOCK_INITIALIZER;
static pthread_rwlock_t dns_replies_rwlock = PTHREAD_RWLOCK_INITIALIZER;

void dns_lock_requests(void)   { pthread_rwlock_wrlock(&dns_requests_rwlock); }
void dns_unlock_requests(void) { pthread_rwlock_unlock(&dns_requests_rwlock); }
void dns_lock_replies(void)    { pthread_rwlock_wrlock(&dns_replies_rwlock); }
void dns_unlock_replies(void)  { pthread_rwlock_unlock(&dns_replies_rwlock); }

static void *dns_thread_resolve(void *ptr);


static void dns_signal_threats(void)
{
	/* signal the childs we have a new request for them */
	pthread_mutex_lock(&dns_mut);
	pthread_cond_signal(&dns_cond);
	pthread_mutex_unlock(&dns_mut);
}

/* ----------------------------------------------------------------------------
 * dns_request_add()
 *
 * add a new DNS resolution request to the queue
 *
 */

static void dns_request_add(DNSREQ *d)
{
	unsigned long pending;

	dns_lock_requests();
	d->next = dns_requests;
	dns_requests = d;
	dns_requests_pending++;
	dns_requests_made++;

	pending = dns_requests_pending;
	dns_unlock_requests();

	/* do we have start a new thread? */
	if(pending > (unsigned long)dns_threads && dns_threads < dns_threads_max) {
		pthread_t thread;

		if(unlikely(debug))
			fprintf(stderr, "%s: Creating new DNS thread\n", PROG);

		if(pthread_create(&thread, NULL, dns_thread_resolve, NULL)) {
			fprintf(stderr, "%s: Cannot create DNS thread.\n", PROG);
			return;
		}
		else if(pthread_detach(thread)) {
			fprintf(stderr, "%s: Cannot detach DNS thread.\n", PROG);
			return;
		}

		dns_threads++;
	}

	dns_signal_threats();
}


/* ----------------------------------------------------------------------------
 * dns_request_done()
 *
 * to be called by a worker thread
 * let the main thread know a DNS resolution has been completed
 *
 */

static void dns_request_done(DNSREQ *d, int added)
{
	dns_lock_requests();
	dns_requests_pending--;
	dns_requests_finished++;

	if(!added) dns_replies_failed++;
	else dns_replies_found += added;

	dns_unlock_requests();

	free(d);
}


/* ----------------------------------------------------------------------------
 * dns_request_failed()
 *
 * to be called by a worker thread
 * handle a DNS failure (mainly for retries)
 *
 */

static void dns_request_failed(DNSREQ *d, int added, int gai_error)
{
	switch(gai_error) {
		case EAI_AGAIN: /* The name server returned a temporary failure indication.  Try again later. */
			if(d->tries > 0) {
				if(!dns_silent)
					fprintf(stderr, "%s: DNS: '%s' will be retried: %s\n", PROG, d->hostname, gai_strerror(gai_error));

				d->tries--;

				dns_lock_requests();
				d->next = dns_requests;
				dns_requests = d;
				dns_requests_retries++;
				dns_replies_found += added;
				dns_unlock_requests();

				return;
			}
			dns_request_done(d, added);
			return;
			break;

		case EAI_SYSTEM:
			fprintf(stderr, "%s: DNS: '%s' system error: %s\n", PROG, d->hostname, strerror(errno));
			dns_request_done(d, added);
			return;
			break;

		case EAI_SOCKTYPE: /* The requested socket type is not supported. */
		case EAI_SERVICE: /* The requested service is not available for the requested socket type. */
		case EAI_MEMORY: /* Out of memory. */
		case EAI_BADFLAGS: /* hints.ai_flags contains invalid flags; or, hints.ai_flags included AI_CANONNAME and name was NULL. */
			fprintf(stderr, "%s: DNS: '%s' error: %s\n", PROG, d->hostname, gai_strerror(gai_error));
			dns_request_done(d, added);
			return;
			break;

		case EAI_NONAME: /* The node or service is not known */
		case EAI_FAIL:   /* The name server returned a permanent failure indication. */
		case EAI_FAMILY: /* The requested address family is not supported. */
		default:
			if(!dns_silent)
				fprintf(stderr, "%s: DNS: '%s' failed permanently: %s\n", PROG, d->hostname, gai_strerror(gai_error));
			dns_request_done(d, added);
			return;
			break;
	}
}


/* ----------------------------------------------------------------------------
 * dns_request_get()
 *
 * to be called by a worker thread
 * get a request from the requests queue
 *
 */

static DNSREQ *dns_request_get(void)
{
	DNSREQ *ret = NULL;

	/*
	 * if(unlikely(debug))
	 * fprintf(stderr, "%s: DNS THREAD waiting for DNS REQUEST\n", PROG);
	 */

	while(!ret) {
		if(dns_requests) {
			dns_lock_requests();
			if(dns_requests) {
				ret = dns_requests;
				dns_requests = dns_requests->next;
				ret->next = NULL;
			}
			dns_unlock_requests();
			if(ret) return ret;
		}

		pthread_mutex_lock(&dns_mut);
		while(!dns_requests)
			pthread_cond_wait(&dns_cond, &dns_mut);
		pthread_mutex_unlock(&dns_mut);
	}

	return ret;
}


/* ----------------------------------------------------------------------------
 * dns_thread_resolve()
 *
 * a pthread worker to get requests and generate replies
 *
 */

static void *dns_thread_resolve(void *ptr)
{
	DNSREQ *d;

	if(ptr) { ; }

	/*
	 * if(unlikely(debug))
	 *	fprintf(stderr, "%s: DNS THREAD created\n", PROG);
	 */

	while((d = dns_request_get())) {
		int added = 0;

		/*
		 * if(unlikely(debug))
		 *	fprintf(stderr, "%s: DNS THREAD resolving DNS REQUEST for '%s'\n", PROG, d->hostname);
		 */

		int r;
		struct addrinfo *result, *rp, hints;

		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_flags = 0;
		hints.ai_protocol = 0;

		r = getaddrinfo(d->hostname, "80", &hints, &result);
		if(r != 0) {
			dns_request_failed(d, 0, r);
			continue;
		}

		for (rp = result; rp != NULL; rp = rp->ai_next) {
			char host[MAX_INPUT_ELEMENT + 1] = "";
			network_addr_t net;
			int err = 0;
			DNSREP *p;

			r = getnameinfo(rp->ai_addr, rp->ai_addrlen, host, sizeof(host), NULL, 0, NI_NUMERICHOST);
			if (r != 0) {
				fprintf(stderr, "%s: DNS: '%s' failed to get IP string: %s\n", PROG, d->hostname, gai_strerror(r));
				continue;
			}

			net = str_to_netaddr(host, &err);
			if(err) {
				fprintf(stderr, "%s: DNS: '%s' cannot parse the IP '%s': %s\n", PROG, d->hostname, host, gai_strerror(r));
				continue;
			}

			p = malloc(sizeof(DNSREP));
			if(!p) {
				fprintf(stderr, "%s: DNS: out of memory while resolving host '%s'\n", PROG, d->hostname);
				continue;
			}

			if(unlikely(debug))
				fprintf(stderr, "%s: DNS: '%s' = %s\n", PROG, d->hostname, ip2str(net.addr));

			p->ip = net.addr;
			dns_lock_replies();
			p->next = dns_replies;
			dns_replies = p;
			added++;
			dns_unlock_replies();
		}

		freeaddrinfo(result);
		dns_request_done(d, added);
	}

	return NULL;
}

/* ----------------------------------------------------------------------------
 * dns_process_replies()
 *
 * dequeue the resolved hostnames by adding them to the ipset
 *
 */

static void dns_process_replies(ipset *ips)
{
	if(!dns_replies) return;

	dns_lock_replies();
	while(dns_replies) {
		DNSREP *p;

		/*
		 * if(unlikely(debug))
		 *	fprintf(stderr, "%s: Got DNS REPLY '%s'\n", PROG, ip2str(dns_replies->ip));
		 */

		ipset_add(ips, dns_replies->ip, dns_replies->ip);

		p = dns_replies->next;
		free(dns_replies);
		dns_replies = p;
	}
	dns_unlock_replies();
}


/* ----------------------------------------------------------------------------
 * dns_request()
 *
 * attempt to resolv a hostname
 * the result (one or more) will be appended to the ipset supplied
 *
 * this is asynchronous - it will just queue the request and spawn worker
 * threats to do the DNS resolution.
 *
 * the IPs will be added to the ipset, either at the next call to this
 * function, or by calling dns_done().
 *
 * So, to use it:
 * 1. call dns_request() to request dns resolutions (any number)
 * 2. call dns_done() when you finish requesting hostnames
 * 3. the resolved IPs are in the ipset you supplied
 *
 * All ipset manipulation is done at this threat, so if control is
 * outside the above 2 functions, you are free to do whatever you like
 * with the ipset.
 *
 * Important: you cannot use dns_request() and dns_done() with more
 * than 1 ipset at the same time. The resulting IPs will be multiplexed.
 * When you call dns_done() on one ipset, you can proceed with the next.
 *
 */

static void dns_request(ipset *ips, char *hostname)
{
	DNSREQ *d;

	/* dequeue if possible */
	dns_process_replies(ips);

	/*
	 * if(unlikely(debug))
	 *	fprintf(stderr, "%s: Adding DNS REQUEST for '%s'\n", PROG, hostname);
	 */

	d = malloc(sizeof(DNSREQ) + strlen(hostname) + 1);
	if(!d) goto cleanup;

	strcpy(d->hostname, hostname);
	d->tries = 20;

	/* add the request to the queue */
	dns_request_add(d);

	return;

cleanup:
	fprintf(stderr, "%s: out of memory, while trying to resolv '%s'\n", PROG, hostname);
}


/* ----------------------------------------------------------------------------
 * dns_done()
 *
 * wait for the DNS requests made to finish.
 *
 */

static void dns_done(ipset *ips)
{
	unsigned long dots = 40, shown = 0, should_show = 0;

	if(ips) { ; }

	if(!dns_requests_made) return;

	while(dns_requests_pending) {
		if(unlikely(debug))
			fprintf(stderr, "%s: DNS: waiting %lu DNS resolutions to finish...\n", PROG, dns_requests_pending);
		else if(dns_progress) {
			should_show = dots * dns_requests_finished / dns_requests_made;
			for(; shown < should_show; shown++) {
				if(!(shown % 10)) fprintf(stderr, "%lu%%", shown * 100 / dots);
				else fprintf(stderr, ".");
			}
		}

		dns_process_replies(ips);

		if(dns_requests_pending) {
			dns_signal_threats();
			sleep(1);
		}
	}
	dns_process_replies(ips);

	if(unlikely(debug))
		fprintf(stderr, "%s: DNS: made %lu DNS requests, failed %lu, retries: %lu, IPs got %lu, threads used %d of %d\n", PROG, dns_requests_made, dns_replies_failed, dns_requests_retries, dns_replies_found, dns_threads, dns_threads_max);
	else if(dns_progress) {
		for(; shown <= dots; shown++) {
			if(!(shown % 10)) fprintf(stderr, "%lu%%", shown * 100 / dots);
			else fprintf(stderr, ".");
		}
		fprintf(stderr, "\n");
	}
}

/* ----------------------------------------------------------------------------
 * ipset_load()
 *
 * loads a file and stores all entries it finds to a new ipset it creates
 * if the filename is NULL, stdin is used
 *
 * the result is not optimized. To optimize it call ipset_optimize().
 *
 */

static ipset *ipset_load(const char *filename) {
	FILE *fp = stdin;
	int lineid = 0;
	char line[MAX_LINE + 1], ipstr[MAX_INPUT_ELEMENT + 1], ipstr2[MAX_INPUT_ELEMENT + 1];
	ipset *ips = ipset_create((filename && *filename)?filename:"stdin", 0);

	if(unlikely(!ips)) return NULL;

	if (likely(filename && *filename)) {
		fp = fopen(filename, "r");
		if (unlikely(!fp)) {
			fprintf(stderr, "%s: %s - %s\n", PROG, filename, strerror(errno));
			return NULL;
		}
	}

	/* load it */
	if(unlikely(debug)) fprintf(stderr, "%s: Loading from %s\n", PROG, ips->filename);

	/* it will be removed, if the loaded ipset is not optimized on disk */
	ips->flags |= IPSET_FLAG_OPTIMIZED;

	if(!fgets(line, MAX_LINE, fp)) return ips;

	if(unlikely(!strcmp(line, BINARY_HEADER_V10))) {
		if(ipset_load_binary_v10(fp, ips, 1)) {
			fprintf(stderr, "%s: Cannot fast load %s\n", PROG, filename);
			ipset_free(ips);
			ips = NULL;
		}

		if(likely(fp != stdin)) fclose(fp);
		if(unlikely(debug)) if(ips) fprintf(stderr, "%s: Binary loaded %s %s\n", PROG, (ips->flags & IPSET_FLAG_OPTIMIZED)?"optimized":"non-optimized", ips->filename);

		return ips;
	}

	do {
		lineid++;

		switch(parse_line(line, lineid, ipstr, ipstr2, MAX_INPUT_ELEMENT)) {
			case LINE_IS_INVALID:
				/* cannot read line */
				fprintf(stderr, "%s: Cannot understand line No %d from %s: %s\n", PROG, lineid, ips->filename, line);
				break;

			case LINE_IS_EMPTY:
				/* nothing on this line */
				break;

			case LINE_HAS_1_IP:
				/* 1 IP on this line */
				if(unlikely(!ipset_add_ipstr(ips, ipstr)))
					fprintf(stderr, "%s: Cannot understand line No %d from %s: %s\n", PROG, lineid, ips->filename, line);
				break;

			case LINE_HAS_2_IPS:
				/* 2 IPs in range on this line */
				{
					int err = 0;
					in_addr_t lo, hi;
					network_addr_t netaddr1, netaddr2;
					netaddr1 = str_to_netaddr(ipstr, &err);
					if(likely(!err)) netaddr2 = str_to_netaddr(ipstr2, &err);
					if(unlikely(err)) {
						fprintf(stderr, "%s: Cannot understand line No %d from %s: %s\n", PROG, lineid, ips->filename, line);
						continue;
					}

					lo = (netaddr1.addr < netaddr2.addr)?netaddr1.addr:netaddr2.addr;
					hi = (netaddr1.broadcast > netaddr2.broadcast)?netaddr1.broadcast:netaddr2.broadcast;
					ipset_add(ips, lo, hi);
				}
				break;

			case LINE_HAS_1_HOSTNAME:
				if(unlikely(debug))
					fprintf(stderr, "%s: DNS resolution for hostname '%s' from line %d of file %s.\n", PROG, ipstr, lineid, ips->filename);

				/* resolve_hostname(ips, ipstr); */
				dns_request(ips, ipstr);
				break;

			default:
				fprintf(stderr, "%s: Cannot understand result code. This is an internal error.\n", PROG);
				exit(1);
				break;
		}
	} while(likely(ips && fgets(line, MAX_LINE, fp)));

	if(likely(fp != stdin)) fclose(fp);

	dns_done(ips);

	if(unlikely(!ips)) return NULL;

	if(unlikely(debug)) fprintf(stderr, "%s: Loaded %s %s\n", PROG, (ips->flags & IPSET_FLAG_OPTIMIZED)?"optimized":"non-optimized", ips->filename);

	/*
	 * if(unlikely(!ips->entries)) {
	 *	free(ips);
	 *	return NULL;
	 * }
	 */

	return ips;
}


/* ----------------------------------------------------------------------------
 * ipset_reduce()
 *
 * takes an ipset, an acceptable increase % and a minimum accepted entries
 * and disables entries in the global prefix_enabled[] array, so that once
 * the ipset is printed, only the enabled prefixes will be used
 *
 * prefix_enable[] is not reset before use, so that it can be initialized with
 * some of the prefixes enabled and others disabled already (user driven)
 *
 * this function does not alter the given ipset and it does not print it
 */

static void ipset_reduce(ipset *ips, int acceptable_increase, int min_accepted) {
	int i, n = ips->entries, total = 0, acceptable, iterations = 0, initial = 0, eliminated = 0;

	if(unlikely(!(ips->flags & IPSET_FLAG_OPTIMIZED)))
		ipset_optimize(ips);

	/* reset the prefix counters */
	for(i = 0; i <= 32; i++)
		prefix_counters[i] = 0;

	/* disable printing */
	split_range_disable_printing = 1;

	/* find how many prefixes are there */
	if(unlikely(debug)) fprintf(stderr, "\nCounting prefixes in %s\n", ips->filename);
	for(i = 0; i < n ;i++)
		split_range(0, 0, ips->netaddrs[i].addr, ips->netaddrs[i].broadcast);

	/* count them */
	if(unlikely(debug)) fprintf(stderr, "Break down by prefix:\n");
	total = 0;
	for(i = 0; i <= 32 ;i++) {
		if(prefix_counters[i]) {
			if(unlikely(debug)) fprintf(stderr, "	- prefix /%d counts %d entries\n", i, prefix_counters[i]);
			total += prefix_counters[i];
			initial++;
		}
		else prefix_enabled[i] = 0;
	}
	if(unlikely(debug)) fprintf(stderr, "Total %d entries generated\n", total);

	/* find the upper limit */
	acceptable = total * acceptable_increase / 100;
	if(acceptable < min_accepted) acceptable = min_accepted;
	if(unlikely(debug)) fprintf(stderr, "Acceptable is to reach %d entries by reducing prefixes\n", acceptable);

	/* reduce the possible prefixes */
	while(total < acceptable) {
		int min = -1, to = -1, min_increase = acceptable * 10, j, multiplier, increase;
		int old_to_counters;

		iterations++;

		/* find the prefix with the least increase */
		for(i = 0; i <= 31 ;i++) {
			if(!prefix_counters[i] || !prefix_enabled[i]) continue;

			for(j = i + 1, multiplier = 2; j <= 32 ; j++, multiplier *= 2) {
				if(!prefix_counters[j]) continue;

				increase = prefix_counters[i] * (multiplier - 1);
				if(unlikely(debug)) fprintf(stderr, "		> Examining merging prefix %d to %d (increase by %d)\n", i, j, increase);

				if(increase < min_increase) {
					min_increase = increase;
					min = i;
					to = j;
				}
				break;
			}
		}

		if(min == -1 || to == -1 || min == to) {
			if(unlikely(debug)) fprintf(stderr, "	Nothing more to reduce\n");
			break;
		}

	 	multiplier = 1;
		for(i = min; i < to; i++) multiplier *= 2;

		increase = prefix_counters[min] * multiplier - prefix_counters[min];
		if(unlikely(debug)) fprintf(stderr, "		> Selected prefix %d (%d entries) to be merged in %d (total increase by %d)\n", min, prefix_counters[min], to, increase);

		if(total + increase > acceptable) {
			if(unlikely(debug)) fprintf(stderr, "	Cannot proceed to increase total %d by %d, above acceptable %d.\n", total, increase, acceptable);
			break;
		}

		old_to_counters = prefix_counters[to];

		total += increase;
		prefix_counters[to] += increase + prefix_counters[min];
		prefix_counters[min] = 0;
		prefix_enabled[min] = 0;
		eliminated++;
		if(unlikely(debug)) fprintf(stderr, "		Eliminating prefix %d in %d (had %d, now has %d entries), total is now %d (increased by %d)\n", min, to, old_to_counters, prefix_counters[to], total, increase);
	}

	if(unlikely(debug)) fprintf(stderr, "\nEliminated %d out of %d prefixes (%d remain in the final set).\n\n", eliminated, initial, initial - eliminated);

	/* reset the prefix counters */
	for(i = 0; i <= 32; i++)
		prefix_counters[i] = 0;

	/* enable printing */
	split_range_disable_printing = 0;
}


/* ----------------------------------------------------------------------------
 * ipset_print()
 *
 * print the ipset given to stdout
 *
 */

#define PRINT_RANGE 1
#define PRINT_CIDR 2
#define PRINT_SINGLE_IPS 3
#define PRINT_BINARY 4

static void ipset_print(ipset *ips, int print) {
	int i, n = ips->entries;
	unsigned long int total = 0;

	if(unlikely(!(ips->flags & IPSET_FLAG_OPTIMIZED)))
		ipset_optimize(ips);

	if(print == PRINT_BINARY) {
		ipset_save_binary_v10(ips);
		return;
	}

	if(unlikely(debug)) fprintf(stderr, "%s: Printing %s having %lu entries, %lu unique IPs\n", PROG, ips->filename, ips->entries, ips->unique_ips);

	switch(print) {
		case PRINT_CIDR:
			/* reset the prefix counters */
			for(i = 0; i <= 32; i++)
				prefix_counters[i] = 0;

			n = ips->entries;
			for(i = 0; i < n ;i++) {
				total += split_range(0, 0, ips->netaddrs[i].addr, ips->netaddrs[i].broadcast);
			}
			break;

		case PRINT_SINGLE_IPS:
			n = ips->entries;
			for(i = 0; i < n ;i++) {
				in_addr_t x, start = ips->netaddrs[i].addr, end = ips->netaddrs[i].broadcast;
				if(unlikely(start > end)) {
					fprintf(stderr, "%s: WARNING: invalid range reversed start=%s", PROG, ip2str(start));
					fprintf(stderr, " end=%s\n", ip2str(end));
					x = end;
					end = start;
					start = x;
				}
				if(unlikely(end - start > (256 * 256 * 256))) {
					fprintf(stderr, "%s: too big range eliminated start=%s", PROG, ip2str(start));
					fprintf(stderr, " end=%s gives %lu IPs\n", ip2str(end), (unsigned long)(end - start));
					continue;
				}
				for( x = start ; x >= start && x <= end ; x++ ) {
					print_addr_single(x);
					total++;
				}
			}
			break;

		default:
			n = ips->entries;
			for(i = 0; i < n ;i++) {
				print_addr_range(ips->netaddrs[i].addr, ips->netaddrs[i].broadcast);
				total++;
			}
			break;
	}

	/* print prefix break down */
	if(unlikely(debug)) {
		int prefixes = 0;

		if (print == PRINT_CIDR) {

			fprintf(stderr, "\n%lu printed CIDRs, break down by prefix:\n", total);

			total = 0;
			for(i = 0; i <= 32 ;i++) {
				if(prefix_counters[i]) {
					fprintf(stderr, "	- prefix /%d counts %d entries\n", i, prefix_counters[i]);
					total += prefix_counters[i];
					prefixes++;
				}
			}
		}
		else if (print == PRINT_SINGLE_IPS) prefixes = 1;

		{
			char *units = "";
			if (print == PRINT_CIDR) units = "CIDRs";
			else if (print == PRINT_SINGLE_IPS) units = "IPs";
			else units = "ranges";

			fprintf(stderr, "\ntotals: %lu lines read, %lu distinct IP ranges found, %d CIDR prefixes, %lu %s printed, %lu unique IPs\n", ips->lines, ips->entries, prefixes, total, units, ips->unique_ips);
		}
	}
}


/* ----------------------------------------------------------------------------
 * ipset_merge()
 *
 * merges the second ipset (add) to the first ipset (to)
 * they may not be optimized
 * the result is never optimized (even if the sources are)
 * to optimize it call ipset_optimize()
 *
 */

static inline void ipset_merge(ipset *to, ipset *add) {
	if(unlikely(debug)) fprintf(stderr, "%s: Merging %s to %s\n", PROG, add->filename, to->filename);

	ipset_expand(to, add->entries);

	memcpy(&to->netaddrs[to->entries], &add->netaddrs[0], add->entries * sizeof(network_addr_t));

	to->entries = to->entries + add->entries;
	to->lines += add->lines;

	if(unlikely(to->flags & IPSET_FLAG_OPTIMIZED))
		to->flags ^= IPSET_FLAG_OPTIMIZED;
}


/* ----------------------------------------------------------------------------
 * ipset_copy()
 *
 * it returns a new ipset that is an exact copy of the ipset given
 *
 */

static inline ipset *ipset_copy(ipset *ips1) {
	ipset *ips;

	if(unlikely(debug)) fprintf(stderr, "%s: Copying %s\n", PROG, ips1->filename);

	ips = ipset_create(ips1->filename, ips1->entries);
	if(unlikely(!ips)) return NULL;

	/*strcpy(ips->name, ips1->name); */
	memcpy(&ips->netaddrs[0], &ips1->netaddrs[0], ips1->entries * sizeof(network_addr_t));

	ips->entries = ips1->entries;
	ips->unique_ips = ips1->unique_ips;
	ips->lines = ips1->lines;
	ips->flags = ips1->flags;

	return ips;
}


/* ----------------------------------------------------------------------------
 * ipset_combine()
 *
 * it returns a new ipset that has all the entries of both ipsets given
 * the result is never optimized, even when the source ipsets are
 *
 */

static inline ipset *ipset_combine(ipset *ips1, ipset *ips2) {
	ipset *ips;

	if(unlikely(debug)) fprintf(stderr, "%s: Combining %s and %s\n", PROG, ips1->filename, ips2->filename);

	ips = ipset_create("combined", ips1->entries + ips2->entries);
	if(unlikely(!ips)) return NULL;

	memcpy(&ips->netaddrs[0], &ips1->netaddrs[0], ips1->entries * sizeof(network_addr_t));
	memcpy(&ips->netaddrs[ips1->entries], &ips2->netaddrs[0], ips2->entries * sizeof(network_addr_t));

	ips->entries = ips1->entries + ips2->entries;
	ips->lines = ips1->lines + ips2->lines;

	return ips;
}

/* ----------------------------------------------------------------------------
 * ipset_histogram()
 *
 * generate histogram for ipset
 *
 */

/*
int ipset_histogram(ipset *ips, const char *path) {
	make sure the path exists
	if this is the first time:
	 - create a directory for this ipset, in path
	 - create the 'new' directory inside this ipset path
	 - assume the 'latest' is empty
	 - keep the starting date
	 - print an empty histogram
	save in 'new' the IPs of current excluding the 'latest'
	save 'current' as 'latest'
	assume the histogram is complete
	for each file in 'new'
	 - if the file is <= to histogram start date, the histogram is incomplete
	 - calculate the hours passed to the 'current'
	 - find the IPs in this file common to 'current' = 'stillthere'
	 - find the IPs in this file not in 'stillthere' = 'removed'
	 - if there are IPs in 'removed', add an entry to the retention histogram
	 - if there are no IPs in 'stillthere', delete the file
	 - else replace the file with the contents of 'stillthere'
	return 0;
}
*/


/* ----------------------------------------------------------------------------
 * usage()
 *
 * print help for the user
 *
 */

static void usage(const char *me) {
	fprintf(stderr, "\n"
		"iprange\n"
		"manage IP ranges\n"
		"version: " VERSION "\n"
		"\n"
		"Original,   Copyright (C) 2003 Gabriel L. Somlo\n"
		"Adapted,    Copyright (C) 2004 Paul Townsend\n"
		"Refactored, Copyright (C) 2015 Costa Tsaousis for FireHOL\n"
		"License: GPL\n"
		"\n"
		"Usage: %s [options] file1 file2 file3 ...\n"
		"\n"
		"options (multiple options are aliases):\n"
		"\n"
		"	--------------------------------------------------------------\n"
		"	CIDR OUTPUT MODES\n"
		"\n"
		"	--optimize\n"
		"	--combine\n"
		"	--merge\n"
		"	--union\n"
		"	-J\n"
		"		> MERGE mode (the default)\n"
		"		returns all IPs found on all files\n"
		"		the resulting set is sorted\n"
		"\n"
		"	--common\n"
		"	--intersect\n"
		"		> COMMON mode\n"
		"		intersect all files to find their common IPs\n"
		"		the resulting set is sorted\n"
		"\n"
		"	--except\n"
		"	--exclude-next\n"
		"		> EXCEPT mode\n"
		"		here is how it works:\n"
		"		1. merge all files before this parameter (ipset A)\n"
		"		2. remove all IPs found in the files after this\n"
		"		   parameter, from ipset A and print what remains\n"
		"		the resulting set is sorted\n"
		"\n"
		"	--diff\n"
		"	--diff-next\n"
		"		> DIFF mode\n"
		"		here is how it works:\n"
		"		1. merge all files before this parameter (ipset A)\n"
		"		2. merge all files after this parameter (ipset B)\n"
		"		3. print all differences between A and B, i.e IPs\n"
		"		   found is either A or B, but not both\n"
		"		the resulting set is sorted\n"
		"		when there are differences, iprange exits with 1\n"
		"		when ipsets A and B are the same, exits with 0"
		"\n"
		"	--ipset-reduce PERCENT\n"
		"	--reduce-factor PERCENT\n"
		"		> IPSET REDUCE mode\n"
		"		merge all files and print the merged set\n"
		"		but try to reduce the number of prefixes (subnets)\n"
		"		found, while allowing some increase in entries\n"
		"		the PERCENT is how much percent to allow\n"
		"		increase on the number of entries in order to reduce\n"
		"		the prefixes (subnets)\n"
		"		(the internal default PERCENT is 20)\n"
		"		(use -v to see exactly what it does)\n"
		"		the resulting set is sorted\n"
		"\n"
		"	--ipset-reduce-entries ENTRIES\n"
		"	--reduce-entries ENTRIES\n"
		"		> IPSET REDUCE mode\n"
		"		allow increasing the entries above PERCENT, if\n"
		"		they are below ENTRIES\n"
		"		(the internal default ENTRIES is 16384)\n"
/*
		"\n"
		"	--histogram\n"
		"		> IPSET HISTOGRAM mode\n"
		"		maintain histogram data for ipset and dump current\n"
		"		status\n"
		"\n"
		"	--histogram-dir PATH\n"
		"		> IPSET HISTOGRAM mode\n"
		"		the directory to keep histogram data\n"
*/
		"\n"
		"\n"
		"	--------------------------------------------------------------\n"
		"	CSV OUTPUT MODES\n"
		"\n"
		"	--compare\n"
		"		> COMPARE ALL mode (CSV output)\n"
		"		compare all files with all other files\n"
		"		add --header to get the CSV header too\n"
		"\n"
		"	--compare-first\n"
		"		> COMPARE FIRST mode (CSV output)\n"
		"		compare the first file with all other files\n"
		"		add --header to get the CSV header too\n"
		"\n"
		"	--compare-next\n"
		"		> COMPARE NEXT mode (CSV output)\n"
		"		compare all the files that appear before this\n"
		"		parameter, to all files that appear after this\n"
		"		parameter\n"
		"		add --header to get the CSV header too\n"
		"\n"
		"	--count-unique\n"
		"	-C\n"
		"		> COUNT UNIQUE mode (CSV output)\n"
		"		merge all files and print its counts\n"
		"		add --header to get the CSV header too\n"
		"\n"
		"	--count-unique-all\n"
		"		> COUNT UNIQUE ALL mode (CSV output)\n"
		"		print counts for each file\n"
		"		add --header to get the CSV header too\n"
		"\n"
		"\n"
		"	--------------------------------------------------------------\n"
		"	OPTIONS THAT AFFECT INPUT\n"
		"\n"
		"	--dont-fix-network\n"
		"		by default, the network address of all CIDRs\n"
		"		is used (i.e. 1.1.1.17/24 is read as 1.1.1.0/24)\n"
		"		this option disables this feature\n"
		"		(i.e. 1.1.1.17/24 is read as 1.1.1.17-1.1.1.255)\n"
		"\n"
		"	--default-prefix PREFIX\n"
		"	-p PREFIX\n"
		"		Set the default prefix for all IPs without mask\n"
		"		the default is 32\n"
		"\n"
		"\n"
		"	--------------------------------------------------------------\n"
		"	OPTIONS THAT AFFECT CIDR OUTPUT\n"
		"\n"
		"	--min-prefix N\n"
		"		do not generate prefixes larger than N\n"
		"		i.e. if N is 24 then /24 to /32 entries will be\n"
		"		     generated (a /16 network will be generated\n"
		"		     using multiple /24 networks)\n"
		"		this is useful to optimize netfilter/iptables\n"
		"		ipsets, where each different prefix increases the\n"
		"		lookup time for each packet, but the number of\n"
		"		entries in the ipset do not affect its performance\n"
		"		with this setting more entries will be produced\n"
		"		to accomplish the same match\n"
		"		warning: misuse of this parameter can create a large\n"
		"		         number of entries in the generated set\n"
		"\n"
		"	--prefixes N,N,N, ...\n"
		"		enable only the given prefixes to express all CIDRs\n"
		"		prefix 32 is always enabled\n"
		"		warning: misuse of this parameter can create a large\n"
		"		         number of entries in the generated set\n"
		"\n"
		"	--print-ranges\n"
		"	-j\n"
		"		print IP ranges (A.A.A.A-B.B.B.B)\n"
		"		the default is to print CIDRs (A.A.A.A/B)\n"
		"		it only applies when the output is not CSV\n"
		"\n"
		"	--print-single-ips\n"
		"	-1\n"
		"		print single IPs\n"
		"		this can produce large output\n"
		"		the default is to print CIDRs (A.A.A.A/B)\n"
		"		it only applies when the output is not CSV\n"
		"\n"
		"	--print-binary\n"
		"		print binary data\n"
		"		this is the fastest way to print a large ipset\n"
		"		the result can be read by iprange on the same\n"
		"		architecture (no conversion of endianess)\n"
		"\n"
		"	--print-prefix STRING\n"
		"		print STRING before each IP, range or CIDR\n"
		"		this sets both --print-prefix-ips and\n"
		"		--print-prefix-nets\n"
		"\n"
		"	--print-prefix-ips STRING\n"
		"		print STRING before each single IP\n"
		"		useful for entering single IPs to a different\n"
		"		ipset than the networks\n"
		"\n"
		"	--print-prefix-nets STRING\n"
		"		print STRING before each range or CIDR\n"
		"		useful for entering sunbets to a different\n"
		"		ipset than single IPs\n"
		"\n"
		"	--print-suffix STRING\n"
		"		print STRING after each IP, range or CIDR\n"
		"		this sets both --print-suffix-ips and\n"
		"		--print-suffix-nets\n"
		"\n"
		"	--print-suffix-ips STRING\n"
		"		print STRING after each single IP\n"
		"		useful for giving single IPs different\n"
		"		ipset options\n"
		"\n"
		"	--print-suffix-nets STRING\n"
		"		print STRING after each range or CIDR\n"
		"		useful for giving subnets different\n"
		"		ipset options\n"
		"\n"
		"	--quiet\n"
		"		do not print the actual ipset\n"
		"		can only be used in DIFF mode\n"
		"\n"
		"\n"
		"	--------------------------------------------------------------\n"
		"	OPTIONS THAT AFFECT CSV OUTPUT\n"
		"\n"
		"	--header\n"
		"		when the output is CSV, print the header line\n"
		"		the default is to not print the header line\n"
		"\n"
		"\n"
		"	--------------------------------------------------------------\n"
		"	OPTIONS THAT AFFECT DNS RESOLUTION\n"
		"\n"
		"	--dns-threads NUMBER\n"
		"		the number of parallel DNS queries to execute\n"
		"		when the input files contain hostnames\n"
		"		the default is %d\n"
		"\n"
		"	--dns-silent\n"
		"		do not print DNS resolution errors\n"
		"		the default is to print all DNS related errors\n"
		"\n"
		"	--dns-progress\n"
		"		print DNS resolution progress bar\n"
		"\n"
		"\n"
		"	--------------------------------------------------------------\n"
		"	OTHER OPTIONS\n"
		"\n"
		"	--has-compare\n"
		"	--has-reduce\n"
		"		exits with 0\n"
		"		other versions of iprange will exit with 1\n"
		"		use this option in scripts to find if this\n"
		"		version of iprange is present in a system\n"
		"\n"
		"	-v\n"
		"		be verbose on stderr\n"
		"\n"
		"	--version\n"
		"		print version\n"
		"\n"
		"	--help\n"
		"	-h\n"
		"		print this message\n"
		"\n"
		"\n"
		"	--------------------------------------------------------------\n"
		"	INPUT FILES\n"
		"\n"
		"	fileN\n"
		"		a filename or - for stdin\n"
		"		each filename can be followed by [as NAME]\n"
		"		to change its name in the CSV output\n"
		"\n"
		"		if no filename is given, stdin is assumed\n"
		"\n"
		"		files may contain any or all of the following:\n"
		"		- comments starting with # or ;\n"
		"		- one IP per line (without mask)\n"
		"		- a CIDR per line (A.A.A.A/B)\n"
		"		- an IP range per line (A.A.A.A - B.B.B.B)\n"
		"		- a CIDR range per line (A.A.A.A/B - C.C.C.C/D)\n"
		"		  the range is calculated as the network address of\n"
		"		  A.A.A.A/B to the broadcast address of C.C.C.C/D\n"
		"		  (this is affected by --dont-fix-network)\n"
		"		- CIDRs can be given in either prefix or netmask\n"
		"		  format in all cases (including ranges)\n"
		"		- one hostname per line, to be resolved with DNS\n"
		"		  (if the IP resolves to multiple IPs, all of them\n"
		"		  will be added to the ipset)\n"
		"		  hostnames cannot be given as ranges\n"
		"		- spaces and empty lines are ignored\n"
		"\n"
		"		any number of files can be given\n"
		"\n"
		, me, dns_threads_max);
	exit(1);
}


/* ----------------------------------------------------------------------------
 * version()
 *
 * print version for the user
 *
 */

static void version() {
	fprintf(stdout, "%s\n", VERSION);
	exit(1);
}

#define MODE_COMBINE 1
#define MODE_COMPARE 2
#define MODE_COMPARE_FIRST 3
#define MODE_COMPARE_NEXT 4
#define MODE_COUNT_UNIQUE_MERGED 5
#define MODE_COUNT_UNIQUE_ALL 6
#define MODE_REDUCE 7
#define MODE_COMMON 8
#define MODE_EXCLUDE_NEXT 9
#define MODE_DIFF 10
/*#define MODE_HISTOGRAM 11 */

int main(int argc, char **argv) {
/*	char histogram_dir[FILENAME_MAX + 1] = "/var/lib/iprange"; */

	struct timeval start_dt, load_dt, print_dt, stop_dt;

	int ipset_reduce_factor = 120;
	int ipset_reduce_min_accepted = 16384;
	int ret = 0, quiet = 0;

	ipset *root = NULL, *ips = NULL, *first = NULL, *second = NULL;
	int i, mode = MODE_COMBINE, print = PRINT_CIDR, header = 0, read_second = 0;

	gettimeofday(&start_dt, NULL);

	if ((PROG = strrchr(argv[0], '/')))
		PROG++;
	else
		PROG = argv[0];

	for(i = 1; i < argc ; i++) {
		if(i+1 < argc && !strcmp(argv[i], "as")) {
			if(!read_second) {
				if(root) {
					strncpy(root->filename, argv[++i], FILENAME_MAX);
					root->filename[FILENAME_MAX] = '\0';
				}
			}
			else {
				if(second) {
					strncpy(second->filename, argv[++i], FILENAME_MAX);
					second->filename[FILENAME_MAX] = '\0';
				}
			}
		}
		else if(i+1 < argc && !strcmp(argv[i], "--min-prefix")) {
			int j, min_prefix = atoi(argv[++i]);
			if(min_prefix < 1 || min_prefix > 32) {
				fprintf(stderr, "Only prefixes 1 to 31 can be disabled. %d is invalid.\n", min_prefix);
				exit(1);
			}
			for(j = 0; j < min_prefix; j++)
				prefix_enabled[j] = 0;
		}
		else if(i+1 < argc && !strcmp(argv[i], "--prefixes")) {
			char *s = NULL, *e = argv[++i];
			int j;

			for(j = 0; j < 32; j++)
				prefix_enabled[j] = 0;

			while(e && *e && e != s) {
				s = e;
				j = strtol(s, &e, 10);
				if(j <= 0 || j > 32) {
					fprintf(stderr, "%s: Only prefixes from 1 to 32 can be set (32 is always enabled). %d is invalid.\n", PROG, j);
					exit(1);
				}
				if(debug) fprintf(stderr, "Enabling prefix %d\n", j);
				prefix_enabled[j] = 1;
				if(*e == ',' || *e == ' ') e++;
			}

			if(e && *e) {
				fprintf(stderr, "%s: Invalid prefix '%s'\n", PROG, e);
				exit(1);
			}
		}
		else if(i+1 < argc && (
			   !strcmp(argv[i], "--default-prefix")
			|| !strcmp(argv[i], "-p")
			)) {
			default_prefix = atoi(argv[++i]);
		}
		else if(i+1 < argc && (
			   !strcmp(argv[i], "--ipset-reduce")
			|| !strcmp(argv[i], "--reduce-factor")
			)) {
			ipset_reduce_factor = 100 + atoi(argv[++i]);
			mode = MODE_REDUCE;
		}
		else if(i+1 < argc && (
			   !strcmp(argv[i], "--ipset-reduce-entries")
			|| !strcmp(argv[i], "--reduce-entries")
			)) {
			ipset_reduce_min_accepted = atoi(argv[++i]);
			mode = MODE_REDUCE;
		}
		else if(!strcmp(argv[i], "--optimize")
			|| !strcmp(argv[i], "--combine")
			|| !strcmp(argv[i], "--merge")
			|| !strcmp(argv[i], "--union")
			|| !strcmp(argv[i], "--union-all")
			|| !strcmp(argv[i], "-J")
			) {
			mode = MODE_COMBINE;
		}
		else if(!strcmp(argv[i], "--common")
			|| !strcmp(argv[i], "--intersect")
			|| !strcmp(argv[i], "--intersect-all")) {
			mode = MODE_COMMON;
		}
		else if(!strcmp(argv[i], "--exclude-next")
			|| !strcmp(argv[i], "--except")
			|| !strcmp(argv[i], "--complement-next")
			|| !strcmp(argv[i], "--complement")) {
			mode = MODE_EXCLUDE_NEXT;
			read_second = 1;
			if(!root) {
				fprintf(stderr, "%s: An ipset is needed before --except\n", PROG);
				exit(1);
			}
		}
		else if(!strcmp(argv[i], "--diff")
			|| !strcmp(argv[i], "--diff-next")) {
			mode = MODE_DIFF;
			read_second = 1;
			if(!root) {
				fprintf(stderr, "%s: An ipset is needed before --diff\n", PROG);
				exit(1);
			}
		}
		else if(!strcmp(argv[i], "--compare")) {
			mode = MODE_COMPARE;
		}
		else if(!strcmp(argv[i], "--compare-first")) {
			mode = MODE_COMPARE_FIRST;
		}
		else if(!strcmp(argv[i], "--compare-next")) {
			mode = MODE_COMPARE_NEXT;
			read_second = 1;
			if(!root) {
				fprintf(stderr, "%s: An ipset is needed before --compare-next\n", PROG);
				exit(1);
			}
		}
		else if(!strcmp(argv[i], "--count-unique")
			|| !strcmp(argv[i], "-C")) {
			mode = MODE_COUNT_UNIQUE_MERGED;
		}
		else if(!strcmp(argv[i], "--count-unique-all")) {
			mode = MODE_COUNT_UNIQUE_ALL;
		}
/*
		else if(!strcmp(argv[i], "--histogram")) {
			mode = MODE_HISTOGRAM;
		}
		else if(i+1 < argc && !strcmp(argv[i], "--histogram-dir")) {
			mode = MODE_HISTOGRAM;
			strncpy(histogram_dir, argv[++i], FILENAME_MAX);
		}
*/
		else if(!strcmp(argv[i], "--version")) {
			version();
		}
		else if(!strcmp(argv[i], "--help")
			|| !strcmp(argv[i], "-h")) {
			usage(argv[0]);
		}
		else if(!strcmp(argv[i], "-v")) {
			debug = 1;
		}
		else if(!strcmp(argv[i], "--print-ranges")
			|| !strcmp(argv[i], "-j")) {
			print = PRINT_RANGE;
		}
		else if(!strcmp(argv[i], "--print-binary")) {
			print = PRINT_BINARY;
		}
		else if(!strcmp(argv[i], "--print-single-ips")
			|| !strcmp(argv[i], "-1")) {
			print = PRINT_SINGLE_IPS;
		}
		else if(i+1 < argc && !strcmp(argv[i], "--print-prefix")) {
			print_prefix_ips  = argv[++i];
			print_prefix_nets = print_prefix_ips;
		}
		else if(i+1 < argc && !strcmp(argv[i], "--print-prefix-ips")) {
			print_prefix_ips = argv[++i];
		}
		else if(i+1 < argc && !strcmp(argv[i], "--print-prefix-nets")) {
			print_prefix_nets = argv[++i];
		}
		else if(i+1 < argc && !strcmp(argv[i], "--print-suffix")) {
			print_suffix_ips = argv[++i];
			print_suffix_nets = print_suffix_ips;
		}
		else if(i+1 < argc && !strcmp(argv[i], "--print-suffix-ips")) {
			print_suffix_ips = argv[++i];
		}
		else if(i+1 < argc && !strcmp(argv[i], "--print-suffix-nets")) {
			print_suffix_nets = argv[++i];
		}
		else if(!strcmp(argv[i], "--header")) {
			header = 1;
		}
		else if(!strcmp(argv[i], "--quiet")) {
			quiet = 1;
		}
		else if(!strcmp(argv[i], "--dont-fix-network")) {
			cidr_use_network = 0;
		}
		else if(i+1 < argc && !strcmp(argv[i], "--dns-threads")) {
			dns_threads_max = atoi(argv[++i]);
			if(dns_threads_max < 1) dns_threads_max = 1;
		}
		else if(!strcmp(argv[i], "--dns-silent")) {
			dns_silent = 1;
		}
		else if(!strcmp(argv[i], "--dns-progress")) {
			dns_progress = 1;
		}
		else if(!strcmp(argv[i], "--has-compare")
			|| !strcmp(argv[i], "--has-reduce")) {
			fprintf(stderr, "yes, compare and reduce is present.\n");
			exit(0);
		}
		else {
			if(!strcmp(argv[i], "-"))
				ips = ipset_load(NULL);
			else
				ips = ipset_load(argv[i]);

			if(!ips) {
				fprintf(stderr, "%s: Cannot load ipset: %s\n", PROG, argv[i]);
				exit(1);
			}

			if(read_second) {
				ips->next = second;
				second = ips;
				if(ips->next) ips->next->prev = ips;
			}
			else {
				if(!first) first = ips;
				ips->next = root;
				root = ips;
				if(ips->next) ips->next->prev = ips;
			}
		}
	}

	/*
	 * if no ipset was given on the command line
	 * assume stdin
	 */

	if(!root) {
		first = root = ipset_load(NULL);
		if(!root) {
			fprintf(stderr, "%s: No ipsets to merge.\n", PROG);
			exit(1);
		}
	}

	gettimeofday(&load_dt, NULL);

	if(mode == MODE_COMBINE || mode == MODE_REDUCE || mode == MODE_COUNT_UNIQUE_MERGED) {
		/* for debug mode to show something meaningful */
		strcpy(root->filename, "combined ipset");

		for(ips = root->next; ips ;ips = ips->next)
			ipset_merge(root, ips);

		/* ipset_optimize(root); */
		if(mode == MODE_REDUCE) ipset_reduce(root, ipset_reduce_factor, ipset_reduce_min_accepted);

		gettimeofday(&print_dt, NULL);

		if(mode == MODE_COMBINE || mode == MODE_REDUCE)
			ipset_print(root, print);

		else if(mode == MODE_COUNT_UNIQUE_MERGED) {
			if(unlikely(header)) printf("entries,unique_ips\n");
			printf("%lu,%lu\n", root->lines, ipset_unique_ips(root));
		}
	}
	else if(mode == MODE_COMMON) {
		ipset *common = NULL, *ips2 = NULL;

		if(!root->next) {
			fprintf(stderr, "%s: two ipsets at least are needed to be compared to find their common IPs.\n", PROG);
			exit(1);
		}

		/* ipset_optimize_all(root); */

		common = ipset_common(root, root->next);
		for(ips = root->next->next; ips ;ips = ips->next) {
			ips2 = ipset_common(common, ips);
			ipset_free(common);
			common = ips2;
		}

		gettimeofday(&print_dt, NULL);
		ipset_print(common, print);
	}
	else if(mode == MODE_DIFF) {
		if(!root || !second) {
			fprintf(stderr, "%s: two ipsets at least are needed to be diffed.\n", PROG);
			exit(1);
		}

		for(ips = root->next; ips ;ips = ips->next)
			ipset_merge(root, ips);
		if(root->next) strcpy(root->filename, "ipset A");

		for(ips = second->next; ips ;ips = ips->next)
			ipset_merge(second, ips);
		if(second->next) strcpy(root->filename, "ipset B");

		ips = ipset_diff(root, second);

		gettimeofday(&print_dt, NULL);
		if(!quiet) ipset_print(ips, print);
		gettimeofday(&print_dt, NULL);

		if(ips->unique_ips) ret = 1;
		else ret = 0;
	}
	else if(mode == MODE_COMPARE) {
		ipset *ips2;

		if(!root->next) {
			fprintf(stderr, "%s: two ipsets at least are needed to be compared.\n", PROG);
			exit(1);
		}

		if(unlikely(header)) printf("name1,name2,entries1,entries2,ips1,ips2,combined_ips,common_ips\n");

		/* ipset_optimize_all(root); */

		for(ips = root; ips ;ips = ips->next) {
			for(ips2 = ips; ips2 ;ips2 = ips2->next) {
				ipset *comips;

				if(ips == ips2) continue;

#ifdef COMPARE_WITH_COMMON
				comips = ipset_common(ips, ips2);
				if(!comips) {
					fprintf(stderr, "%s: Cannot find the common IPs of ipset %s and %s\n", PROG, ips->filename, ips2->filename);
					exit(1);
				}
				fprintf(stdout, "%s,%s,%lu,%lu,%lu,%lu,%lu,%lu\n", ips->filename, ips2->filename, ips->lines, ips2->lines, ips->unique_ips, ips2->unique_ips, ips->unique_ips + ips2->unique_ips - comips->unique_ips, comips->unique_ips);
				ipset_free(comips);
#else
				comips = ipset_combine(ips, ips2);
				if(!compips) {
					fprintf(stderr, "%s: Cannot merge ipset %s and %s\n", PROG, ips->filename, ips2->filename);
					exit(1);
				}

				ipset_optimize(comips);
				fprintf(stdout, "%s,%s,%lu,%lu,%lu,%lu,%lu,%lu\n", ips->filename, ips2->filename, ips->lines, ips2->lines, ips->unique_ips, ips2->unique_ips, comips->unique_ips, ips->unique_ips + ips2->unique_ips - comips->unique_ips);
				ipset_free(comips);
#endif
			}
		}
		gettimeofday(&print_dt, NULL);
	}
	else if(mode == MODE_COMPARE_NEXT) {
		ipset *ips2;

		if(!second) {
			fprintf(stderr, "%s: no files given after the --compare-next parameter.\n", PROG);
			exit(1);
		}

		if(unlikely(header)) printf("name1,name2,entries1,entries2,ips1,ips2,combined_ips,common_ips\n");

		/* ipset_optimize_all(root); */
		/* ipset_optimize_all(second); */

		for(ips = root; ips ;ips = ips->next) {
			for(ips2 = second; ips2 ;ips2 = ips2->next) {
#ifdef COMPARE_WITH_COMMON
				ipset *common = ipset_common(ips, ips2);
				if(!common) {
					fprintf(stderr, "%s: Cannot find the common IPs of ipset %s and %s\n", PROG, ips->filename, ips2->filename);
					exit(1);
				}
				fprintf(stdout, "%s,%s,%lu,%lu,%lu,%lu,%lu,%lu\n", ips->filename, ips2->filename, ips->lines, ips2->lines, ips->unique_ips, ips2->unique_ips, ips->unique_ips + ips2->unique_ips - common->unique_ips, common->unique_ips);
				ipset_free(common);
#else
				ipset *combined = ipset_combine(ips, ips2);
				if(!combined) {
					fprintf(stderr, "%s: Cannot merge ipset %s and %s\n", PROG, ips->filename, ips2->filename);
					exit(1);
				}

				ipset_optimize(combined);
				fprintf(stdout, "%s,%s,%lu,%lu,%lu,%lu,%lu,%lu\n", ips->filename, ips2->filename, ips->lines, ips2->lines, ips->unique_ips, ips2->unique_ips, combined->unique_ips, ips->unique_ips + ips2->unique_ips - combined->unique_ips);
				ipset_free(combined);
#endif
			}
		}
		gettimeofday(&print_dt, NULL);
	}
	else if(mode == MODE_COMPARE_FIRST) {
		if(!root->next) {
			fprintf(stderr, "%s: two ipsets at least are needed to be compared.\n", PROG);
			exit(1);
		}

		if(unlikely(header)) printf("name,entries,unique_ips,common_ips\n");

		/* ipset_optimize_all(root); */

		for(ips = root; ips ;ips = ips->next) {
			ipset *comips;

			if(ips == first) continue;

#ifdef COMPARE_WITH_COMMON
			comips = ipset_common(ips, first);
			if(!comips) {
				fprintf(stderr, "%s: Cannot find the comips IPs of ipset %s and %s\n", PROG, ips->filename, first->filename);
				exit(1);
			}
			printf("%s,%lu,%lu,%lu\n", ips->filename, ips->lines, ips->unique_ips, comips->unique_ips);
			ipset_free(comips);
#else
			comips = ipset_combine(ips, first);
			if(!comips) {
				fprintf(stderr, "%s: Cannot merge ipset %s and %s\n", PROG, ips->filename, first->filename);
				exit(1);
			}

			ipset_optimize(comips);
			printf("%s,%lu,%lu,%lu\n", ips->filename, ips->lines, ips->unique_ips, ips->unique_ips + first->unique_ips - comips->unique_ips);
			ipset_free(comips);
#endif
		}
		gettimeofday(&print_dt, NULL);
	}
	else if(mode == MODE_EXCLUDE_NEXT) {
		ipset *excluded;

		if(!second) {
			fprintf(stderr, "%s: no files given after the --exclude-next parameter.\n", PROG);
			exit(1);
		}

		/* merge them */
		for(ips = root->next; ips ;ips = ips->next)
			ipset_merge(root, ips);

		/* ipset_optimize(root); */
		/* ipset_optimize_all(second); */

		excluded = root;
		root = root->next;
		for(ips = second; ips ;ips = ips->next) {
			ipset *tmp = ipset_exclude(excluded, ips);
			if(!tmp) {
				fprintf(stderr, "%s: Cannot exclude the IPs of ipset %s from %s\n", PROG, ips->filename, excluded->filename);
				exit(1);
			}

			ipset_free(excluded);
			excluded = tmp;
		}
		gettimeofday(&print_dt, NULL);
		ipset_print(excluded, print);
	}
	else if(mode == MODE_COUNT_UNIQUE_ALL) {
		if(unlikely(header)) printf("name,entries,unique_ips\n");

		ipset_optimize_all(root);

		for(ips = root; ips ;ips = ips->next) {
			printf("%s,%lu,%lu\n", ips->filename, ips->lines, ips->unique_ips);
		}
		gettimeofday(&print_dt, NULL);
	}
/*
	else if(mode == MODE_HISTOGRAM) {
		for(ips = root; ips ;ips = ips->next) {
			ipset_histogram(ips, histogram_dir);
		}
	}
*/
	else {
		fprintf(stderr, "%s: Unknown mode.\n", PROG);
		exit(1);
	}

	gettimeofday(&stop_dt, NULL);
	if(debug)
		fprintf(stderr, "completed in %0.5f seconds (read %0.5f + think %0.5f + speak %0.5f)\n"
			, ((double)(stop_dt.tv_sec  * 1000000 + stop_dt.tv_usec) - (double)(start_dt.tv_sec * 1000000 + start_dt.tv_usec)) / (double)1000000
			, ((double)(load_dt.tv_sec  * 1000000 + load_dt.tv_usec) - (double)(start_dt.tv_sec * 1000000 + start_dt.tv_usec)) / (double)1000000
			, ((double)(print_dt.tv_sec  * 1000000 + print_dt.tv_usec) - (double)(load_dt.tv_sec * 1000000 + load_dt.tv_usec)) / (double)1000000
			, ((double)(stop_dt.tv_sec  * 1000000 + stop_dt.tv_usec) - (double)(print_dt.tv_sec * 1000000 + print_dt.tv_usec)) / (double)1000000
		);

	exit(ret);
}
