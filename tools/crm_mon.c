/*
 * Copyright 2004-2023 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <sys/param.h>

#include <crm/crm.h>

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <sys/utsname.h>

#include <crm/msg_xml.h>
#include <crm/services.h>
#include <crm/lrmd.h>
#include <crm/common/cmdline_internal.h>
#include <crm/common/internal.h>  // pcmk__ends_with_ext()
#include <crm/common/ipc.h>
#include <crm/common/mainloop.h>
#include <crm/common/output.h>
#include <crm/common/output_internal.h>
#include <crm/common/results.h>
#include <crm/common/util.h>
#include <crm/common/xml.h>
#include <crm/common/xml_internal.h>

#include <crm/cib/internal.h>
#include <crm/pengine/status.h>
#include <crm/pengine/internal.h>
#include <pacemaker-internal.h>
#include <crm/stonith-ng.h>
#include <crm/fencing/internal.h>

#include "crm_mon.h"

#define SUMMARY "Provides a summary of cluster's current state.\n\n" \
                "Outputs varying levels of detail in a number of different formats."

/*
 * Definitions indicating which items to print
 */

static uint32_t show;
static uint32_t show_opts = pcmk_show_pending;

/*
 * Definitions indicating how to output
 */

static mon_output_format_t output_format = mon_output_unset;

/* other globals */
static GIOChannel *io_channel = NULL;
static GMainLoop *mainloop = NULL;
static guint reconnect_timer = 0;
static mainloop_timer_t *refresh_timer = NULL;

static enum pcmk_pacemakerd_state pcmkd_state = pcmk_pacemakerd_state_invalid;
static cib_t *cib = NULL;
static stonith_t *st = NULL;
static xmlNode *current_cib = NULL;

static GError *error = NULL;
static pcmk__common_args_t *args = NULL;
static pcmk__output_t *out = NULL;
static GOptionContext *context = NULL;
static gchar **processed_args = NULL;

static time_t last_refresh = 0;
volatile crm_trigger_t *refresh_trigger = NULL;

static enum pcmk__fence_history fence_history = pcmk__fence_history_none;

int interactive_fence_level = 0;

static pcmk__supported_format_t formats[] = {
#if CURSES_ENABLED
    CRM_MON_SUPPORTED_FORMAT_CURSES,
#endif
    PCMK__SUPPORTED_FORMAT_HTML,
    PCMK__SUPPORTED_FORMAT_NONE,
    PCMK__SUPPORTED_FORMAT_TEXT,
    PCMK__SUPPORTED_FORMAT_XML,
    { NULL, NULL, NULL }
};

PCMK__OUTPUT_ARGS("crm-mon-disconnected", "const char *",
                  "enum pcmk_pacemakerd_state")
static int
crm_mon_disconnected_default(pcmk__output_t *out, va_list args)
{
    return pcmk_rc_no_output;
}

PCMK__OUTPUT_ARGS("crm-mon-disconnected", "const char *",
                  "enum pcmk_pacemakerd_state")
static int
crm_mon_disconnected_html(pcmk__output_t *out, va_list args)
{
    const char *desc = va_arg(args, const char *);
    enum pcmk_pacemakerd_state state =
        (enum pcmk_pacemakerd_state) va_arg(args, int);

    if (out->dest != stdout) {
        out->reset(out);
    }

    pcmk__output_create_xml_text_node(out, "span", "Not connected to CIB");

    if (desc != NULL) {
        pcmk__output_create_xml_text_node(out, "span", ": ");
        pcmk__output_create_xml_text_node(out, "span", desc);
    }

    if (state != pcmk_pacemakerd_state_invalid) {
        const char *state_s = pcmk__pcmkd_state_enum2friendly(state);

        pcmk__output_create_xml_text_node(out, "span", " (");
        pcmk__output_create_xml_text_node(out, "span", state_s);
        pcmk__output_create_xml_text_node(out, "span", ")");
    }

    out->finish(out, CRM_EX_DISCONNECT, true, NULL);
    return pcmk_rc_ok;
}

PCMK__OUTPUT_ARGS("crm-mon-disconnected", "const char *",
                  "enum pcmk_pacemakerd_state")
static int
crm_mon_disconnected_text(pcmk__output_t *out, va_list args)
{
    const char *desc = va_arg(args, const char *);
    enum pcmk_pacemakerd_state state =
        (enum pcmk_pacemakerd_state) va_arg(args, int);
    int rc = pcmk_rc_ok;

    if (out->dest != stdout) {
        out->reset(out);
    }

    if (state != pcmk_pacemakerd_state_invalid) {
        rc = out->info(out, "Not connected to CIB%s%s (%s)",
                       (desc != NULL)? ": " : "", pcmk__s(desc, ""),
                       pcmk__pcmkd_state_enum2friendly(state));
    } else {
        rc = out->info(out, "Not connected to CIB%s%s",
                       (desc != NULL)? ": " : "", pcmk__s(desc, ""));
    }

    out->finish(out, CRM_EX_DISCONNECT, true, NULL);
    return rc;
}

PCMK__OUTPUT_ARGS("crm-mon-disconnected", "const char *",
                  "enum pcmk_pacemakerd_state")
static int
crm_mon_disconnected_xml(pcmk__output_t *out, va_list args)
{
    const char *desc = va_arg(args, const char *);
    enum pcmk_pacemakerd_state state =
        (enum pcmk_pacemakerd_state) va_arg(args, int);
    const char *state_s = NULL;

    if (out->dest != stdout) {
        out->reset(out);
    }

    if (state != pcmk_pacemakerd_state_invalid) {
        state_s = pcmk_pacemakerd_api_daemon_state_enum2text(state);
    }

    pcmk__output_create_xml_node(out, "crm-mon-disconnected",
                                 XML_ATTR_DESC, desc,
                                 "pacemakerd-state", state_s,
                                 NULL);

    out->finish(out, CRM_EX_DISCONNECT, true, NULL);
    return pcmk_rc_ok;
}

static pcmk__message_entry_t fmt_functions[] = {
    { "crm-mon-disconnected", "default", crm_mon_disconnected_default },
    { "crm-mon-disconnected", "html", crm_mon_disconnected_html },
    { "crm-mon-disconnected", "text", crm_mon_disconnected_text },
    { "crm-mon-disconnected", "xml", crm_mon_disconnected_xml },
    { NULL, NULL, NULL },
};

/* Define exit codes for monitoring-compatible output
 * For nagios plugins, the possibilities are
 * OK=0, WARN=1, CRIT=2, and UNKNOWN=3
 */
#define MON_STATUS_WARN    CRM_EX_ERROR
#define MON_STATUS_CRIT    CRM_EX_INVALID_PARAM
#define MON_STATUS_UNKNOWN CRM_EX_UNIMPLEMENT_FEATURE

#define RECONNECT_MSECS 5000

struct {
    guint reconnect_ms;
    enum mon_exec_mode exec_mode;
    gboolean fence_connect;
    gboolean print_pending;
    gboolean show_bans;
    gboolean watch_fencing;
    char *pid_file;
    char *external_agent;
    char *external_recipient;
    char *neg_location_prefix;
    char *only_node;
    char *only_rsc;
    GSList *user_includes_excludes;
    GSList *includes_excludes;
} options = {
    .reconnect_ms = RECONNECT_MSECS,
    .exec_mode = mon_exec_unset,
    .fence_connect = TRUE,
};

static crm_exit_t clean_up(crm_exit_t exit_code);
static void crm_diff_update(const char *event, xmlNode * msg);
static void clean_up_on_connection_failure(int rc);
static int mon_refresh_display(gpointer user_data);
static int setup_cib_connection(void);
static int setup_fencer_connection(void);
static int setup_api_connections(void);
static void mon_st_callback_event(stonith_t * st, stonith_event_t * e);
static void mon_st_callback_display(stonith_t * st, stonith_event_t * e);
static void refresh_after_event(gboolean data_updated, gboolean enforce);

static uint32_t
all_includes(mon_output_format_t fmt) {
    if (fmt == mon_output_monitor || fmt == mon_output_plain || fmt == mon_output_console) {
        return ~pcmk_section_options;
    } else {
        return pcmk_section_all;
    }
}

static uint32_t
default_includes(mon_output_format_t fmt) {
    switch (fmt) {
        case mon_output_monitor:
        case mon_output_plain:
        case mon_output_console:
        case mon_output_html:
        case mon_output_cgi:
            return pcmk_section_summary
                   |pcmk_section_nodes
                   |pcmk_section_resources
                   |pcmk_section_failures;

        case mon_output_xml:
            return all_includes(fmt);

        default:
            return 0;
    }
}

struct {
    const char *name;
    uint32_t bit;
} sections[] = {
    { "attributes", pcmk_section_attributes },
    { "bans", pcmk_section_bans },
    { "counts", pcmk_section_counts },
    { "dc", pcmk_section_dc },
    { "failcounts", pcmk_section_failcounts },
    { "failures", pcmk_section_failures },
    { PCMK__VALUE_FENCING, pcmk_section_fencing_all },
    { "fencing-failed", pcmk_section_fence_failed },
    { "fencing-pending", pcmk_section_fence_pending },
    { "fencing-succeeded", pcmk_section_fence_worked },
    { "maint-mode", pcmk_section_maint_mode },
    { "nodes", pcmk_section_nodes },
    { "operations", pcmk_section_operations },
    { "options", pcmk_section_options },
    { "resources", pcmk_section_resources },
    { "stack", pcmk_section_stack },
    { "summary", pcmk_section_summary },
    { "tickets", pcmk_section_tickets },
    { "times", pcmk_section_times },
    { NULL }
};

