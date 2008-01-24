/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 Richard Hughes <richard@hughsie.com>
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

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>

#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */

#include <glib/gi18n.h>
#include <glib/gprintf.h>

#include <gmodule.h>
#include <libgbus.h>

#include <pk-common.h>
#include <pk-package-id.h>
#include <pk-enum.h>
#include <pk-network.h>

#include "pk-debug.h"
#include "pk-backend-internal.h"
#include "pk-backend-spawn.h"
#include "pk-marshal.h"
#include "pk-enum.h"
#include "pk-spawn.h"
#include "pk-time.h"
#include "pk-inhibit.h"
#include "pk-thread-list.h"

#define PK_BACKEND_SPAWN_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), PK_TYPE_BACKEND_SPAWN, PkBackendSpawnPrivate))
#define PK_BACKEND_SPAWN_PERCENTAGE_INVALID	101

struct PkBackendSpawnPrivate
{
	PkSpawn			*spawn;
	PkBackend		*backend;
	gchar			*name;
	gulong			 signal_finished;
	gulong			 signal_stdout;
	gulong			 signal_stderr;
};

G_DEFINE_TYPE (PkBackendSpawn, pk_backend_spawn, G_TYPE_OBJECT)

/**
 * pk_backend_spawn_parse_common_output:
 *
 * If you are editing this function creating a new backend,
 * then you are probably doing something wrong.
 **/
static gboolean
pk_backend_spawn_parse_common_output (PkBackendSpawn *backend_spawn, const gchar *line)
{
	gchar **sections;
	guint size;
	gchar *command;
	gboolean ret = TRUE;
	PkInfoEnum info;
	PkRestartEnum restart;
	PkGroupEnum group;
	gulong package_size;

	g_return_val_if_fail (backend_spawn != NULL, FALSE);

	/* check if output line */
	if (line == NULL || strstr (line, "\t") == NULL)
		return FALSE;

	/* split by tab */
	sections = g_strsplit (line, "\t", 0);
	command = sections[0];

	/* get size */
	size = g_strv_length (sections);

	if (pk_strequal (command, "package") == TRUE) {
		if (size != 4) {
			pk_warning ("invalid command '%s'", command);
			ret = FALSE;
			goto out;
		}
		if (pk_package_id_check (sections[2]) == FALSE) {
			pk_warning ("invalid package_id");
			ret = FALSE;
			goto out;
		}
		info = pk_info_enum_from_text (sections[1]);
		if (info == PK_INFO_ENUM_UNKNOWN) {
			pk_backend_message (backend_spawn->priv->backend, PK_MESSAGE_ENUM_DAEMON,
					    "Info enum not recognised, and hence ignored: '%s'", sections[1]);
			ret = FALSE;
			goto out;
		}
		pk_backend_package (backend_spawn->priv->backend, info, sections[2], sections[3]);
	} else if (pk_strequal (command, "description") == TRUE) {
		if (size != 7) {
			pk_warning ("invalid command '%s'", command);
			ret = FALSE;
			goto out;
		}
		group = pk_group_enum_from_text (sections[3]);

		/* ITS4: ignore, checked for overflow */
		package_size = atol (sections[6]);
		if (package_size > 1073741824) {
			pk_warning ("package size cannot be larger than one Gb");
			ret = FALSE;
			goto out;
		}
		pk_backend_description (backend_spawn->priv->backend, sections[1], sections[2],
					group, sections[4], sections[5],
					package_size);
	} else if (pk_strequal (command, "files") == TRUE) {
		if (size != 3) {
			pk_warning ("invalid command '%s'", command);
			ret = FALSE;
			goto out;
		}
		pk_backend_files (backend_spawn->priv->backend, sections[1], sections[2]);
	} else if (pk_strequal (command, "repo-detail") == TRUE) {
		if (size != 4) {
			pk_warning ("invalid command '%s'", command);
			ret = FALSE;
			goto out;
		}
		if (pk_strequal (sections[3], "true") == TRUE) {
			pk_backend_repo_detail (backend_spawn->priv->backend, sections[1], sections[2], TRUE);
		} else if (pk_strequal (sections[3], "false") == TRUE) {
			pk_backend_repo_detail (backend_spawn->priv->backend, sections[1], sections[2], FALSE);
		} else {
			pk_warning ("invalid qualifier '%s'", sections[3]);
			ret = FALSE;
			goto out;
		}
	} else if (pk_strequal (command, "updatedetail") == TRUE) {
		if (size != 9) {
			pk_warning ("invalid command '%s'", command);
			ret = FALSE;
			goto out;
		}
		restart = pk_restart_enum_from_text (sections[7]);
		if (restart == PK_RESTART_ENUM_UNKNOWN) {
			pk_backend_message (backend_spawn->priv->backend, PK_MESSAGE_ENUM_DAEMON,
					    "Restart enum not recognised, and hence ignored: '%s'", sections[7]);
			ret = FALSE;
			goto out;
		}
		pk_backend_update_detail (backend_spawn->priv->backend, sections[1], sections[2],
					  sections[3], sections[4], sections[5],
					  sections[6], restart, sections[8]);
	} else {
		pk_warning ("invalid command '%s'", command);
	}
out:
	g_strfreev (sections);
	return ret;
}

