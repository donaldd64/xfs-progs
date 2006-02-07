/*
 * Copyright (c) 2000-2004 Silicon Graphics, Inc.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <xfs/libxfs.h>
#include <attr/attributes.h>
#include <xfs/handle.h>
#include <xfs/dmapi.h>

#include "hsmapi.h"

/* This version of the HSM API supports the DMF attribute used in the initial
 * DMF release, as well as the attribute used in the pseudo multiple managed
 * region DMF release.
*/

/* DMF attribute name, size, and format as stored within XFS. (Stolen directly
   from "dmfsapi/dmf_dmattr.H".
*/

#define	DMF_ATTR_NAME	"SGI_DMI_DMFATTR"	/* name of DMF's attr */

typedef	struct {
	u_char	fsys;		/* filesystem type */
	u_char	version;	/* attribute format version */
	u_char	state[2];	/* dm_state in MSB form */
	u_char	flags[2];	/* dm_flags in MSB form */
	u_char	bfid[16];	/* Bitfile ID in MSB form */
} XFSattrvalue0_t;

typedef	struct {
	u_char	rg_offset[8];	/* region offset in MSB form */
	u_char	rg_size[8];	/* region length in MSB form */
	u_char	rg_state[2];	/* region dm_state in MSB form */
	u_char	rg_flags;	/* managed region event bits */
	u_char	rg_fbits;	/* region flag bits */
} XFSattrregion_t;

typedef	struct {
	u_char	fsys;		/* filesystem type */
	u_char	version;	/* attribute format version */
	u_char	state[2];	/* global dm_state in MSB form */
	u_char	flags[2];	/* global dm_flags in MSB form */
	u_char	bfid[16];	/* Bitfile ID in MSB form. */
	u_char	sitetag[4];	/* site tag */
	u_char	regcnt[2];	/* number of regions in MSB form */
} XFSattrvalue1_t;

#define	MIN_FORMAT1_ATTR_LEN	( sizeof(XFSattrvalue1_t) + \
				  sizeof(XFSattrregion_t) )

/* supported fsys values */

/* XFS DMAPI (w/o MMR ) */
#define	FSYS_TYPE_XFS		1

/* supported version values */

/* original DMF attr format */
#define	DMF_ATTR_FORMAT_0	0
/* DMF attr with multiple regions (real or pseudo) or with a non-zero
 * site tag. attrs of this format consist of a XFSattrvalue1_t struct
 * followed by 1 or more XFSattrregion_t structs */
#define	DMF_ATTR_FORMAT_1	1

/* Interesting state field values */

#define	DMF_ST_DUALSTATE	2	/* file has backups plus online data */
#define	DMF_ST_OFFLINE		3	/* file has backups, no online data */
#define	DMF_ST_UNMIGRATING	4	/* file data is being staged in */
#define	DMF_ST_PARTIAL		6	/* file has backups plus parts online */

/* Interesting bit combinations within the bs_dmevmask field of xfs_bstat_t:
 * OFL, UNM, and PAR files have exactly these bits set.
 * DUL and MIG files have all but the DM_EVENT_READ bit set */
#define DMF_EV_BITS	( (1<<DM_EVENT_DESTROY) | \
			  (1<<DM_EVENT_READ)    | \
			  (1<<DM_EVENT_WRITE)   | \
			  (1<<DM_EVENT_TRUNCATE) )

/* OFL file's managed region event flags */
#define DMF_MR_FLAGS	( 0x1 | 0x2 | 0x4 )

/* The following definitions provide the internal format of the hsm_fs_ctxt_t
   and hsm_f_ctxt_t structures, respectively.
*/

typedef	struct	{
	int		dumpversion;
	dm_fsid_t	fsid;
} dmf_fs_ctxt_t;

typedef	struct	{
	dmf_fs_ctxt_t	fsys;
	off64_t		filesize;
	int		candidate;
	int		attrlen;
	char		attrval[5000];	/* sized bigger than any poss. value */
} dmf_f_ctxt_t;