static uint32_t
find_section_bit(const char *name) {
    for (int i = 0; sections[i].name != NULL; i++) {
        if (pcmk__str_eq(sections[i].name, name, pcmk__str_casei)) {
            return sections[i].bit;
        }
    }

    return 0;
}

static gboolean
apply_exclude(const gchar *excludes, GError **error) {
    char **parts = NULL;
    gboolean result = TRUE;

    parts = g_strsplit(excludes, ",", 0);
    for (char **s = parts; *s != NULL; s++) {
        uint32_t bit = find_section_bit(*s);

        if (pcmk__str_eq(*s, "all", pcmk__str_none)) {
            show = 0;
        } else if (pcmk__str_eq(*s, PCMK__VALUE_NONE, pcmk__str_none)) {
            show = all_includes(output_format);
        } else if (bit != 0) {
            show &= ~bit;
        } else {
            g_set_error(error, PCMK__EXITC_ERROR, CRM_EX_USAGE,
                        "--exclude options: all, attributes, bans, counts, dc, "
                        "failcounts, failures, fencing, fencing-failed, "
                        "fencing-pending, fencing-succeeded, maint-mode, nodes, "
                        PCMK__VALUE_NONE ", operations, options, resources, "
                        "stack, summary, tickets, times");
            result = FALSE;
            break;
        }
    }
    g_strfreev(parts);
    return result;
}

static gboolean
apply_include(const gchar *includes, GError **error) {
    char **parts = NULL;
    gboolean result = TRUE;

    parts = g_strsplit(includes, ",", 0);
    for (char **s = parts; *s != NULL; s++) {
        uint32_t bit = find_section_bit(*s);

        if (pcmk__str_eq(*s, "all", pcmk__str_none)) {
            show = all_includes(output_format);
        } else if (pcmk__starts_with(*s, "bans")) {
            show |= pcmk_section_bans;
            if (options.neg_location_prefix != NULL) {
                free(options.neg_location_prefix);
                options.neg_location_prefix = NULL;
            }

            if (strlen(*s) > 4 && (*s)[4] == ':') {
                options.neg_location_prefix = strdup(*s+5);
            }
        } else if (pcmk__str_any_of(*s, "default", "defaults", NULL)) {
            show |= default_includes(output_format);
        } else if (pcmk__str_eq(*s, PCMK__VALUE_NONE, pcmk__str_none)) {
            show = 0;
        } else if (bit != 0) {
            show |= bit;
        } else {
            g_set_error(error, PCMK__EXITC_ERROR, CRM_EX_USAGE,
                        "--include options: all, attributes, bans[:PREFIX], counts, dc, "
                        "default, failcounts, failures, fencing, fencing-failed, "
                        "fencing-pending, fencing-succeeded, maint-mode, nodes, "
                        PCMK__VALUE_NONE ", operations, options, resources, "
                        "stack, summary, tickets, times");
            result = FALSE;
            break;
        }
    }
    g_strfreev(parts);
    return result;
}

static gboolean
apply_include_exclude(GSList *lst, GError **error) {
    gboolean rc = TRUE;
    GSList *node = lst;

    while (node != NULL) {
        char *s = node->data;

        if (pcmk__starts_with(s, "--include=")) {
            rc = apply_include(s+10, error);
        } else if (pcmk__starts_with(s, "-I=")) {
            rc = apply_include(s+3, error);
        } else if (pcmk__starts_with(s, "--exclude=")) {
            rc = apply_exclude(s+10, error);
        } else if (pcmk__starts_with(s, "-U=")) {
            rc = apply_exclude(s+3, error);
        }

        if (rc != TRUE) {
            break;
        }

        node = node->next;
    }

    return rc;
}

static gboolean
user_include_exclude_cb(const gchar *option_name, const gchar *optarg, gpointer data, GError **err) {
    char *s = crm_strdup_printf("%s=%s", option_name, optarg);

    options.user_includes_excludes = g_slist_append(options.user_includes_excludes, s);
    return TRUE;
}

static gboolean
include_exclude_cb(const gchar *option_name, const gchar *optarg, gpointer data, GError **err) {
    char *s = crm_strdup_printf("%s=%s", option_name, optarg);

    options.includes_excludes = g_slist_append(options.includes_excludes, s);
    return TRUE;
}

static gboolean
as_cgi_cb(const gchar *option_name, const gchar *optarg, gpointer data, GError **err) {
    pcmk__str_update(&args->output_ty, "html");
    output_format = mon_output_cgi;
    options.exec_mode = mon_exec_one_shot;
    return TRUE;
}

static gboolean
as_html_cb(const gchar *option_name, const gchar *optarg, gpointer data, GError **err) {
    pcmk__str_update(&args->output_dest, optarg);
    pcmk__str_update(&args->output_ty, "html");
    output_format = mon_output_html;
    umask(S_IWGRP | S_IWOTH);   // World-readable HTML
    return TRUE;
}

static gboolean
as_simple_cb(const gchar *option_name, const gchar *optarg, gpointer data, GError **err) {
    pcmk__str_update(&args->output_ty, "text");
    output_format = mon_output_monitor;
    options.exec_mode = mon_exec_one_shot;
    return TRUE;
}

static gboolean
as_xml_cb(const gchar *option_name, const gchar *optarg, gpointer data, GError **err) {
    pcmk__str_update(&args->output_ty, "xml");
    output_format = mon_output_legacy_xml;
    return TRUE;
}

static gboolean
fence_history_cb(const gchar *option_name, const gchar *optarg, gpointer data, GError **err) {
    if (optarg == NULL) {
        interactive_fence_level = 2;
    } else {
        pcmk__scan_min_int(optarg, &interactive_fence_level, 0);
    }

    switch (interactive_fence_level) {
        case 3:
            options.fence_connect = TRUE;
            fence_history = pcmk__fence_history_full;
            return include_exclude_cb("--include", PCMK__VALUE_FENCING, data,
                                      err);

        case 2:
            options.fence_connect = TRUE;
            fence_history = pcmk__fence_history_full;
            return include_exclude_cb("--include", PCMK__VALUE_FENCING, data,
                                      err);

        case 1:
            options.fence_connect = TRUE;
            fence_history = pcmk__fence_history_full;
            return include_exclude_cb("--include", "fencing-failed,fencing-pending", data, err);

        case 0:
            options.fence_connect = FALSE;
            fence_history = pcmk__fence_history_none;
            return include_exclude_cb("--exclude", PCMK__VALUE_FENCING, data,
                                      err);

        default:
            g_set_error(err, PCMK__EXITC_ERROR, CRM_EX_INVALID_PARAM, "Fence history must be 0-3");
            return FALSE;
    }
}

static gboolean
group_by_node_cb(const gchar *option_name, const gchar *optarg, gpointer data, GError **err) {
    show_opts |= pcmk_show_rscs_by_node;
    return TRUE;
}

static gboolean
hide_headers_cb(const gchar *option_name, const gchar *optarg, gpointer data, GError **err) {
    return user_include_exclude_cb("--exclude", "summary", data, err);
}

static gboolean
inactive_resources_cb(const gchar *option_name, const gchar *optarg, gpointer data, GError **err) {
    show_opts |= pcmk_show_inactive_rscs;
    return TRUE;
}

static gboolean
no_curses_cb(const gchar *option_name, const gchar *optarg, gpointer data, GError **err) {
    pcmk__str_update(&args->output_ty, "text");
    output_format = mon_output_plain;
    return TRUE;
}

static gboolean
print_brief_cb(const gchar *option_name, const gchar *optarg, gpointer data, GError **err) {
    show_opts |= pcmk_show_brief;
    return TRUE;
}

static gboolean
print_detail_cb(const gchar *option_name, const gchar *optarg, gpointer data, GError **err) {
    show_opts |= pcmk_show_details;
    return TRUE;
}

static gboolean
print_description_cb(const gchar *option_name, const gchar *optarg, gpointer data, GError **err) {
    show_opts |= pcmk_show_description;
    return TRUE;
}

static gboolean
print_timing_cb(const gchar *option_name, const gchar *optarg, gpointer data, GError **err) {
    show_opts |= pcmk_show_timing;
    return user_include_exclude_cb("--include", "operations", data, err);
}

static gboolean
reconnect_cb(const gchar *option_name, const gchar *optarg, gpointer data, GError **err) {
    int rc = crm_get_msec(optarg);

    if (rc == -1) {
        g_set_error(err, PCMK__EXITC_ERROR, CRM_EX_INVALID_PARAM, "Invalid value for -i: %s", optarg);
        return FALSE;
    } else {
        options.reconnect_ms = crm_parse_interval_spec(optarg);

        if (options.exec_mode != mon_exec_daemonized) {
            // Reconnect interval applies to daemonized too, so don't override
            options.exec_mode = mon_exec_update;
        }
    }

    return TRUE;
}

/*!
 * \internal
 * \brief Enable one-shot mode
 *
 * \param[in]  option_name  Name of option being parsed (ignored)
 * \param[in]  optarg       Value to be parsed (ignored)
 * \param[in]  data         User data (ignored)
 * \param[out] err          Where to store error (ignored)
 */
