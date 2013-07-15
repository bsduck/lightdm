/*
 * Copyright (C) 2010-2011 Robert Ancell.
 * Author: Robert Ancell <robert.ancell@canonical.com>
 * 
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See http://www.gnu.org/copyleft/gpl.html the full text of the
 * license.
 */

#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "seat.h"
#include "configuration.h"
#include "guest-account.h"
#include "greeter.h"

enum {
    SESSION_ADDED,
    SESSION_REMOVED,
    STOPPED,
    LAST_SIGNAL
};
static guint signals[LAST_SIGNAL] = { 0 };

struct SeatPrivate
{
    /* Configuration for this seat */
    GHashTable *properties;

    /* TRUE if able to switch users */
    gboolean can_switch;

    /* TRUE if display server can be shared for sessions */
    gboolean share_display_server;

    /* Name of guest account */
    gchar *guest_username;

    /* The display servers on this seat */
    GList *display_servers;

    /* The sessions on this seat */
    GList *sessions;

    /* Greeter connection */
    Greeter *greeter;

    /* TRUE if stopping this seat (waiting for displays to stop) */
    gboolean stopping;

    /* TRUE if stopped */
    gboolean stopped;
};

/* PAM services to use */
#define GREETER_SERVICE   "lightdm-greeter"
#define USER_SERVICE      "lightdm"
#define AUTOLOGIN_SERVICE "lightdm-autologin"

G_DEFINE_TYPE (Seat, seat, G_TYPE_OBJECT);

typedef struct
{
    const gchar *name;
    GType type;
} SeatModule;
static GHashTable *seat_modules = NULL;

static DisplayServer *create_display_server (Seat *seat);

void
seat_register_module (const gchar *name, GType type)
{
    SeatModule *module;

    if (!seat_modules)
        seat_modules = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    g_debug ("Registered seat module %s", name);

    module = g_malloc0 (sizeof (SeatModule));
    module->name = g_strdup (name);
    module->type = type;
    g_hash_table_insert (seat_modules, g_strdup (name), module);
}

Seat *
seat_new (const gchar *module_name)
{
    Seat *seat;
    SeatModule *m = NULL;
  
    g_return_val_if_fail (module_name != NULL, NULL);

    if (seat_modules)
        m = g_hash_table_lookup (seat_modules, module_name);
    if (!m)
        return NULL;

    seat = g_object_new (m->type, NULL);

    return seat;
}

void
seat_set_property (Seat *seat, const gchar *name, const gchar *value)
{
    g_return_if_fail (seat != NULL);
    g_hash_table_insert (seat->priv->properties, g_strdup (name), g_strdup (value));
}

gboolean
seat_has_property (Seat *seat, const gchar *name)
{
    g_return_val_if_fail (seat != NULL, FALSE);
    return g_hash_table_lookup (seat->priv->properties, name) != NULL;
}

const gchar *
seat_get_string_property (Seat *seat, const gchar *name)
{
    g_return_val_if_fail (seat != NULL, NULL);
    return g_hash_table_lookup (seat->priv->properties, name);
}

gboolean
seat_get_boolean_property (Seat *seat, const gchar *name)
{
    return g_strcmp0 (seat_get_string_property (seat, name), "true") == 0;
}

gint
seat_get_integer_property (Seat *seat, const gchar *name)
{
    const gchar *value;

    value = seat_get_string_property (seat, name);
    return value ? atoi (value) : 0;
}

void
seat_set_can_switch (Seat *seat, gboolean can_switch)
{
    g_return_if_fail (seat != NULL);

    seat->priv->can_switch = can_switch;
}

void
seat_set_share_display_server (Seat *seat, gboolean share_display_server)
{
    g_return_if_fail (seat != NULL);

    seat->priv->share_display_server = share_display_server;
}

gboolean
seat_start (Seat *seat)
{
    g_return_val_if_fail (seat != NULL, FALSE);
  
    SEAT_GET_CLASS (seat)->setup (seat);
    return SEAT_GET_CLASS (seat)->start (seat);
}

GList *
seat_get_sessions (Seat *seat)
{
    g_return_val_if_fail (seat != NULL, NULL);
    return seat->priv->sessions;
}

