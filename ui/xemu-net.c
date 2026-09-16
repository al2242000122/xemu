/*
 * xemu Network Management
 *
 * Wrapper functions to configure network settings at runtime.
 *
 * Copyright (C) 2020-2021 Matt Borgerson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "xemu-net.h"
#include "xemu-settings.h"

#include "qemu/osdep.h"
#include "qapi/qapi-commands-net.h"
#include "qemu/sockets.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "monitor/qdev.h"
#include "qobject/qdict.h"
#include "qemu/option.h"
#include "qemu/config-file.h"
#include "net/net.h"
#include "net/hub.h"
#ifdef CONFIG_SLIRP
#include "net/slirp.h"
#include <libslirp.h>
#endif
#if defined(_WIN32) && !defined(XBOX) && !defined(CONFIG_UWP)
#include <pcap/pcap.h>
#endif
#include "xemu-notifications.h"

static const char *id = "xemu-netdev";
static const char *id_hubport = "xemu-netdev-hubport";
static const char *id_vlan = "xemu-vlan-netdev";
static const char *id_vlan_hubport = "xemu-vlan-netdev-hubport";

static bool xemu_net_uses_slirp(void)
{
    return g_config.net.backend == CONFIG_NET_BACKEND_NAT ||
           (g_config.net.backend == CONFIG_NET_BACKEND_VLAN &&
            g_config.net.vlan.host);
}

static bool add_netdev_from_qdict(QDict *qdict, Error **errp)
{
    QemuOpts *opts = qemu_opts_from_qdict(qemu_find_opts("netdev"), qdict,
                                          errp);
    qobject_unref(qdict);
    if (!opts) {
        return false;
    }
    netdev_add(opts, errp);
    if (*errp) {
        qemu_opts_del(opts);
        return false;
    }
    return true;
}

static bool add_hubport(const char *hubport_id, const char *netdev_id,
                        Error **errp)
{
    QDict *qdict = qdict_new();
    qdict_put_str(qdict, "id", hubport_id);
    qdict_put_str(qdict, "type", "hubport");
    qdict_put_int(qdict, "hubid", 0);
    qdict_put_str(qdict, "netdev", netdev_id);
    return add_netdev_from_qdict(qdict, errp);
}

void xemu_net_enable(void)
{
    Error *local_err = NULL;

    NetClientState *nc = qemu_find_netdev(id);
    if (nc != NULL) {
        return;
    }

    // Create the netdev
    QDict *qdict;
    if (xemu_net_uses_slirp()) {
#ifndef CONFIG_SLIRP
        xemu_queue_error_message("NAT networking is unavailable in this build");
        return;
#else
        qdict = qdict_new();
        qdict_put_str(qdict, "id",   id);
        qdict_put_str(qdict, "type", "user");
#endif
    } else if (g_config.net.backend == CONFIG_NET_BACKEND_UDP) {
        qdict = qdict_new();
        qdict_put_str(qdict, "id",        id);
        qdict_put_str(qdict, "type",      "socket");
        qdict_put_str(qdict, "udp",       g_config.net.udp.remote_addr);
        qdict_put_str(qdict, "localaddr", g_config.net.udp.bind_addr);
    } else if (g_config.net.backend == CONFIG_NET_BACKEND_PCAP) {
#if defined(XBOX) || defined(CONFIG_UWP)
        xemu_queue_error_message("PCAP networking is unavailable in UWP");
        return;
#elif defined(_WIN32)
        if (pcap_load_library()) {
            return;
        }
#endif
        qdict = qdict_new();
        qdict_put_str(qdict, "id",        id);
        qdict_put_str(qdict, "type",      "pcap");
        qdict_put_str(qdict, "ifname",    g_config.net.pcap.netif);
    } else if (g_config.net.backend == CONFIG_NET_BACKEND_VLAN) {
        qdict = qdict_new();
        qdict_put_str(qdict, "id", id);
        qdict_put_str(qdict, "type", "socket");
        qdict_put_str(qdict, "udp", g_config.net.vlan.remote_addr);
        qdict_put_str(qdict, "localaddr", g_config.net.vlan.bind_addr);
    } else {
        // Unsupported backend type
        return;
    }

    if (!add_netdev_from_qdict(qdict, &local_err)) {
        xemu_queue_error_message(error_get_pretty(local_err));
        error_report_err(local_err);
        return;
    }

    if (g_config.net.backend == CONFIG_NET_BACKEND_VLAN &&
        g_config.net.vlan.host) {
        qdict = qdict_new();
        qdict_put_str(qdict, "id", id_vlan);
        qdict_put_str(qdict, "type", "socket");
        qdict_put_str(qdict, "udp", g_config.net.vlan.remote_addr);
        qdict_put_str(qdict, "localaddr", g_config.net.vlan.bind_addr);
        if (!add_netdev_from_qdict(qdict, &local_err)) {
            xemu_queue_error_message(error_get_pretty(local_err));
            error_report_err(local_err);
            xemu_net_disable();
            return;
        }
    }

    // Create the hubport
    if (!add_hubport(id_hubport, id, &local_err)) {
        xemu_queue_error_message(error_get_pretty(local_err));
        error_report_err(local_err);
        xemu_net_disable();
        return;
    }

    if (g_config.net.backend == CONFIG_NET_BACKEND_VLAN &&
        g_config.net.vlan.host &&
        !add_hubport(id_vlan_hubport, id_vlan, &local_err)) {
        xemu_queue_error_message(error_get_pretty(local_err));
        error_report_err(local_err);
        xemu_net_disable();
        return;
    }

    if (xemu_net_uses_slirp()) {
#ifdef CONFIG_SLIRP
        void *s = slirp_get_state_from_netdev(id);
        if (!s) {
            xemu_queue_error_message("NAT backend did not initialize");
            xemu_net_disable();
            return;
        }

        struct in_addr host_addr = { .s_addr = INADDR_ANY };
        struct in_addr guest_addr = { .s_addr = 0 };
        inet_aton("10.0.2.15", &guest_addr);

        for (int i = 0; i < g_config.net.nat.forward_ports_count; i++) {
            bool is_udp = g_config.net.nat.forward_ports[i].protocol ==
                          CONFIG_NET_NAT_FORWARD_PORTS_PROTOCOL_UDP;
            int host_port = g_config.net.nat.forward_ports[i].host;
            int guest_port = g_config.net.nat.forward_ports[i].guest;

            if (slirp_add_hostfwd(s, is_udp, host_addr, host_port, guest_addr,
                                  guest_port) < 0) {
                error_setg(&local_err,
                           "Could not set host forwarding rule "
                           "%d -> %d (%s)\n",
                           host_port, guest_port, is_udp ? "udp" : "tcp");
                xemu_queue_error_message(error_get_pretty(local_err));
                break;
            }

        }
#endif
    }

    if (local_err) {
        xemu_net_disable();
    }

    qmp_set_link("nvnet.0", true, NULL);
    g_config.net.enable = true;
}

static void remove_netdev(const char *name)
{
    NetClientState *nc;
    QemuOpts *opts;

    nc = qemu_find_netdev(name);
    if (!nc) {
        // error_set(errp, ERROR_CLASS_DEVICE_NOT_FOUND,
        //           "Device '%s' not found", name);
        return;
    }

    opts = qemu_opts_find(qemu_find_opts_err("netdev", NULL), name);
    if (!opts) {
        // error_setg(errp, "Device '%s' is not a netdev", name);
        return;
    }
    qemu_opts_del(opts);
    qemu_del_net_client(nc);
}

static void clear_slirp_port_forwards(void)
{
#ifdef CONFIG_SLIRP
    void *s = slirp_get_state_from_netdev(id);
    if (!s) {
        return;
    }

    struct in_addr host_addr = { .s_addr = INADDR_ANY };
    for (int i = 0; i < g_config.net.nat.forward_ports_count; i++) {
        slirp_remove_hostfwd(s,
                             g_config.net.nat.forward_ports[i].protocol ==
                                 CONFIG_NET_NAT_FORWARD_PORTS_PROTOCOL_UDP,
                             host_addr,
                             g_config.net.nat.forward_ports[i].host);
    }
#endif
}

void xemu_net_disable(void)
{
    if (xemu_net_uses_slirp()) {
        clear_slirp_port_forwards();
    }

    remove_netdev(id);
    remove_netdev(id_hubport);
    remove_netdev(id_vlan);
    remove_netdev(id_vlan_hubport);

    qmp_set_link("nvnet.0", false, NULL);
    g_config.net.enable = false;
}

int xemu_net_is_enabled(void)
{
    NetClientState *nc;
    nc = qemu_find_netdev(id);
    g_config.net.enable = (nc != NULL);
    return g_config.net.enable;
}