/******************************************************************************
* Name
*       msb_store - store a variable to u_char array in MSB format
*
* Returns
*	void
******************************************************************************/
static inline void
msb_store(
	u_char		*dest,
	u_int64_t	src,
	int		length)
{
        int             i;

        for (i = length - 1; i >= 0; i--) {
                dest[i] = (u_char)(src & 0xff);
                src >>= 8;
        }
}

/******************************************************************************
* Name
*       msb_load - load a variable from a u_char array in MSB format
*
* Returns
*	value
******************************************************************************/
static inline u_int64_t
msb_load(
	u_char		*src,
	int		length)
{
        u_int64_t        tmp = 0;
        int             i;

        for (i = 0; i < length; i++) {
                tmp = (tmp << 8) | src[i];
        }
        return tmp;
}

/******************************************************************************
* Name
*	HsmInitFsysContext - allocate and initialize an HSM filesystem context
*
* Description
*	HsmInitFsysContext allocates and initializes an HSM filesystem
*	context to hold all filesystem information that might later be needed
*	by other HSM routines.  The context is read-only, and can be shared
*	by multiple xfsdump dump streams.  It should eventually be freed by
*	calling HsmDeleteFsysContext().  The caller must provide the mount
*	point of the filesystem to be dumped and the HSM API version that
*	xfsdump was compiled with.
*
* Returns
*	!= NULL, then a pointer to the filesystem context that was allocated.
*	== NULL, either the HSM libary is not compatible with xfsdump, or
*	      the filesystem is not under HSM management.
******************************************************************************/

extern hsm_fs_ctxt_t *
HsmInitFsysContext(
const	char		*mountpoint,
	int		dumpversion)
{
	dmf_fs_ctxt_t	*dmf_fs_ctxtp;
	void		*fshanp;
	size_t		fshlen = 0;
	dm_fsid_t	fsid;
	int		error;

	if (dumpversion != HSM_API_VERSION_1) {
		return NULL;		/* we can't handle this version */
	}

	/* Get the filesystem's DMAPI fsid for later use in building file
	   handles in HsmInitFileContext.  We use path_to_handle() because
	   dm_path_to_handle() doesn't work if the filesystem isn't mounted
	   with -o dmi.
	*/

	if (path_to_fshandle((char *)mountpoint, &fshanp, &fshlen)) {
		return NULL;
	}
	error = dm_handle_to_fsid(fshanp, fshlen, &fsid);
	free_handle(fshanp, fshlen);
	if (error) {
		return NULL;
	}

	/* Malloc space for a filesystem context, and initialize any fields
	   needed later by other routines.
	*/

	if ((dmf_fs_ctxtp = malloc(sizeof(dmf_fs_ctxt_t))) == NULL) {
		return NULL;
	}
	dmf_fs_ctxtp->dumpversion = dumpversion;
	dmf_fs_ctxtp->fsid = fsid;

	return (hsm_fs_ctxt_t *)dmf_fs_ctxtp;
}


/******************************************************************************
* Name
*	HsmDeleteFsysContext - delete an HSM filesystem context
*
* Description
*	HsmDeleteFsysContext releases all storage previously allocated to a
*	HSM filesystem context via HsmInitFsysContext.
*
* Returns
*	None.
******************************************************************************/

extern void
HsmDeleteFsysContext(
	hsm_fs_ctxt_t	*contextp)
{
	free(contextp);
}


/******************************************************************************
* Name
*	HsmEstimateFileSpace - return estimated offline file size
*
* Description
*	HsmEstimateFileSpace is called from within estimate_dump_space() only
*	if -a is selected.  It estimates the number of bytes needed to dump
*	the file assuming that all dual-residency data will be dumped as holes.
*
* Returns
*	!= 0, then *bytes contains the estimated size of the file in bytes.
*	== 0, then no estimate made.  Caller should use his default estimator.
******************************************************************************/