void
seat_set_active_session (Seat *seat, Session *session)
{
    g_return_if_fail (seat != NULL);
    SEAT_GET_CLASS (seat)->set_active_session (seat, session);
}

Session *
seat_get_active_session (Seat *seat)
{
    g_return_val_if_fail (seat != NULL, NULL);
    return SEAT_GET_CLASS (seat)->get_active_session (seat);
}

gboolean
seat_get_can_switch (Seat *seat)
{
    g_return_val_if_fail (seat != NULL, FALSE);
    return seat->priv->can_switch;
}

gboolean
seat_get_allow_guest (Seat *seat)
{
    g_return_val_if_fail (seat != NULL, FALSE);  
    return seat_get_boolean_property (seat, "allow-guest") && guest_account_is_installed ();
}

gboolean
seat_get_greeter_allow_guest (Seat *seat)
{
    g_return_val_if_fail (seat != NULL, FALSE);  
    return seat_get_allow_guest (seat) && seat_get_boolean_property (seat, "greeter-allow-guest");
}

static gboolean
switch_to_user (Seat *seat, const gchar *username, gboolean unlock)
{
    GList *link;

    /* Switch to active display if it exists */
    // FIXME

    return FALSE;
}

static gboolean
run_script (Seat *seat, DisplayServer *display_server, const gchar *script_name, User *user)
{
    Process *script;
    gboolean result = FALSE;
  
    script = process_new ();

    process_set_command (script, script_name);

    /* Set POSIX variables */
    process_set_clear_environment (script, TRUE);
    process_set_env (script, "SHELL", "/bin/sh");

    /* Variables required for regression tests */
    if (g_getenv ("LIGHTDM_TEST_ROOT"))
    {
        process_set_env (script, "LIGHTDM_TEST_ROOT", g_getenv ("LIGHTDM_TEST_ROOT"));
        process_set_env (script, "LD_PRELOAD", g_getenv ("LD_PRELOAD"));
        process_set_env (script, "LD_LIBRARY_PATH", g_getenv ("LD_LIBRARY_PATH"));
        process_set_env (script, "PATH", g_getenv ("PATH"));
    }
    else
        process_set_env (script, "PATH", "/usr/local/bin:/usr/bin:/bin");

    if (user)
    {
        process_set_env (script, "USER", user_get_name (user));
        process_set_env (script, "LOGNAME", user_get_name (user));
        process_set_env (script, "HOME", user_get_home_directory (user));
    }
    else
        process_set_env (script, "HOME", "/");

    SEAT_GET_CLASS (seat)->run_script (seat, display_server, script);

    if (process_start (script, TRUE))
    {
        int exit_status;

        exit_status = process_get_exit_status (script);
        if (WIFEXITED (exit_status))
        {
            g_debug ("Exit status of %s: %d", script_name, WEXITSTATUS (exit_status));
            result = WEXITSTATUS (exit_status) == EXIT_SUCCESS;
        }
    }

    g_object_unref (script);

    return result;
}

static void
seat_real_run_script (Seat *seat, DisplayServer *display_server, Process *process)
{  
}

static void
emit_upstart_signal (const gchar *signal)
{
    g_return_if_fail (signal != NULL);
    g_return_if_fail (signal[0] != 0);

    if (getuid () != 0)
        return;

    gchar *cmd = g_strdup_printf ("initctl -q emit %s DISPLAY_MANAGER=lightdm", signal);
    g_spawn_command_line_async (cmd, NULL); /* OK if it fails, probably not installed */
    g_free (cmd);
}

static void
check_stopped (Seat *seat)
{
    if (seat->priv->stopping &&
        !seat->priv->stopped &&
        g_list_length (seat->priv->display_servers) == 0 &&
        g_list_length (seat->priv->sessions) == 0)
    {
        seat->priv->stopped = TRUE;
        g_debug ("Seat stopped");
        g_signal_emit (seat, signals[STOPPED], 0);
    }
}

static void
display_server_stopped_cb (DisplayServer *display_server, Seat *seat)
{
    g_debug ("Display server stopped");

    g_signal_handlers_disconnect_matched (display_server, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, seat);
    seat->priv->display_servers = g_list_remove (seat->priv->display_servers, display_server);
    g_object_unref (display_server);

    check_stopped (seat);

    if (!seat->priv->stopping && g_list_length (seat->priv->display_servers) == 0)
    {
        g_debug ("Stopping seat, all display servers have stopped");
        seat_stop (seat);
    }
}