/**
 * pk_backend_spawn_parse_common_error:
 *
 * If you are editing this function creating a new backend,
 * then you are probably doing something wrong.
 **/
static gboolean
pk_backend_spawn_parse_common_error (PkBackendSpawn *backend_spawn, const gchar *line)
{
	gchar **sections;
	guint size;
	gint percentage;
	gchar *command;
	gchar *text;
	PkErrorCodeEnum error_enum;
	PkStatusEnum status_enum;
	PkMessageEnum message_enum;
	PkRestartEnum restart_enum;
	gboolean ret = TRUE;

	g_return_val_if_fail (backend_spawn != NULL, FALSE);

	/* check if output line */
	if (line == NULL)
		return FALSE;

	/* split by tab */
	sections = g_strsplit (line, "\t", 0);
	command = sections[0];

	/* get size */
	for (size=0; sections[size]; size++);

	if (pk_strequal (command, "percentage") == TRUE) {
		if (size != 2) {
			pk_warning ("invalid command '%s'", command);
			ret = FALSE;
			goto out;
		}
		ret = pk_strtoint (sections[1], &percentage);
		if (ret == FALSE) {
			pk_warning ("invalid percentage value %s", sections[1]);
		} else if (percentage < 0 || percentage > 100) {
			pk_warning ("invalid percentage value %i", percentage);
			ret = FALSE;
		} else {
			pk_backend_set_percentage (backend_spawn->priv->backend, percentage);
		}
	} else if (pk_strequal (command, "subpercentage") == TRUE) {
		if (size != 2) {
			pk_warning ("invalid command '%s'", command);
			ret = FALSE;
			goto out;
		}
		ret = pk_strtoint (sections[1], &percentage);
		if (ret == FALSE) {
			pk_warning ("invalid subpercentage value %s", sections[1]);
		} else if (percentage < 0 || percentage > 100) {
			pk_warning ("invalid subpercentage value %i", percentage);
			ret = FALSE;
		} else {
			pk_backend_set_sub_percentage (backend_spawn->priv->backend, percentage);
		}
	} else if (pk_strequal (command, "error") == TRUE) {
		if (size != 3) {
			pk_warning ("invalid command '%s'", command);
			ret = FALSE;
			goto out;
		}
		error_enum = pk_error_enum_from_text (sections[1]);
		if (error_enum == PK_ERROR_ENUM_UNKNOWN) {
			pk_backend_message (backend_spawn->priv->backend, PK_MESSAGE_ENUM_DAEMON,
					    "Error enum not recognised, and hence ignored: '%s'", sections[1]);
			ret = FALSE;
			goto out;
		}
		/* convert back all the ;'s to newlines */
		text = g_strdup (sections[2]);
		g_strdelimit (text, ";", '\n');
		pk_backend_error_code (backend_spawn->priv->backend, error_enum, text);
		g_free (text);
	} else if (pk_strequal (command, "requirerestart") == TRUE) {
		if (size != 3) {
			pk_warning ("invalid command '%s'", command);
			ret = FALSE;
			goto out;
		}
		restart_enum = pk_restart_enum_from_text (sections[1]);
		pk_backend_require_restart (backend_spawn->priv->backend, restart_enum, sections[2]);
	} else if (pk_strequal (command, "message") == TRUE) {
		if (size != 3) {
			pk_warning ("invalid command '%s'", command);
			ret = FALSE;
			goto out;
		}
		message_enum = pk_message_enum_from_text (sections[1]);
		if (message_enum == PK_MESSAGE_ENUM_UNKNOWN) {
			pk_backend_message (backend_spawn->priv->backend, PK_MESSAGE_ENUM_DAEMON,
					    "Message enum not recognised, and hence ignored: '%s'", sections[1]);
			ret = FALSE;
			goto out;
		}
		pk_backend_message (backend_spawn->priv->backend, message_enum, sections[2]);
	} else if (pk_strequal (command, "change-transaction-data") == TRUE) {
		if (size != 2) {
			pk_warning ("invalid command '%s'", command);
			ret = FALSE;
			goto out;
		}
		pk_backend_set_transaction_data (backend_spawn->priv->backend, sections[1]);
	} else if (pk_strequal (command, "status") == TRUE) {
		if (size != 2) {
			pk_warning ("invalid command '%s'", command);
			ret = FALSE;
			goto out;
		}
		status_enum = pk_status_enum_from_text (sections[1]);
		if (status_enum == PK_STATUS_ENUM_UNKNOWN) {
			pk_backend_message (backend_spawn->priv->backend, PK_MESSAGE_ENUM_DAEMON,
					    "Status enum not recognised, and hence ignored: '%s'", sections[1]);
			ret = FALSE;
			goto out;
		}
		pk_backend_set_status (backend_spawn->priv->backend, status_enum);
	} else if (pk_strequal (command, "allow-cancel") == TRUE) {
		if (size != 2) {
			pk_warning ("invalid command '%s'", command);
			ret = FALSE;
			goto out;
		}
		if (pk_strequal (sections[1], "true") == TRUE) {
			pk_backend_set_allow_cancel (backend_spawn->priv->backend, TRUE);
		} else if (pk_strequal (sections[1], "false") == TRUE) {
			pk_backend_set_allow_cancel (backend_spawn->priv->backend, FALSE);
		} else {
			pk_warning ("invalid section '%s'", sections[1]);
			ret = FALSE;
			goto out;
		}
	} else if (pk_strequal (command, "no-percentage-updates") == TRUE) {
		if (size != 1) {
			pk_warning ("invalid command '%s'", command);
			ret = FALSE;
			goto out;
		}
		pk_backend_no_percentage_updates (backend_spawn->priv->backend);
	} else if (pk_strequal (command, "repo-signature-required") == TRUE) {
		ret = FALSE;
		goto out;
	} else {
		pk_warning ("invalid command '%s'", command);
	}
out:
	g_strfreev (sections);
	return ret;
}