extern int
HsmEstimateFileSpace(
	hsm_fs_ctxt_t	*contextp,
const	xfs_bstat_t	*statp,
	off64_t		*bytes)
{
	/* This code is assuming that there are no MIG files, and so any
	   file with DMAPI event bits set will be dumped as OFL.
	   It is too expensive to actually differentiate MIG files from
	   other types just for the sake of an estimate. The non-dir dump
	   size estimation will be somewhat low if there are MIG files,
	   which could affect the distribution of a multi-stream dump.
	*/
	if ((statp->bs_mode & S_IFMT) != S_IFREG) {
		return 0;       /* not a regular file */
	}
	if ((statp->bs_xflags & XFS_XFLAG_HASATTR) == 0) {
		return 0;       /* no DMF attribute can possibly exist */
	}
	if ((statp->bs_dmevmask & DMF_EV_BITS) == 0) {
		return 0;
	}

	*bytes = 0;
	return 1;
}


/******************************************************************************
* Name
*	HsmEstimateFileOffset - return estimated file offset
*
* Description
*	HsmEstimateFileOffset is called from within quantity2offset() only
*	if -a is selected.  It estimates the offset within the file that has
*	'bytecount' bytes of physical data preceding it assuming that all
*	dual-residency data in the file will be dumped as holes.
*
* Returns
*	!= 0, then *byteoffset contains the estimated offset within the file.
*	== 0, then no estimate made.  Caller should use his default estimator.
******************************************************************************/

/* ARGSUSED */
extern int
HsmEstimateFileOffset(
	hsm_fs_ctxt_t	*contextp,
const	xfs_bstat_t	*statp,
	off64_t		bytecount,
	off64_t		*byteoffset)
{
	dmf_fs_ctxt_t	*dmf_fs_ctxtp = (dmf_fs_ctxt_t *)contextp;
	dmf_f_ctxt_t	dmf_f_ctxt;

	/* This is an implicit HsmAllocateFileContext call. */

	dmf_f_ctxt.fsys = *dmf_fs_ctxtp;
	dmf_f_ctxt.candidate = 0;

	/* Initialize the file context to determine the file's state. */

	if (HsmInitFileContext((hsm_f_ctxt_t *)&dmf_f_ctxt, statp)) {
		return 0;
	}

	/* If the file is dualstate, make it appear offline. */

	if (dmf_f_ctxt.candidate) {
		*byteoffset = statp->bs_size;
		return 1;
	} else {
		return 0;
	}
}


/******************************************************************************
* Name
*	HsmAllocateFileContext - allocate an HSM file context
*
* Description
*	HsmAllocateFileContext mallocs the maximum-sized file context that
*	might later needed by HsmInitFileContext().  The context is
*	read-write.  Each xfsdump stream must have its own file context.  This
*	context should eventually be freed by calling HsmDeleteFileContext().
*	The caller must provide the HSM filesystem context for the filesystem
*	being dumped.
*
* Returns
*	!= NULL, then a pointer to the file context that was allocated.
******************************************************************************/

extern hsm_f_ctxt_t *
HsmAllocateFileContext(
	hsm_fs_ctxt_t	*contextp)
{
	dmf_f_ctxt_t	*dmf_f_ctxtp;

	if ((dmf_f_ctxtp = malloc(sizeof(dmf_f_ctxt_t))) == NULL) {
		return NULL;
	}

	/* Save the filesystem information in the file context. */

	dmf_f_ctxtp->fsys = *(dmf_fs_ctxt_t *)contextp;
	dmf_f_ctxtp->candidate = 0;

	return (hsm_f_ctxt_t *)dmf_f_ctxtp;
}


/******************************************************************************
* Name
*	HsmDeleteFileContext - delete a previously created HSM file context
*
* Description
*	HsmDeleteFileContext releases all storage previously allocated to a
*	HSM file context via HsmAllocateFileContext.
*
* Returns
*	None.
******************************************************************************/

extern void
HsmDeleteFileContext(
	hsm_f_ctxt_t	*contextp)
{
	free(contextp);
}


