/*
 * NC-SI (Network Controller Sideband Interface) "echo" model
 *
 * Copyright (C) 2016 IBM Corp.
 *
 * This code is licensed under the GPL version 2 or later. See the
 * COPYING file in the top-level directory.
 */
#include "qemu/osdep.h"
#include <slirp.h>

#include "ncsi-pkt.h"

/*
 * ncsi header + checksum + max payload (NCSI_PKT_CMD_GVI)
 */
#define NCSI_LEN (sizeof(struct ncsi_pkt_hdr) + 4 + 36)

void ncsi_input(Slirp *slirp, const uint8_t *pkt, int pkt_len)
{
    struct ncsi_pkt_hdr *nh = (struct ncsi_pkt_hdr *)(pkt + ETH_HLEN);
    uint8_t ncsi_reply[ETH_HLEN + NCSI_LEN];
    struct ethhdr *reh = (struct ethhdr *)ncsi_reply;
    struct ncsi_rsp_pkt_hdr *rnh = (struct ncsi_rsp_pkt_hdr *)
        (ncsi_reply + ETH_HLEN);

    memset(ncsi_reply, 0, sizeof(ncsi_reply));

    memset(reh->h_dest, 0xff, ETH_ALEN);
    memset(reh->h_source, 0xff, ETH_ALEN);
    reh->h_proto = htons(ETH_P_NCSI);

    rnh->common.mc_id        = nh->mc_id;
    rnh->common.revision     = NCSI_PKT_REVISION;
    rnh->common.reserved     = 0;
    rnh->common.id           = nh->id;
    rnh->common.type         = nh->type + 0x80;
    rnh->common.channel      = nh->channel;
    rnh->common.length       = htons(4);
    rnh->common.reserved1[0] = 0;
    rnh->common.reserved1[1] = 0;
    rnh->code         = htons(NCSI_PKT_RSP_C_COMPLETED);
    rnh->reason       = htons(NCSI_PKT_RSP_R_NO_ERROR);

    switch (nh->type) {
    case NCSI_PKT_CMD_SMA:
        rnh->common.length = htons(4);
        break;
    case NCSI_PKT_CMD_GVI:
        rnh->common.length = htons(36);
        break;
    case NCSI_PKT_CMD_GC: {
        struct ncsi_rsp_gc_pkt *rsp = (struct ncsi_rsp_gc_pkt *) rnh;

        rnh->common.length = htons(32);
        rsp->cap = htonl(~0);
        rsp->bc_cap = htonl(~0);
        rsp->mc_cap = htonl(~0);
        rsp->buf_cap = htonl(~0);
        rsp->aen_cap = htonl(~0);
        rsp->vlan_mode = 0xff;
        rsp->uc_cnt = 2;
        break;
    }

    case NCSI_PKT_CMD_GLS: {
        struct ncsi_rsp_gls_pkt *rsp = (struct ncsi_rsp_gls_pkt *) rnh;

        rnh->common.length = htons(16);
        rsp->status = htonl(0x1);
        break;
    }
    default:
        break;
    }

    slirp_output(slirp->opaque, ncsi_reply, sizeof(ncsi_reply));
}
