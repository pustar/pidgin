/*
 * gaim
 *
 * Gaim is the legal property of its developers, whose names are too numerous
 * to list here.  Please refer to the COPYRIGHT file distributed with this
 * source distribution.
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "internal.h"
#include "gtkgaim.h"

#include "account.h"
#include "conversation.h"
#include "core.h"
#include "debug.h"
#include "eventloop.h"
#include "ft.h"
#include "log.h"
#include "notify.h"
#include "prefs.h"
#include "prpl.h"
#include "pounce.h"
#include "sound.h"
#include "status.h"
#include "util.h"

#include "gtkaccount.h"
#include "gtkblist.h"
#include "gtkconn.h"
#include "gtkconv.h"
#include "gtkdebug.h"
#include "gtkdialogs.h"
#include "gtkeventloop.h"
#include "gtkft.h"
#include "gtknotify.h"
#include "gtkplugin.h"
#include "gtkpounce.h"
#include "gtkprefs.h"
#include "gtkprivacy.h"
#include "gtkrequest.h"
#include "gtkroomlist.h"
#include "gtksound.h"
#include "gtkutils.h"
#include "gtkstock.h"

#if HAVE_SIGNAL_H
# include <signal.h>
#endif

#include <getopt.h>

#ifdef HAVE_STARTUP_NOTIFICATION
# define SN_API_NOT_YET_FROZEN
# include <libsn/sn-launchee.h>
# include <gdk/gdkx.h>
#endif

#ifdef HAVE_STARTUP_NOTIFICATION
static SnLauncheeContext *sn_context = NULL;
static SnDisplay *sn_display = NULL;
#endif

int docklet_count = 0;

#if HAVE_SIGNAL_H
/*
 * Lists of signals we wish to catch and those we wish to ignore.
 * Each list terminated with -1
 */
static int catch_sig_list[] = {
	SIGSEGV,
	SIGHUP,
	SIGINT,
	SIGTERM,
	SIGQUIT,
	SIGCHLD,
	-1
};

static int ignore_sig_list[] = {
	SIGPIPE,
	-1
};
#endif

static int
dologin_named(const char *name)
{
	GaimAccount *account;
	char **names;
	int i;
	int ret = -1;

	if (name != NULL) { /* list of names given */
		names = g_strsplit(name, ",", 64);
		for (i = 0; names[i] != NULL; i++) {
			account = gaim_accounts_find(names[i], NULL);
			if (account != NULL) { /* found a user */
				ret = 0;
				gaim_account_connect(account, gaim_account_get_status(account, "online"));
			}
		}
		g_strfreev(names);
	} else { /* no name given, use the first account */
		account = (GaimAccount *)gaim_accounts_get_all()->data;
		ret = 0;
		gaim_account_connect(account, gaim_account_get_status(account, "online"));
	}

	return ret;
}

#if HAVE_SIGNAL_H
static void
clean_pid(void)
{
	int status;
	pid_t pid;

	do {
		pid = waitpid(-1, &status, WNOHANG);
	} while (pid != 0 && pid != (pid_t)-1);

	if ((pid == (pid_t) - 1) && (errno != ECHILD)) {
		char errmsg[BUFSIZ];
		snprintf(errmsg, BUFSIZ, "Warning: waitpid() returned %d", pid);
		perror(errmsg);
	}
}

