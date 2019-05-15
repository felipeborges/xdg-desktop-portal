/*
 * Copyright © 2019 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Felipe Borges <feborges@redhat.com>
 */

#include "config.h"

#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "wallpaper.h"
#include "request.h"
#include "documents.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

typedef struct _Wallpaper Wallpaper;
typedef struct _WallpaperClass WallpaperClass;

struct _Wallpaper
{
  XdpWallpaperSkeleton parent_instance;
};

struct _WallpaperClass
{
  XdpWallpaperSkeletonClass parent_class;
};

static Wallpaper *wallpaper;

GType wallpaper_get_type (void) G_GNUC_CONST;
static void wallpaper_iface_init (XdpWallpaperIface *iface);

G_DEFINE_TYPE_WITH_CODE (Wallpaper, wallpaper, XDP_TYPE_WALLPAPER_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_WALLPAPER, wallpaper_iface_init));

static gboolean
handle_set_file (XdpWallpaper *object,
                           GDBusMethodInvocation *invocation,
                           const gchar *arg_uri)
{
  Request *request = request_from_invocation (invocation);

  REQUEST_AUTOLOCK (request);

  g_object_set_data_full (G_OBJECT (request), "uri", g_strdup (arg_uri), g_free);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));
  xdg_wallpaper_complete_set_file (object, invocation, request->id);

  return TRUE;
}

static void
wallpaper_iface_init (XdpWallpaperIface *iface)
{
  iface->handle_set_file = handle_set_file;
}

static void
wallpaper_init (Wallpaper *fc)
{
  xdp_wallpaper_set_version (XDP_WALLPAPER (fc), 1);
}

static void
wallpaper_class_init (WallpaperClass *klass)
{
}

GDBusInterfaceSkeleton *
wallpaper_create (GDBusConnection *connection,
                  const char      *dbus_name)
{
  wallpaper = g_object_new (wallpaper_get_type (), NULL);

  return G_DBUS_INTERFACE_SKELETON (wallpaper);
}