/******************************************************************************
* Name
*	HsmInitFileContext - initialize the HSM context for a particular file
*
* Description
*	HsmInitFileContext initializes an existing HSM file context for
*	subsequent operations on a particular regular file.  Other HSM routines
*	use the context to access information collected by HsmInitFileContext
*	about the file rather than having to recollect the file's information
*	on each call.
*
* Returns
*	== 0, context was created.
*	!= 0, if something is wrong with the file and it should not be dumped.
******************************************************************************/

extern int
HsmInitFileContext(
	hsm_f_ctxt_t	*contextp,
const	xfs_bstat_t	*statp)
{
	dmf_f_ctxt_t	*dmf_f_ctxtp = (dmf_f_ctxt_t *)contextp;
	XFSattrvalue0_t	*dmfattrp;
	void		*hanp;
	size_t		hlen = 0;
	dm_ino_t	ino;
	dm_igen_t	igen;
	int		state;
	int		error;
	attr_multiop_t	attr_op;

	dmf_f_ctxtp->candidate = 0; /* assume file will NOT be of interest */

	/* Try and rule out a dualstate inode by doing some quick tests. */

	if ((statp->bs_mode & S_IFMT) != S_IFREG) {
		return 0;	/* not a regular file */
	}
	if ((statp->bs_xflags & XFS_XFLAG_HASATTR) == 0) {
		return 0;	/* no DMF attribute exists */
	}
	if ((statp->bs_dmevmask & DMF_EV_BITS) == 0 ) {
		return 0;	/* no interesting DMAPI bits set */
	}

	/* We have a likely candidate, so we have to pay the price and look
	   for the DMF attribute.  (It could be in a disk block separate from
	   the inode.)
	*/

	ino = (dm_ino_t)statp->bs_ino;
	igen = (dm_igen_t)statp->bs_gen;
	if (dm_make_handle(&dmf_f_ctxtp->fsys.fsid, &ino, &igen, &hanp, &hlen) != 0) {
		return 0;	/* can't make a proper handle */
	}

	attr_op.am_opcode    = ATTR_OP_GET;
	attr_op.am_error     = 0;
	attr_op.am_attrname  = DMF_ATTR_NAME;
	attr_op.am_attrvalue = dmf_f_ctxtp->attrval;
	attr_op.am_length    = sizeof(dmf_f_ctxtp->attrval);
	attr_op.am_flags     = ATTR_ROOT;

	error = attr_multi_by_handle(hanp, hlen, &attr_op, 1, 0);
	free_handle(hanp, hlen);
	if (error || attr_op.am_error)
		return 0; /* no DMF attribute */

	dmf_f_ctxtp->attrlen = attr_op.am_length;
	dmfattrp = (XFSattrvalue0_t *)dmf_f_ctxtp->attrval;

	if (dmfattrp->fsys != FSYS_TYPE_XFS)
		return 0; /* unsupported filesystem version */

	switch(dmfattrp->version) {
	case DMF_ATTR_FORMAT_0:
		if (dmf_f_ctxtp->attrlen != sizeof(XFSattrvalue0_t))
			return 0; /* wrong size */
		break;
	case DMF_ATTR_FORMAT_1:
		if (dmf_f_ctxtp->attrlen < MIN_FORMAT1_ATTR_LEN)
			return 0; /* wrong size */
		break;
	default:
		return 0; /* unsupported attr version */
	}

	state = (int)msb_load(dmfattrp->state, sizeof(dmfattrp->state));
	switch (state) {
	case DMF_ST_DUALSTATE:
	case DMF_ST_UNMIGRATING:
	case DMF_ST_PARTIAL:
	case DMF_ST_OFFLINE:
		/* We have a DMF file that can be treated as offline */
		dmf_f_ctxtp->candidate = 1;
		dmf_f_ctxtp->filesize = statp->bs_size;
		break;
	
	default:
		break;
	}
	return 0;
}


/******************************************************************************
* Name
*	HsmModifyInode - modify a xfs_bstat_t to make a file appear offline
*
* Description
*	HsmModifyInode uses the context provided by a previous
*	HsmInitFileContext call to determine how to modify a xfs_bstat_t
*	structure to make a dual-residency HSM file appear to be offline.
*
* Returns
*	!= 0, xfs_bstat_t structure was modified.
*	== 0, if something is wrong with the file and it should not be dumped.
******************************************************************************/

