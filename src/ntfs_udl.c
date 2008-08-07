/**
 * ntfsundelete - Part of the Linux-NTFS project.
 *
 * Copyright (c) 2002-2005 Richard Russon
 * Copyright (c) 2004-2005 Holger Ohmacht
 * Copyright (c) 2005 Anton Altaparmakov
 *
 * This utility will recover deleted files from an NTFS volume.
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
 * along with this program (in the main directory of the Linux-NTFS
 * distribution in the file COPYING); if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_FEATURES_H
#include <features.h>
#endif
#include <stdio.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#include "types.h"
#include "common.h"
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_TIME_H
#include <time.h>
#endif
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif
#ifdef HAVE_STDARG_H
#include <stdarg.h>
#endif

#if !defined(REG_NOERROR) || (REG_NOERROR != 0)
#define REG_NOERROR 0
#endif

#include "list.h"
#include "log.h"
#include "ntfs_udl.h"
#include "intrf.h"
#include "intrfn.h"

#ifdef HAVE_LIBNTFS
#include <ntfs/bootsect.h>
#include <ntfs/mft.h>
#include <ntfs/attrib.h>
#include <ntfs/layout.h>
#include <ntfs/inode.h>
#include <ntfs/device.h>
#include <ntfs/debug.h>
#include <ntfs/ntfstime.h>
#ifdef HAVE_NTFS_VERSION_H
#include <ntfs/version.h>
#endif
#ifdef HAVE_ICONV_H
#include <iconv.h>
#endif
#include "dir.h"
#include "ntfs_inc.h"
#include "ntfs_dir.h"
#include "ntfs_utl.h"
#include "dir.h"

extern const char *monstr[];

struct options {
	char		*device;	/* Device/File to work with */
	int		 percent;	/* Minimum recoverability */
	int		 uinode;	/* Undelete this inode */
	char		*dest;		/* Save file to this directory */
	int              optimistic;    /* Undelete in-use clusters as well */
};

struct filename {
	struct td_list_head list;		/* Previous/Next links */
	ntfschar	*uname;		/* Filename in unicode */
	int		 uname_len;	/* and its length */
	long long	 size_alloc;	/* Allocated size (multiple of cluster size) */
	long long	 size_data;	/* Actual size of data */
	FILE_ATTR_FLAGS	 flags;
	time_t		 date_c;	/* Time created */
	time_t		 date_a;	/*	altered */
	time_t		 date_m;	/*	mft record changed */
	time_t		 date_r;	/*	read */
	char		*name;		/* Filename in current locale */
	FILE_NAME_TYPE_FLAGS name_space;
	long long	 parent_mref;
	char		*parent_name;
	char		 padding[7];	/* Unused: padding to 64 bit. */
};

struct data {
	struct td_list_head list;		/* Previous/Next links */
	char		*name;		/* Stream name in current locale */
	ntfschar	*uname;		/* Unicode stream name */
	int		 uname_len;	/* and its length */
	int		 resident;	/* Stream is resident */
	int		 compressed;	/* Stream is compressed */
	int		 encrypted;	/* Stream is encrypted */
	long long	 size_alloc;	/* Allocated size (multiple of cluster size) */
	long long	 size_data;	/* Actual size of data */
	long long	 size_init;	/* Initialised size, may be less than data size */
	long long	 size_vcn;	/* Highest VCN in the data runs */
	runlist_element *runlist;	/* Decoded data runs */
	int		 percent;	/* Amount potentially recoverable */
	void		*data;		/* If resident, a pointer to the data */
	char		 padding[4];	/* Unused: padding to 64 bit. */
};

struct ufile {
	long long	 inode;		/* MFT record number */
	time_t		 date;		/* Last modification date/time */
	struct td_list_head name;		/* A list of filenames */
	struct td_list_head data;		/* A list of data streams */
	char		*pref_name;	/* Preferred filename */
	char		*pref_pname;	/*	     parent filename */
	long long	 max_size;	/* Largest size we find */
	int		 attr_list;	/* MFT record may be one of many */
	int		 directory;	/* MFT record represents a directory */
	MFT_RECORD	*mft;		/* Raw MFT record */
	char		 padding[4];	/* Unused: padding to 64 bit. */
};

#if 0
static const char *NONE      = "<none>";
#endif
static const char *UNKNOWN   = "unknown";
static struct options opts;

/**
 * free_file - Release the resources used by a file object
 * @file:  The unwanted file object
 *
 * This will free up the memory used by a file object and iterate through the
 * object's children, freeing their resources too.
 *
 * Return:  none
 */
static void free_file(struct ufile *file)
{
  struct td_list_head *item, *tmp;

  if (!file)
    return;

  td_list_for_each_safe(item, tmp, &file->name) { /* List of filenames */
    struct filename *f = td_list_entry(item, struct filename, list);
    free(f->name);
    free(f->parent_name);
    free(f);
  }

  td_list_for_each_safe(item, tmp, &file->data) { /* List of data streams */
    struct data *d = td_list_entry(item, struct data, list);
    free(d->name);
    free(d->runlist);
    free(d);
  }

  free(file->mft);
  free(file);
}

/**
 * verify_parent - confirm a record is parent of a file
 * @name:	a filename of the file
 * @rec:	the mft record of the possible parent
 *
 * Check that @rec is the parent of the file represented by @name.
 * If @rec is a directory, but it is created after @name, then we
 * can't determine whether @rec is really @name's parent.
 *
 * Return:	@rec's filename, either same name space as @name or lowest space.
 *		NULL if can't determine parenthood or on error.
 */
