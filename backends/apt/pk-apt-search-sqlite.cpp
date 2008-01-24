/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 Ali Sabil <ali.sabil@gmail.com>
 *
 * Licensed under the GNU General Public License Version 2
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <gmodule.h>
#include <glib.h>
#include <string.h>
#include <pk-backend.h>
#include "pk-sqlite-pkg-cache.h"
#include <apt-pkg/configuration.h>
#include <apt-pkg/init.h>
#include "pk-apt-build-db.h"

/**
 * backend_get_groups:
 */
extern "C" void
backend_get_groups (PkBackend *backend, PkEnumList *elist)
{
	g_return_if_fail (backend != NULL);
	pk_enum_list_append_multiple (elist,
				      PK_GROUP_ENUM_ACCESSORIES,
				      PK_GROUP_ENUM_GAMES,
				      PK_GROUP_ENUM_GRAPHICS,
				      PK_GROUP_ENUM_INTERNET,
				      PK_GROUP_ENUM_OFFICE,
				      PK_GROUP_ENUM_OTHER,
				      PK_GROUP_ENUM_PROGRAMMING,
				      PK_GROUP_ENUM_MULTIMEDIA,
				      PK_GROUP_ENUM_SYSTEM,
				      -1);
}

/**
 * backend_get_filters:
 */
extern "C" void
backend_get_filters (PkBackend *backend, PkEnumList *elist)
{
	g_return_if_fail (backend != NULL);
	pk_enum_list_append_multiple (elist,
				      PK_FILTER_ENUM_GUI,
				      PK_FILTER_ENUM_INSTALLED,
				      PK_FILTER_ENUM_DEVELOPMENT,
				      -1);
}

/**
 * backend_get_description:
 */

extern "C" void
backend_get_description (PkBackend *backend, const gchar *package_id)
{
	sqlite_get_description(backend,package_id);
}

/**
 * backend_search_details:
 */

extern "C" void
backend_search_details (PkBackend *backend, const gchar *filter, const gchar *search)
{
	sqlite_search_details(backend,filter,search);
}

/**
 * backend_search_name:
 */
extern "C" void
backend_search_name (PkBackend *backend, const gchar *filter, const gchar *search)
{
	sqlite_search_name(backend,filter,search);
}

/**
 * backend_search_group:
 */
extern "C" void
backend_search_group (PkBackend *backend, const gchar *filter, const gchar *search)
{
	g_return_if_fail (backend != NULL);
	pk_backend_set_allow_cancel (backend, TRUE);
	pk_backend_spawn_helper (backend, "search-group.py", filter, search, NULL);
}

static gboolean inited = FALSE;

#define APT_DB PK_DB_DIR "/apt.db"

extern "C" void backend_init_search(PkBackend *backend)
{
	if (!inited)
	{
		gchar *apt_fname = NULL;
		if (pkgInitConfig(*_config) == false)
			pk_debug("pkginitconfig was false");
		if (pkgInitSystem(*_config, _system) == false)
			pk_debug("pkginitsystem was false");

		apt_fname = g_strconcat(
				_config->Find("Dir").c_str(),
				_config->Find("Dir::Cache").c_str(),
				_config->Find("Dir::Cache::pkgcache").c_str(),
				NULL);

		//sqlite_set_installed_check(is_installed);
		sqlite_init_cache(backend, APT_DB, apt_fname, apt_build_db);
		g_free(apt_fname);
		inited = TRUE;
	}
}

extern "C" void backend_finish_search(PkBackend *backend)
{
	sqlite_finish_cache(backend);
}