static gboolean
one_shot_cb(const gchar *option_name, const gchar *optarg, gpointer data,
            GError **err)
{
    options.exec_mode = mon_exec_one_shot;
    return TRUE;
}

/*!
 * \internal
 * \brief Enable daemonized mode
 *
 * \param[in]  option_name  Name of option being parsed (ignored)
 * \param[in]  optarg       Value to be parsed (ignored)
 * \param[in]  data         User data (ignored)
 * \param[out] err          Where to store error (ignored)
 */
static gboolean
daemonize_cb(const gchar *option_name, const gchar *optarg, gpointer data,
             GError **err)
{
    options.exec_mode = mon_exec_daemonized;
    return TRUE;
}

static gboolean
show_attributes_cb(const gchar *option_name, const gchar *optarg, gpointer data, GError **err) {
    return user_include_exclude_cb("--include", "attributes", data, err);
}

static gboolean
show_bans_cb(const gchar *option_name, const gchar *optarg, gpointer data, GError **err) {
    if (optarg != NULL) {
        char *s = crm_strdup_printf("bans:%s", optarg);
        gboolean rc = user_include_exclude_cb("--include", s, data, err);
        free(s);
        return rc;
    } else {
        return user_include_exclude_cb("--include", "bans", data, err);
    }
}

static gboolean
show_failcounts_cb(const gchar *option_name, const gchar *optarg, gpointer data, GError **err) {
    return user_include_exclude_cb("--include", "failcounts", data, err);
}

static gboolean
show_operations_cb(const gchar *option_name, const gchar *optarg, gpointer data, GError **err) {
    return user_include_exclude_cb("--include", "failcounts,operations", data, err);
}

static gboolean
show_tickets_cb(const gchar *option_name, const gchar *optarg, gpointer data, GError **err) {
    return user_include_exclude_cb("--include", "tickets", data, err);
}

static gboolean
use_cib_file_cb(const gchar *option_name, const gchar *optarg, gpointer data, GError **err) {
    setenv("CIB_file", optarg, 1);
    options.exec_mode = mon_exec_one_shot;
    return TRUE;
}

#define INDENT "                                    "

/* *INDENT-OFF* */
static GOptionEntry addl_entries[] = {
    { "interval", 'i', 0, G_OPTION_ARG_CALLBACK, reconnect_cb,
      "Update frequency (default is 5 seconds)",
      "TIMESPEC" },

    { "one-shot", '1', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK,
      one_shot_cb,
      "Display the cluster status once and exit",
      NULL },

    { "daemonize", 'd', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK,
      daemonize_cb,
      "Run in the background as a daemon.\n"
      INDENT "Requires at least one of --output-to and --external-agent.",
      NULL },

    { "pid-file", 'p', 0, G_OPTION_ARG_FILENAME, &options.pid_file,
      "(Advanced) Daemon pid file location",
      "FILE" },

    { "external-agent", 'E', 0, G_OPTION_ARG_FILENAME, &options.external_agent,
      "A program to run when resource operations take place",
      "FILE" },

    { "external-recipient", 'e', 0, G_OPTION_ARG_STRING, &options.external_recipient,
      "A recipient for your program (assuming you want the program to send something to someone).",
      "RCPT" },

    { "watch-fencing", 'W', 0, G_OPTION_ARG_NONE, &options.watch_fencing,
      "Listen for fencing events. For use with --external-agent.",
      NULL },

    { "xml-file", 'x', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_CALLBACK, use_cib_file_cb,
      NULL,
      NULL },

    { NULL }
};

static GOptionEntry display_entries[] = {
    { "include", 'I', 0, G_OPTION_ARG_CALLBACK, user_include_exclude_cb,
      "A list of sections to include in the output.\n"
      INDENT "See `Output Control` help for more information.",
      "SECTION(s)" },

    { "exclude", 'U', 0, G_OPTION_ARG_CALLBACK, user_include_exclude_cb,
      "A list of sections to exclude from the output.\n"
      INDENT "See `Output Control` help for more information.",
      "SECTION(s)" },

    { "node", 0, 0, G_OPTION_ARG_STRING, &options.only_node,
      "When displaying information about nodes, show only what's related to the given\n"
      INDENT "node, or to all nodes tagged with the given tag",
      "NODE" },

    { "resource", 0, 0, G_OPTION_ARG_STRING, &options.only_rsc,
      "When displaying information about resources, show only what's related to the given\n"
      INDENT "resource, or to all resources tagged with the given tag",
      "RSC" },

    { "group-by-node", 'n', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, group_by_node_cb,
      "Group resources by node",
      NULL },

    { "inactive", 'r', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, inactive_resources_cb,
      "Display inactive resources",
      NULL },

    { "failcounts", 'f', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, show_failcounts_cb,
      "Display resource fail counts",
      NULL },

    { "operations", 'o', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, show_operations_cb,
      "Display resource operation history",
      NULL },

    { "timing-details", 't', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, print_timing_cb,
      "Display resource operation history with timing details",
      NULL },

    { "tickets", 'c', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, show_tickets_cb,
      "Display cluster tickets",
      NULL },

    { "fence-history", 'm', G_OPTION_FLAG_OPTIONAL_ARG, G_OPTION_ARG_CALLBACK, fence_history_cb,
      "Show fence history:\n"
      INDENT "0=off, 1=failures and pending (default without option),\n"
      INDENT "2=add successes (default without value for option),\n"
      INDENT "3=show full history without reduction to most recent of each flavor",
      "LEVEL" },

    { "neg-locations", 'L', G_OPTION_FLAG_OPTIONAL_ARG, G_OPTION_ARG_CALLBACK, show_bans_cb,
      "Display negative location constraints [optionally filtered by id prefix]",
      NULL },

    { "show-node-attributes", 'A', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, show_attributes_cb,
      "Display node attributes",
      NULL },

    { "hide-headers", 'D', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, hide_headers_cb,
      "Hide all headers",
      NULL },

    { "show-detail", 'R', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, print_detail_cb,
      "Show more details (node IDs, individual clone instances)",
      NULL },

    { "show-description", 0, G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, print_description_cb,
      "Show resource descriptions",
      NULL },

    { "brief", 'b', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, print_brief_cb,
      "Brief output",
      NULL },

    { "pending", 'j', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &options.print_pending,
      "Display pending state if 'record-pending' is enabled",
      NULL },

    { NULL }
};

static GOptionEntry deprecated_entries[] = {
    { "as-html", 'h', G_OPTION_FLAG_FILENAME, G_OPTION_ARG_CALLBACK, as_html_cb,
      "Write cluster status to the named HTML file.\n"
      INDENT "Use --output-as=html --output-to=FILE instead.",
      "FILE" },

    { "as-xml", 'X', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, as_xml_cb,
      "Write cluster status as XML to stdout. This will enable one-shot mode.\n"
      INDENT "Use --output-as=xml instead.",
      NULL },

    { "simple-status", 's', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK,
      as_simple_cb,
      "Display the cluster status once as a simple one line output\n"
      INDENT "(suitable for nagios)",
      NULL },

    { "disable-ncurses", 'N', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, no_curses_cb,
      "Disable the use of ncurses.\n"
      INDENT "Use --output-as=text instead.",
      NULL },

    { "web-cgi", 'w', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, as_cgi_cb,
      "Web mode with output suitable for CGI (preselected when run as *.cgi).\n"
      INDENT "Use --output-as=html --html-cgi instead.",
      NULL },

    { NULL }
};
/* *INDENT-ON* */

/* Reconnect to the CIB and fencing agent after reconnect_ms has passed.  This sounds
 * like it would be more broadly useful, but only ever happens after a disconnect via
 * mon_cib_connection_destroy.
 */
static gboolean
reconnect_after_timeout(gpointer data)
{
#if CURSES_ENABLED
    if (output_format == mon_output_console) {
        clear();
        refresh();
    }
#endif

    out->transient(out, "Reconnecting...");
    if (setup_api_connections() == pcmk_rc_ok) {
        // Trigger redrawing the screen (needs reconnect_timer == 0)
        reconnect_timer = 0;
        refresh_after_event(FALSE, TRUE);
        return G_SOURCE_REMOVE;
    }

    out->message(out, "crm-mon-disconnected",
                 "Latest connection attempt failed", pcmkd_state);

    reconnect_timer = g_timeout_add(options.reconnect_ms,
                                    reconnect_after_timeout, NULL);
    return G_SOURCE_REMOVE;
}

/* Called from various places when we are disconnected from the CIB or from the
 * fencing agent.  If the CIB connection is still valid, this function will also
 * attempt to sign off and reconnect.
 */
static void
mon_cib_connection_destroy(gpointer user_data)
{
    const char *msg = "Connection to the cluster lost";

    pcmkd_state = pcmk_pacemakerd_state_invalid;

    /* No crm-mon-disconnected message for console; a working implementation
     * is not currently worth the effort
     */
    out->transient(out, "%s", msg);

    out->message(out, "crm-mon-disconnected", msg, pcmkd_state);

    if (refresh_timer != NULL) {
        /* we'll trigger a refresh after reconnect */
        mainloop_timer_stop(refresh_timer);
    }
    if (reconnect_timer) {
        /* we'll trigger a new reconnect-timeout at the end */
        g_source_remove(reconnect_timer);
        reconnect_timer = 0;
    }

    /* the client API won't properly reconnect notifications if they are still
     * in the table - so remove them
     */
    stonith_api_delete(st);
    st = NULL;

    if (cib) {
        cib->cmds->signoff(cib);
        reconnect_timer = g_timeout_add(options.reconnect_ms,
                                        reconnect_after_timeout, NULL);
    }
}