static FILE_NAME_ATTR* verify_parent(struct filename* name, MFT_RECORD* rec)
{
	ATTR_RECORD *attr30;
	FILE_NAME_ATTR *filename_attr = NULL, *lowest_space_name = NULL;
	ntfs_attr_search_ctx *ctx;
	int found_same_space = 1;

	if (!name || !rec)
		return NULL;

	if (!(rec->flags & MFT_RECORD_IS_DIRECTORY)) {
		return NULL;
	}

	ctx = ntfs_attr_get_search_ctx(NULL, rec);
	if (!ctx) {
		log_error("ERROR: Couldn't create a search context.\n");
		return NULL;
	}

	attr30 = find_attribute(AT_FILE_NAME, ctx);
	if (!attr30) {
		return NULL;
	}

	filename_attr = (FILE_NAME_ATTR*)((char*)attr30 + le16_to_cpu(attr30->value_offset));
	/* if name is older than this dir -> can't determine */
	if (ntfs2utc(filename_attr->creation_time) > name->date_c) {
		return NULL;
	}
	if (filename_attr->file_name_type != name->name_space) {
		found_same_space = 0;
		lowest_space_name = filename_attr;

		while (!found_same_space && (attr30 = find_attribute(AT_FILE_NAME, ctx))) {
			filename_attr = (FILE_NAME_ATTR*)((char*)attr30 + le16_to_cpu(attr30->value_offset));

			if (filename_attr->file_name_type == name->name_space) {
				found_same_space = 1;
			} else {
				if (filename_attr->file_name_type < lowest_space_name->file_name_type) {
					lowest_space_name = filename_attr;
				}
			}
		}
	}

	ntfs_attr_put_search_ctx(ctx);

	return (found_same_space ? filename_attr : lowest_space_name);
}

/**
 * get_parent_name - Find the name of a file's parent.
 * @name:	the filename whose parent's name to find
 */
static void get_parent_name(struct filename* name, ntfs_volume* vol)
{
  ntfs_attr* mft_data;
  MFT_RECORD* rec;

  if (!name || !vol)
    return;

  mft_data = ntfs_attr_open(vol->mft_ni, AT_DATA, AT_UNNAMED, 0);
  if (!mft_data) {
    log_error("ERROR: Couldn't open $MFT/$DATA\n");
    return;
  }
  rec = calloc(1, vol->mft_record_size);
  if (!rec) {
    log_error("ERROR: Couldn't allocate memory in get_parent_name()\n");
    ntfs_attr_close(mft_data);
    return;
  }

  {
    FILE_NAME_ATTR* filename_attr;
    long long inode_num;
    int ok;
    inode_num = MREF(name->parent_mref);
    name->parent_name = NULL;
    do
    {
      ok=0;
      if (ntfs_attr_pread(mft_data, vol->mft_record_size * inode_num, vol->mft_record_size, rec) < 1)
      {
	log_error("ERROR: Couldn't read MFT Record %lld.\n", inode_num);
      }
      else if ((filename_attr = verify_parent(name, rec)))
      {
	char *parent_name=NULL;
	if (ntfs_ucstombs(filename_attr->file_name,
	      filename_attr->file_name_length,
	      &parent_name, 0) < 0)
	{
	  log_error("ERROR: Couldn't translate filename to current locale.\n");
	  parent_name = NULL;
	}
	else
	{
	  if(name->parent_name==NULL || parent_name==NULL)
	    name->parent_name=parent_name;
	  else
	  {
	    char *npn=MALLOC(strlen(parent_name)+strlen(name->parent_name)+2);
	    sprintf(npn, "%s/%s", parent_name, name->parent_name);
	    free(name->parent_name);
	    name->parent_name=npn;
	  }
	  if(inode_num!=MREF(filename_attr->parent_directory))
	  {
	    inode_num=MREF(filename_attr->parent_directory);
	    ok=1;
	  }
	}
      }
    } while(ok);
  }
  free(rec);
  ntfs_attr_close(mft_data);
  return;
}

/**
 * get_filenames - Read an MFT Record's $FILENAME attributes
 * @file:  The file object to work with
 *
 * A single file may have more than one filename.  This is quite common.
 * Windows creates a short DOS name for each long name, e.g. LONGFI~1.XYZ,
 * LongFiLeName.xyZ.
 *
 * The filenames that are found are put in filename objects and added to a
 * linked list of filenames in the file object.  For convenience, the unicode
 * filename is converted into the current locale and stored in the filename
 * object.
 *
 * One of the filenames is picked (the one with the lowest numbered namespace)
 * and its locale friendly name is put in pref_name.
 *
 * Return:  n  The number of $FILENAME attributes found
 *	   -1  Error
 */
static int get_filenames(struct ufile *file, ntfs_volume* vol)
{
	ATTR_RECORD *rec;
	FILE_NAME_ATTR *attr;
	ntfs_attr_search_ctx *ctx;
	struct filename *name;
	int count = 0;
	int space = 4;

	if (!file)
		return -1;

	ctx = ntfs_attr_get_search_ctx(NULL, file->mft);
	if (!ctx)
		return -1;

	while ((rec = find_attribute(AT_FILE_NAME, ctx))) {
		/* We know this will always be resident. */
		attr = (FILE_NAME_ATTR *) ((char *) rec + le16_to_cpu(rec->value_offset));

		name = calloc(1, sizeof(*name));
		if (!name) {
			log_error("ERROR: Couldn't allocate memory in get_filenames().\n");
			count = -1;
			break;
		}

		name->uname      = attr->file_name;
		name->uname_len  = attr->file_name_length;
		name->name_space = attr->file_name_type;
		name->size_alloc = sle64_to_cpu(attr->allocated_size);
		name->size_data  = sle64_to_cpu(attr->data_size);
		name->flags      = attr->file_attributes;

		name->date_c     = ntfs2utc(attr->creation_time);
		name->date_a     = ntfs2utc(attr->last_data_change_time);
		name->date_m     = ntfs2utc(attr->last_mft_change_time);
		name->date_r     = ntfs2utc(attr->last_access_time);

		if (ntfs_ucstombs(name->uname, name->uname_len, &name->name,
				0) < 0) {
			log_error("ERROR: Couldn't translate filename to current locale.\n");
		}

		name->parent_name = NULL;

		{
		  	long long inode_num;
			name->parent_mref = attr->parent_directory;
			inode_num = MREF(name->parent_mref);
			get_parent_name(name, vol);
		}

		if (name->name_space < space) {
			file->pref_name = name->name;
			file->pref_pname = name->parent_name;
			space = name->name_space;
		}

		file->max_size = max(file->max_size, name->size_alloc);
		file->max_size = max(file->max_size, name->size_data);

		td_list_add_tail(&name->list, &file->name);
		count++;
	}

	ntfs_attr_put_search_ctx(ctx);
	log_debug("File has %d names.\n", count);
	return count;
}