extern int
HsmModifyInode(
	hsm_f_ctxt_t	*contextp,
	xfs_bstat_t	*statp)
{
	dmf_f_ctxt_t	*dmf_f_ctxtp = (dmf_f_ctxt_t *)contextp;

	if (dmf_f_ctxtp->candidate) {
		statp->bs_dmevmask = DMF_EV_BITS;
	}
	return 1;
}


/******************************************************************************
* Name
*	HsmModifyExtentMap - modify getbmapx array to make file appear offline
*
* Description
*	HsmModifyExtentMap uses the context provided by a previous
*	HsmInitFileContext call to determine how to modify a contiguous array
*	of getbmapx structures to make a dual-residency HSM file appear to
*	be offline.
*
* Returns
*	!= 0, getbmapx array was successfully modified.
*	== 0, if something is wrong with the file and it should not be dumped.
******************************************************************************/

extern int
HsmModifyExtentMap(
	hsm_f_ctxt_t	*contextp,
	struct getbmapx	*bmap)
{
	dmf_f_ctxt_t	*dmf_f_ctxtp = (dmf_f_ctxt_t *)contextp;
	__int64_t	length;

	if (bmap[0].bmv_entries <= 0) {
		return 1;	/* caller must already be at EOF */
	}

	if (!dmf_f_ctxtp->candidate) {
		return 1;	/* not a dualstate file; dump as normal */
	}

	/* We are dumping a dualstate file.  Make it look like there is only
	   one getbmapx extent and that it contains a hole which extends from
	   the current offset to the end of the file.  The bmap[1].bmv_offset
	   should already be correct.
	*/

	length = BTOBB(dmf_f_ctxtp->filesize) - bmap[1].bmv_offset;

	if (length > 0) {
		bmap[0].bmv_entries = 1;	/* rest of file is one extent */

		bmap[1].bmv_block = -1;		/* convert to a hole */
		bmap[1].bmv_length = length;
	} else {
		bmap[0].bmv_entries = 0;	/* indicate at EOF */
	}

	return 1;
}


/******************************************************************************
* Name
*	HsmFilterExistingAttribute - filter out unwanted extended attributes
*
* Description
*	HsmFilterExistingAttribute uses the context provided by a previous
*	HsmInitFileContext call to determine whether or not the extended
*	attribute with name 'namep' should be included in a file's dump image.
*	(An extended attribute can be modified within the dump by filtering
*	it out with this routine, then adding the new version of the attribute
*	back with HsmAddNewAttribute.)
*
*	Note: this routine must be idempotent.  It is possible that xfsdump
*	will call this routine a second time for the same attribute if after
*	the first call it discovers that there isn't room in its buffer to
*	hold the attribute value.
*
* Returns
*	!= 0, the attribute was successfully examined.  If '*skip_entry' is
*	      non-zero, xfsdump will not add this attribute to the dump.
*	== 0, if something is wrong with the file and it should not be dumped.
******************************************************************************/

extern int
HsmFilterExistingAttribute(
	hsm_f_ctxt_t	*hsm_f_ctxtp,
const	char		*namep,		/* attribute name */
	u_int32_t	valuesz,	/* value size */
	int		flag,
	int		*skip_entry)
{
	dmf_f_ctxt_t	*dmf_f_ctxtp = (dmf_f_ctxt_t *)hsm_f_ctxtp;

	*skip_entry = 0;	/* assume we will not remove this attribute */

	if (!dmf_f_ctxtp->candidate) {
		return 1;	/* not a dualstate file */
	}
	if (flag != ATTR_ROOT) {
		return 1;	/* not a root attribute */
	}
	if (strcmp(namep, DMF_ATTR_NAME)) {
		return 1;	/* not the right attribute */
	}

	if (valuesz < sizeof(XFSattrvalue0_t)) {
		return 0;	/* attribute is corrupt */
	}

	/* Remove the existing DMF attribute, as we will later replace it with
	   our own version.
	*/

	*skip_entry = 1;
	return 1;
}