/**
 * pk_backend_spawn_helper_delete:
 **/
static gboolean
pk_backend_spawn_helper_delete (PkBackendSpawn *backend_spawn)
{
	g_return_val_if_fail (backend_spawn != NULL, FALSE);
	if (backend_spawn->priv->spawn == NULL) {
		pk_warning ("spawn object not in use");
		return FALSE;
	}
	pk_debug ("deleting spawn %p", backend_spawn->priv->spawn);
	g_signal_handler_disconnect (backend_spawn->priv->spawn, backend_spawn->priv->signal_finished);
	g_signal_handler_disconnect (backend_spawn->priv->spawn, backend_spawn->priv->signal_stdout);
	g_signal_handler_disconnect (backend_spawn->priv->spawn, backend_spawn->priv->signal_stderr);
	g_object_unref (backend_spawn->priv->spawn);
	backend_spawn->priv->spawn = NULL;
	return TRUE;
}

/**
 * pk_backend_spawn_finished_cb:
 **/
static void
pk_backend_spawn_finished_cb (PkSpawn *spawn, PkExitEnum exit, PkBackendSpawn *backend_spawn)
{
	g_return_if_fail (backend_spawn != NULL);

	pk_debug ("deleting spawn %p, exit %s", backend_spawn, pk_exit_enum_to_text (exit));
	pk_backend_spawn_helper_delete (backend_spawn);

	/* if we quit the process, set an error */
	if (exit == PK_EXIT_ENUM_QUIT) {
		/* we just call this failed, and set an error */
		pk_backend_error_code (backend_spawn->priv->backend, PK_ERROR_ENUM_PROCESS_QUIT,
				       "Transaction was cancelled");
	}

	/* if we killed the process, set an error */
	if (exit == PK_EXIT_ENUM_KILL) {
		/* we just call this failed, and set an error */
		pk_backend_error_code (backend_spawn->priv->backend, PK_ERROR_ENUM_PROCESS_KILL,
				       "Transaction was cancelled");
	}

	if (FALSE && /*TODO: backend_spawn->priv->set_error == FALSE*/
	    exit == PK_EXIT_ENUM_FAILED) {
		pk_backend_error_code (backend_spawn->priv->backend, PK_ERROR_ENUM_INTERNAL_ERROR,
				       "Helper returned non-zero return value but did not set error");
	}
	pk_backend_finished (backend_spawn->priv->backend);
}