void
sighandler(int sig)
{
	switch (sig) {
	case SIGHUP:
		gaim_debug_warning("sighandler", "Caught signal %d\n", sig);
		gaim_connections_disconnect_all();
		break;
	case SIGSEGV:
#ifndef DEBUG
		fprintf(stderr, "Gaim has segfaulted and attempted to dump a core file.\n"
			"This is a bug in the software and has happened through\n"
			"no fault of your own.\n\n"
			"It is possible that this bug is already fixed in CVS.\n"
			"If you can reproduce the crash, please notify the gaim\n"
			"maintainers by reporting a bug at\n"
			GAIM_WEBSITE "bug.php\n\n"
			"Please make sure to specify what you were doing at the time,\n"
			"and post the backtrace from the core file. If you do not know\n"
			"how to get the backtrace, please get instructions at\n"
			GAIM_WEBSITE "gdb.php. If you need further\n"
			"assistance, please IM either RobFlynn or SeanEgn and\n"
			"they can help you.\n");
#else
		fprintf(stderr, "Hi, user.  We need to talk.\n"
			"I think something's gone wrong here.  It's probably my fault.\n"
			"No, really, it's not you... it's me... no no no, I think we get along well\n"
			"it's just that.... well, I want to see other people.  I... what?!?  NO!  I haven't\n"
			"been cheating on you!!  How many times do you want me to tell you?!  And for the\n"
			"last time, it's just a rash!\n");
		/*g_on_error_query (g_get_prgname());*/
#endif
		abort();
		break;
	case SIGCHLD:
		clean_pid();
		signal(SIGCHLD, sighandler);    /* restore signal catching on this one! */
		break;
	default:
		gaim_debug_warning("sighandler", "Caught signal %d\n", sig);
		gaim_connections_disconnect_all();

		gaim_plugins_unload_all();

		if (gtk_main_level())
			gtk_main_quit();
		exit(0);
	}
}
#endif

static int
ui_main()
{
#ifndef _WIN32
	GList *icons = NULL;
	GdkPixbuf *icon = NULL;
	char *icon_path;
#endif

	if (current_smiley_theme == NULL) {
		smiley_theme_probe();
		if (smiley_themes != NULL) {
			struct smiley_theme *smile = smiley_themes->data;
			load_smiley_theme(smile->path, TRUE);
		}
	}

	gaim_gtk_blist_setup_sort_methods();

#ifndef _WIN32
	/* use the nice PNG icon for all the windows */
	icon_path = g_build_filename(DATADIR, "pixmaps", "gaim", "icons", "online.png", NULL);
	icon = gdk_pixbuf_new_from_file(icon_path, NULL);
	g_free(icon_path);
	if (icon) {
		icons = g_list_append(icons,icon);
		gtk_window_set_default_icon_list(icons);
		g_object_unref(G_OBJECT(icon));
		g_list_free(icons);
	} else {
		gaim_debug_error("ui_main",
						 "Failed to load the default window icon!\n");
	}
#endif

	return 0;
}

static void
debug_init(void)
{
	gaim_debug_set_ui_ops(gaim_gtk_debug_get_ui_ops());
	gaim_gtk_debug_init();
}

static void
gaim_gtk_ui_init(void)
{
	/* Set the UI operation structures. */
	gaim_accounts_set_ui_ops(gaim_gtk_accounts_get_ui_ops());
	gaim_conversations_set_win_ui_ops(gaim_gtk_conversations_get_win_ui_ops());
	gaim_xfers_set_ui_ops(gaim_gtk_xfers_get_ui_ops());
	gaim_blist_set_ui_ops(gaim_gtk_blist_get_ui_ops());
	gaim_notify_set_ui_ops(gaim_gtk_notify_get_ui_ops());
	gaim_privacy_set_ui_ops(gaim_gtk_privacy_get_ui_ops());
	gaim_request_set_ui_ops(gaim_gtk_request_get_ui_ops());
	gaim_sound_set_ui_ops(gaim_gtk_sound_get_ui_ops());
	gaim_connections_set_ui_ops(gaim_gtk_connections_get_ui_ops());

	gaim_gtk_stock_init();
	gaim_gtk_prefs_init();
	gaim_gtk_account_init();
	gaim_gtk_blist_init();
	gaim_gtk_conversations_init();
	gaim_gtk_pounces_init();
	gaim_gtk_privacy_init();
	gaim_gtk_xfers_init();
	gaim_gtk_roomlist_init();
}

static void
gaim_gtk_quit(void)
{
	/* XXX? */
	/* YYY is there an XXX here? */

	/* captain's log, stardate... */
	/* LOG system_log(log_quit, NULL, NULL, OPT_LOG_BUDDY_SIGNON | OPT_LOG_MY_SIGNON); */

#ifdef USE_SM
	/* unplug */
	session_end();
#endif

	/* Save the plugins we have loaded for next time. */
	gaim_gtk_plugins_save();

	/* and end it all... */
	gtk_main_quit();
}

static GaimCoreUiOps core_ops =
{
	gaim_gtk_prefs_init,
	debug_init,
	gaim_gtk_ui_init,
	gaim_gtk_quit
};