static void
session_stopped_cb (Session *session, Seat *seat)
{
    DisplayServer *display_server;
    const gchar *script;
    Session *greeter_session;

    g_debug ("Session stopped");

    display_server = session_get_display_server (session);

    /* Cleanup */
    script = seat_get_string_property (seat, "session-cleanup-script");
    if (script)
        run_script (seat, display_server, script, session_get_user (session));

    if (seat->priv->guest_username && strcmp (session_get_username (session), seat->priv->guest_username) == 0)
    {
        g_free (seat->priv->guest_username);
        seat->priv->guest_username = NULL;
    }

    g_signal_handlers_disconnect_matched (session, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, seat);
    seat->priv->sessions = g_list_remove (seat->priv->sessions, session);

    check_stopped (seat);
  
    /* If this is the greeter session then start the user session */
    greeter_session = greeter_get_authentication_session (seat->priv->greeter);
    if (!seat->priv->stopping &&
        seat->priv->greeter &&
        session == greeter_get_session (seat->priv->greeter) &&
        greeter_session &&
        seat->priv->share_display_server)
    {
        g_debug ("Starting session re-using greeter display server");
        session_run (greeter_session);
    }
    else if (display_server)
        display_server_stop (display_server);

    g_object_unref (session);
}

static Session *
create_session (Seat *seat)
{
    Session *session;

    session = SEAT_GET_CLASS (seat)->create_session (seat);
    seat->priv->sessions = g_list_append (seat->priv->sessions, session);
    g_signal_connect (session, "stopped", G_CALLBACK (session_stopped_cb), seat);

    /* Variables required for regression tests */
    if (g_getenv ("LIGHTDM_TEST_ROOT"))
    {
        session_set_env (session, "LIGHTDM_TEST_ROOT", g_getenv ("LIGHTDM_TEST_ROOT"));
        session_set_env (session, "DBUS_SYSTEM_BUS_ADDRESS", g_getenv ("DBUS_SYSTEM_BUS_ADDRESS"));
        session_set_env (session, "DBUS_SESSION_BUS_ADDRESS", g_getenv ("DBUS_SESSION_BUS_ADDRESS"));
        session_set_env (session, "LD_PRELOAD", g_getenv ("LD_PRELOAD"));
        session_set_env (session, "LD_LIBRARY_PATH", g_getenv ("LD_LIBRARY_PATH"));
        session_set_env (session, "GI_TYPELIB_PATH", g_getenv ("GI_TYPELIB_PATH"));
    }

    return session;
}

gboolean
seat_switch_to_greeter (Seat *seat)
{
    DisplayServer *display_server;

    g_return_val_if_fail (seat != NULL, FALSE);

    if (!seat->priv->can_switch)
        return FALSE;

    /* Switch to greeter if one open (shouldn't be though) */
    if (switch_to_user (seat, NULL, FALSE))
        return TRUE;

    // FIXME
}

gboolean
seat_switch_to_user (Seat *seat, const gchar *username, const gchar *session_name)
{
    g_return_val_if_fail (seat != NULL, FALSE);
    g_return_val_if_fail (username != NULL, FALSE);

    if (!seat->priv->can_switch)
        return FALSE;

    g_debug ("Switching to user %s", username);

    /* Switch to session if one open */
    if (switch_to_user (seat, username, FALSE))
        return TRUE;

    // FIXME

    return FALSE;
}

gboolean
seat_switch_to_guest (Seat *seat, const gchar *session_name)
{
    g_return_val_if_fail (seat != NULL, FALSE);

    if (!seat->priv->can_switch || !seat_get_allow_guest (seat))
        return FALSE;

    /* Switch to session if one open */
    if (seat->priv->guest_username)
    {
        g_debug ("Switching to existing guest account %s", seat->priv->guest_username);
        return switch_to_user (seat, seat->priv->guest_username, FALSE);
    }

    // FIXME

    return FALSE;
}

