/*
 * QEMU libpcap network client
 *
 * Copyright (C) 2021 Matt Borgerson
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

#ifndef NET_PCAP_H
#define NET_PCAP_H

#ifdef XBOX
typedef struct pcap_if {
    struct pcap_if *next;
    char *name;
    char *description;
} pcap_if_t;
#else
#include <pcap/pcap.h>
#endif

#if defined(_WIN32) && !defined(XBOX)
#include "net/capture_win_ifnames.h"
#endif

#endif