static GaimCoreUiOps *
gaim_gtk_core_get_ui_ops(void)
{
	return &core_ops;
}

static void
show_usage(const char *name, gboolean terse)
{
	char *text;
	char *text_conv;
	GError *error = NULL;

	if (terse) {
		text = g_strdup_printf(_("Gaim %s. Try `%s -h' for more information.\n"), VERSION, name);
	} else {
		text = g_strdup_printf(_("Gaim %s\n"
		       "Usage: %s [OPTION]...\n\n"
		       "  -a, --acct          display account editor window\n"
		       "  -c, --config=DIR    use DIR for config files\n"
		       "  -d, --debug         print debugging messages to stdout\n"
		       "  -h, --help          display this help and exit\n"
		       "  -n, --nologin       don't automatically login\n"
		       "  -l, --login[=NAME]  automatically login (optional argument NAME specifies\n"
		       "                      account(s) to use, seperated by commas)\n"
		       "  -v, --version       display the current version and exit\n"), VERSION, name);
	}

	/* tries to convert 'text' to users locale */
	text_conv = g_locale_from_utf8(text, -1, NULL, NULL, &error);
	if (text_conv != NULL) {
		puts(text_conv);
		g_free(text_conv);
	}
	/* use 'text' as a fallback */
	else {
		g_warning("%s\n", error->message);
		g_error_free(error);
		puts(text);
	}
	g_free(text);
}

#ifdef HAVE_STARTUP_NOTIFICATION
static void
sn_error_trap_push(SnDisplay *display, Display *xdisplay)
{
	gdk_error_trap_push();
}

static void
sn_error_trap_pop(SnDisplay *display, Display *xdisplay)
{
	gdk_error_trap_pop();
}

static void
startup_notification_complete(void)
{
	Display *xdisplay;

	xdisplay = GDK_DISPLAY();
	sn_display = sn_display_new(xdisplay,
								sn_error_trap_push,
								sn_error_trap_pop);
	sn_context =
		sn_launchee_context_new_from_environment(sn_display,
												 DefaultScreen(xdisplay));

	if (sn_context != NULL)
	{
		sn_launchee_context_complete(sn_context);
		sn_launchee_context_unref(sn_context);

		sn_display_unref(sn_display);
	}
}
#endif /* HAVE_STARTUP_NOTIFICATION */

#ifndef _WIN32
static char *gaim_find_binary_location(void *symbol, void *data)
{
	static char *fullname = NULL;
	static gboolean first = TRUE;

	char *argv0 = data;
	struct stat st;
	char *basebuf, *linkbuf, *fullbuf;

	if (!first)
		/* We've already been through this. */
		return strdup(fullname);

	first = FALSE;

	if (!argv0)
		return NULL;


	basebuf = g_find_program_in_path(argv0);

	/* But we still need to deal with symbolic links */
	lstat(basebuf, &st);
	while ((st.st_mode & S_IFLNK) == S_IFLNK) {
		linkbuf = g_malloc(1024);
		readlink(basebuf, linkbuf, 1024);
		if (linkbuf[0] == G_DIR_SEPARATOR) {
			/* an absolute path */
			fullbuf = g_strdup(linkbuf);
		} else {
			char *dirbuf = g_path_get_dirname(basebuf);
			/* a relative path */
			fullbuf = g_strdup_printf("%s%s%s",
						  dirbuf, G_DIR_SEPARATOR_S,
						  linkbuf);
			g_free(dirbuf);
		}
		/* There's no memory leak here.  Really! */
		g_free(linkbuf);
		g_free(basebuf);
		basebuf = fullbuf;
		lstat(basebuf, &st);
	}

	fullname = basebuf;
	return strdup(fullname);
}
#endif /* #ifndef _WIN32 */

