# contrib/pg_probackup/nls.mk
CATALOG_NAME     = pg_probackup
AVAIL_LANGUAGES  = ru
GETTEXT_FILES    = src/help.c
GETTEXT_TRIGGERS = $(FRONTEND_COMMON_GETTEXT_TRIGGERS)
GETTEXT_FLAGS    = $(FRONTEND_COMMON_GETTEXT_FLAGS)
