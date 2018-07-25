/*
 * ROX-Filer, filer for the ROX desktop project
 * Copyright (C) 2006, Thomas Leonard and others (see changelog for details).
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* diritem.c - get details about files */

/* Don't load icons larger than 400K (this is rather excessive, basically
 * we just want to stop people crashing the filer with huge icons).
 */
#define MAX_ICON_SIZE (400 * 1024)

#include "config.h"

#include <gtk/gtk.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "global.h"

#include "diritem.h"
#include "support.h"
#include "gui_support.h"
#include "mount.h"
#include "type.h"
#include "usericons.h"
#include "options.h"
#include "fscache.h"
#include "pixmaps.h"
#include "xtypes.h"

#define RECENT_DELAY (5 * 60)	/* Time in seconds to consider a file recent */
#define ABOUT_NOW(time) (diritem_recent_time - time < RECENT_DELAY)
/* If you want to make use of the RECENT flag, make sure this is set to
 * the current time before calling diritem_restat().
 */
time_t diritem_recent_time;
static GMutex m_diritems;

/****************************************************************
 *			EXTERNAL INTERFACE			*
 ****************************************************************/

void diritem_init(void)
{
	read_globicons();
}

/* Bring this item's structure uptodate.
 * 'parent' is optional; it saves one stat() for directories.
 */
void diritem_restat(
		const guchar *path,
		DirItem *retitem,
		struct stat *parent,
		gboolean examine_now)
{
	struct stat	info;

	g_mutex_lock(&m_diritems);
	DirItem newitem = *retitem;
	g_mutex_unlock(&m_diritems);

	DirItem *item = &newitem;

	if (item->_image)
	{
		g_mutex_lock(&m_diritems);
		if (retitem->_image)
			g_object_unref(retitem->_image);
		retitem->_image = NULL;
		item->_image = NULL;
		g_mutex_unlock(&m_diritems);
	}

	item->flags = 0;
	item->mime_type = NULL;

	if (mc_lstat(path, &info) == -1)
	{
		item->lstat_errno = errno;
		item->base_type = TYPE_ERROR;
		item->size = 0;
		item->mode = 0;
		item->mtime = item->ctime = item->atime = 0;
		item->uid = (uid_t) -1;
		item->gid = (gid_t) -1;
	}
	else
	{
		guchar *target_path;

		item->lstat_errno = 0;
		item->size = info.st_size;
		item->mode = info.st_mode;
		item->atime = info.st_atime;
		item->ctime = info.st_ctime;
		item->mtime = info.st_mtime;
		item->uid = info.st_uid;
		item->gid = info.st_gid;
		if (ABOUT_NOW(item->mtime) || ABOUT_NOW(item->ctime))
			item->flags |= ITEM_FLAG_RECENT;

		if (xattr_have(path))
			item->flags |= ITEM_FLAG_HAS_XATTR;

		item->label = xlabel_get(path);

		if (S_ISLNK(info.st_mode))
		{
			if (mc_stat(path, &info))
				item->base_type = TYPE_ERROR;
			else
				item->base_type =
					mode_to_base_type(info.st_mode);

			item->flags |= ITEM_FLAG_SYMLINK;

			target_path = pathdup(path);
		}
		else
		{
			item->base_type = mode_to_base_type(info.st_mode);
			target_path = (guchar *) path;
		}

		if (item->base_type == TYPE_DIRECTORY)
		{
			item->size = 0;
			if (mount_is_mounted(target_path, &info,
					target_path == path ? parent : NULL))
				item->flags |= ITEM_FLAG_MOUNT_POINT
						| ITEM_FLAG_MOUNTED;
			else if (g_hash_table_lookup(fstab_mounts,
							target_path))
				item->flags |= ITEM_FLAG_MOUNT_POINT;
		}

		if (path != target_path)
			g_free(target_path);
	}

	if (item->base_type == TYPE_DIRECTORY)
	{
		/* KRJW: info.st_uid will be the uid of the dir, regardless
		 * of whether `path' is a dir or a symlink to one.  Note that
		 * if path is a symlink to a dir, item->uid will be the uid
		 * of the *symlink*, but we really want the uid of the dir
		 * to which the symlink points.
		 */
		check_globicon(path, item);

		if (item->flags & ITEM_FLAG_MOUNT_POINT)
		{
			item->mime_type = inode_mountpoint;
			/* Try to avoid automounter problems */
		}
		else if (info.st_mode & S_IWOTH)
		{/* Don't trust world-writable dirs */}
		else
			item->flags |= ITEM_FLAG_DIR_NEED_EXAMINE;

	}
	else if (item->base_type == TYPE_FILE)
	{
		if (item->flags & ITEM_FLAG_SYMLINK)
		{
			guchar *link_path;
			link_path = pathdup(path);
			item->mime_type = type_from_path(link_path
					? link_path
					: path);
			g_free(link_path);
		}
		else
			item->mime_type = type_from_path(path);

		/* Note: for symlinks we need the mode of the target */
		if (info.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))
		{
			/* Note that the flag is set for ALL executable
			 * files, but the mime_type must also be executable
			 * for clicking on the file to run it.
			 */
			item->flags |= ITEM_FLAG_EXEC_FILE;

			if (item->mime_type == NULL ||
			    item->mime_type == application_octet_stream)
			{
				item->mime_type = application_executable;
			}
			else if (item->mime_type == text_plain &&
			         !strchr(item->leafname, '.'))
			{
				item->mime_type = application_x_shellscript;
			}
		}
		else if (item->mime_type == application_x_desktop)
		{
			item->flags |= ITEM_FLAG_EXEC_FILE;
		}

		if (!item->mime_type)
			item->mime_type = text_plain;

		check_globicon(path, item);

		if (item->mime_type == application_x_desktop && item->_image == NULL)
		{
			g_mutex_lock(&m_diritems);
			item->_image = g_fscache_lookup(desktop_icon_cache, path);
			g_mutex_unlock(&m_diritems);
		}
	}
	else
		check_globicon(path, item);

	if (!item->mime_type)
		item->mime_type = mime_type_from_base_type(item->base_type);

	item->flags &= ~ITEM_FLAG_NEED_RESCAN_QUEUE;

	g_mutex_lock(&m_diritems);
	*retitem = newitem;
	g_mutex_unlock(&m_diritems);

	if (examine_now && item->flags & ITEM_FLAG_DIR_NEED_EXAMINE)
		diritem_examine_dir(path, retitem);
}