/**
 * get_data - Read an MFT Record's $DATA attributes
 * @file:  The file object to work with
 * @vol:  An ntfs volume obtained from ntfs_mount
 *
 * A file may have more than one data stream.  All files will have an unnamed
 * data stream which contains the file's data.  Some Windows applications store
 * extra information in a separate stream.
 *
 * The streams that are found are put in data objects and added to a linked
 * list of data streams in the file object.
 *
 * Return:  n  The number of $FILENAME attributes found
 *	   -1  Error
 */
static int get_data(struct ufile *file, ntfs_volume *vol)
{
	ATTR_RECORD *rec;
	ntfs_attr_search_ctx *ctx;
	int count = 0;
	struct data *data;

	if (!file)
		return -1;

	ctx = ntfs_attr_get_search_ctx(NULL, file->mft);
	if (!ctx)
		return -1;

	while ((rec = find_attribute(AT_DATA, ctx))) {
		data = calloc(1, sizeof(*data));
		if (!data) {
			log_error("ERROR: Couldn't allocate memory in get_data().\n");
			count = -1;
			break;
		}

		data->resident   = !rec->non_resident;
		data->compressed = rec->flags & ATTR_IS_COMPRESSED;
		data->encrypted  = rec->flags & ATTR_IS_ENCRYPTED;

		if (rec->name_length) {
			data->uname     = (ntfschar *) ((char *) rec + le16_to_cpu(rec->name_offset));
			data->uname_len = rec->name_length;

			if (ntfs_ucstombs(data->uname, data->uname_len, &data->name,
					0) < 0) {
				log_error("ERROR: Cannot translate name into current locale.\n");
			}
		}

		if (data->resident) {
			data->size_data  = le32_to_cpu(rec->value_length);
			data->data	 = ((char*) (rec)) + le16_to_cpu(rec->value_offset);
		} else {
			data->size_alloc = sle64_to_cpu(rec->allocated_size);
			data->size_data  = sle64_to_cpu(rec->data_size);
			data->size_init  = sle64_to_cpu(rec->initialized_size);
			data->size_vcn   = sle64_to_cpu(rec->highest_vcn) + 1;
		}

		data->runlist = ntfs_mapping_pairs_decompress(vol, rec, NULL);
		if (!data->runlist) {
			log_debug("Couldn't decompress the data runs.\n");
		}

		file->max_size = max(file->max_size, data->size_data);
		file->max_size = max(file->max_size, data->size_init);

		td_list_add_tail(&data->list, &file->data);
		count++;
	}

	ntfs_attr_put_search_ctx(ctx);
	log_debug("File has %d data streams.\n", count);
	return count;
}

/**
 * read_record - Read an MFT record into memory
 * @vol:     An ntfs volume obtained from ntfs_mount
 * @record:  The record number to read
 *
 * Read the specified MFT record and gather as much information about it as
 * possible.
 *
 * Return:  Pointer  A ufile object containing the results
 *	    NULL     Error
 */
static struct ufile * read_record(ntfs_volume *vol, long long record)
{
	ATTR_RECORD *attr10, *attr20, *attr90;
	struct ufile *file;
	ntfs_attr *mft;

	if (!vol)
		return NULL;

	file = calloc(1, sizeof(*file));
	if (!file) {
		log_error("ERROR: Couldn't allocate memory in read_record()\n");
		return NULL;
	}

	TD_INIT_LIST_HEAD(&file->name);
	TD_INIT_LIST_HEAD(&file->data);
	file->inode = record;

	file->mft = MALLOC(vol->mft_record_size);

	mft = ntfs_attr_open(vol->mft_ni, AT_DATA, AT_UNNAMED, 0);
	if (!mft) {
		log_error("ERROR: Couldn't open $MFT/$DATA\n");
		free_file(file);
		return NULL;
	}

	if (ntfs_attr_mst_pread(mft, vol->mft_record_size * record, 1, vol->mft_record_size, file->mft) < 1) {
		log_error("ERROR: Couldn't read MFT Record %lld.\n", record);
		ntfs_attr_close(mft);
		free_file(file);
		return NULL;
	}

	ntfs_attr_close(mft);
	mft = NULL;

	attr10 = find_first_attribute(AT_STANDARD_INFORMATION,	file->mft);
	attr20 = find_first_attribute(AT_ATTRIBUTE_LIST,	file->mft);
	attr90 = find_first_attribute(AT_INDEX_ROOT,		file->mft);

	log_debug("Attributes present: %s %s %s.\n", attr10?"0x10":"",
			attr20?"0x20":"", attr90?"0x90":"");

	if (attr10) {
		STANDARD_INFORMATION *si;
		si = (STANDARD_INFORMATION *) ((char *) attr10 + le16_to_cpu(attr10->value_offset));
		file->date = ntfs2utc(si->last_data_change_time);
	}

	if (attr20 || !attr10)
		file->attr_list = 1;
	if (attr90)
		file->directory = 1;

	if (get_filenames(file, vol) < 0) {
		log_error("ERROR: Couldn't get filenames.\n");
	}
	if (get_data(file, vol) < 0) {
		log_error("ERROR: Couldn't get data streams.\n");
	}

	return file;
}

/**
 * calc_percentage - Calculate how much of the file is recoverable
 * @file:  The file object to work with
 * @vol:   An ntfs volume obtained from ntfs_mount
 *
 * Read through all the $DATA streams and determine if each cluster in each
 * stream is still free disk space.  This is just measuring the potential for
 * recovery.  The data may have still been overwritten by a another file which
 * was then deleted.
 *
 * Files with a resident $DATA stream will have a 100% potential.
 *
 * N.B.  If $DATA attribute spans more than one MFT record (i.e. badly
 *       fragmented) then only the data in this segment will be used for the
 *       calculation.
 *
 * N.B.  Currently, compressed and encrypted files cannot be recovered, so they
 *       will return 0%.
 *
 * Return:  n  The percentage of the file that _could_ be recovered
 *	   -1  Error
 */
