/*-------------------------------------------------------------------------
 *
 * pg_probackup.c: Backup/Recovery manager for PostgreSQL.
 *
 * Portions Copyright (c) 2009-2013, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 * Portions Copyright (c) 2015-2017, Postgres Professional
 *
 *-------------------------------------------------------------------------
 */

#include "pg_probackup.h"
#include "streamutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>

const char *PROGRAM_VERSION	= "1.1.5";
const char *PROGRAM_URL		= "https://github.com/postgrespro/pg_probackup";
const char *PROGRAM_EMAIL	= "https://github.com/postgrespro/pg_probackup/issues";

/* path configuration */
char *backup_path;
char *pgdata;
char arclog_path[MAXPGPATH];

/* directory configuration */
pgBackup	current;
ProbackupSubcmd	backup_subcmd;

char 	*backup_id_string_param = NULL;
bool	backup_logs = false;

bool	smooth_checkpoint;
int		num_threads = 1;
bool	stream_wal = false;
bool	from_replica = false;
bool	progress = false;
bool	delete_wal = false;
bool	delete_expired = false;
bool	apply_to_all = false;
bool	force_delete = false;
uint32  archive_timeout = 300; /* Wait timeout for WAL segment archiving */

uint64	system_identifier = 0;

uint32	retention_redundancy = 0;
uint32	retention_window = 0;

/* restore configuration */
static char		   *target_time;
static char		   *target_xid;
static char		   *target_inclusive;
static TimeLineID	target_tli;

static void opt_backup_mode(pgut_option *opt, const char *arg);

static pgut_option options[] =
{
	/* directory options */
	{ 's', 'D', "pgdata",				&pgdata,		SOURCE_CMDLINE },
	{ 's', 'B', "backup-path",			&backup_path,	SOURCE_CMDLINE },
	/* common options */
	{ 'u', 'j', "threads",				&num_threads,	SOURCE_CMDLINE },
	{ 'b', 8, "stream",					&stream_wal,	SOURCE_CMDLINE },
	{ 'b', 11, "progress",				&progress,		SOURCE_CMDLINE },
	{ 's', 'i', "backup-id",			&backup_id_string_param, SOURCE_CMDLINE },
	/* backup options */
	{ 'b', 10, "backup-pg-log",			&backup_logs,	SOURCE_CMDLINE },
	{ 'f', 'b', "backup-mode",			opt_backup_mode,	SOURCE_CMDLINE },
	{ 'b', 'C', "smooth-checkpoint",	&smooth_checkpoint,	SOURCE_CMDLINE },
	{ 's', 'S', "slot",					&replication_slot,	SOURCE_CMDLINE },
	{ 'u',  2, "archive-timeout",		&archive_timeout,	SOURCE_CMDLINE },
	{ 'b',  19, "delete-expired",		&delete_expired,	SOURCE_CMDLINE },
	/* restore options */
	{ 's',  3, "time",					&target_time,		SOURCE_CMDLINE },
	{ 's',  4, "xid",					&target_xid,		SOURCE_CMDLINE },
	{ 's',  5, "inclusive",				&target_inclusive,	SOURCE_CMDLINE },
	{ 'u',  6, "timeline",				&target_tli,		SOURCE_CMDLINE },
	{ 'f', 'T', "tablespace-mapping",	opt_tablespace_map,	SOURCE_CMDLINE },
	/* delete options */
	{ 'b', 12, "wal",					&delete_wal,		SOURCE_CMDLINE },
	{ 'b', 16, "expired",				&delete_expired,	SOURCE_CMDLINE },
	{ 'b', 17, "all",					&apply_to_all,		SOURCE_CMDLINE },
	/* TODO not implemented yet */
	{ 'b', 18, "force",					&force_delete,		SOURCE_CMDLINE },
	/* configure options */
	{ 'u', 13, "retention-redundancy", &retention_redundancy,	SOURCE_CMDLINE },
	{ 'u', 14, "retention-window",	&retention_window,		SOURCE_CMDLINE },
	/* other */
	{ 'U', 15, "system-identifier",		&system_identifier,		SOURCE_FILE_STRICT },

	{ 's', 'd', "pgdatabase"	, &pgut_dbname, SOURCE_CMDLINE },
	{ 's', 'h', "pghost"		, &host, SOURCE_CMDLINE },
	{ 's', 'p', "pgport"		, &port, SOURCE_CMDLINE },
	{ 's', 'U', "pguser"	, &username, SOURCE_CMDLINE },
	{ 'b', 'q', "quiet"		, &quiet, SOURCE_CMDLINE },
	{ 'b', 'v', "verbose"	, &verbose, SOURCE_CMDLINE },
	{ 'B', 'w', "no-password"	, &prompt_password, SOURCE_CMDLINE },
	{ 0 }
};