DirItem *diritem_new(const guchar *leafname)
{
	DirItem		*item;

	item = g_new0(DirItem, 1);
	item->leafname = g_strdup(leafname);
	item->base_type = TYPE_UNKNOWN;
	item->flags = ITEM_FLAG_NEED_RESCAN_QUEUE | ITEM_FLAG_NOT_DELETE;
	item->leafname_collate = collate_key_new(item->leafname);

	return item;
}

void diritem_free(DirItem *item)
{
	g_return_if_fail(item != NULL);

	if (item->_image)
		g_object_unref(item->_image);
	item->_image = NULL;
	collate_key_free(item->leafname_collate);
	if (item->label)
		g_free(item->label);
	item->label = NULL;
	g_free(item->leafname);
	g_free(item);
}

/* For use by di_image() only. Sets item->_image. */
MaskedPixmap *_diritem_get_image(DirItem *item)
{
	MaskedPixmap *ret;

	g_mutex_lock(&m_diritems);
	if (!item->_image && item->base_type != TYPE_UNKNOWN)
	{
		if (item->base_type == TYPE_ERROR)
		{
			item->_image = im_error;
			g_object_ref(im_error);
		}
		else
			item->_image = type_to_icon(item->mime_type);
	}
	g_mutex_unlock(&m_diritems);

	ret = item->_image;

	return ret;
}