static int calc_percentage(struct ufile *file, ntfs_volume *vol)
{
	runlist_element *rl = NULL;
	struct td_list_head *pos;
	struct data *data;
	long long i, j;
	long long start, end;
	int clusters_inuse, clusters_free;
	int percent = 0;

	if (!file || !vol)
		return -1;

	if (file->directory) {
		return 0;
	}

	if (td_list_empty(&file->data)) {
		return 0;
	}

	td_list_for_each(pos, &file->data) {
		data  = td_list_entry(pos, struct data, list);
		clusters_inuse = 0;
		clusters_free  = 0;

		if (data->encrypted) {
			log_debug("File is encrypted, recovery is impossible.\n");
			continue;
		}

		if (data->compressed) {
			log_debug("File is compressed, recovery not yet implemented.\n");
			continue;
		}

		if (data->resident) {
			percent = 100;
			data->percent = 100;
			continue;
		}

		rl = data->runlist;
		if (!rl) {
			log_debug("File has no runlist, hence no data.\n");
			continue;
		}

		if (rl[0].length <= 0) {
			log_debug("File has an empty runlist, hence no data.\n");
			continue;
		}

		if (rl[0].lcn == LCN_RL_NOT_MAPPED) {	/* extended mft record */
			log_debug("Missing segment at beginning, %lld clusters\n", (long long)rl[0].length);
			clusters_inuse += rl[0].length;
			rl++;
		}

		for (i = 0; rl[i].length > 0; i++) {
			if (rl[i].lcn == LCN_RL_NOT_MAPPED) {
				log_debug("Missing segment at end, %lld clusters\n",
						(long long)rl[i].length);
				clusters_inuse += rl[i].length;
				continue;
			}

			if (rl[i].lcn == LCN_HOLE) {
				clusters_free += rl[i].length;
				continue;
			}

			start = rl[i].lcn;
			end   = rl[i].lcn + rl[i].length;

			for (j = start; j < end; j++) {
				if (utils_cluster_in_use(vol, j))
					clusters_inuse++;
				else
					clusters_free++;
			}
		}

		if ((clusters_inuse + clusters_free) == 0) {
			log_error("ERROR: Unexpected error whilst "
				"calculating percentage for inode %lld\n",
				file->inode);
			continue;
		}

		data->percent = (clusters_free * 100) /
				(clusters_inuse + clusters_free);

		percent = max(percent, data->percent);
	}
	return percent;
}

#if 0
/**
 * list_record - Print a one line summary of the file
 * @file:  The file to work with
 *
 * Print a one line description of a file.
 *
 *   Inode    Flags  %age  Date            Size  Filename
 *
 * The output will contain the file's inode number (MFT Record), some flags,
 * the percentage of the file that is recoverable, the last modification date,
 * the size and the filename.
 *
 * The flags are F/D = File/Directory, N/R = Data is (Non-)Resident,
 * C = Compressed, E = Encrypted, ! = Metadata may span multiple records.
 *
 * N.B.  The file size is stored in many forms in several attributes.   This
 *       display the largest it finds.
 *
 * N.B.  If the filename is missing, or couldn't be converted to the current
 *       locale, "<none>" will be displayed.
 *
 * Return:  none
 */
static void list_record(struct ufile *file)
{
	char buffer[20];
	struct td_list_head *item;
	const char *name = NULL;
	long long size = 0;
	int percent = 0;

	char flagd = '.', flagr = '.', flagc = '.', flagx = '.';

	strftime(buffer, sizeof(buffer), "%F", localtime(&file->date));

	if (file->attr_list)
		flagx = '!';

	if (file->directory)
		flagd = 'D';
	else
		flagd = 'F';

	td_list_for_each(item, &file->data) {
		struct data *d = td_list_entry(item, struct data, list);

		if (!d->name) {
			if (d->resident)   flagr = 'R';
			else		   flagr = 'N';
			if (d->compressed) flagc = 'C';	/* These two are mutually exclusive */
			if (d->encrypted)  flagc = 'E';

			percent = max(percent, d->percent);
		}

		size = max(size, d->size_data);
		size = max(size, d->size_init);
	}

	if (file->pref_name)
		name = file->pref_name;
	else
		name = NONE;

	if(file->pref_pname)
	{
	  log_info("%-8lld %c%c%c%c   %3d%%  %s %9lld  %s/%s\n",
	      file->inode, flagd, flagr, flagc, flagx,
	      percent, buffer, size, file->pref_pname, name);
	}
	else
	{
	  log_info("%-8lld %c%c%c%c   %3d%%  %s %9lld  %s\n",
	      file->inode, flagd, flagr, flagc, flagx,
	      percent, buffer, size, name);
	}
}
#endif

/**
 * write_data - Write out a block of data
 * @fd:       File descriptor to write to
 * @buffer:   Data to write
 * @bufsize:  Amount of data to write
 *
 * Write a block of data to a file descriptor.
 *
 * Return:  -1  Error, something went wrong
 *	     0  Success, all the data was written
 */
static unsigned int write_data(int fd, const char *buffer,
	unsigned int bufsize)
{
	ssize_t result1, result2;

	if (!buffer) {
		errno = EINVAL;
		return -1;
	}

	result1 = write(fd, buffer, bufsize);
	if ((result1 == (ssize_t) bufsize) || (result1 < 0))
		return result1;

	/* Try again with the rest of the buffer */
	buffer  += result1;
	bufsize -= result1;

	result2 = write(fd, buffer, bufsize);
	if (result2 < 0)
		return result1;

	return result1 + result2;
}

/**
 * create_pathname - Create a path/file from some components
 * @dir:      Directory in which to create the file
 * @dir2:     Pathname to give the file (optional)
 * @name:     Filename to give the file (optional)
 * @stream:   Name of the stream (optional)
 * @buffer:   Store the result here
 * @bufsize:  Size of buffer
 *
 * Create a filename from various pieces.  The output will be of the form:
 *	dir/file
 *	dir/file:stream
 *	file
 *	file:stream
 *
 * All the components are optional.  If the name is missing, "unknown" will be
 * used.  If the directory is missing the file will be created in the current
 * directory.  If the stream name is present it will be appended to the
 * filename, delimited by a colon.
 *
 * N.B. If the buffer isn't large enough the name will be truncated.
 *
 * Return:  n  Length of the allocated name
 */