/* Signal handler installed into the mainloop for normal program shutdown */
static void
mon_shutdown(int nsig)
{
    clean_up(CRM_EX_OK);
}

#if CURSES_ENABLED
static volatile sighandler_t ncurses_winch_handler;

/* Signal handler installed the regular way (not into the main loop) for when
 * the screen is resized.  Commonly, this happens when running in an xterm and
 * the user changes its size.
 */
static void
mon_winresize(int nsig)
{
    static int not_done;
    int lines = 0, cols = 0;

    if (!not_done++) {
        if (ncurses_winch_handler)
            /* the original ncurses WINCH signal handler does the
             * magic of retrieving the new window size;
             * otherwise, we'd have to use ioctl or tgetent */
            (*ncurses_winch_handler) (SIGWINCH);
        getmaxyx(stdscr, lines, cols);
        resizeterm(lines, cols);
        /* Alert the mainloop code we'd like the refresh_trigger to run next
         * time the mainloop gets around to checking.
         */
        mainloop_set_trigger((crm_trigger_t *) refresh_trigger);
    }
    not_done--;
}
#endif

static int
setup_fencer_connection(void)
{
    int rc = pcmk_ok;

    if (options.fence_connect && st == NULL) {
        st = stonith_api_new();
    }

    if (!options.fence_connect || st == NULL || st->state != stonith_disconnected) {
        return rc;
    }

    rc = st->cmds->connect(st, crm_system_name, NULL);
    if (rc == pcmk_ok) {
        crm_trace("Setting up stonith callbacks");
        if (options.watch_fencing) {
            st->cmds->register_notification(st, T_STONITH_NOTIFY_DISCONNECT,
                                            mon_st_callback_event);
            st->cmds->register_notification(st, T_STONITH_NOTIFY_FENCE, mon_st_callback_event);
        } else {
            st->cmds->register_notification(st, T_STONITH_NOTIFY_DISCONNECT,
                                            mon_st_callback_display);
            st->cmds->register_notification(st, T_STONITH_NOTIFY_HISTORY, mon_st_callback_display);
        }
    } else {
        stonith_api_delete(st);
        st = NULL;
    }

    return rc;
}

static int
setup_cib_connection(void)
{
    int rc = pcmk_rc_ok;

    CRM_CHECK(cib != NULL, return EINVAL);

    if (cib->state != cib_disconnected) {
        // Already connected with notifications registered for CIB updates
        return rc;
    }

    rc = cib__signon_query(out, &cib, &current_cib);

    if (rc == pcmk_rc_ok) {
        rc = pcmk_legacy2rc(cib->cmds->set_connection_dnotify(cib,
            mon_cib_connection_destroy));
        if (rc == EPROTONOSUPPORT) {
            out->err(out,
                     "CIB client does not support connection loss "
                     "notifications; crm_mon will be unable to reconnect after "
                     "connection loss");
            rc = pcmk_rc_ok;
        }

        if (rc == pcmk_rc_ok) {
            cib->cmds->del_notify_callback(cib, T_CIB_DIFF_NOTIFY,
                                           crm_diff_update);
            rc = pcmk_legacy2rc(cib->cmds->add_notify_callback(cib,
                                    T_CIB_DIFF_NOTIFY, crm_diff_update));
        }

        if (rc != pcmk_rc_ok) {
            if (rc == EPROTONOSUPPORT) {
                out->err(out,
                         "CIB client does not support CIB diff "
                         "notifications");
            } else {
                out->err(out, "CIB diff notification setup failed");
            }

            out->err(out, "Cannot monitor CIB changes; exiting");
            cib__clean_up_connection(&cib);
            stonith_api_delete(st);
            st = NULL;
        }
    }
    return rc;
}

/* This is used to set up the fencing options after the interactive UI has been stared.
 * fence_history_cb can't be used because it builds up a list of includes/excludes that
 * then have to be processed with apply_include_exclude and that could affect other
 * things.
 */
static void
set_fencing_options(int level)
{
    switch (level) {
        case 3:
            options.fence_connect = TRUE;
            fence_history = pcmk__fence_history_full;
            show |= pcmk_section_fencing_all;
            break;

        case 2:
            options.fence_connect = TRUE;
            fence_history = pcmk__fence_history_full;
            show |= pcmk_section_fencing_all;
            break;

        case 1:
            options.fence_connect = TRUE;
            fence_history = pcmk__fence_history_full;
            show |= pcmk_section_fence_failed | pcmk_section_fence_pending;
            break;

        default:
            interactive_fence_level = 0;
            options.fence_connect = FALSE;
            fence_history = pcmk__fence_history_none;
            show &= ~pcmk_section_fencing_all;
            break;
    }
}

static int
setup_api_connections(void)
{
    int rc = pcmk_rc_ok;

    CRM_CHECK(cib != NULL, return EINVAL);

    if (cib->state != cib_disconnected) {
        return rc;
    }

    if (cib->variant == cib_native) {
        rc = pcmk__pacemakerd_status(out, crm_system_name,
                                     options.reconnect_ms / 2, false,
                                     &pcmkd_state);
        if (rc != pcmk_rc_ok) {
            return rc;
        }

        switch (pcmkd_state) {
            case pcmk_pacemakerd_state_running:
            case pcmk_pacemakerd_state_remote:
            case pcmk_pacemakerd_state_shutting_down:
                /* Fencer and CIB may still be available while shutting down or
                 * running on a Pacemaker Remote node
                 */
                break;
            default:
                // Fencer and CIB are definitely unavailable
                return ENOTCONN;
        }

        setup_fencer_connection();
    }

    rc = setup_cib_connection();
    return rc;
}

#if CURSES_ENABLED
static const char *
get_option_desc(char c)
{
    const char *desc = "No help available";

    for (GOptionEntry *entry = display_entries; entry != NULL; entry++) {
        if (entry->short_name == c) {
            desc = entry->description;
            break;
        }
    }
    return desc;
}

#define print_option_help(out, option, condition) \
    curses_formatted_printf(out, "%c %c: \t%s\n", ((condition)? '*': ' '), option, get_option_desc(option));

/* This function is called from the main loop when there is something to be read
 * on stdin, like an interactive user's keystroke.  All it does is read the keystroke,
 * set flags (or show the page showing which keystrokes are valid), and redraw the
 * screen.  It does not do anything with connections to the CIB or fencing agent
 * agent what would happen in mon_refresh_display.
 */
static gboolean
detect_user_input(GIOChannel *channel, GIOCondition condition, gpointer user_data)
{
    int c;
    gboolean config_mode = FALSE;

    while (1) {

        /* Get user input */
        c = getchar();

        switch (c) {
            case 'm':
                interactive_fence_level++;
                if (interactive_fence_level > 3) {
                    interactive_fence_level = 0;
                }

                set_fencing_options(interactive_fence_level);
                break;
            case 'c':
                show ^= pcmk_section_tickets;
                break;
            case 'f':
                show ^= pcmk_section_failcounts;
                break;
            case 'n':
                show_opts ^= pcmk_show_rscs_by_node;
                break;
            case 'o':
                show ^= pcmk_section_operations;
                if (!pcmk_is_set(show, pcmk_section_operations)) {
                    show_opts &= ~pcmk_show_timing;
                }
                break;
            case 'r':
                show_opts ^= pcmk_show_inactive_rscs;
                break;
            case 'R':
                show_opts ^= pcmk_show_details;
#ifdef PCMK__COMPAT_2_0
                // Keep failed action output the same as 2.0.x
                show_opts |= pcmk_show_failed_detail;
#endif
                break;
            case 't':
                show_opts ^= pcmk_show_timing;
                if (pcmk_is_set(show_opts, pcmk_show_timing)) {
                    show |= pcmk_section_operations;
                }
                break;
            case 'A':
                show ^= pcmk_section_attributes;
                break;
            case 'L':
                show ^= pcmk_section_bans;
                break;
            case 'D':
                /* If any header is shown, clear them all, otherwise set them all */
                if (pcmk_any_flags_set(show, pcmk_section_summary)) {
                    show &= ~pcmk_section_summary;
                } else {
                    show |= pcmk_section_summary;
                }
                /* Regardless, we don't show options in console mode. */
                show &= ~pcmk_section_options;
                break;
            case 'b':
                show_opts ^= pcmk_show_brief;
                break;
            case 'j':
                show_opts ^= pcmk_show_pending;
                break;
            case '?':
                config_mode = TRUE;
                break;
            default:
                /* All other keys just redraw the screen. */
                goto refresh;
        }

        if (!config_mode)
            goto refresh;

        clear();
        refresh();

        curses_formatted_printf(out, "%s", "Display option change mode\n");
        print_option_help(out, 'c', pcmk_is_set(show, pcmk_section_tickets));
        print_option_help(out, 'f', pcmk_is_set(show, pcmk_section_failcounts));
        print_option_help(out, 'n', pcmk_is_set(show_opts, pcmk_show_rscs_by_node));
        print_option_help(out, 'o', pcmk_is_set(show, pcmk_section_operations));
        print_option_help(out, 'r', pcmk_is_set(show_opts, pcmk_show_inactive_rscs));
        print_option_help(out, 't', pcmk_is_set(show_opts, pcmk_show_timing));
        print_option_help(out, 'A', pcmk_is_set(show, pcmk_section_attributes));
        print_option_help(out, 'L', pcmk_is_set(show, pcmk_section_bans));
        print_option_help(out, 'D', !pcmk_is_set(show, pcmk_section_summary));
#ifdef PCMK__COMPAT_2_0
        print_option_help(out, 'R', pcmk_any_flags_set(show_opts, pcmk_show_details & ~pcmk_show_failed_detail));
#else
        print_option_help(out, 'R', pcmk_any_flags_set(show_opts, pcmk_show_details));
#endif
        print_option_help(out, 'b', pcmk_is_set(show_opts, pcmk_show_brief));
        print_option_help(out, 'j', pcmk_is_set(show_opts, pcmk_show_pending));
        curses_formatted_printf(out, "%d m: \t%s\n", interactive_fence_level, get_option_desc('m'));
        curses_formatted_printf(out, "%s", "\nToggle fields via field letter, type any other key to return\n");
    }

refresh:
    refresh_after_event(FALSE, TRUE);

    return TRUE;
}
#endif  // CURSES_ENABLED