/**
 * pk_backend_spawn_stdout_cb:
 **/
static void
pk_backend_spawn_stdout_cb (PkBackendSpawn *spawn, const gchar *line, PkBackendSpawn *backend_spawn)
{
	g_return_if_fail (backend_spawn != NULL);
	pk_debug ("stdout from %p = '%s'", spawn, line);
	pk_backend_spawn_parse_common_output (backend_spawn, line);
}

/**
 * pk_backend_spawn_stderr_cb:
 **/
static void
pk_backend_spawn_stderr_cb (PkBackendSpawn *spawn, const gchar *line, PkBackendSpawn *backend_spawn)
{
	g_return_if_fail (backend_spawn != NULL);
	pk_debug ("stderr from %p = '%s'", spawn, line);
	pk_backend_spawn_parse_common_error (backend_spawn, line);
}

/**
 * pk_backend_spawn_helper_new:
 **/
static gboolean
pk_backend_spawn_helper_new (PkBackendSpawn *backend_spawn)
{
	g_return_val_if_fail (backend_spawn != NULL, FALSE);

	if (backend_spawn->priv->spawn != NULL) {
		pk_warning ("spawn object already in use");
		return FALSE;
	}
	backend_spawn->priv->spawn = pk_spawn_new ();
	pk_debug ("allocating spawn %p", backend_spawn->priv->spawn);
	backend_spawn->priv->signal_finished =
		g_signal_connect (backend_spawn->priv->spawn, "finished",
				  G_CALLBACK (pk_backend_spawn_finished_cb), backend_spawn);
	backend_spawn->priv->signal_stdout =
		g_signal_connect (backend_spawn->priv->spawn, "stdout",
				  G_CALLBACK (pk_backend_spawn_stdout_cb), backend_spawn);
	backend_spawn->priv->signal_stderr =
		g_signal_connect (backend_spawn->priv->spawn, "stderr",
				  G_CALLBACK (pk_backend_spawn_stderr_cb), backend_spawn);
	return TRUE;
}

/**
 * pk_backend_spawn_helper_internal:
 **/
static gboolean
pk_backend_spawn_helper_internal (PkBackendSpawn *backend_spawn, const gchar *script, const gchar *argument)
{
	gboolean ret;
	gchar *filename;
	gchar *command;

	g_return_val_if_fail (backend_spawn != NULL, FALSE);

#if PK_BUILD_LOCAL
	/* prefer the local version */
	filename = g_build_filename ("..", "backends", backend_spawn->priv->name, "helpers", script, NULL);
	if (g_file_test (filename, G_FILE_TEST_EXISTS) == FALSE) {
		pk_debug ("local helper not found '%s'", filename);
		g_free (filename);
		filename = g_build_filename (DATADIR, "PackageKit", "helpers", backend_spawn->priv->name, script, NULL);
	}
#else
	filename = g_build_filename (DATADIR, "PackageKit", "helpers", backend_spawn->priv->name, script, NULL);
#endif
	pk_debug ("using spawn filename %s", filename);

	if (argument != NULL) {
		command = g_strdup_printf ("%s %s", filename, argument);
	} else {
		command = g_strdup (filename);
	}

	pk_backend_spawn_helper_new (backend_spawn);
	ret = pk_spawn_command (backend_spawn->priv->spawn, command);
	if (ret == FALSE) {
		pk_backend_spawn_helper_delete (backend_spawn);
		pk_backend_error_code (backend_spawn->priv->backend, PK_ERROR_ENUM_INTERNAL_ERROR, "Spawn of helper '%s' failed", command);
		pk_backend_finished (backend_spawn->priv->backend);
	}
	g_free (filename);
	g_free (command);
	return ret;
}

/**
 * pk_backend_spawn_get_name:
 **/
const gchar *
pk_backend_spawn_get_name (PkBackendSpawn *backend_spawn)
{
	g_return_val_if_fail (backend_spawn != NULL, FALSE);
	return backend_spawn->priv->name;
}

/**
 * pk_backend_spawn_set_name:
 **/
gboolean
pk_backend_spawn_set_name (PkBackendSpawn *backend_spawn, const gchar *name)
{
	g_return_val_if_fail (backend_spawn != NULL, FALSE);

	g_free (backend_spawn->priv->name);
	backend_spawn->priv->name = g_strdup (name);
	return TRUE;
}

