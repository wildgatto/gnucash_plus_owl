/*
 * gnc-plugin-retirement.c --
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, contact:
 *
 * Free Software Foundation           Voice:  +1-617-542-5942
 * 51 Franklin Street, Fifth Floor    Fax:    +1-617-542-2652
 * Boston, MA  02110-1301,  USA       gnu@gnu.org
 */

#include <config.h>

#include <gtk/gtk.h>
#include <glib/gi18n.h>

#include "gnc-plugin-retirement.h"
#include "gnc-plugin-page-retirement.h"
#include "gnc-plugin-page-estate.h"
#include "gnc-main-window.h"
#include "gnc-ui.h"

#define PLUGIN_ACTIONS_NAME "gnc-plugin-retirement-actions"
#define PLUGIN_UI_FILENAME  "gnc-plugin-retirement.ui"

static QofLogModule log_module = GNC_MOD_GUI;

static void gnc_plugin_retirement_finalize (GObject *object);
static void gnc_plugin_retirement_cmd_open (GSimpleAction *simple,
                                            GVariant      *parameter,
                                            gpointer       user_data);
static void gnc_plugin_retirement_cmd_open_estate (GSimpleAction *simple,
                                                   GVariant      *parameter,
                                                   gpointer       user_data);

static GActionEntry gnc_plugin_actions [] =
{
    { "ToolsRetirementPlannerAction", gnc_plugin_retirement_cmd_open, NULL, NULL, NULL },
    { "ToolsEstatePlannerAction", gnc_plugin_retirement_cmd_open_estate, NULL, NULL, NULL },
};
static guint gnc_plugin_n_actions = G_N_ELEMENTS(gnc_plugin_actions);

static const gchar *gnc_plugin_load_ui_items [] =
{
    "ToolsPlaceholder1",
    NULL,
};

struct _GncPluginRetirement
{
    GncPlugin gnc_plugin;
};

G_DEFINE_TYPE(GncPluginRetirement, gnc_plugin_retirement, GNC_TYPE_PLUGIN)

GncPlugin *
gnc_plugin_retirement_new (void)
{
    GncPluginRetirement *plugin;
    ENTER(" ");

    GNC_TYPE_PLUGIN_PAGE_RETIREMENT;
    GNC_TYPE_PLUGIN_PAGE_ESTATE;

    plugin = g_object_new (GNC_TYPE_PLUGIN_RETIREMENT, NULL);
    LEAVE(" ");
    return GNC_PLUGIN(plugin);
}

static void
gnc_plugin_retirement_class_init (GncPluginRetirementClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GncPluginClass *plugin_class = GNC_PLUGIN_CLASS(klass);

    object_class->finalize = gnc_plugin_retirement_finalize;

    plugin_class->plugin_name  = GNC_PLUGIN_RETIREMENT_NAME;
    plugin_class->actions_name = PLUGIN_ACTIONS_NAME;
    plugin_class->actions      = gnc_plugin_actions;
    plugin_class->n_actions    = gnc_plugin_n_actions;
    plugin_class->ui_filename  = PLUGIN_UI_FILENAME;
    plugin_class->ui_updates   = gnc_plugin_load_ui_items;
}

static void
gnc_plugin_retirement_init (GncPluginRetirement *plugin)
{
}

static void
gnc_plugin_retirement_finalize (GObject *object)
{
    g_return_if_fail (GNC_IS_PLUGIN_RETIREMENT(object));
    G_OBJECT_CLASS (gnc_plugin_retirement_parent_class)->finalize (object);
}

static void
gnc_plugin_retirement_cmd_open (GSimpleAction *simple,
                                GVariant      *parameter,
                                gpointer       user_data)
{
    GncMainWindowActionData *data = user_data;
    GncPluginPage *page;

    g_return_if_fail (data != NULL);

    page = gnc_plugin_page_retirement_new ();
    gnc_main_window_open_page (data->window, page);
}

static void
gnc_plugin_retirement_cmd_open_estate (GSimpleAction *simple,
                                       GVariant      *parameter,
                                       gpointer       user_data)
{
    GncMainWindowActionData *data = user_data;
    GncPluginPage *page;

    g_return_if_fail (data != NULL);

    page = gnc_plugin_page_estate_new ();
    gnc_main_window_open_page (data->window, page);
}
