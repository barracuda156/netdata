// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin_proc.h"

#define PLUGIN_PROC_MODULE_NET_SOCKSTAT_NAME "/proc/net/sockstat"

static struct proc_net_sockstat {
    kernel_uint_t sockets_used;

    kernel_uint_t tcp_inuse;
    kernel_uint_t tcp_orphan;
    kernel_uint_t tcp_tw;
    kernel_uint_t tcp_alloc;
    kernel_uint_t tcp_mem;

    kernel_uint_t udp_inuse;
    kernel_uint_t udp_mem;

    kernel_uint_t udplite_inuse;

    kernel_uint_t raw_inuse;

    kernel_uint_t frag_inuse;
    kernel_uint_t frag_memory;
} sockstat_root = { 0 };


static const RRDVAR_ACQUIRED
    *tcp_mem_low_threshold = NULL,
    *tcp_mem_pressure_threshold = NULL,
    *tcp_mem_high_threshold = NULL,
    *tcp_max_orphans_var = NULL;

void proc_net_sockstat_plugin_cleanup(void) {
    // Cleanup any acquired RRDVARs
    if (tcp_mem_low_threshold) {
        rrdvar_host_variable_release(localhost, tcp_mem_low_threshold);
        tcp_mem_low_threshold = NULL;
    }
    if (tcp_mem_pressure_threshold) {
        rrdvar_host_variable_release(localhost, tcp_mem_pressure_threshold);
        tcp_mem_pressure_threshold = NULL;
    }
    if (tcp_mem_high_threshold) {
        rrdvar_host_variable_release(localhost, tcp_mem_high_threshold);
        tcp_mem_high_threshold = NULL;
    }
    if (tcp_max_orphans_var) {
        rrdvar_host_variable_release(localhost, tcp_max_orphans_var);
        tcp_max_orphans_var = NULL;
    }
}

static int read_tcp_mem(void) {
    static char *filename = NULL;

    if(unlikely(!tcp_mem_low_threshold)) {
        tcp_mem_low_threshold      = rrdvar_host_variable_add_and_acquire(localhost, "tcp_mem_low");
        tcp_mem_pressure_threshold = rrdvar_host_variable_add_and_acquire(localhost, "tcp_mem_pressure");
        tcp_mem_high_threshold     = rrdvar_host_variable_add_and_acquire(localhost, "tcp_mem_high");
    }

    if(unlikely(!filename)) {
        char buffer[FILENAME_MAX + 1];
        snprintfz(buffer, FILENAME_MAX, "%s/proc/sys/net/ipv4/tcp_mem", netdata_configured_host_prefix);
        filename = strdupz(buffer);
    }

    char buffer[200 + 1], *start, *end;
    if(read_txt_file(filename, buffer, sizeof(buffer)) != 0) return 1;
    buffer[200] = '\0';

    unsigned long long low = 0, pressure = 0, high = 0;

    start = buffer;
    low = strtoull(start, &end, 10);

    start = end;
    pressure = strtoull(start, &end, 10);

    start = end;
    high = strtoull(start, &end, 10);

    // fprintf(stderr, "TCP MEM low = %llu, pressure = %llu, high = %llu\n", low, pressure, high);

    rrdvar_host_variable_set(localhost, tcp_mem_low_threshold, low * sysconf(_SC_PAGESIZE) / 1024.0);
    rrdvar_host_variable_set(localhost, tcp_mem_pressure_threshold, pressure * sysconf(_SC_PAGESIZE) / 1024.0);
    rrdvar_host_variable_set(localhost, tcp_mem_high_threshold, high * sysconf(_SC_PAGESIZE) / 1024.0);

    return 0;
}