gboolean
seat_lock (Seat *seat, const gchar *username)
{
    g_return_val_if_fail (seat != NULL, FALSE);

    if (!seat->priv->can_switch)
        return FALSE;

    g_debug ("Locking seat");

    /* Switch to greeter if one open (shouldn't be though) */
    if (switch_to_user (seat, NULL, FALSE))
        return TRUE;

    // FIXME

    return FALSE;
}

void
seat_stop (Seat *seat)
{
    g_return_if_fail (seat != NULL);

    if (seat->priv->stopping)
        return;

    g_debug ("Stopping seat");
    seat->priv->stopping = TRUE;
    SEAT_GET_CLASS (seat)->stop (seat);
}

gboolean
seat_get_is_stopping (Seat *seat)
{
    g_return_val_if_fail (seat != NULL, FALSE);
    return seat->priv->stopping;
}

static void
seat_real_setup (Seat *seat)
{
}

static void
session_authentication_complete_cb (Session *session, Seat *seat)
{
    session_run (session);
}

static void
display_server_ready_cb (DisplayServer *display_server, Seat *seat)
{
    GList *link;
    gboolean used_display_server = FALSE;

    /* Start the sessions waiting for this display server */
    for (link = seat->priv->sessions; link; link = link->next)
    {
        Session *session = link->data;

        if (session_get_display_server (session) != display_server)
            continue;

        g_signal_connect (session, "authentication-complete", G_CALLBACK (session_authentication_complete_cb), seat);
        if (seat->priv->greeter && greeter_get_session (seat->priv->greeter) == session)
        {
            g_debug ("Display server ready, running greeter");
            greeter_start (seat->priv->greeter);
        }
        else if (!session_get_is_authenticated (session))
        {
            g_debug ("Display server ready, starting session");
            session_start (session);
        }
        else
        {
            g_debug ("Display server ready, running user session");        
            session_run (session);
        }
        used_display_server = TRUE;
    }

    if (!used_display_server)
    {
        g_debug ("Stopping not required display server");
        display_server_stop (display_server);
    }
}

static DisplayServer *
create_display_server (Seat *seat)
{
    DisplayServer *display_server;

    display_server = SEAT_GET_CLASS (seat)->create_display_server (seat);
    seat->priv->display_servers = g_list_append (seat->priv->display_servers, display_server);
    g_signal_connect (display_server, "ready", G_CALLBACK (display_server_ready_cb), seat);
    g_signal_connect (display_server, "stopped", G_CALLBACK (display_server_stopped_cb), seat);

    return display_server;
}

static Session *
greeter_create_session_cb (Greeter *greeter, Seat *seat)
{
    return create_session (seat);
}

static gchar **
get_session_argv_from_filename (const gchar *filename, const gchar *session_wrapper)
{
    GKeyFile *session_desktop_file;
    gboolean result;
    int argc;
    gchar *command = NULL, **argv, *path;
    GError *error = NULL;

    /* Read the command from the .desktop file */
    session_desktop_file = g_key_file_new ();
    result = g_key_file_load_from_file (session_desktop_file, filename, G_KEY_FILE_NONE, &error);
    if (error)
        g_debug ("Failed to load session file %s: %s", filename, error->message);
    g_clear_error (&error);
    if (result)
    {
        command = g_key_file_get_string (session_desktop_file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_EXEC, NULL);
        if (!command)
            g_debug ("No command in session file %s", filename);
    }
    g_key_file_free (session_desktop_file);

    if (!command)
        return NULL;

    /* If configured, run sessions through a wrapper */
    if (session_wrapper)
    {
        argv = g_malloc (sizeof (gchar *) * 3);
        path = g_find_program_in_path (session_wrapper);
        argv[0] = path ? path : g_strdup (session_wrapper);
        argv[1] = command;
        argv[2] = NULL;
        return argv;
    }

    /* Split command into an array listing and make command absolute */
    result = g_shell_parse_argv (command, &argc, &argv, &error);
    if (error)
        g_debug ("Invalid session command '%s': %s", command, error->message);
    g_clear_error (&error);
    g_free (command);
    if (!result)
        return NULL;
    path = g_find_program_in_path (argv[0]);
    if (path)
    {
        g_free (argv[0]);
        argv[0] = path;
    }
  
    return argv;
}