/******************************************************************************
* Name
*	HsmAddNewAttribute - add zero or more HSM attributes to a file's dump
*
* Description
*	HsmAddNewAttribute uses the context provided by a previous
*	HsmInitFileContext call to determine whether or not additional HSM
*	extended attributes need to be added to a file's dump image.  On the
*	first call for a file, 'cursor' will be zero.  xfsdump will increment
*	'cursor' by one each time it asks for a new attribute.  When no more
*	attributes are to be added, '*namepp' should be set to NULL.
*
*	Note: this routine must be idempotent.  It is possible that xfsdump
*	will call this routine a second time using the same cursor value if
*	it discovers that there isn't room in its buffer to hold the attribute
*	value it was given in the first call.
*
* Returns
*	!= 0, call was successful.  If '*namepp' is non-NULL, then it is the
*	      name of an attribute to be added to the file's dump.  '*valuep'
*	      points the the value of the attribute, and '*valueszp' is the
*	      value's size.  If '*namep* is NULL, then there are no more
*	      attributes to be added.
*	== 0, if something is wrong with the file and it should not be dumped.
******************************************************************************/

extern int
HsmAddNewAttribute(
	hsm_f_ctxt_t	*hsm_f_ctxtp,
	int		cursor,
	int		flag,
	char		**namepp,	/* pointer to new attribute name */
	char		**valuepp,	/* pointer to its value */
	u_int32_t	*valueszp)	/* pointer to the value size */
{
	dmf_f_ctxt_t	*dmf_f_ctxtp = (dmf_f_ctxt_t *)hsm_f_ctxtp;
	XFSattrvalue1_t	*dmfattr1p = (XFSattrvalue1_t *)dmf_f_ctxtp->attrval;

	*namepp = NULL;		/* assume we won't add an attribute */

	if (!dmf_f_ctxtp->candidate) {
		return 1;	/* not a dualstate file */
	}
	if (flag != ATTR_ROOT) {
		return 1;	/* not in the root attribute section */
	}

	if (cursor > 0) {
		return 1;	/* there is only one attribute to add */
	}

	/* DMF writes format0 (XFSattrvalue0_t) attributes unless:
	 *    - the file has multiple regions
	 *    - the file has a non-zero site tag
	 *
	 * Here we are writing a single region (OFL), so we only dump a
	 * format1 attribute if the file has a non-zero site tag.
	 */
	if (dmfattr1p->version == DMF_ATTR_FORMAT_1 &&
	    msb_load(dmfattr1p->sitetag, sizeof(dmfattr1p->sitetag)) != 0) {
		XFSattrregion_t *reg;
		reg = (XFSattrregion_t *)( dmf_f_ctxtp->attrval +
				           sizeof(XFSattrvalue1_t) );
		dmf_f_ctxtp->attrlen = MIN_FORMAT1_ATTR_LEN;

		/* make one offline region the size of the whole file */
		msb_store(dmfattr1p->regcnt, 1, sizeof(dmfattr1p->regcnt));
		msb_store(reg->rg_offset, 0, sizeof(reg->rg_offset));
		msb_store(reg->rg_size, dmf_f_ctxtp->filesize, sizeof(reg->rg_size));
		msb_store(reg->rg_state, DMF_ST_OFFLINE, sizeof(reg->rg_state));
		reg->rg_flags = DMF_MR_FLAGS;
		reg->rg_fbits = 0;
	} else {
		/* writing a format0 attr. ensure correct length and version */
		dmfattr1p->version = DMF_ATTR_FORMAT_0;
		dmf_f_ctxtp->attrlen = sizeof(XFSattrvalue0_t);
	}

	/* set the global state to offline */
	msb_store(dmfattr1p->state, DMF_ST_OFFLINE, sizeof(dmfattr1p->state));

	*valuepp = (char *)dmfattr1p;
	*namepp = DMF_ATTR_NAME;
	*valueszp = dmf_f_ctxtp->attrlen;
	return 1;
}