// Basically crm_signal_handler(SIGCHLD, SIG_IGN) plus the SA_NOCLDWAIT flag
static void
avoid_zombies(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(struct sigaction));
    if (sigemptyset(&sa.sa_mask) < 0) {
        crm_warn("Cannot avoid zombies: %s", pcmk_rc_str(errno));
        return;
    }
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = SA_RESTART|SA_NOCLDWAIT;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        crm_warn("Cannot avoid zombies: %s", pcmk_rc_str(errno));
    }
}

static GOptionContext *
build_arg_context(pcmk__common_args_t *args, GOptionGroup **group) {
    GOptionContext *context = NULL;

    GOptionEntry extra_prog_entries[] = {
        { "quiet", 'Q', 0, G_OPTION_ARG_NONE, &(args->quiet),
          "Be less descriptive in output.",
          NULL },

        { NULL }
    };

#if CURSES_ENABLED
    const char *fmts = "console (default), html, text, xml, none";
#else
    const char *fmts = "text (default), html, xml, none";
#endif // CURSES_ENABLED
    const char *desc = NULL;

    desc = "Notes:\n\n"
           "If this program is called as crm_mon.cgi, --output-as=html and\n"
           "--html-cgi are automatically added to the command line\n"
           "arguments.\n\n"

           "Time Specification:\n\n"
           "The TIMESPEC in any command line option can be specified in many\n"
           "different formats. It can be an integer number of seconds, a\n"
           "number plus units (us/usec/ms/msec/s/sec/m/min/h/hr), or an ISO\n"
           "8601 period specification.\n\n"

           "Output Control:\n\n"
           "By default, a particular set of sections are written to the\n"
           "output destination. The default varies based on the output\n"
           "format: XML includes all sections by default, while other output\n"
           "formats include less. This set can be modified with the --include\n"
           "and --exclude command line options. Each option may be passed\n"
           "multiple times, and each can specify a comma-separated list of\n"
           "sections. The options are applied to the default set, in order\n"
           "from left to right as they are passed on the command line. For a\n"
           "list of valid sections, pass --include=list or --exclude=list.\n\n"

           "Interactive Use:\n\n"
#if CURSES_ENABLED
           "When run interactively, crm_mon can be told to hide and show\n"
           "various sections of output. To see a help screen explaining the\n"
           "options, press '?'. Any key stroke aside from those listed will\n"
           "cause the screen to refresh.\n\n"
#else
           "The local installation of Pacemaker was built without support for\n"
           "interactive (console) mode. A curses library must be available at\n"
           "build time to support interactive mode.\n\n"
#endif // CURSES_ENABLED

           "Examples:\n\n"
#if CURSES_ENABLED
           "Display the cluster status on the console with updates as they\n"
           "occur:\n\n"
           "\tcrm_mon\n\n"
#endif // CURSES_ENABLED

           "Display the cluster status once and exit:\n\n"
           "\tcrm_mon -1\n\n"

           "Display the cluster status, group resources by node, and include\n"
           "inactive resources in the list:\n\n"
           "\tcrm_mon --group-by-node --inactive\n\n"

           "Start crm_mon as a background daemon and have it write the\n"
           "cluster status to an HTML file:\n\n"
           "\tcrm_mon --daemonize --output-as html "
           "--output-to /path/to/docroot/filename.html\n\n"

           "Display the cluster status as XML:\n\n"
           "\tcrm_mon --output-as xml\n\n";

    context = pcmk__build_arg_context(args, fmts, group, NULL);
    pcmk__add_main_args(context, extra_prog_entries);
    g_option_context_set_description(context, desc);

    pcmk__add_arg_group(context, "display", "Display Options:",
                        "Show display options", display_entries);
    pcmk__add_arg_group(context, "additional", "Additional Options:",
                        "Show additional options", addl_entries);
    pcmk__add_arg_group(context, "deprecated", "Deprecated Options:",
                        "Show deprecated options", deprecated_entries);

    return context;
}

/* If certain format options were specified, we want to set some extra
 * options.  We can just process these like they were given on the
 * command line.
 */
static void
add_output_args(void) {
    GError *err = NULL;

    if (output_format == mon_output_plain) {
        if (!pcmk__force_args(context, &err, "%s --text-fancy", g_get_prgname())) {
            g_propagate_error(&error, err);
            clean_up(CRM_EX_USAGE);
        }
    } else if (output_format == mon_output_cgi) {
        if (!pcmk__force_args(context, &err, "%s --html-cgi", g_get_prgname())) {
            g_propagate_error(&error, err);
            clean_up(CRM_EX_USAGE);
        }
    } else if (output_format == mon_output_xml) {
        if (!pcmk__force_args(context, &err, "%s --xml-simple-list --xml-substitute", g_get_prgname())) {
            g_propagate_error(&error, err);
            clean_up(CRM_EX_USAGE);
        }
    } else if (output_format == mon_output_legacy_xml) {
        output_format = mon_output_xml;
        if (!pcmk__force_args(context, &err, "%s --xml-legacy --xml-substitute", g_get_prgname())) {
            g_propagate_error(&error, err);
            clean_up(CRM_EX_USAGE);
        }
    }
}

/*!
 * \internal
 * \brief Set output format based on \p --output-as arguments and mode arguments
 *
 * When the deprecated output format arguments (\p --as-cgi, \p --as-html,
 * \p --simple-status, \p --as-xml) are parsed, callback functions set
 * \p output_format (and the umask if appropriate). If none of the deprecated
 * arguments were specified, this function does the same based on the current
 * \p --output-as arguments and the \p --one-shot and \p --daemonize arguments.
 *
 * \param[in,out] args  Command line arguments
 */
static void
reconcile_output_format(pcmk__common_args_t *args)
{
    if (output_format != mon_output_unset) {
        /* One of the deprecated arguments was used, and we're finished. Note
         * that this means the deprecated arguments take precedence.
         */
        return;
    }

    if (pcmk__str_eq(args->output_ty, "none", pcmk__str_none)) {
        output_format = mon_output_none;

    } else if (pcmk__str_eq(args->output_ty, "html", pcmk__str_none)) {
        output_format = mon_output_html;
        umask(S_IWGRP | S_IWOTH);   // World-readable HTML

    } else if (pcmk__str_eq(args->output_ty, "xml", pcmk__str_none)) {
        output_format = mon_output_xml;

#if CURSES_ENABLED
    } else if (pcmk__str_eq(args->output_ty, "console",
                            pcmk__str_null_matches)) {
        /* Console is the default format if no conflicting options are given.
         *
         * Use text output instead if one of the following conditions is met:
         * * We've requested daemonized or one-shot mode (console output is
         *   incompatible with modes other than mon_exec_update)
         * * We requested the version, which is effectively one-shot
         * * We specified a non-stdout output destination (console mode is
         *   compatible only with stdout)
         */
        if ((options.exec_mode == mon_exec_daemonized)
            || (options.exec_mode == mon_exec_one_shot)
            || args->version
            || !pcmk__str_eq(args->output_dest, "-", pcmk__str_null_matches)) {

            pcmk__str_update(&args->output_ty, "text");
            output_format = mon_output_plain;
        } else {
            pcmk__str_update(&args->output_ty, "console");
            output_format = mon_output_console;
            crm_enable_stderr(FALSE);
        }
#endif // CURSES_ENABLED

    } else if (pcmk__str_eq(args->output_ty, "text", pcmk__str_null_matches)) {
        /* Text output was explicitly requested, or it's the default because
         * curses is not enabled
         */
        pcmk__str_update(&args->output_ty, "text");
        output_format = mon_output_plain;
    }

    // Otherwise, invalid format. Let pcmk__output_new() throw an error.
}

/*!
 * \internal
 * \brief Set execution mode to the output format's default if appropriate
 *
 * \param[in,out] args  Command line arguments
 */
static void
set_default_exec_mode(const pcmk__common_args_t *args)
{
    if (output_format == mon_output_console) {
        /* Update is the only valid mode for console, but set here instead of
         * reconcile_output_format() for isolation and consistency
         */
        options.exec_mode = mon_exec_update;

    } else if (options.exec_mode == mon_exec_unset) {
        // Default to one-shot mode for all other formats
        options.exec_mode = mon_exec_one_shot;

    } else if ((options.exec_mode == mon_exec_update)
               && pcmk__str_eq(args->output_dest, "-",
                               pcmk__str_null_matches)) {
        // If not using console format, update mode cannot be used with stdout
        options.exec_mode = mon_exec_one_shot;
    }
}