static gchar **
get_session_argv (const gchar *sessions_dir, const gchar *session_name, const gchar *session_wrapper)
{
    gchar **dirs, **argv;
    int i;

    g_return_val_if_fail (sessions_dir != NULL, NULL);
    g_return_val_if_fail (session_name != NULL, NULL);

    dirs = g_strsplit (sessions_dir, ":", -1);
    for (i = 0; dirs[i]; i++)
    {
        gchar *filename, *path;

        filename = g_strdup_printf ("%s.desktop", session_name);
        path = g_build_filename (dirs[i], filename, NULL);
        g_free (filename);
        argv = get_session_argv_from_filename (path, session_wrapper);
        g_free (path);
        if (argv)
            break;
    }
    g_strfreev (dirs);

    return argv;
}

static gboolean
greeter_start_session_cb (Greeter *greeter, SessionType type, const gchar *session_name, Seat *seat)
{
    Session *session;
    gchar *sessions_dir = NULL;
    gchar **argv;

    /* Get the session to use */
    session = greeter_get_authentication_session (seat->priv->greeter);

    /* Get session command to run */
    switch (type)
    {
    case SESSION_TYPE_LOCAL:
        sessions_dir = config_get_string (config_get_instance (), "LightDM", "sessions-directory");
        break;
    case SESSION_TYPE_REMOTE:
        sessions_dir = config_get_string (config_get_instance (), "LightDM", "remote-sessions-directory");
        break;
    }
    if (!session_name)
        session_name = seat_get_string_property (seat, "user-session");
    argv = get_session_argv (sessions_dir, session_name, seat_get_string_property (seat, "session-wrapper"));
    g_free (sessions_dir);
    session_set_argv (session, argv);
    g_strfreev (argv);

    /* If no session information found, then can't start the session */
    if (!argv)
        return FALSE;

    /* If can re-use the display server, stop the greeter first */
    if (seat->priv->share_display_server)
    {
        /* Run on the same display server after the greeter has stopped */
        session_set_display_server (session, session_get_display_server (greeter_get_session (seat->priv->greeter)));
        session_stop (greeter_get_session (seat->priv->greeter));

        return TRUE;
    }
    /* Otherwise start a new display server for this session */
    else
    {
        DisplayServer *display_server;

        display_server = create_display_server (seat);
        session_set_display_server (session, display_server);

        return display_server_start (display_server);
    }
}

static gboolean
seat_real_start (Seat *seat)
{
    const gchar *autologin_username;
    int autologin_timeout;
    gboolean autologin_guest;
    gboolean do_autologin;
    gboolean autologin_in_background;
    const gchar *user_session;
    Session *session;
    DisplayServer *display_server;
    User *user;
    gchar *sessions_dir;
    const gchar *wrapper = NULL;
    const gchar *session_name = NULL;
    gchar **argv;

    g_debug ("Starting seat");

    /* Get autologin settings */
    autologin_username = seat_get_string_property (seat, "autologin-user");
    if (g_strcmp0 (autologin_username, "") == 0)
        autologin_username = NULL;
    autologin_timeout = seat_get_integer_property (seat, "autologin-user-timeout");
    autologin_guest = seat_get_boolean_property (seat, "autologin-guest");
    autologin_in_background = seat_get_boolean_property (seat, "autologin-in-background");
    do_autologin = autologin_username != NULL || autologin_guest;

    session = create_session (seat);

    /* Autologin or start a greeter */
    if (autologin_timeout == 0 && autologin_guest)
    {
        session_set_pam_service (session, AUTOLOGIN_SERVICE);
        session_set_do_authenticate (session, TRUE);
        session_set_is_guest (session, TRUE);
        sessions_dir = config_get_string (config_get_instance (), "LightDM", "sessions-directory");
        wrapper = seat_get_string_property (seat, "session-wrapper");
    }
    else if (autologin_timeout == 0 && autologin_username != NULL)
    {
        session_set_pam_service (session, AUTOLOGIN_SERVICE);
        session_set_username (session, autologin_username);
        session_set_do_authenticate (session, TRUE);
        user = session_get_user (session);
        sessions_dir = config_get_string (config_get_instance (), "LightDM", "sessions-directory");
        wrapper = seat_get_string_property (seat, "session-wrapper");
        session_name = user_get_xsession (user);
    }
    else
    {
        gchar *greeter_user;

        session_set_pam_service (session, GREETER_SERVICE);
        greeter_user = config_get_string (config_get_instance (), "LightDM", "greeter-user");
        session_set_username (session, greeter_user);
        g_free (greeter_user);
        sessions_dir = config_get_string (config_get_instance (), "LightDM", "greeters-directory");
        wrapper = seat_get_string_property (seat, "session-wrapper");

        seat->priv->greeter = greeter_new (session, USER_SERVICE, AUTOLOGIN_SERVICE);
        g_signal_connect (seat->priv->greeter, "create-session", G_CALLBACK (greeter_create_session_cb), seat);
        g_signal_connect (seat->priv->greeter, "start-session", G_CALLBACK (greeter_start_session_cb), seat);
    }

    if (!session_name)
        session_name = seat_get_string_property (seat, "user-session");
    argv = get_session_argv (sessions_dir, session_name, wrapper);
    g_free (sessions_dir);
    session_set_argv (session, argv);
    g_strfreev (argv);

    /* If no session information found, then can't start the session */
    if (!argv)
    {
        seat_stop (seat);
        return FALSE;
    }

    /* Start display server to show session on */
    display_server = create_display_server (seat);
    session_set_display_server (session, display_server);

    return display_server_start (display_server);
}