/* FUCKING GET ME A TOWEL! */
#ifdef _WIN32
int gaim_main(HINSTANCE hint, int argc, char *argv[])
#else
int main(int argc, char *argv[])
#endif
{
	gboolean opt_acct = FALSE;
	gboolean opt_help = FALSE;
	gboolean opt_login = FALSE;
	gboolean opt_nologin = FALSE;
	gboolean opt_version = FALSE;
	char *opt_config_dir_arg = NULL;
	char *opt_login_arg = NULL;
	char *opt_session_arg = NULL;
	int dologin_ret = -1;
	char *plugin_search_paths[3];
#if HAVE_SIGNAL_H
	int sig_indx;	/* for setting up signal catching */
	sigset_t sigset;
	void (*prev_sig_disp)();
#endif
	int opt;
	gboolean gui_check;
	gboolean debug_enabled;
	gchar *gaimrc, *accountsxml;
#if HAVE_SIGNAL_H
	char errmsg[BUFSIZ];
#endif

	struct option long_options[] = {
		{"acct",     no_argument,       NULL, 'a'},
		{"config",   required_argument, NULL, 'c'},
		{"debug",    no_argument,       NULL, 'd'},
		{"help",     no_argument,       NULL, 'h'},
		{"login",    optional_argument, NULL, 'l'},
		{"nologin",  no_argument,       NULL, 'n'},
		{"session",  required_argument, NULL, 's'},
		{"version",  no_argument,       NULL, 'v'},
		{0, 0, 0, 0}
	};

#ifdef DEBUG
	debug_enabled = TRUE;
#else
	debug_enabled = FALSE;
#endif

#ifndef _WIN32
	br_set_locate_fallback_func(gaim_find_binary_location, argv[0]);
#endif
#ifdef ENABLE_NLS
	bindtextdomain(PACKAGE, LOCALEDIR);
	bind_textdomain_codeset(PACKAGE, "UTF-8");
	textdomain(PACKAGE);
#endif

#if HAVE_SIGNAL_H
	/* Let's not violate any PLA's!!!! */
	/* jseymour: whatever the fsck that means */
	/* Robot101: for some reason things like gdm like to block     *
	 * useful signals like SIGCHLD, so we unblock all the ones we  *
	 * declare a handler for. thanks JSeymour and Vann.            */
	if (sigemptyset(&sigset)) {
		snprintf(errmsg, BUFSIZ, "Warning: couldn't initialise empty signal set");
		perror(errmsg);
	}
	for(sig_indx = 0; catch_sig_list[sig_indx] != -1; ++sig_indx) {
		if((prev_sig_disp = signal(catch_sig_list[sig_indx], sighandler)) == SIG_ERR) {
			snprintf(errmsg, BUFSIZ, "Warning: couldn't set signal %d for catching",
				catch_sig_list[sig_indx]);
			perror(errmsg);
		}
		if(sigaddset(&sigset, catch_sig_list[sig_indx])) {
			snprintf(errmsg, BUFSIZ, "Warning: couldn't include signal %d for unblocking",
				catch_sig_list[sig_indx]);
			perror(errmsg);
		}
	}
	for(sig_indx = 0; ignore_sig_list[sig_indx] != -1; ++sig_indx) {
		if((prev_sig_disp = signal(ignore_sig_list[sig_indx], SIG_IGN)) == SIG_ERR) {
			snprintf(errmsg, BUFSIZ, "Warning: couldn't set signal %d to ignore",
				ignore_sig_list[sig_indx]);
			perror(errmsg);
		}
	}

	if (sigprocmask(SIG_UNBLOCK, &sigset, NULL)) {
		snprintf(errmsg, BUFSIZ, "Warning: couldn't unblock signals");
		perror(errmsg);
	}
#endif

	/* scan command-line options */
	opterr = 1;
	while ((opt = getopt_long(argc, argv,
#ifndef _WIN32
				  "ac:dhnl::s:v",
#else
				  "ac:dhnl::v",
#endif
				  long_options, NULL)) != -1) {
		switch (opt) {
		case 'a':	/* account editor */
			opt_acct = TRUE;
			break;
		case 'c':	/* config dir */
			g_free(opt_config_dir_arg);
			opt_config_dir_arg = g_strdup(optarg);
			break;
		case 'd':	/* debug */
			debug_enabled = TRUE;
			break;
		case 'h':	/* help */
			opt_help = TRUE;
			break;
		case 'n':	/* no autologin */
			opt_nologin = TRUE;
			break;
		case 'l':	/* login, option username */
			opt_login = TRUE;
			g_free(opt_login_arg);
			if (optarg != NULL)
				opt_login_arg = g_strdup(optarg);
			break;
		case 's':	/* use existing session ID */
			g_free(opt_session_arg);
			opt_session_arg = g_strdup(optarg);
			break;
		case 'v':	/* version */
			opt_version = TRUE;
			break;
		case '?':	/* show terse help */
		default:
			show_usage(argv[0], TRUE);
			return 0;
			break;
		}
	}

	/* show help message */
	if (opt_help) {
		show_usage(argv[0], FALSE);
		return 0;
	}
	/* show version message */
	if (opt_version) {
		printf("Gaim %s\n", VERSION);
		return 0;
	}

	/* set a user-specified config directory */
	if (opt_config_dir_arg != NULL) {
		set_gaim_user_dir(opt_config_dir_arg);
	}

	gui_check = gtk_init_check(&argc, &argv);
	if (!gui_check) {
		char *display = gdk_get_display();

		g_warning("cannot open display: %s", display ? display : "unset");
		g_free(display);

		return 1;
	}

#ifdef _WIN32
	wgaim_init(hint);
#endif
	gaim_core_set_ui_ops(gaim_gtk_core_get_ui_ops());
	gaim_eventloop_set_ui_ops(gaim_gtk_eventloop_get_ui_ops());

	if (!gaim_core_init(GAIM_GTK_UI)) {
		fprintf(stderr,
				"Initialization of the Gaim core failed. Dumping core.\n"
				"Please report this!\n");
		abort();
	}

	gaim_debug_set_enabled(debug_enabled);

	plugin_search_paths[0] = g_strdup(LIBDIR);
	plugin_search_paths[1] = g_strdup(gaim_user_dir());
	plugin_search_paths[2] = g_build_filename(gaim_user_dir(), "plugins", NULL);

	gaim_plugins_set_search_paths(sizeof(plugin_search_paths) /
								  sizeof(*plugin_search_paths),
								  plugin_search_paths);

	g_free(plugin_search_paths[0]);
	g_free(plugin_search_paths[1]);
	g_free(plugin_search_paths[2]);

	gaim_plugins_probe(NULL);

	/* XXX - Remove this check.  Maybe in 2005.  --KingAnt, 25 Jul 2004 */
	gaimrc = g_build_filename(gaim_home_dir(), ".gaimrc", NULL);
	accountsxml = g_build_filename(gaim_user_dir(), "accounts.xml", NULL);
	if (g_file_test(gaimrc, G_FILE_TEST_EXISTS) &&
		!g_file_test(accountsxml, G_FILE_TEST_EXISTS)) {
		gaim_notify_error(NULL, NULL, _("Unable to load preferences"), _("Gaim was not able to load your preferences because they are stored in an old format that is no longer used.  Please reconfigure your settings using the Preferences window."));
	}
	g_free(gaimrc);
	g_free(accountsxml);

	gaim_set_blist(gaim_blist_new());
	gaim_blist_load();

	gaim_prefs_load();
	gaim_prefs_update_old();
	gaim_gtk_prefs_update_old();

	/* load plugins we had when we quit */
	gaim_plugins_load_saved("/gaim/gtk/plugins/loaded");

	gaim_pounces_load();

	ui_main();

#ifdef USE_SM
	session_init(argv[0], opt_session_arg, opt_config_dir_arg);
#endif
	if (opt_session_arg != NULL) {
		g_free(opt_session_arg);
		opt_session_arg = NULL;
	}
	if (opt_config_dir_arg != NULL) {
		g_free(opt_config_dir_arg);
		opt_config_dir_arg = NULL;
	}

	if (gaim_prefs_get_bool("/gaim/gtk/debug/enabled"))
		gaim_gtk_debug_window_show();

	gaim_blist_show();

	if (opt_login) {
		dologin_ret = dologin_named(opt_login_arg);
		if (opt_login_arg != NULL) {
			g_free(opt_login_arg);
			opt_login_arg = NULL;
		}
	}

	if (!opt_acct && !opt_nologin)
		gaim_accounts_auto_login(GAIM_GTK_UI);

	if (opt_acct || (gaim_accounts_get_all() == NULL)) {
		gaim_gtk_accounts_window_show();
	}

#ifdef HAVE_STARTUP_NOTIFICATION
	startup_notification_complete();
#endif

	gtk_main();

#ifdef _WIN32
	wgaim_cleanup();
#endif

	return 0;
}