static void
clean_up_on_connection_failure(int rc)
{
    if (output_format == mon_output_monitor) {
        g_set_error(&error, PCMK__EXITC_ERROR, CRM_EX_ERROR, "CLUSTER CRIT: Connection to cluster failed: %s",
                    pcmk_rc_str(rc));
        clean_up(MON_STATUS_CRIT);
    } else if (rc == ENOTCONN) {
        if (pcmkd_state == pcmk_pacemakerd_state_remote) {
            g_set_error(&error, PCMK__EXITC_ERROR, CRM_EX_ERROR, "Error: remote-node not connected to cluster");
        } else {
            g_set_error(&error, PCMK__EXITC_ERROR, CRM_EX_ERROR, "Error: cluster is not available on this node");
        }
    } else {
        g_set_error(&error, PCMK__EXITC_ERROR, CRM_EX_ERROR, "Connection to cluster failed: %s", pcmk_rc_str(rc));
    }

    clean_up(pcmk_rc2exitc(rc));
}

static void
one_shot(void)
{
    int rc = pcmk__status(out, cib, fence_history, show, show_opts,
                          options.only_node, options.only_rsc,
                          options.neg_location_prefix,
                          output_format == mon_output_monitor, 0);

    if (rc == pcmk_rc_ok) {
        clean_up(pcmk_rc2exitc(rc));
    } else {
        clean_up_on_connection_failure(rc);
    }
}

static void
exit_on_invalid_cib(void)
{
    if (cib != NULL) {
        return;
    }

    // Shouldn't really be possible
    g_set_error(&error, PCMK__EXITC_ERROR, CRM_EX_ERROR, "Invalid CIB source");
    clean_up(CRM_EX_ERROR);
}

int
main(int argc, char **argv)
{
    int rc = pcmk_rc_ok;
    GOptionGroup *output_group = NULL;

    args = pcmk__new_common_args(SUMMARY);
    context = build_arg_context(args, &output_group);
    pcmk__register_formats(output_group, formats);

    options.pid_file = strdup("/tmp/ClusterMon.pid");
    pcmk__cli_init_logging("crm_mon", 0);

    // Avoid needing to wait for subprocesses forked for -E/--external-agent
    avoid_zombies();

    if (pcmk__ends_with_ext(argv[0], ".cgi")) {
        output_format = mon_output_cgi;
        options.exec_mode = mon_exec_one_shot;
    }

    processed_args = pcmk__cmdline_preproc(argv, "ehimpxEILU");

    fence_history_cb("--fence-history", "1", NULL, NULL);

    /* Set an HTML title regardless of what format we will eventually use.  This can't
     * be done in add_output_args.  That function is called after command line
     * arguments are processed in the next block, which means it'll override whatever
     * title the user provides.  Doing this here means the user can give their own
     * title on the command line.
     */
    if (!pcmk__force_args(context, &error, "%s --html-title \"Cluster Status\"",
                          g_get_prgname())) {
        return clean_up(CRM_EX_USAGE);
    }

    if (!g_option_context_parse_strv(context, &processed_args, &error)) {
        return clean_up(CRM_EX_USAGE);
    }

    for (int i = 0; i < args->verbosity; i++) {
        crm_bump_log_level(argc, argv);
    }

    if (!args->version) {
        if (args->quiet) {
            include_exclude_cb("--exclude", "times", NULL, NULL);
        }

        if (options.watch_fencing) {
            fence_history_cb("--fence-history", "0", NULL, NULL);
            options.fence_connect = TRUE;
        }

        /* create the cib-object early to be able to do further
         * decisions based on the cib-source
         */
        cib = cib_new();

        exit_on_invalid_cib();

        switch (cib->variant) {
            case cib_native:
                // Everything (fencer, CIB, pcmkd status) should be available
                break;

            case cib_file:
                // Live fence history is not meaningful
                fence_history_cb("--fence-history", "0", NULL, NULL);

                /* Notifications are unsupported; nothing to monitor
                 * @COMPAT: Let setup_cib_connection() handle this by exiting?
                 */
                options.exec_mode = mon_exec_one_shot;
                break;

            case cib_remote:
                // We won't receive any fencing updates
                fence_history_cb("--fence-history", "0", NULL, NULL);
                break;

            default:
                /* something is odd */
                exit_on_invalid_cib();
                break;
        }

        if ((options.exec_mode == mon_exec_daemonized)
            && !options.external_agent
            && pcmk__str_eq(args->output_dest, "-", pcmk__str_null_matches)) {

            g_set_error(&error, PCMK__EXITC_ERROR, CRM_EX_USAGE,
                        "--daemonize requires at least one of --output-to "
                        "(with value not set to '-') and --external-agent");
            return clean_up(CRM_EX_USAGE);
        }
    }

    reconcile_output_format(args);
    set_default_exec_mode(args);
    add_output_args();

    /* output_format MUST NOT BE CHANGED AFTER THIS POINT. */

    rc = pcmk__output_new(&out, args->output_ty, args->output_dest, argv);
    if (rc != pcmk_rc_ok) {
        g_set_error(&error, PCMK__EXITC_ERROR, CRM_EX_ERROR, "Error creating output format %s: %s",
                    args->output_ty, pcmk_rc_str(rc));
        return clean_up(CRM_EX_ERROR);
    }

    /* If we had a valid format for pcmk__output_new(), output_format should be
     * set by now.
     */
    CRM_ASSERT(output_format != mon_output_unset);

    if (options.exec_mode == mon_exec_daemonized) {
        if (!options.external_agent && (output_format == mon_output_none)) {
            g_set_error(&error, PCMK__EXITC_ERROR, CRM_EX_USAGE,
                        "--daemonize requires --external-agent if used with "
                        "--output-as=none");
            return clean_up(CRM_EX_USAGE);
        }
        crm_enable_stderr(FALSE);
        cib_delete(cib);
        cib = NULL;
        pcmk__daemonize(crm_system_name, options.pid_file);
        cib = cib_new();
        exit_on_invalid_cib();
    }

    show = default_includes(output_format);

    /* Apply --include/--exclude flags we used internally.  There's no error reporting
     * here because this would be a programming error.
     */
    apply_include_exclude(options.includes_excludes, &error);

    /* And now apply any --include/--exclude flags the user gave on the command line.
     * These are done in a separate pass from the internal ones because we want to
     * make sure whatever the user specifies overrides whatever we do.
     */
    if (!apply_include_exclude(options.user_includes_excludes, &error)) {
        return clean_up(CRM_EX_USAGE);
    }

    /* Sync up the initial value of interactive_fence_level with whatever was set with
     * --include/--exclude= options.
     */
    if (pcmk_all_flags_set(show, pcmk_section_fencing_all)) {
        interactive_fence_level = 3;
    } else if (pcmk_is_set(show, pcmk_section_fence_worked)) {
        interactive_fence_level = 2;
    } else if (pcmk_any_flags_set(show, pcmk_section_fence_failed | pcmk_section_fence_pending)) {
        interactive_fence_level = 1;
    } else {
        interactive_fence_level = 0;
    }

    pcmk__register_lib_messages(out);
    crm_mon_register_messages(out);
    pe__register_messages(out);
    stonith__register_messages(out);

    // Messages internal to this file, nothing curses-specific
    pcmk__register_messages(out, fmt_functions);

    if (args->version) {
        out->version(out, false);
        return clean_up(CRM_EX_OK);
    }

    /* Extra sanity checks when in CGI mode */
    if (output_format == mon_output_cgi) {
        if (cib->variant == cib_file) {
            g_set_error(&error, PCMK__EXITC_ERROR, CRM_EX_USAGE, "CGI mode used with CIB file");
            return clean_up(CRM_EX_USAGE);
        } else if (options.external_agent != NULL) {
            g_set_error(&error, PCMK__EXITC_ERROR, CRM_EX_USAGE, "CGI mode cannot be used with --external-agent");
            return clean_up(CRM_EX_USAGE);
        } else if (options.exec_mode == mon_exec_daemonized) {
            g_set_error(&error, PCMK__EXITC_ERROR, CRM_EX_USAGE, "CGI mode cannot be used with -d");
            return clean_up(CRM_EX_USAGE);
        }
    }

    if (output_format == mon_output_xml) {
        show_opts |= pcmk_show_inactive_rscs | pcmk_show_timing;
    }

    if ((output_format == mon_output_html || output_format == mon_output_cgi) &&
        out->dest != stdout) {
        pcmk__html_add_header("meta", "http-equiv", "refresh", "content",
                              pcmk__itoa(options.reconnect_ms / 1000), NULL);
    }

#ifdef PCMK__COMPAT_2_0
    // Keep failed action output the same as 2.0.x
    show_opts |= pcmk_show_failed_detail;
#endif

    crm_info("Starting %s", crm_system_name);

    cib__set_output(cib, out);

    if (options.exec_mode == mon_exec_one_shot) {
        one_shot();
    }

    out->message(out, "crm-mon-disconnected",
                 "Waiting for initial connection", pcmkd_state);
    do {
        out->transient(out, "Connecting to cluster...");
        rc = setup_api_connections();

        if (rc != pcmk_rc_ok) {
            if ((rc == ENOTCONN) || (rc == ECONNREFUSED)) {
                out->transient(out, "Connection failed. Retrying in %ums...",
                               options.reconnect_ms);
            }

            // Give some time to view all output even if we won't retry
            pcmk__sleep_ms(options.reconnect_ms);
#if CURSES_ENABLED
            if (output_format == mon_output_console) {
                clear();
                refresh();
            }
#endif
        }
    } while ((rc == ENOTCONN) || (rc == ECONNREFUSED));

    if (rc != pcmk_rc_ok) {
        clean_up_on_connection_failure(rc);
    }

    set_fencing_options(interactive_fence_level);
    mon_refresh_display(NULL);

    mainloop = g_main_loop_new(NULL, FALSE);

    mainloop_add_signal(SIGTERM, mon_shutdown);
    mainloop_add_signal(SIGINT, mon_shutdown);
#if CURSES_ENABLED
    if (output_format == mon_output_console) {
        ncurses_winch_handler = crm_signal_handler(SIGWINCH, mon_winresize);
        if (ncurses_winch_handler == SIG_DFL ||
            ncurses_winch_handler == SIG_IGN || ncurses_winch_handler == SIG_ERR)
            ncurses_winch_handler = NULL;

        io_channel = g_io_channel_unix_new(STDIN_FILENO);
        g_io_add_watch(io_channel, G_IO_IN, detect_user_input, NULL);
    }
#endif

    /* When refresh_trigger->trigger is set to TRUE, call mon_refresh_display.  In
     * this file, that is anywhere mainloop_set_trigger is called.
     */
    refresh_trigger = mainloop_add_trigger(G_PRIORITY_LOW, mon_refresh_display, NULL);

    g_main_loop_run(mainloop);
    g_main_loop_unref(mainloop);

    if (io_channel != NULL) {
        g_io_channel_shutdown(io_channel, TRUE, NULL);
    }

    crm_info("Exiting %s", crm_system_name);

    return clean_up(CRM_EX_OK);
}