/*
 * Entry point of pg_probackup command.
 */
int
main(int argc, char *argv[])
{
	int				i;

	/* initialize configuration */
	pgBackup_init(&current);

	PROGRAM_NAME = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], "pgscripts");

	/* Parse subcommands and non-subcommand options */
	if (argc > 1)
	{
		if (strcmp(argv[1], "init") == 0)
			backup_subcmd = INIT;
		else if (strcmp(argv[1], "backup") == 0)
			backup_subcmd = BACKUP;
		else if (strcmp(argv[1], "restore") == 0)
			backup_subcmd = RESTORE;
		else if (strcmp(argv[1], "validate") == 0)
			backup_subcmd = VALIDATE;
		else if (strcmp(argv[1], "show") == 0)
			backup_subcmd = SHOW;
		else if (strcmp(argv[1], "delete") == 0)
			backup_subcmd = DELETE;
		else if (strcmp(argv[1], "config") == 0)
			backup_subcmd = CONFIGURE;
		else if (strcmp(argv[1], "--help") == 0
				|| strcmp(argv[1], "help") == 0
				|| strcmp(argv[1], "-?") == 0)
		{
			help(true);
			exit(0);
		}
		else if (strcmp(argv[1], "--version") == 0
				 || strcmp(argv[1], "version") == 0
				 || strcmp(argv[1], "-V") == 0)
		{
			fprintf(stderr, "%s %s\n", PROGRAM_NAME, PROGRAM_VERSION);
			exit(0);
		}
		else
			elog(ERROR, "Invalid subcommand");
	}

	/* Parse command line arguments */
	i = pgut_getopt(argc, argv, options);

	if (backup_path == NULL)
	{
		/* Try to read BACKUP_PATH from environment variable */
		backup_path = getenv("BACKUP_PATH");
		if (backup_path == NULL)
			elog(ERROR, "required parameter not specified: BACKUP_PATH (-B, --backup-path)");
	}
	else
	{
		char		path[MAXPGPATH];
		/* Check if backup_path is directory. */
		struct stat stat_buf;
		int			rc = stat(backup_path, &stat_buf);

		/* If rc == -1,  there is no file or directory. So it's OK. */
		if (rc != -1 && !S_ISDIR(stat_buf.st_mode))
			elog(ERROR, "-B, --backup-path must be a path to directory");

		/* Do not read options from file or env if we're going to set them */
		if (backup_subcmd != CONFIGURE)
		{
			/* Read options from configuration file */
			join_path_components(path, backup_path, BACKUP_CATALOG_CONF_FILE);
			pgut_readopt(path, options, ERROR);

			/* Read environment variables */
			pgut_getopt_env(options);
		}
	}

	if (backup_id_string_param != NULL)
	{
		current.backup_id = base36dec(backup_id_string_param);
		if (current.backup_id == 0)
			elog(ERROR, "Invalid backup-id");
	}

	/* setup stream options */
	if (pgut_dbname != NULL)
		dbname = pstrdup(pgut_dbname);
	if (host != NULL)
		dbhost = pstrdup(host);
	if (port != NULL)
		dbport = pstrdup(port);
	if (username != NULL)
		dbuser = pstrdup(username);

	/* path must be absolute */
	if (!is_absolute_path(backup_path))
		elog(ERROR, "-B, --backup-path must be an absolute path");
	if (pgdata != NULL && !is_absolute_path(pgdata))
		elog(ERROR, "-D, --pgdata must be an absolute path");

	join_path_components(arclog_path, backup_path, "wal");

	/* setup exclusion list for file search */
	for (i = 0; pgdata_exclude_dir[i]; i++);		/* find first empty slot */

	if(!backup_logs)
		pgdata_exclude_dir[i++] = "pg_log";

	if (target_time != NULL && target_xid != NULL)
		elog(ERROR, "You can't specify recovery-target-time and recovery-target-xid at the same time");

	if (num_threads < 1)
		num_threads = 1;

	/* do actual operation */
	switch (backup_subcmd)
	{
		case INIT:
			return do_init();
		case BACKUP:
			return do_backup();
		case RESTORE:
			return do_restore_or_validate(current.backup_id,
						  target_time, target_xid,
						  target_inclusive, target_tli,
						  true);
		case VALIDATE:
			return do_restore_or_validate(current.backup_id,
						  target_time, target_xid,
						  target_inclusive, target_tli,
						  false);
		case SHOW:
			return do_show(current.backup_id);
		case DELETE:
			if (delete_expired && backup_id_string_param)
				elog(ERROR, "You cannot specify --delete-expired and --backup-id options together");
			if (delete_expired)
				return do_retention_purge();
			else
				return do_delete(current.backup_id);
		case CONFIGURE:
			/* TODO fixit */
			if (argc == 4)
				return do_configure(true);
			else
				return do_configure(false);
	}

	return 0;
}