static int create_pathname(const char *dir, const char *dir2, const char *name,
	const char *stream, char *buffer, int bufsize)
{
  char *namel;
  if (name==NULL)
    name = UNKNOWN;
  namel=gen_local_filename(name, NULL);
  if(dir2)
  {
    char *dir2l=gen_local_filename(dir2, NULL);
    if (stream)
    {
      char *streaml=gen_local_filename(stream, NULL);
      snprintf(buffer, bufsize, "%s/%s/%s:%s", dir, dir2l, namel, streaml);
      free(streaml);
    }
    else
      snprintf(buffer, bufsize, "%s/%s/%s", dir, dir2l, namel);
    free(dir2l);
  }
  else
  {
    if (stream)
    {
      char *streaml=gen_local_filename(stream, NULL);
      snprintf(buffer, bufsize, "%s/%s:%s", dir, namel, streaml);
      free(streaml);
    }
    else
      snprintf(buffer, bufsize, "%s/%s", dir, namel);
  }
  free(namel);
  return strlen(buffer);
}

/**
 * open_file - Open a file to write to
 * @pathname:  Path, name and stream of the file to open
 *
 * Create a file and return the file descriptor.
 *
 * Existing file will be overwritten.
 *
 * Return:  -1  Error, failed to create the file
 *	     n  Success, this is the file descriptor
 */