/**
 * pk_backend_spawn_kill:
 **/
gboolean
pk_backend_spawn_kill (PkBackendSpawn *backend_spawn)
{
	g_return_val_if_fail (backend_spawn != NULL, FALSE);

	if (backend_spawn->priv->spawn == NULL) {
		pk_warning ("cannot kill missing process");
		return FALSE;
	}
	pk_spawn_kill (backend_spawn->priv->spawn);
	return TRUE;
}

/**
 * pk_backend_spawn_helper:
 **/
gboolean
pk_backend_spawn_helper (PkBackendSpawn *backend_spawn, const gchar *script, const gchar *first_element, ...)
{
	gboolean ret;
	va_list args;
	gchar *arguments;

	g_return_val_if_fail (backend_spawn != NULL, FALSE);

	/* get the argument list */
	va_start (args, first_element);
	arguments = pk_strbuild_va (first_element, &args);
	va_end (args);

	ret = pk_backend_spawn_helper_internal (backend_spawn, script, arguments);
	g_free (arguments);
	return ret;
}

/**
 * pk_backend_spawn_finalize:
 **/
static void
pk_backend_spawn_finalize (GObject *object)
{
	PkBackendSpawn *backend_spawn;
	g_return_if_fail (object != NULL);
	g_return_if_fail (PK_IS_BACKEND_SPAWN (object));

	backend_spawn = PK_BACKEND_SPAWN (object);
	if (backend_spawn->priv->spawn != NULL) {
		pk_backend_spawn_helper_delete (backend_spawn);
	}
	g_free (backend_spawn->priv->name);
	g_object_unref (backend_spawn->priv->backend);

	G_OBJECT_CLASS (pk_backend_spawn_parent_class)->finalize (object);
}

/**
 * pk_backend_spawn_class_init:
 **/
static void
pk_backend_spawn_class_init (PkBackendSpawnClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = pk_backend_spawn_finalize;
	g_type_class_add_private (klass, sizeof (PkBackendSpawnPrivate));
}

/**
 * pk_backend_spawn_init:
 **/
static void
pk_backend_spawn_init (PkBackendSpawn *backend_spawn)
{
	backend_spawn->priv = PK_BACKEND_SPAWN_GET_PRIVATE (backend_spawn);
	backend_spawn->priv->spawn = NULL;
	backend_spawn->priv->name = NULL;
	backend_spawn->priv->backend = pk_backend_new ();
}

/**
 * pk_backend_spawn_new:
 **/
PkBackendSpawn *
pk_backend_spawn_new (void)
{
	PkBackendSpawn *backend_spawn;
	backend_spawn = g_object_new (PK_TYPE_BACKEND_SPAWN, NULL);
	return PK_BACKEND_SPAWN (backend_spawn);
}

/***************************************************************************
 ***                          MAKE CHECK TESTS                           ***
 ***************************************************************************/
#ifdef PK_BUILD_TESTS
#include <libselftest.h>