static int
send_custom_trap(const char *node, const char *rsc, const char *task, int target_rc, int rc,
                 int status, const char *desc)
{
    pid_t pid;

    /*setenv needs chars, these are ints */
    char *rc_s = pcmk__itoa(rc);
    char *status_s = pcmk__itoa(status);
    char *target_rc_s = pcmk__itoa(target_rc);

    crm_debug("Sending external notification to '%s' via '%s'", options.external_recipient, options.external_agent);

    if(rsc) {
        setenv("CRM_notify_rsc", rsc, 1);
    }
    if (options.external_recipient) {
        setenv("CRM_notify_recipient", options.external_recipient, 1);
    }
    setenv("CRM_notify_node", node, 1);
    setenv("CRM_notify_task", task, 1);
    setenv("CRM_notify_desc", desc, 1);
    setenv("CRM_notify_rc", rc_s, 1);
    setenv("CRM_notify_target_rc", target_rc_s, 1);
    setenv("CRM_notify_status", status_s, 1);

    pid = fork();
    if (pid == -1) {
        crm_perror(LOG_ERR, "notification fork() failed.");
    }
    if (pid == 0) {
        /* crm_debug("notification: I am the child. Executing the nofitication program."); */
        execl(options.external_agent, options.external_agent, NULL);
        exit(CRM_EX_ERROR);
    }

    crm_trace("Finished running custom notification program '%s'.", options.external_agent);
    free(target_rc_s);
    free(status_s);
    free(rc_s);
    return 0;
}

static int
handle_rsc_op(xmlNode *xml, void *userdata)
{
    const char *node_id = (const char *) userdata;
    int rc = -1;
    int status = -1;
    int target_rc = -1;
    gboolean notify = TRUE;

    char *rsc = NULL;
    char *task = NULL;
    const char *desc = NULL;
    const char *magic = NULL;
    const char *id = NULL;
    const char *node = NULL;

    xmlNode *n = xml;
    xmlNode * rsc_op = xml;

    if(strcmp((const char*)xml->name, XML_LRM_TAG_RSC_OP) != 0) {
        pcmk__xe_foreach_child(xml, NULL, handle_rsc_op, (void *) node_id);
        return pcmk_rc_ok;
    }

    id = pe__xe_history_key(rsc_op);

    magic = crm_element_value(rsc_op, XML_ATTR_TRANSITION_MAGIC);
    if (magic == NULL) {
        /* non-change */
        return pcmk_rc_ok;
    }

    if (!decode_transition_magic(magic, NULL, NULL, NULL, &status, &rc,
                                 &target_rc)) {
        crm_err("Invalid event %s detected for %s", magic, id);
        return pcmk_rc_ok;
    }

    if (parse_op_key(id, &rsc, &task, NULL) == FALSE) {
        crm_err("Invalid event detected for %s", id);
        goto bail;
    }

    node = crm_element_value(rsc_op, XML_LRM_ATTR_TARGET);

    while ((n != NULL) && !pcmk__xe_is(n, XML_CIB_TAG_STATE)) {
        n = n->parent;
    }

    if(node == NULL && n) {
        node = crm_element_value(n, XML_ATTR_UNAME);
    }

    if (node == NULL && n) {
        node = ID(n);
    }

    if (node == NULL) {
        node = node_id;
    }

    if (node == NULL) {
        crm_err("No node detected for event %s (%s)", magic, id);
        goto bail;
    }

    /* look up where we expected it to be? */
    desc = pcmk_rc_str(pcmk_rc_ok);
    if ((status == PCMK_EXEC_DONE) && (target_rc == rc)) {
        crm_notice("%s of %s on %s completed: %s", task, rsc, node, desc);
        if (rc == PCMK_OCF_NOT_RUNNING) {
            notify = FALSE;
        }

    } else if (status == PCMK_EXEC_DONE) {
        desc = services_ocf_exitcode_str(rc);
        crm_warn("%s of %s on %s failed: %s", task, rsc, node, desc);

    } else {
        desc = pcmk_exec_status_str(status);
        crm_warn("%s of %s on %s failed: %s", task, rsc, node, desc);
    }

    if (notify && options.external_agent) {
        send_custom_trap(node, rsc, task, target_rc, rc, status, desc);
    }

  bail:
    free(rsc);
    free(task);
    return pcmk_rc_ok;
}

/* This function is just a wrapper around mainloop_set_trigger so that it can be
 * called from a mainloop directly.  It's simply another way of ensuring the screen
 * gets redrawn.
 */
static gboolean
mon_trigger_refresh(gpointer user_data)
{
    mainloop_set_trigger((crm_trigger_t *) refresh_trigger);
    return FALSE;
}

static int
handle_op_for_node(xmlNode *xml, void *userdata)
{
    const char *node = crm_element_value(xml, XML_ATTR_UNAME);

    if (node == NULL) {
        node = ID(xml);
    }

    handle_rsc_op(xml, (void *) node);
    return pcmk_rc_ok;
}

static void
crm_diff_update_v2(const char *event, xmlNode * msg)
{
    xmlNode *change = NULL;
    xmlNode *diff = get_message_xml(msg, F_CIB_UPDATE_RESULT);

    for (change = pcmk__xml_first_child(diff); change != NULL;
         change = pcmk__xml_next(change)) {
        const char *name = NULL;
        const char *op = crm_element_value(change, XML_DIFF_OP);
        const char *xpath = crm_element_value(change, XML_DIFF_PATH);
        xmlNode *match = NULL;
        const char *node = NULL;

        if(op == NULL) {
            continue;

        } else if(strcmp(op, "create") == 0) {
            match = change->children;

        } else if(strcmp(op, "move") == 0) {
            continue;

        } else if(strcmp(op, "delete") == 0) {
            continue;

        } else if(strcmp(op, "modify") == 0) {
            match = first_named_child(change, XML_DIFF_RESULT);
            if(match) {
                match = match->children;
            }
        }

        if(match) {
            name = (const char *)match->name;
        }

        crm_trace("Handling %s operation for %s %p, %s", op, xpath, match, name);
        if(xpath == NULL) {
            /* Version field, ignore */

        } else if(name == NULL) {
            crm_debug("No result for %s operation to %s", op, xpath);
            CRM_ASSERT(strcmp(op, "delete") == 0 || strcmp(op, "move") == 0);

        } else if(strcmp(name, XML_TAG_CIB) == 0) {
            pcmk__xe_foreach_child(first_named_child(match, XML_CIB_TAG_STATUS),
                                   NULL, handle_op_for_node, NULL);

        } else if(strcmp(name, XML_CIB_TAG_STATUS) == 0) {
            pcmk__xe_foreach_child(match, NULL, handle_op_for_node, NULL);

        } else if(strcmp(name, XML_CIB_TAG_STATE) == 0) {
            node = crm_element_value(match, XML_ATTR_UNAME);
            if (node == NULL) {
                node = ID(match);
            }
            handle_rsc_op(match, (void *) node);

        } else if(strcmp(name, XML_CIB_TAG_LRM) == 0) {
            node = ID(match);
            handle_rsc_op(match, (void *) node);

        } else if(strcmp(name, XML_LRM_TAG_RESOURCES) == 0) {
            char *local_node = pcmk__xpath_node_id(xpath, "lrm");

            handle_rsc_op(match, local_node);
            free(local_node);

        } else if(strcmp(name, XML_LRM_TAG_RESOURCE) == 0) {
            char *local_node = pcmk__xpath_node_id(xpath, "lrm");

            handle_rsc_op(match, local_node);
            free(local_node);

        } else if(strcmp(name, XML_LRM_TAG_RSC_OP) == 0) {
            char *local_node = pcmk__xpath_node_id(xpath, "lrm");

            handle_rsc_op(match, local_node);
            free(local_node);

        } else {
            crm_trace("Ignoring %s operation for %s %p, %s", op, xpath, match, name);
        }
    }
}

