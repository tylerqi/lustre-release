/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2001 Cluster File Systems, Inc. <braam@clusterfs.com>
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * (Un)packing of MDS and OST request records
 *
 */

#include <linux/module.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/sched.h>
#include <linux/lp.h>
#include <linux/slab.h>
#include <linux/ioport.h>
#include <linux/fcntl.h>
#include <linux/delay.h>
#include <linux/skbuff.h>
#include <linux/proc_fs.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <linux/list.h>
#include <asm/io.h>
#include <asm/segment.h>
#include <asm/system.h>
#include <asm/poll.h>
#include <asm/uaccess.h>

#include <linux/obd_support.h>
#include <linux/lustre_lib.h>
#include <linux/lustre_idl.h>
#include <linux/lustre_mds.h>


int mds_pack_req(char *name, int namelen, char *tgt, int tgtlen, 
		 struct mds_req_hdr **hdr, struct mds_req **req, 
		 int *len, char **buf)
{
	char *ptr;
        struct mds_req_packed *preq;

	*len = sizeof(**hdr) + size_round(namelen) + size_round(tgtlen) + 
		sizeof(*preq); 

	*buf = kmalloc(*len, GFP_KERNEL);
	if (!*buf) {
		EXIT;
		return -ENOMEM;
	}

	memset(*buf, 0, *len); 
	*hdr = (struct mds_req_hdr *)(*buf);

	preq = (struct mds_req_packed *)(*buf + sizeof(**hdr));
	ptr = *buf + sizeof(**hdr) + sizeof(*preq);

	(*hdr)->type =  MDS_TYPE_REQ;

	(*req)->namelen = NTOH__u32(namelen);
	if (name) { 
                preq->name_offset = (__u32)(ptr - (char *)preq);
		LOGL(name, namelen, ptr); 
	} 

	(*req)->tgtlen = NTOH__u32(tgtlen);
	if (tgt) { 
                preq->tgt_offset = (__u32)(ptr - (char *)preq);
		LOGL(tgt, tgtlen, ptr);
	}
	return 0;
}


int mds_unpack_req(char *buf, int len, 
		   struct mds_req_hdr **hdr, struct mds_req **req)
{
        struct mds_req_packed *preq;
        __u32 off1, off2;
        char *name, *tgt;

	if (len < sizeof(**hdr) + sizeof(**req)) { 
		EXIT;
		return -EINVAL;
	}

	*hdr = (struct mds_req_hdr *) (buf);
	preq = (struct mds_req_packed *) (buf + sizeof(**hdr));
        off1 = preq->name_offset;
        off2 = preq->tgt_offset;

        *req = (struct mds_req *) (buf + sizeof(**hdr));
	(*req)->namelen = NTOH__u32((*req)->namelen); 
	(*req)->tgtlen = NTOH__u32((*req)->namelen); 

	if (len < sizeof(**hdr) + sizeof(**req) + (*req)->namelen + 
	    (*req)->tgtlen ) { 
		EXIT;
		return -EINVAL;
	}

	if ((*req)->namelen) { 
		name = buf + sizeof(**hdr) + off1;
	} else { 
		name = NULL;
	}

	if ((*req)->tgtlen) { 
		tgt = buf + sizeof(**hdr) + off2;
	} else { 
		tgt = NULL;
	}
        (*req)->name = name;
        (*req)->tgt = tgt;

	EXIT;
	return 0;
}

int mds_pack_rep(char *name, int namelen, char *tgt, int tgtlen, 
		 struct mds_rep_hdr **hdr, struct mds_rep **rep, 
		 int *len, char **buf)
{
	char *ptr;
        struct mds_rep_packed *prep;

	*len = sizeof(**hdr) + size_round(namelen) + size_round(tgtlen) + 
		sizeof(*prep); 

	*buf = kmalloc(*len, GFP_KERNEL);
	if (!*buf) {
		EXIT;
		return -ENOMEM;
	}

	memset(*buf, 0, *len); 
	*hdr = (struct mds_rep_hdr *)(*buf);

	prep = (struct mds_rep_packed *)(*buf + sizeof(**hdr));
	ptr = *buf + sizeof(**hdr) + sizeof(*prep);

	(*hdr)->type =  MDS_TYPE_REP;

	(*rep)->namelen = NTOH__u32(namelen);
	if (name) { 
                prep->name_offset = (__u32)(ptr - (char *)prep);
		LOGL(name, namelen, ptr); 
	} 

	(*rep)->tgtlen = NTOH__u32(tgtlen);
	if (tgt) { 
                prep->tgt_offset = (__u32)(ptr - (char *)prep);
		LOGL(tgt, tgtlen, ptr);
	}
	return 0;
}