void
libst_backend_spawn (LibSelfTest *test)
{
	PkBackendSpawn *backend_spawn;
	const gchar *text;
	gboolean ret;
	GTimer *timer;

	timer = g_timer_new ();

	if (libst_start (test, "PkBackendSpawn", CLASS_AUTO) == FALSE) {
		return;
	}

	/************************************************************/
	libst_title (test, "get an backend_spawn");
	backend_spawn = pk_backend_spawn_new ();
	if (backend_spawn != NULL) {
		libst_success (test, NULL);
	} else {
		libst_failed (test, NULL);
	}

	/************************************************************/
	libst_title (test, "get backend name");
	text = pk_backend_spawn_get_name (backend_spawn);
	if (text == NULL) {
		libst_success (test, NULL);
	} else {
		libst_failed (test, "invalid name %s", text);
	}

	/************************************************************/
	libst_title (test, "set backend name");
	ret = pk_backend_spawn_set_name (backend_spawn, "dummy");
	if (ret == TRUE) {
		libst_success (test, NULL);
	} else {
		libst_failed (test, "invalid set name");
	}

	/************************************************************/
	libst_title (test, "get backend name");
	text = pk_backend_spawn_get_name (backend_spawn);
	if (pk_strequal(text, "dummy") == TRUE) {
		libst_success (test, NULL);
	} else {
		libst_failed (test, "invalid name %s", text);
	}

	/************************************************************
	 **********       Check parsing common error      ***********
	 ************************************************************/
	libst_title (test, "test pk_backend_spawn_parse_common_error Percentage1");
	ret = pk_backend_spawn_parse_common_error (backend_spawn, "percentage\t0");
	if (ret == TRUE) {
		libst_success (test, NULL);
	} else {
		libst_failed (test, "did not validate correctly");
	}

	/************************************************************/
	libst_title (test, "test pk_backend_spawn_parse_common_error Percentage2");
	ret = pk_backend_spawn_parse_common_error (backend_spawn, "percentage\tbrian");
	if (ret == FALSE) {
		libst_success (test, NULL);
	} else {
		libst_failed (test, "did not validate correctly");
	}

	/************************************************************/
	libst_title (test, "test pk_backend_spawn_parse_common_error Percentage3");
	ret = pk_backend_spawn_parse_common_error (backend_spawn, "percentage\t12345");
	if (ret == FALSE) {
		libst_success (test, NULL);
	} else {
		libst_failed (test, "did not validate correctly");
	}

	/************************************************************/
	libst_title (test, "test pk_backend_spawn_parse_common_error Percentage4");
	ret = pk_backend_spawn_parse_common_error (backend_spawn, "percentage\t");
	if (ret == FALSE) {
		libst_success (test, NULL);
	} else {
		libst_failed (test, "did not validate correctly");
	}

	/************************************************************/
	libst_title (test, "test pk_backend_spawn_parse_common_error Percentage5");
	ret = pk_backend_spawn_parse_common_error (backend_spawn, "percentage");
	if (ret == FALSE) {
		libst_success (test, NULL);
	} else {
		libst_failed (test, "did not validate correctly");
	}

	/************************************************************/
	libst_title (test, "test pk_backend_spawn_parse_common_error Subpercentage");
	ret = pk_backend_spawn_parse_common_error (backend_spawn, "subpercentage\t17");
	if (ret == TRUE) {
		libst_success (test, NULL);
	} else {
		libst_failed (test, "did not validate correctly");
	}

	/************************************************************/
	libst_title (test, "test pk_backend_spawn_parse_common_error NoPercentageUpdates");
	ret = pk_backend_spawn_parse_common_error (backend_spawn, "no-percentage-updates");
	if (ret == TRUE) {
		libst_success (test, NULL);
	} else {
		libst_failed (test, "did not validate correctly");
	}

	/************************************************************/
	libst_title (test, "test pk_backend_spawn_parse_common_error Error1");
	ret = pk_backend_spawn_parse_common_error (backend_spawn, "error\ttransaction-error\tdescription text");
	if (ret == TRUE) {
		libst_success (test, NULL);
	} else {
		libst_failed (test, "did not validate correctly");
	}

	/************************************************************/
	libst_title (test, "test pk_backend_spawn_parse_common_error failure");
	ret = pk_backend_spawn_parse_common_error (backend_spawn, "error\tnot-present-woohoo\tdescription text");
	if (ret == FALSE) {
		libst_success (test, NULL);
	} else {
		libst_failed (test, "did not detect incorrect enum");
	}

	/************************************************************/
	libst_title (test, "test pk_backend_spawn_parse_common_error Status");
	ret = pk_backend_spawn_parse_common_error (backend_spawn, "status\tquery");
	if (ret == TRUE) {
		libst_success (test, NULL);
	} else {
		libst_failed (test, "did not validate correctly");
	}

	/************************************************************/
	libst_title (test, "test pk_backend_spawn_parse_common_error RequireRestart");
	ret = pk_backend_spawn_parse_common_error (backend_spawn, "requirerestart\tsystem\tdetails about the restart");
	if (ret == TRUE) {
		libst_success (test, NULL);
	} else {
		libst_failed (test, "did not validate correctly");
	}

	/************************************************************/
	libst_title (test, "test pk_backend_spawn_parse_common_error AllowUpdate1");
	ret = pk_backend_spawn_parse_common_error (backend_spawn, "allow-cancel\ttrue");
	if (ret == TRUE) {
		libst_success (test, NULL);
	} else {
		libst_failed (test, "did not validate correctly");
	}

	/************************************************************/
	libst_title (test, "test pk_backend_spawn_parse_common_error AllowUpdate2");
	ret = pk_backend_spawn_parse_common_error (backend_spawn, "allow-cancel\tbrian");
	if (ret == FALSE) {
		libst_success (test, NULL);
	} else {
		libst_failed (test, "did not validate correctly");
	}

	g_timer_destroy (timer);
	g_object_unref (backend_spawn);

	libst_end (test);
}
#endif