static void
crm_diff_update_v1(const char *event, xmlNode * msg)
{
    /* Process operation updates */
    xmlXPathObject *xpathObj = xpath_search(msg,
                                            "//" F_CIB_UPDATE_RESULT "//" XML_TAG_DIFF_ADDED
                                            "//" XML_LRM_TAG_RSC_OP);
    int lpc = 0, max = numXpathResults(xpathObj);

    for (lpc = 0; lpc < max; lpc++) {
        xmlNode *rsc_op = getXpathResult(xpathObj, lpc);

        handle_rsc_op(rsc_op, NULL);
    }
    freeXpathObject(xpathObj);
}

static void
crm_diff_update(const char *event, xmlNode * msg)
{
    int rc = -1;
    static bool stale = FALSE;
    gboolean cib_updated = FALSE;
    xmlNode *diff = get_message_xml(msg, F_CIB_UPDATE_RESULT);

    out->progress(out, false);

    if (current_cib != NULL) {
        rc = xml_apply_patchset(current_cib, diff, TRUE);

        switch (rc) {
            case -pcmk_err_diff_resync:
            case -pcmk_err_diff_failed:
                crm_notice("[%s] Patch aborted: %s (%d)", event, pcmk_strerror(rc), rc);
                free_xml(current_cib); current_cib = NULL;
                break;
            case pcmk_ok:
                cib_updated = TRUE;
                break;
            default:
                crm_notice("[%s] ABORTED: %s (%d)", event, pcmk_strerror(rc), rc);
                free_xml(current_cib); current_cib = NULL;
        }
    }

    if (current_cib == NULL) {
        crm_trace("Re-requesting the full cib");
        cib->cmds->query(cib, NULL, &current_cib, cib_scope_local | cib_sync_call);
    }

    if (options.external_agent) {
        int format = 0;
        crm_element_value_int(diff, "format", &format);
        switch(format) {
            case 1:
                crm_diff_update_v1(event, msg);
                break;
            case 2:
                crm_diff_update_v2(event, msg);
                break;
            default:
                crm_err("Unknown patch format: %d", format);
        }
    }

    if (current_cib == NULL) {
        if(!stale) {
            out->info(out, "--- Stale data ---");
        }
        stale = TRUE;
        return;
    }

    stale = FALSE;
    refresh_after_event(cib_updated, FALSE);
}

static int
mon_refresh_display(gpointer user_data)
{
    int rc = pcmk_rc_ok;

    last_refresh = time(NULL);

    if (output_format == mon_output_none) {
        return G_SOURCE_REMOVE;
    }

    if (fence_history == pcmk__fence_history_full &&
        !pcmk_all_flags_set(show, pcmk_section_fencing_all) &&
        output_format != mon_output_xml) {
        fence_history = pcmk__fence_history_reduced;
    }

    // Get an up-to-date pacemakerd status for the cluster summary
    if (cib->variant == cib_native) {
        pcmk__pacemakerd_status(out, crm_system_name, options.reconnect_ms / 2,
                                false, &pcmkd_state);
    }

    if (out->dest != stdout) {
        out->reset(out);
    }

    rc = pcmk__output_cluster_status(out, st, cib, current_cib, pcmkd_state,
                                     fence_history, show, show_opts,
                                     options.only_node,options.only_rsc,
                                     options.neg_location_prefix,
                                     output_format == mon_output_monitor);

    if (output_format == mon_output_monitor && rc != pcmk_rc_ok) {
        clean_up(MON_STATUS_WARN);
        return G_SOURCE_REMOVE;
    } else if (rc == pcmk_rc_schema_validation) {
        clean_up(CRM_EX_CONFIG);
        return G_SOURCE_REMOVE;
    }

    if (out->dest != stdout) {
        out->finish(out, CRM_EX_OK, true, NULL);
    }

    return G_SOURCE_CONTINUE;
}

/* This function is called for fencing events (see setup_fencer_connection() for
 * which ones) when --watch-fencing is used on the command line
 */
static void
mon_st_callback_event(stonith_t * st, stonith_event_t * e)
{
    if (st->state == stonith_disconnected) {
        /* disconnect cib as well and have everything reconnect */
        mon_cib_connection_destroy(NULL);
    } else if (options.external_agent) {
        char *desc = stonith__event_description(e);

        send_custom_trap(e->target, NULL, e->operation, pcmk_ok, e->result, 0, desc);
        free(desc);
    }
}

/* Cause the screen to be redrawn (via mainloop_set_trigger) when various conditions are met:
 *
 * - If the last update occurred more than reconnect_ms ago (defaults to 5s, but
 *   can be changed via the -i command line option), or
 * - After every 10 CIB updates, or
 * - If it's been 2s since the last update
 *
 * This function sounds like it would be more broadly useful, but it is only called when a
 * fencing event is received or a CIB diff occurrs.
 */
static void
refresh_after_event(gboolean data_updated, gboolean enforce)
{
    static int updates = 0;
    time_t now = time(NULL);

    if (data_updated) {
        updates++;
    }

    if(refresh_timer == NULL) {
        refresh_timer = mainloop_timer_add("refresh", 2000, FALSE, mon_trigger_refresh, NULL);
    }

    if (reconnect_timer > 0) {
        /* we will receive a refresh request after successful reconnect */
        mainloop_timer_stop(refresh_timer);
        return;
    }

    /* as we're not handling initial failure of fencer-connection as
     * fatal give it a retry here
     * not getting here if cib-reconnection is already on the way
     */
    setup_fencer_connection();

    if (enforce ||
        ((now - last_refresh) > (options.reconnect_ms / 1000)) ||
        updates >= 10) {
        mainloop_set_trigger((crm_trigger_t *) refresh_trigger);
        mainloop_timer_stop(refresh_timer);
        updates = 0;

    } else {
        mainloop_timer_start(refresh_timer);
    }
}

/* This function is called for fencing events (see setup_fencer_connection() for
 * which ones) when --watch-fencing is NOT used on the command line
 */
static void
mon_st_callback_display(stonith_t * st, stonith_event_t * e)
{
    if (st->state == stonith_disconnected) {
        /* disconnect cib as well and have everything reconnect */
        mon_cib_connection_destroy(NULL);
    } else {
        out->progress(out, false);
        refresh_after_event(TRUE, FALSE);
    }
}

/*
 * De-init ncurses, disconnect from the CIB manager, disconnect fencing,
 * deallocate memory and show usage-message if requested.
 *
 * We don't actually return, but nominally returning crm_exit_t allows a usage
 * like "return clean_up(exit_code);" which helps static analysis understand the
 * code flow.
 */
static crm_exit_t
clean_up(crm_exit_t exit_code)
{
    /* Quitting crm_mon is much more complicated than it ought to be. */

    /* (1) Close connections, free things, etc. */
    cib__clean_up_connection(&cib);
    stonith_api_delete(st);
    free(options.neg_location_prefix);
    free(options.only_node);
    free(options.only_rsc);
    free(options.pid_file);
    g_slist_free_full(options.includes_excludes, free);

    g_strfreev(processed_args);

    /* (2) If this is abnormal termination and we're in curses mode, shut down
     * curses first.  Any messages displayed to the screen before curses is shut
     * down will be lost because doing the shut down will also restore the
     * screen to whatever it looked like before crm_mon was started.
     */
    if (((error != NULL) || (exit_code == CRM_EX_USAGE))
        && (output_format == mon_output_console)
        && (out != NULL)) {

        out->finish(out, exit_code, false, NULL);
        pcmk__output_free(out);
        out = NULL;
    }

    /* (3) If this is a command line usage related failure, print the usage
     * message.
     */
    if (exit_code == CRM_EX_USAGE && (output_format == mon_output_console || output_format == mon_output_plain)) {
        char *help = g_option_context_get_help(context, TRUE, NULL);

        fprintf(stderr, "%s", help);
        g_free(help);
    }

    pcmk__free_arg_context(context);

    /* (4) If this is any kind of error, print the error out and exit.  Make
     * sure to handle situations both before and after formatted output is
     * set up.  We want errors to appear formatted if at all possible.
     */
    if (error != NULL) {
        if (out != NULL) {
            out->err(out, "%s: %s", g_get_prgname(), error->message);
            out->finish(out, exit_code, true, NULL);
            pcmk__output_free(out);
        } else {
            fprintf(stderr, "%s: %s\n", g_get_prgname(), error->message);
        }

        g_clear_error(&error);
        crm_exit(exit_code);
    }

    /* (5) Print formatted output to the screen if we made it far enough in
     * crm_mon to be able to do so.
     */
    if (out != NULL) {
        if (options.exec_mode != mon_exec_daemonized) {
            out->finish(out, exit_code, true, NULL);
        }

        pcmk__output_free(out);
        pcmk__unregister_formats();
    }

    crm_exit(exit_code);
}