int mds_unpack_rep(char *buf, int len, 
		   struct mds_rep_hdr **hdr, struct mds_rep **rep)
{
        struct mds_rep_packed *prep;
        __u32 off1, off2;

	if (len < sizeof(**hdr) + sizeof(**rep)) { 
		EXIT;
		return -EINVAL;
	}

	*hdr = (struct mds_rep_hdr *) (buf);
	prep = (struct mds_rep_packed *) (buf + sizeof(**hdr));
        off1 = prep->name_offset;
        off2 = prep->tgt_offset;

        *rep = (struct mds_rep *) (buf + sizeof(**hdr));
	(*rep)->namelen = NTOH__u32((*rep)->namelen); 
	(*rep)->tgtlen = NTOH__u32((*rep)->namelen); 

	if (len < sizeof(**hdr) + sizeof(**rep) + (*rep)->namelen + 
	    (*rep)->tgtlen ) { 
		EXIT;
		return -EINVAL;
	}

	if ((*rep)->namelen) { 
		(*rep)->name = buf + sizeof(**hdr) + off1;
	} else { 
		(*rep)->name = NULL;
	}

	if ((*rep)->tgtlen) { 
		(*rep)->tgt = buf + sizeof(**hdr) + off2;
	} else { 
		(*rep)->tgt = NULL;
	}
        

	EXIT;
	return 0;
}

#if 0
int mds_pack_rep(char *name, int namelen, char *tgt, int tgtlen, 
		 struct mds_rep_hdr **hdr, struct mds_rep **rep, 
		 int *len, char **buf)
{
	char *ptr;

	*len = sizeof(**hdr) + size_round(namelen) + size_round(tgtlen) + 
		sizeof(**rep); 

	*buf = kmalloc(*len, GFP_KERNEL);
	if (!*buf) {
		EXIT;
		return -ENOMEM;
	}

	memset(*buf, 0, *len); 
	*hdr = (struct mds_rep_hdr *)(*buf);
	*rep = (struct mds_rep *)(*buf + sizeof(**hdr));
	ptr = *buf + sizeof(**hdr) + sizeof(**rep);

	(*rep)->namelen = NTOH__u32(namelen);
	if (name) { 
		LOGL(name, namelen, ptr); 
	} 

	(*rep)->tgtlen = NTOH__u32(tgtlen);
	if (tgt) { 
		LOGL(tgt, tgtlen, ptr);
	}
	return 0;
}


int mds_unpack_rep(char *buf, int len, 
		   struct mds_rep_hdr **hdr, struct mds_rep **rep)
{
	if (len < sizeof(**hdr) + sizeof(**rep)) { 
		EXIT;
		return -EINVAL;
	}

	*hdr = (struct mds_rep_hdr *) (buf);
	*rep = (struct mds_rep *) (buf + sizeof(**hdr));
	(*rep)->namelen = NTOH__u32((*rep)->namelen); 
	(*rep)->tgtlen = NTOH__u32((*rep)->namelen); 

	if (len < sizeof(**hdr) + sizeof(**rep) + (*rep)->namelen + 
	    (*rep)->tgtlen ) { 
		EXIT;
		return -EINVAL;
	}

	if ((*rep)->namelen) { 
		(*rep)->name = buf + sizeof(**hdr) + sizeof(**rep);
	} else { 
		(*rep)->name = NULL;
	}

	if ((*rep)->tgtlen) { 
		(*rep)->tgt = buf + sizeof(**hdr) + sizeof(**rep) + 
			size_round((*rep)->namelen);
	} else { 
		(*rep)->tgt = NULL;
	}

	EXIT;
	return 0;
}
#endif