static void
seat_real_set_active_session (Seat *seat, Session *session)
{
}

static Session *
seat_real_get_active_session (Seat *seat)
{
    return NULL;
}

static void
seat_real_stop (Seat *seat)
{
    GList *list, *link;

    check_stopped (seat);
    if (seat->priv->stopped)
        return;

    /* Stop all the display servers and sessions on the seat. Copy the list as
     * it might be modified if a display server / session stops during this loop */
    list = g_list_copy (seat->priv->display_servers);
    for (link = list; link; link = link->next)
    {
        DisplayServer *display_server = link->data;
        g_debug ("Stopping display server");
        display_server_stop (display_server);
    }
    g_list_free (list);
    list = g_list_copy (seat->priv->sessions);
    for (link = list; link; link = link->next)
    {
        Session *session = link->data;
        g_debug ("Stopping session");
        if (session_get_is_stopped (session))
            session_stopped_cb (session, seat);
        else
            session_stop (session);
    }
    g_list_free (list);
}

static void
seat_init (Seat *seat)
{
    seat->priv = G_TYPE_INSTANCE_GET_PRIVATE (seat, SEAT_TYPE, SeatPrivate);
    seat->priv->properties = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    seat->priv->share_display_server = TRUE;
}

static void
seat_finalize (GObject *object)
{
    Seat *self;
    GList *link;

    self = SEAT (object);

    g_hash_table_unref (self->priv->properties);
    g_free (self->priv->guest_username);
    for (link = self->priv->display_servers; link; link = link->next)
    {
        DisplayServer *display_server = link->data;
        g_signal_handlers_disconnect_matched (display_server, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, self);
    }  
    g_list_free_full (self->priv->display_servers, g_object_unref);
    for (link = self->priv->sessions; link; link = link->next)
    {
        Session *session = link->data;
        g_signal_handlers_disconnect_matched (session, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, self);
    }  
    g_list_free_full (self->priv->sessions, g_object_unref);

    G_OBJECT_CLASS (seat_parent_class)->finalize (object);
}

static void
seat_class_init (SeatClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    klass->setup = seat_real_setup;
    klass->start = seat_real_start;
    klass->set_active_session = seat_real_set_active_session;
    klass->get_active_session = seat_real_get_active_session;
    klass->run_script = seat_real_run_script;
    klass->stop = seat_real_stop;

    object_class->finalize = seat_finalize;

    g_type_class_add_private (klass, sizeof (SeatPrivate));

    signals[SESSION_ADDED] =
        g_signal_new ("session-added",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (SeatClass, session_added),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__OBJECT,
                      G_TYPE_NONE, 1, SESSION_TYPE);
    signals[SESSION_REMOVED] =
        g_signal_new ("session-removed",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (SeatClass, session_removed),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__OBJECT,
                      G_TYPE_NONE, 1, SESSION_TYPE);
    signals[STOPPED] =
        g_signal_new ("stopped",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (SeatClass, stopped),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);
}