/* Fill in more details of the DirItem for a directory item.
 * - Looks for an image (but maybe still NULL on error)
 * - Updates ITEM_FLAG_APPDIR
 */
gboolean diritem_examine_dir(const guchar *path, DirItem *item)
{
	guchar *rpath = pathdup(path); //realpath

	int oldsize = item->size;
	DIR *d = mc_opendir(rpath);
	if (d)
	{
		int cnt = 0;
		while ((mc_readdir(d))) cnt++;
		item->size = cnt - 2; //. and ..
		mc_closedir(d);
	}

	int pathlen = strlen(rpath);
	//pathbuf has length of path + '/AppIcon.xpm'
	gchar *pathbuf = g_new(gchar, pathlen + 13);
	gchar *inspt  = pathbuf + pathlen;

	strcpy(pathbuf, rpath);
	g_free(rpath);

	MaskedPixmap *newimage = NULL;

	struct stat info;
	if (mc_lstat(pathbuf, &info) != 0)
		goto out;

	uid_t uid = info.st_uid;

	/* Finding the icon:
	 *
	 * - If it contains a .DirIcon then that's the icon
	 * - If it contains an AppRun then it's an application
	 * - If it contains an AppRun but no .DirIcon then try to
	 *   use AppIcon.xpm as the icon.
	 *
	 * .DirIcon and AppRun must have the same owner as the
	 * directory itself, to prevent abuse of /tmp, etc.
	 * For symlinks, we want the symlink's owner.
	 */
	strcpy(inspt, "/.DirIcon");

	if (mc_lstat(pathbuf, &info) != 0 || info.st_uid != uid)
		goto no_diricon;	/* Missing, or wrong owner */

	if (S_ISLNK(info.st_mode) && mc_stat(pathbuf, &info) != 0)
		goto no_diricon;	/* Bad symlink */

	if (info.st_size > MAX_ICON_SIZE || !S_ISREG(info.st_mode))
		goto no_diricon;	/* Too big, or non-regular file */

	/* Try to load image; may still get NULL... */
	newimage = g_fscache_lookup(pixmap_cache, pathbuf);

no_diricon:

	/* Try to find AppRun... */
	strcpy(inspt + 1, /*"/"*/ "AppRun");

	if (mc_lstat(pathbuf, &info) != 0 || info.st_uid != uid)
		goto out;	/* Missing, or wrong owner */

	if (!(info.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
		goto out;	/* Not executable */

	g_mutex_lock(&m_diritems);
	item->flags |= ITEM_FLAG_APPDIR;
	g_mutex_unlock(&m_diritems);

	/* Try to load AppIcon.xpm... */

	if (newimage)
		goto out;	/* Already got an icon */

	/* Note: since AppRun is valid we don't need to check AppIcon.xpm
	 *	 so carefully.
	 */

	strcpy(inspt + 4, /*"/App"*/ "Icon.xpm");

	if (mc_stat(pathbuf, &info) != 0)
		goto out;	/* Missing, or broken symlink */

	if (info.st_size > MAX_ICON_SIZE || !S_ISREG(info.st_mode))
		goto out;	/* Too big, or non-regular file */

	/* Try to load image; may still get NULL... */
	newimage = g_fscache_lookup(pixmap_cache, pathbuf);

out:
	g_free(pathbuf);

	g_mutex_lock(&m_diritems);
	item->flags &= ~ITEM_FLAG_DIR_NEED_EXAMINE;
	g_mutex_unlock(&m_diritems);

	if ((item->flags & ITEM_FLAG_APPDIR) && !newimage)
	{
		/* This is an application without an icon */
		newimage = im_appdir;
		g_object_ref(newimage);
	}

	if (newimage)
	{
		g_mutex_lock(&m_diritems);
		if (item->_image)
			g_object_unref(item->_image);

		item->_image = newimage;
		g_mutex_unlock(&m_diritems);
		return TRUE;
	}

	return item->size != oldsize;
}