/* TODO Update help in accordance with new options */
void
pgut_help(bool details)
{
	printf(_("%s manage backup/recovery of PostgreSQL database.\n\n"), PROGRAM_NAME);
	printf(_("Usage:\n"));
	printf(_("  %s [option...] init\n"), PROGRAM_NAME);
	printf(_("  %s [option...] backup\n"), PROGRAM_NAME);
	printf(_("  %s [option...] restore [backup-ID]\n"), PROGRAM_NAME);
	printf(_("  %s [option...] show [backup-ID]\n"), PROGRAM_NAME);
	printf(_("  %s [option...] validate [backup-ID]\n"), PROGRAM_NAME);
	printf(_("  %s [option...] delete backup-ID\n"), PROGRAM_NAME);
	printf(_("  %s [option...] delwal [backup-ID]\n"), PROGRAM_NAME);
	printf(_("  %s [option...] retention show|purge\n"), PROGRAM_NAME);

	if (!details)
		return;

	printf(_("\nCommon Options:\n"));
	printf(_("  -B, --backup-path=PATH    location of the backup storage area\n"));
	printf(_("  -D, --pgdata=PATH         location of the database storage area\n"));
	/*printf(_("  -c, --check               show what would have been done\n"));*/
	printf(_("\nBackup options:\n"));
	printf(_("  -b, --backup-mode=MODE    backup mode (full, page, ptrack)\n"));
	printf(_("  -C, --smooth-checkpoint   do smooth checkpoint before backup\n"));
	printf(_("      --stream              stream the transaction log and include it in the backup\n"));
	printf(_("      --archive-timeout     wait timeout for WAL segment archiving\n"));
	printf(_("  -S, --slot=SLOTNAME       replication slot to use\n"));
	printf(_("      --backup-pg-log       backup of pg_log directory\n"));
	printf(_("  -j, --threads=NUM         number of parallel threads\n"));
	printf(_("      --progress            show progress\n"));
	printf(_("\nRestore options:\n"));
	printf(_("      --time                time stamp up to which recovery will proceed\n"));
	printf(_("      --xid                 transaction ID up to which recovery will proceed\n"));
	printf(_("      --inclusive           whether we stop just after the recovery target\n"));
	printf(_("      --timeline            recovering into a particular timeline\n"));
	printf(_("  -T, --tablespace-mapping=OLDDIR=NEWDIR\n"));
	printf(_("                            relocate the tablespace in directory OLDDIR to NEWDIR\n"));
	printf(_("  -j, --threads=NUM         number of parallel threads\n"));
	printf(_("      --progress            show progress\n"));
	printf(_("\nDelete options:\n"));
	printf(_("      --wal                 remove unnecessary wal files\n"));
	printf(_("\nRetention options:\n"));
	printf(_("      --redundancy          specifies how many full backups purge command should keep\n"));
	printf(_("      --window              specifies the number of days of recoverability\n"));
}

static void
opt_backup_mode(pgut_option *opt, const char *arg)
{
	current.backup_mode = parse_backup_mode(arg);
}