static kernel_uint_t read_tcp_max_orphans(void) {
    static char *filename = NULL;

    if(unlikely(!filename)) {
        char buffer[FILENAME_MAX + 1];
        snprintfz(buffer, FILENAME_MAX, "%s/proc/sys/net/ipv4/tcp_max_orphans", netdata_configured_host_prefix);
        filename = strdupz(buffer);
    }

    unsigned long long tcp_max_orphans = 0;
    if(read_single_number_file(filename, &tcp_max_orphans) == 0) {

        if(unlikely(!tcp_max_orphans_var))
            tcp_max_orphans_var = rrdvar_host_variable_add_and_acquire(localhost, "tcp_max_orphans");

        rrdvar_host_variable_set(localhost, tcp_max_orphans_var, tcp_max_orphans);
        return  tcp_max_orphans;
    }

    return 0;
}

int do_proc_net_sockstat(int update_every, usec_t dt) {
    (void)dt;

    static procfile *ff = NULL;

    static uint32_t hash_sockets = 0,
                    hash_raw = 0,
                    hash_frag = 0,
                    hash_tcp = 0,
                    hash_udp = 0,
                    hash_udplite = 0;

    static long long update_constants_every = 60, update_constants_count = 0;

    static ARL_BASE *arl_sockets = NULL;
    static ARL_BASE *arl_tcp = NULL;
    static ARL_BASE *arl_udp = NULL;
    static ARL_BASE *arl_udplite = NULL;
    static ARL_BASE *arl_raw = NULL;
    static ARL_BASE *arl_frag = NULL;

    static int do_sockets = -1, do_tcp_sockets = -1, do_tcp_mem = -1, do_udp_sockets = -1, do_udp_mem = -1, do_udplite_sockets = -1, do_raw_sockets = -1, do_frag_sockets = -1, do_frag_mem = -1;

    static char     *keys[7]  = { NULL };
    static uint32_t hashes[7] = { 0 };
    static ARL_BASE *bases[7] = { NULL };

    if(unlikely(!arl_sockets)) {
        do_sockets         = inicfg_get_boolean_ondemand(&netdata_config, "plugin:proc:/proc/net/sockstat", "ipv4 sockets", CONFIG_BOOLEAN_AUTO);
        do_tcp_sockets     = inicfg_get_boolean_ondemand(&netdata_config, "plugin:proc:/proc/net/sockstat", "ipv4 TCP sockets", CONFIG_BOOLEAN_AUTO);
        do_tcp_mem         = inicfg_get_boolean_ondemand(&netdata_config, "plugin:proc:/proc/net/sockstat", "ipv4 TCP memory", CONFIG_BOOLEAN_AUTO);
        do_udp_sockets     = inicfg_get_boolean_ondemand(&netdata_config, "plugin:proc:/proc/net/sockstat", "ipv4 UDP sockets", CONFIG_BOOLEAN_AUTO);
        do_udp_mem         = inicfg_get_boolean_ondemand(&netdata_config, "plugin:proc:/proc/net/sockstat", "ipv4 UDP memory", CONFIG_BOOLEAN_AUTO);
        do_udplite_sockets = inicfg_get_boolean_ondemand(&netdata_config, "plugin:proc:/proc/net/sockstat", "ipv4 UDPLITE sockets", CONFIG_BOOLEAN_AUTO);
        do_raw_sockets     = inicfg_get_boolean_ondemand(&netdata_config, "plugin:proc:/proc/net/sockstat", "ipv4 RAW sockets", CONFIG_BOOLEAN_AUTO);
        do_frag_sockets    = inicfg_get_boolean_ondemand(&netdata_config, "plugin:proc:/proc/net/sockstat", "ipv4 FRAG sockets", CONFIG_BOOLEAN_AUTO);
        do_frag_mem        = inicfg_get_boolean_ondemand(&netdata_config, "plugin:proc:/proc/net/sockstat", "ipv4 FRAG memory", CONFIG_BOOLEAN_AUTO);

        update_constants_every = inicfg_get_duration_seconds(&netdata_config, "plugin:proc:/proc/net/sockstat", "update constants every", update_constants_every);
        update_constants_count = update_constants_every;

        arl_sockets = arl_create("sockstat/sockets", arl_callback_str2kernel_uint_t, 60);
        arl_expect(arl_sockets, "used", &sockstat_root.sockets_used);

        arl_tcp = arl_create("sockstat/TCP", arl_callback_str2kernel_uint_t, 60);
        arl_expect(arl_tcp, "inuse",  &sockstat_root.tcp_inuse);
        arl_expect(arl_tcp, "orphan", &sockstat_root.tcp_orphan);
        arl_expect(arl_tcp, "tw",     &sockstat_root.tcp_tw);
        arl_expect(arl_tcp, "alloc",  &sockstat_root.tcp_alloc);
        arl_expect(arl_tcp, "mem",    &sockstat_root.tcp_mem);

        arl_udp = arl_create("sockstat/UDP", arl_callback_str2kernel_uint_t, 60);
        arl_expect(arl_udp, "inuse", &sockstat_root.udp_inuse);
        arl_expect(arl_udp, "mem", &sockstat_root.udp_mem);

        arl_udplite = arl_create("sockstat/UDPLITE", arl_callback_str2kernel_uint_t, 60);
        arl_expect(arl_udplite, "inuse", &sockstat_root.udplite_inuse);

        arl_raw = arl_create("sockstat/RAW", arl_callback_str2kernel_uint_t, 60);
        arl_expect(arl_raw, "inuse", &sockstat_root.raw_inuse);

        arl_frag = arl_create("sockstat/FRAG", arl_callback_str2kernel_uint_t, 60);
        arl_expect(arl_frag, "inuse", &sockstat_root.frag_inuse);
        arl_expect(arl_frag, "memory", &sockstat_root.frag_memory);

        hash_sockets = simple_hash("sockets");
        hash_tcp = simple_hash("TCP");
        hash_udp = simple_hash("UDP");
        hash_udplite = simple_hash("UDPLITE");
        hash_raw = simple_hash("RAW");
        hash_frag = simple_hash("FRAG");

        keys[0] = "sockets"; hashes[0] = hash_sockets; bases[0] = arl_sockets;
        keys[1] = "TCP";     hashes[1] = hash_tcp;     bases[1] = arl_tcp;
        keys[2] = "UDP";     hashes[2] = hash_udp;     bases[2] = arl_udp;
        keys[3] = "UDPLITE"; hashes[3] = hash_udplite; bases[3] = arl_udplite;
        keys[4] = "RAW";     hashes[4] = hash_raw;     bases[4] = arl_raw;
        keys[5] = "FRAG";    hashes[5] = hash_frag;    bases[5] = arl_frag;
        keys[6] = NULL; // terminator
    }

    update_constants_count += update_every;
    if(unlikely(update_constants_count > update_constants_every)) {
        read_tcp_max_orphans();
        read_tcp_mem();
        update_constants_count = 0;
    }

    if(unlikely(!ff)) {
        char filename[FILENAME_MAX + 1];
        snprintfz(filename, FILENAME_MAX, "%s%s", netdata_configured_host_prefix, "/proc/net/sockstat");
        ff = procfile_open(inicfg_get(&netdata_config, "plugin:proc:/proc/net/sockstat", "filename to monitor", filename), " \t:", PROCFILE_FLAG_DEFAULT);
        if(unlikely(!ff)) return 1;
    }

    ff = procfile_readall(ff);
    if(unlikely(!ff)) return 0; // we return 0, so that we will retry to open it next time

    size_t lines = procfile_lines(ff), l;

    for(l = 0; l < lines ;l++) {
        size_t  words = procfile_linewords(ff, l);
        char     *key = procfile_lineword(ff, l, 0);
        uint32_t hash = simple_hash(key);

        int k;
        for(k = 0; keys[k] ; k++) {
            if(unlikely(hash == hashes[k] && strcmp(key, keys[k]) == 0)) {
                // fprintf(stderr, "KEY: '%s', l=%zu, w=1, words=%zu\n", key, l, words);
                ARL_BASE *arl = bases[k];
                arl_begin(arl);
                size_t w = 1;

                while(w + 1 < words) {
                    char *name  = procfile_lineword(ff, l, w); w++;
                    char *value = procfile_lineword(ff, l, w); w++;
                    // fprintf(stderr, " > NAME '%s', VALUE '%s', l=%zu, w=%zu, words=%zu\n", name, value, l, w, words);
                    if(unlikely(arl_check(arl, name, value) != 0))
                        break;
                }

                break;
            }
        }
    }

    // ------------------------------------------------------------------------

    if (do_sockets == CONFIG_BOOLEAN_YES || do_sockets == CONFIG_BOOLEAN_AUTO) {
        do_sockets = CONFIG_BOOLEAN_YES;

        static RRDSET *st = NULL;
        static RRDDIM *rd_used = NULL;

        if(unlikely(!st)) {
            st = rrdset_create_localhost(
                    "ip"
                    , "sockstat_sockets"
                    , NULL
                    , "sockets"
                    , NULL
                    , "Sockets used for all address families"
                    , "sockets"
                    , PLUGIN_PROC_NAME
                    , PLUGIN_PROC_MODULE_NET_SOCKSTAT_NAME
                    , NETDATA_CHART_PRIO_IP_SOCKETS
                    , update_every
                    , RRDSET_TYPE_LINE
            );

            rd_used = rrddim_add(st, "used", NULL,  1, 1, RRD_ALGORITHM_ABSOLUTE);
        }

        rrddim_set_by_pointer(st, rd_used, (collected_number)sockstat_root.sockets_used);
        rrdset_done(st);
    }

    // ------------------------------------------------------------------------

    if (do_tcp_sockets == CONFIG_BOOLEAN_YES || do_tcp_sockets == CONFIG_BOOLEAN_AUTO) {
        do_tcp_sockets = CONFIG_BOOLEAN_YES;

        static RRDSET *st = NULL;
        static RRDDIM *rd_inuse = NULL,
                      *rd_orphan = NULL,
                      *rd_timewait = NULL,
                      *rd_alloc = NULL;

        if(unlikely(!st)) {
            st = rrdset_create_localhost(
                    "ipv4"
                    , "sockstat_tcp_sockets"
                    , NULL
                    , "tcp"
                    , NULL
                    , "TCP Sockets"
                    , "sockets"
                    , PLUGIN_PROC_NAME
                    , PLUGIN_PROC_MODULE_NET_SOCKSTAT_NAME
                    , NETDATA_CHART_PRIO_IPV4_TCP_SOCKETS
                    , update_every
                    , RRDSET_TYPE_LINE
            );

            rd_alloc    = rrddim_add(st, "alloc",     NULL,   1, 1, RRD_ALGORITHM_ABSOLUTE);
            rd_orphan   = rrddim_add(st, "orphan",    NULL,   1, 1, RRD_ALGORITHM_ABSOLUTE);
            rd_inuse    = rrddim_add(st, "inuse",     NULL,   1, 1, RRD_ALGORITHM_ABSOLUTE);
            rd_timewait = rrddim_add(st, "timewait",  NULL,   1, 1, RRD_ALGORITHM_ABSOLUTE);
        }

        rrddim_set_by_pointer(st, rd_inuse,    (collected_number)sockstat_root.tcp_inuse);
        rrddim_set_by_pointer(st, rd_orphan,   (collected_number)sockstat_root.tcp_orphan);
        rrddim_set_by_pointer(st, rd_timewait, (collected_number)sockstat_root.tcp_tw);
        rrddim_set_by_pointer(st, rd_alloc,    (collected_number)sockstat_root.tcp_alloc);
        rrdset_done(st);
    }

    // ------------------------------------------------------------------------

    if (do_tcp_mem == CONFIG_BOOLEAN_YES || do_tcp_mem == CONFIG_BOOLEAN_AUTO) {
        do_tcp_mem = CONFIG_BOOLEAN_YES;

        static RRDSET *st = NULL;
        static RRDDIM *rd_mem = NULL;

        if(unlikely(!st)) {
            st = rrdset_create_localhost(
                    "ipv4"
                    , "sockstat_tcp_mem"
                    , NULL
                    , "tcp"
                    , NULL
                    , "TCP Sockets Memory"
                    , "KiB"
                    , PLUGIN_PROC_NAME
                    , PLUGIN_PROC_MODULE_NET_SOCKSTAT_NAME
                    , NETDATA_CHART_PRIO_IPV4_TCP_SOCKETS_MEM
                    , update_every
                    , RRDSET_TYPE_AREA
            );

            rd_mem = rrddim_add(st, "mem", NULL, sysconf(_SC_PAGESIZE), 1024, RRD_ALGORITHM_ABSOLUTE);
        }

        rrddim_set_by_pointer(st, rd_mem, (collected_number)sockstat_root.tcp_mem);
        rrdset_done(st);
    }

    // ------------------------------------------------------------------------

    if (do_udp_sockets == CONFIG_BOOLEAN_YES || do_udp_sockets == CONFIG_BOOLEAN_AUTO) {
        do_udp_sockets = CONFIG_BOOLEAN_YES;

        static RRDSET *st = NULL;
        static RRDDIM *rd_inuse = NULL;

        if(unlikely(!st)) {
            st = rrdset_create_localhost(
                    "ipv4"
                    , "sockstat_udp_sockets"
                    , NULL
                    , "udp"
                    , NULL
                    , "IPv4 UDP Sockets"
                    , "sockets"
                    , PLUGIN_PROC_NAME
                    , PLUGIN_PROC_MODULE_NET_SOCKSTAT_NAME
                    , NETDATA_CHART_PRIO_IPV4_UDP_SOCKETS
                    , update_every
                    , RRDSET_TYPE_LINE
            );

            rd_inuse    = rrddim_add(st, "inuse",     NULL,   1, 1, RRD_ALGORITHM_ABSOLUTE);
        }

        rrddim_set_by_pointer(st, rd_inuse,    (collected_number)sockstat_root.udp_inuse);
        rrdset_done(st);
    }

    // ------------------------------------------------------------------------

    if (do_udp_mem == CONFIG_BOOLEAN_YES || do_udp_mem == CONFIG_BOOLEAN_AUTO) {
        do_udp_mem = CONFIG_BOOLEAN_YES;

        static RRDSET *st = NULL;
        static RRDDIM *rd_mem = NULL;

        if(unlikely(!st)) {
            st = rrdset_create_localhost(
                    "ipv4"
                    , "sockstat_udp_mem"
                    , NULL
                    , "udp"
                    , NULL
                    , "IPv4 UDP Sockets Memory"
                    , "KiB"
                    , PLUGIN_PROC_NAME
                    , PLUGIN_PROC_MODULE_NET_SOCKSTAT_NAME
                    , NETDATA_CHART_PRIO_IPV4_UDP_SOCKETS_MEM
                    , update_every
                    , RRDSET_TYPE_AREA
            );

            rd_mem = rrddim_add(st, "mem", NULL, sysconf(_SC_PAGESIZE), 1024, RRD_ALGORITHM_ABSOLUTE);
        }

        rrddim_set_by_pointer(st, rd_mem, (collected_number)sockstat_root.udp_mem);
        rrdset_done(st);
    }

    // ------------------------------------------------------------------------

    if (do_udplite_sockets == CONFIG_BOOLEAN_YES || do_udplite_sockets == CONFIG_BOOLEAN_AUTO) {
        do_udplite_sockets = CONFIG_BOOLEAN_YES;

        static RRDSET *st = NULL;
        static RRDDIM *rd_inuse = NULL;

        if(unlikely(!st)) {
            st = rrdset_create_localhost(
                    "ipv4"
                    , "sockstat_udplite_sockets"
                    , NULL
                    , "udplite"
                    , NULL
                    , "IPv4 UDPLITE Sockets"
                    , "sockets"
                    , PLUGIN_PROC_NAME
                    , PLUGIN_PROC_MODULE_NET_SOCKSTAT_NAME
                    , NETDATA_CHART_PRIO_IPV4_UDPLITE_SOCKETS
                    , update_every
                    , RRDSET_TYPE_LINE
            );

            rd_inuse    = rrddim_add(st, "inuse",     NULL,   1, 1, RRD_ALGORITHM_ABSOLUTE);
        }

        rrddim_set_by_pointer(st, rd_inuse,    (collected_number)sockstat_root.udplite_inuse);
        rrdset_done(st);
    }

    // ------------------------------------------------------------------------

    if (do_raw_sockets == CONFIG_BOOLEAN_YES || do_raw_sockets == CONFIG_BOOLEAN_AUTO) {
        do_raw_sockets = CONFIG_BOOLEAN_YES;

        static RRDSET *st = NULL;
        static RRDDIM *rd_inuse = NULL;

        if(unlikely(!st)) {
            st = rrdset_create_localhost(
                    "ipv4"
                    , "sockstat_raw_sockets"
                    , NULL
                    , "raw"
                    , NULL
                    , "IPv4 RAW Sockets"
                    , "sockets"
                    , PLUGIN_PROC_NAME
                    , PLUGIN_PROC_MODULE_NET_SOCKSTAT_NAME
                    , NETDATA_CHART_PRIO_IPV4_RAW
                    , update_every
                    , RRDSET_TYPE_LINE
            );

            rd_inuse    = rrddim_add(st, "inuse",     NULL,   1, 1, RRD_ALGORITHM_ABSOLUTE);
        }

        rrddim_set_by_pointer(st, rd_inuse,    (collected_number)sockstat_root.raw_inuse);
        rrdset_done(st);
    }

    // ------------------------------------------------------------------------

    if (do_frag_sockets == CONFIG_BOOLEAN_YES || do_frag_sockets == CONFIG_BOOLEAN_AUTO) {
        do_frag_sockets = CONFIG_BOOLEAN_YES;

        static RRDSET *st = NULL;
        static RRDDIM *rd_inuse = NULL;

        if(unlikely(!st)) {
            st = rrdset_create_localhost(
                    "ipv4"
                    , "sockstat_frag_sockets"
                    , NULL
                    , "fragments"
                    , NULL
                    , "IPv4 FRAG Sockets"
                    , "fragments"
                    , PLUGIN_PROC_NAME
                    , PLUGIN_PROC_MODULE_NET_SOCKSTAT_NAME
                    , NETDATA_CHART_PRIO_IPV4_FRAGMENTS_SOCKETS
                    , update_every
                    , RRDSET_TYPE_LINE
            );

            rd_inuse    = rrddim_add(st, "inuse",     NULL,   1, 1, RRD_ALGORITHM_ABSOLUTE);
        }

        rrddim_set_by_pointer(st, rd_inuse,    (collected_number)sockstat_root.frag_inuse);
        rrdset_done(st);
    }

    // ------------------------------------------------------------------------

    if (do_frag_mem == CONFIG_BOOLEAN_YES || do_frag_mem == CONFIG_BOOLEAN_AUTO) {
        do_frag_mem = CONFIG_BOOLEAN_YES;

        static RRDSET *st = NULL;
        static RRDDIM *rd_mem = NULL;

        if(unlikely(!st)) {
            st = rrdset_create_localhost(
                    "ipv4"
                    , "sockstat_frag_mem"
                    , NULL
                    , "fragments"
                    , NULL
                    , "IPv4 FRAG Sockets Memory"
                    , "KiB"
                    , PLUGIN_PROC_NAME
                    , PLUGIN_PROC_MODULE_NET_SOCKSTAT_NAME
                    , NETDATA_CHART_PRIO_IPV4_FRAGMENTS_SOCKETS_MEM
                    , update_every
                    , RRDSET_TYPE_AREA
            );

            rd_mem = rrddim_add(st, "mem", NULL, 1, 1024, RRD_ALGORITHM_ABSOLUTE);
        }

        rrddim_set_by_pointer(st, rd_mem, (collected_number)sockstat_root.frag_memory);
        rrdset_done(st);
    }

    return 0;
}