static int open_file(const char *pathname)
{
  create_dir(pathname, 0);
  return open(pathname, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
}

/**
 * undelete_file - Recover a deleted file from an NTFS volume
 * @vol:    An ntfs volume obtained from ntfs_mount
 * @inode:  MFT Record number to be recovered
 *
 * Read an MFT Record and try an recover any data associated with it.  Some of
 * the clusters may be in use; these will be filled with zeros or the fill byte
 * supplied in the options.
 *
 * Each data stream will be recovered and saved to a file.  The file's name will
 * be the original filename and it will be written to the current directory.
 * Any named data stream will be saved as filename:streamname.
 *
 * The output file's name and location can be altered by using the command line
 * options.
 *
 * N.B.  We cannot tell if someone has overwritten some of the data since the
 *       file was deleted.
 *
 * Return:  -2  Error, something went wrong
 *	    0  Success, the data was recovered
 */
static int undelete_file(ntfs_volume *vol, long long inode)
{
	char pathname[256];
	char *buffer = NULL;
	unsigned int bufsize;
	struct ufile *file;
	int i, j;
	long long start, end;
	runlist_element *rl;
	struct td_list_head *item;
	int fd = -1;
	long long k;
	int result = -2;
	char *name;
	long long cluster_count;	/* I'll need this variable (see below). +mabs */

	if (!vol)
		return -2;

	/* try to get record */
	file = read_record(vol, inode);
	if (!file || !file->mft) {
		log_error("Can't read info from mft record %lld.\n", inode);
		return -2;
	}


	bufsize = vol->cluster_size;
	buffer = MALLOC(bufsize);

	/* calc_percentage() must be called before 
	 * list_record(). Otherwise, when undeleting, a file will always be
	 * listed as 0% recoverable even if successfully undeleted. +mabs
	 */
	if (file->mft->flags & MFT_RECORD_IN_USE) {
          log_error("Record is in use by the mft\n");
          free(buffer);
          free_file(file);
          return -2;
	}

	if (calc_percentage(file, vol) == 0) {
		log_error("File has no recoverable data.\n");
		goto free;
	}

	if (td_list_empty(&file->data)) {
		log_warning("File has no data.  There is nothing to recover.\n");
		goto free;
	}

	td_list_for_each(item, &file->data) {
		struct data *d = td_list_entry(item, struct data, list);

		name = file->pref_name;

		create_pathname(opts.dest, file->pref_pname, name, d->name, pathname, sizeof(pathname));
		if (d->resident) {
			fd = open_file(pathname);
			if (fd < 0) {
				log_error("Couldn't create file %s\n", pathname);
				goto free;
			}

			log_verbose("File has resident data.\n");
			if (write_data(fd, d->data, d->size_data) < d->size_data) {
				log_error("Write failed\n");
				close(fd);
				goto free;
			}

			if (close(fd) < 0) {
				log_error("Close failed\n");
			}
			fd = -1;
		} else {
			rl = d->runlist;
			if (!rl) {
				log_verbose("File has no runlist, hence no data.\n");
				continue;
			}

			if (rl[0].length <= 0) {
				log_verbose("File has an empty runlist, hence no data.\n");
				continue;
			}

			fd = open_file(pathname);
			if (fd < 0) {
				log_error("Couldn't create output file %s\n", pathname);
				goto free;
			}

			if (rl[0].lcn == LCN_RL_NOT_MAPPED) {	/* extended mft record */
				log_verbose("Missing segment at beginning, %lld "
						"clusters.\n",
						(long long)rl[0].length);
				memset(buffer, 0, bufsize);
				for (k = 0; k < rl[0].length * vol->cluster_size; k += bufsize) {
					if (write_data(fd, buffer, bufsize) < bufsize) {
						log_error("Write failed\n");
						close(fd);
						goto free;
					}
				}
			}

			cluster_count = 0LL;
			for (i = 0; rl[i].length > 0; i++) {

				if (rl[i].lcn == LCN_RL_NOT_MAPPED) {
					log_verbose("Missing segment at end, "
							"%lld clusters.\n",
							(long long)rl[i].length);
					memset(buffer, 0, bufsize);
					for (k = 0; k < rl[i].length * vol->cluster_size; k += bufsize) {
						if (write_data(fd, buffer, bufsize) < bufsize) {
							log_error("Write failed\n");
							close(fd);
							goto free;
						}
						cluster_count++;
					}
					continue;
				}

				if (rl[i].lcn == LCN_HOLE) {
					log_verbose("File has a sparse section.\n");
					memset(buffer, 0, bufsize);
					for (k = 0; k < rl[k].length * vol->cluster_size; k += bufsize) {
						if (write_data(fd, buffer, bufsize) < bufsize) {
							log_error("Write failed\n");
							close(fd);
							goto free;
						}
					}
					continue;
				}

				start = rl[i].lcn;
				end   = rl[i].lcn + rl[i].length;

				for (j = start; j < end; j++) {
					if (utils_cluster_in_use(vol, j) && !opts.optimistic) {
						memset(buffer, 0, bufsize);
						if (write_data(fd, buffer, bufsize) < bufsize) {
							log_error("Write failed\n");
							close(fd);
							goto free;
						}
					} else {
						if (ntfs_cluster_read(vol, j, 1, buffer) < 1) {
							log_error("Read failed\n");
							close(fd);
							goto free;
						}
						if (write_data(fd, buffer, bufsize) < bufsize) {
							log_error("Write failed\n");
							close(fd);
							goto free;
						}
						cluster_count++;
					}
				}
			}

			/*
			 * IF data stream currently being recovered is
			 * non-resident AND data stream has no holes (100% recoverability) AND
			 * 0 <= (data->size_alloc - data->size_data) <= vol->cluster_size AND
			 * cluster_count * vol->cluster_size == data->size_alloc THEN file
			 * currently being written is truncated to data->size_data bytes before
			 * it's closed.
			 * This multiple checks try to ensure that only files with consistent
			 * values of size/occupied clusters are eligible for truncation. Note
			 * that resident streams need not be truncated, since the original code
			 * already recovers their exact length.                           +mabs
			 */
			  if (d->percent == 100 && d->size_alloc >= d->size_data &&
			      (d->size_alloc - d->size_data) <= (long long)vol->cluster_size &&
			      cluster_count * (long long)vol->cluster_size == d->size_alloc)
			  {
			    if (ftruncate(fd, (off_t)d->size_data))
			      log_error("Truncation failed\n");
			  }
			  else
			    log_warning("Truncation not performed because file has an "
				"inconsistent $MFT record.\n");

			if (close(fd) < 0) {
				log_error("Close failed\n");
			}
			fd = -1;

		}
		set_date(pathname, file->date, file->date);
	}
	result = 0;
free:
	free(buffer);
	free_file(file);
	return result;
}

static file_data_t *ufile_to_file_data(const struct ufile *file)
{
  file_data_t *new_file;
  int len;
  if(file->pref_name==NULL)
    return NULL;
  new_file=(file_data_t *)MALLOC(sizeof(*new_file));
  len=strlen(file->pref_name);
  if(file->pref_pname!=NULL)
    len+=strlen(file->pref_pname)+1;
  if(len<sizeof(new_file->name))
  {
    if(file->pref_pname!=NULL)
      sprintf((char *)&new_file->name, "%s/%s", file->pref_pname, file->pref_name);
    else
      memcpy(&new_file->name, file->pref_name, len);
  }
  else
  {
    memcpy(new_file->name, file->pref_name,
	strlen(file->pref_name)+1<sizeof(new_file->name)?strlen(file->pref_name)+1:sizeof(new_file->name) );
    new_file->name[sizeof(new_file->name)-1]=0;
  }
  new_file->filestat.st_dev=0;
  new_file->filestat.st_ino=file->inode;
  new_file->filestat.st_mode = (file->directory ?LINUX_S_IFDIR| LINUX_S_IRUGO | LINUX_S_IXUGO:LINUX_S_IFREG | LINUX_S_IRUGO);
  new_file->filestat.st_nlink=0;
  new_file->filestat.st_uid=0;
  new_file->filestat.st_gid=0;
  new_file->filestat.st_rdev=0;
  new_file->filestat.st_size=file->max_size;
#ifdef HAVE_STRUCT_STAT_ST_BLKSIZE
  new_file->filestat.st_blksize=512;	/* FIXME cluster_size */
#ifdef HAVE_STRUCT_STAT_ST_BLOCKS
  if(new_file->filestat.st_blksize!=0)
  {
    new_file->filestat.st_blocks=(new_file->filestat.st_size+new_file->filestat.st_blksize-1)/new_file->filestat.st_blksize;
  }
#endif
#endif
  new_file->filestat.st_atime=new_file->filestat.st_ctime=new_file->filestat.st_mtime=file->date;
  new_file->status=FILE_STATUS_DELETED;
  return new_file;
}

/**
 * scan_disk - Search an NTFS volume for files that could be undeleted
 * @vol:  An ntfs volume obtained from ntfs_mount
 *
 * Read through all the MFT entries looking for deleted files.  For each one
 * determine how much of the data lies in unused disk space.
 *
 * The list can be filtered by name, size and date, using command line options.
 *
 */
static file_data_t *scan_disk(ntfs_volume *vol)
{
	s64 nr_mft_records;
	const int BUFSIZE = 8192;
	char *buffer = NULL;
	int results = 0;
	ntfs_attr *attr;
	long long size;
	long long bmpsize;
	int i, j, k, b;
	int percent;
	struct ufile *file;
	file_data_t *dir_list=NULL;
	file_data_t *current_file=NULL;

	if (!vol)
		return NULL;
#ifdef NTFS_LOG_LEVEL_VERBOSE
	ntfs_log_set_levels(NTFS_LOG_LEVEL_QUIET);
	ntfs_log_set_handler(ntfs_log_handler_stderr);
#endif
	opts.uinode   = -1;
	opts.percent  = 1;

	attr = ntfs_attr_open(vol->mft_ni, AT_BITMAP, AT_UNNAMED, 0);
	if (!attr) {
		log_error("ERROR: Couldn't open $MFT/$BITMAP\n");
		return NULL;
	}
	bmpsize = attr->initialized_size;

	buffer = MALLOC(BUFSIZE);

	nr_mft_records = vol->mft_na->initialized_size >>
			vol->mft_record_size_bits;

	for (i = 0; i < bmpsize; i += BUFSIZE) {
		long long read_count = min((bmpsize - i), BUFSIZE);
		size = ntfs_attr_pread(attr, i, read_count, buffer);
		if (size < 0)
			break;

		for (j = 0; j < size; j++) {
			b = buffer[j];
			for (k = 0; k < 8; k++, b>>=1) {
				if (((i+j)*8+k) >= nr_mft_records)
					goto done;
				if (b & 1)
					continue;
				file = read_record(vol, (i+j)*8+k);
				if (!file) {
					log_error("Couldn't read MFT Record %d.\n", (i+j)*8+k);
					continue;
				}

				percent = calc_percentage(file, vol);
				if (percent >0)
				{
				  file_data_t *new_file;
				  new_file=ufile_to_file_data(file);
				  new_file->prev=current_file;
				  new_file->next=NULL;
				  if(current_file!=NULL)
				    current_file->next=new_file;
				  else
				    dir_list=new_file;
				  current_file=new_file;
#if 0
				  list_record(file);
				  /* Was -u specified with no inode
				     so undelete file by regex */
				  if (opts.mode == MODE_UNDELETE) {
				    if  (!undelete_file(vol, file->inode))
				      log_error("ERROR: Failed to undelete "
					  "inode %lli\n!",
					  file->inode);
				  }
#endif
				  results++;
				}
				free_file(file);
			}
		}
	}
done:
	log_info("\nFiles with potentially recoverable content: %d\n", results);
	free(buffer);
	if (attr)
		ntfs_attr_close(attr);
	return dir_list;
}

#ifdef HAVE_NCURSES
#define INTER_DIR (LINES-25+16)

static void ntfs_undelete_menu_ncurses(disk_t *disk_car, const partition_t *partition, dir_data_t *dir_data, const file_data_t *dir_list)
{
  struct ntfs_dir_struct *ls=(struct ntfs_dir_struct *)dir_data->private_dir_data;
  WINDOW *window=(WINDOW*)dir_data->display;
  while(1)
  {
    int offset=0;
    int pos_num=0;
    const file_data_t *current_file;
    const file_data_t *pos=dir_list;
    int old_LINES=LINES;
    aff_copy(window);
    wmove(window,3,0);
    aff_part(window,AFF_PART_ORDER|AFF_PART_STATUS,disk_car,partition);
    wmove(window,4,0);
    wprintw(window,"Deleted files\n");
    do
    {
      int i;
      int car;
      for(i=0,current_file=dir_list;(current_file!=NULL) && (i<offset);current_file=current_file->next,i++);
      for(i=offset;(current_file!=NULL) &&((i-offset)<INTER_DIR);i++,current_file=current_file->next)
      {
	struct tm		*tm_p;
	char str[11];
	char		datestr[80];
	wmove(window, 6+i-offset, 0);
	wclrtoeol(window);	/* before addstr for BSD compatibility */
	if(current_file==pos)
	  wattrset(window, A_REVERSE);
	if(current_file->filestat.st_mtime!=0)
	{
	  tm_p = localtime(&current_file->filestat.st_mtime);
	  snprintf(datestr, sizeof(datestr),"%2d-%s-%4d %02d:%02d",
	      tm_p->tm_mday, monstr[tm_p->tm_mon],
	      1900 + tm_p->tm_year, tm_p->tm_hour,
	      tm_p->tm_min);
	  /* May have to use %d instead of %e */
	} else {
	  strncpy(datestr, "                 ",sizeof(datestr));
	}
	mode_string(current_file->filestat.st_mode,str);
	wprintw(window, "%s %5u %5u   ", 
	    str, (unsigned int)current_file->filestat.st_uid, (unsigned int)current_file->filestat.st_gid);
	wprintw(window, "%7llu", (long long unsigned int)current_file->filestat.st_size);
	/* screen may overlap due to long filename */
	wprintw(window, " %s %s", datestr, current_file->name);
	if(current_file==pos)
	  wattroff(window, A_REVERSE);
      }
      wmove(window, 6-1, 51);
      wclrtoeol(window);
      if(offset>0)
	wprintw(window, "Previous");
      /* Clear the last line, useful if overlapping */
      wmove(window,6+i-offset,0);
      wclrtoeol(window);
      wmove(window, 6+INTER_DIR, 51);
      wclrtoeol(window);
      if(current_file!=NULL)
	wprintw(window, "Next");
      if(dir_list==NULL)
      {
	wmove(window,6,0);
	wprintw(window,"No deleted file found.");
      }
      /* Redraw the bottom of the screen everytime because very long filenames may have corrupt it*/
      mvwaddstr(window,LINES-2,0,"Use ");
      if(has_colors())
	wbkgdset(window,' ' | A_BOLD | COLOR_PAIR(0));
      waddstr(window,"c");
      if(has_colors())
	wbkgdset(window,' ' | COLOR_PAIR(0));
      waddstr(window," to copy, ");
      if(has_colors())
	wbkgdset(window,' ' | A_BOLD | COLOR_PAIR(0));
      waddstr(window,"q");
      if(has_colors())
	wbkgdset(window,' ' | COLOR_PAIR(0));
      waddstr(window," to quit");
      wrefresh(window);
      /* Using gnome terminal under FC3, TERM=xterm, the screen is not always correct */
      wredrawln(window,0,getmaxy(window));	/* redrawwin def is boggus in pdcur24 */
      car=wgetch(window);
      wmove(window,5,0);
      wclrtoeol(window);
      switch(car)
      {
	case key_ESC:
	case 'q':
	case 'M':
	  return;
      }
      if(dir_list!=NULL)
      {
	switch(car)
	{
	  case KEY_UP:
	  case '8':
	    if(pos->prev!=NULL)
	    {
	      pos=pos->prev;
	      pos_num--;
	    }
	    if(pos_num<offset)
	      offset--;
	    break;
	  case KEY_DOWN:
	  case '2':
	    if(pos->next!=NULL)
	    {
	      pos=pos->next;
	      pos_num++;
	    }
	    if(pos_num>=offset+INTER_DIR)
	      offset++;
	    break;
	  case KEY_PPAGE:
	    for(i=0;(i<INTER_DIR-1)&&(pos->prev!=NULL);i++)
	    {
	      pos=pos->prev;
	      pos_num--;
	      if(pos_num<offset)
		offset--;
	    }
	    break;
	  case KEY_NPAGE:
	    for(i=0;(i<INTER_DIR-1)&&(pos->next!=NULL);i++)
	    {
	      pos=pos->next;
	      pos_num++;
	      if(pos_num>=offset+INTER_DIR)
		offset++;
	    }
	    break;
	  case 'c':
	    if(LINUX_S_ISDIR(pos->filestat.st_mode)==0)
	    {
	      if(dir_data->local_dir==NULL)
	      {
		char *res;
		if(LINUX_S_ISDIR(pos->filestat.st_mode)!=0)
		  res=ask_location("Are you sure you want to copy %s and any files below to the directory %s ? [Y/N]",
		      pos->name);
		else
		  res=ask_location("Are you sure you want to copy %s to the directory %s ? [Y/N]",
		      pos->name);
		dir_data->local_dir=res;
		opts.dest=res;
	      }
	      if(dir_data->local_dir!=NULL)
	      {
		int res=-1;
		wmove(window,5,0);
		wclrtoeol(window);
		if(has_colors())
		  wbkgdset(window,' ' | A_BOLD | COLOR_PAIR(1));
		wprintw(window,"Copying, please wait...");
		if(has_colors())
		  wbkgdset(window,' ' | COLOR_PAIR(0));
		wrefresh(window);
		res=undelete_file(ls->vol, pos->filestat.st_ino);
		wmove(window,5,0);
		wclrtoeol(window);
		if(res < -1)
		{
		  if(has_colors())
		    wbkgdset(window,' ' | A_BOLD | COLOR_PAIR(1));
		  wprintw(window,"Copy failed!");
		}
		else
		{
		  if(has_colors())
		    wbkgdset(window,' ' | A_BOLD | COLOR_PAIR(2));
		  if(res < 0)
		    wprintw(window,"Copy done! (Failed to copy some files)");
		  else
		    wprintw(window,"Copy done!");
		}
		if(has_colors())
		  wbkgdset(window,' ' | COLOR_PAIR(0));
	      }
	    }
	    break;
	}
      }
    } while(old_LINES==LINES);
  }
}
#endif

static void ntfs_undelete_menu(disk_t *disk_car, const partition_t *partition, dir_data_t *dir_data, const file_data_t *dir_list, char**current_cmd)
{
  dir_aff_log(disk_car, partition, dir_data, dir_list);
  if(*current_cmd!=NULL)
  {
    return;	/* Quit */
  }
#ifdef HAVE_NCURSES
  ntfs_undelete_menu_ncurses(disk_car, partition, dir_data, dir_list);
#endif
}

int ntfs_undelete_part(disk_t *disk_car, const partition_t *partition, const int verbose, char **current_cmd)
{
  dir_data_t dir_data;
#ifdef HAVE_NCURSES
  WINDOW *window;
#endif
  int res=dir_partition_ntfs_init(disk_car,partition,&dir_data,verbose);
#ifdef HAVE_NCURSES
  window=newwin(0,0,0,0);	/* full screen */
  dir_data.display=window;
  aff_copy(window);
#else
  dir_data.display=NULL;
#endif
  log_info("\n");
  switch(res)
  {
    case -2:
      screen_buffer_reset();
#ifdef HAVE_NCURSES
      aff_copy(window);
      wmove(window,4,0);
      aff_part(window,AFF_PART_ORDER|AFF_PART_STATUS,disk_car,partition);
#endif
      log_partition(disk_car,partition);
      screen_buffer_add("Support for this filesystem hasn't been enable during compilation.\n");
      screen_buffer_to_log();
      if(*current_cmd==NULL)
      {
#ifdef HAVE_NCURSES
	screen_buffer_display(window,"",NULL);
#endif
      }
      break;
    case -1:
      screen_buffer_reset();
#ifdef HAVE_NCURSES
      aff_copy(window);
      wmove(window,4,0);
      aff_part(window,AFF_PART_ORDER|AFF_PART_STATUS,disk_car,partition);
#endif
      log_partition(disk_car,partition);
      screen_buffer_add("Can't open filesystem. Filesystem seems damaged.\n");
      screen_buffer_to_log();
      if(*current_cmd==NULL)
      {
#ifdef HAVE_NCURSES
	screen_buffer_display(window,"",NULL);
#endif
      }
      break;
    default:
      {
	file_data_t *dir_list;
	struct ntfs_dir_struct *ls=(struct ntfs_dir_struct *)dir_data.private_dir_data;
	dir_list=scan_disk(ls->vol);
	ntfs_undelete_menu(disk_car, partition, &dir_data, dir_list, current_cmd);
	delete_list_file(dir_list);
	dir_data.close(&dir_data);
      }
      break;
  }
#ifdef HAVE_NCURSES
  delwin(window);
  (void) clearok(stdscr, TRUE);
#ifdef HAVE_TOUCHWIN
  touchwin(stdscr);
#endif
#endif
  return res;
}
#else
int ntfs_undelete_part(disk_t *disk_car, const partition_t *partition, const int verbose, char **current_cmd)
{
#ifdef HAVE_NCURSES
  WINDOW *window;
  window=newwin(0,0,0,0);	/* full screen */
  aff_copy(window);
#endif
  log_info("\n");
  screen_buffer_reset();
#ifdef HAVE_NCURSES
  aff_copy(window);
  wmove(window,4,0);
  aff_part(window,AFF_PART_ORDER|AFF_PART_STATUS,disk_car,partition);
#endif
  log_partition(disk_car,partition);
  screen_buffer_add("Support for this filesystem hasn't been enable during compilation.\n");
  screen_buffer_to_log();
  if(*current_cmd==NULL)
  {
#ifdef HAVE_NCURSES
    screen_buffer_display(window,"",NULL);
#endif
  }
#ifdef HAVE_NCURSES
  delwin(window);
  (void) clearok(stdscr, TRUE);
#ifdef HAVE_TOUCHWIN
  touchwin(stdscr);
#endif
#endif
  return -2;
}
#endif