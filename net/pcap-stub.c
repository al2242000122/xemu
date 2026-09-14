/*
 * PCAP backend stub for sandboxed UWP builds.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "net/clients.h"
#include "qapi/error.h"

int net_init_pcap(const Netdev *netdev, const char *name,
                  NetClientState *peer, Error **errp)
{
    error_setg(errp, "pcap networking is unavailable in UWP");
    return -1;
}
