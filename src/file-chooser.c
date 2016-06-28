/*
 * Copyright © 2016 Red Hat, Inc
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
 *       Alexander Larsson <alexl@redhat.com>
 *       Matthias Clasen <mclasen@redhat.com>
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

#include "file-chooser.h"
#include "request.h"
#include "documents.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"

typedef struct _FileChooser FileChooser;
typedef struct _FileChooserClass FileChooserClass;

struct _FileChooser
{
  XdpFileChooserSkeleton parent_instance;
};

struct _FileChooserClass
{
  XdpFileChooserSkeletonClass parent_class;
};

static XdpImplFileChooser *impl;
static FileChooser *file_chooser;

GType file_chooser_get_type (void) G_GNUC_CONST;
static void file_chooser_iface_init (XdpFileChooserIface *iface);

G_DEFINE_TYPE_WITH_CODE (FileChooser, file_chooser, XDP_TYPE_FILE_CHOOSER_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_TYPE_FILE_CHOOSER, file_chooser_iface_init));

static void
emit_response (Request *request,
               gboolean for_save,
               guint arg_response,
               const gchar *const *arg_uris,
               GVariant *arg_options)
{
  GVariantBuilder uris;
  GVariantBuilder results;
  g_autofree char *ruri = NULL;
  gboolean writable = TRUE;
  g_autoptr(GError) error = NULL;
  int i;

  REQUEST_AUTOLOCK (request);

  if (!g_variant_lookup (arg_options, "b", "writable", &writable))
    writable = FALSE;

  g_variant_builder_init (&results, G_VARIANT_TYPE_VARDICT);

  g_variant_builder_init (&uris, G_VARIANT_TYPE ("as"));

  for (i = 0; arg_uris[i] != NULL; i++)
    {
      const char *uri = arg_uris[i];
      ruri = register_document (uri, request->app_id, for_save, writable, &error);
      if (ruri == NULL)
        {
          g_warning ("Failed to register %s: %s\n", uri, error->message);
          g_clear_error (&error);
        }
      else
        {
          g_debug ("convert uri %s -> %s\n", uri, ruri);
          g_variant_builder_add (&uris, "s", ruri);
        }
    }

  g_variant_builder_add (&results, "{&sv}", "uris", g_variant_builder_end (&uris));

  if (request->exported)
    {
      xdp_request_emit_response (XDP_REQUEST (request),
                                 arg_response,
                                 g_variant_builder_end (&results));
      request_unexport (request);
    }
}

static void
handle_open_file_response (XdpImplRequest *object,
                           guint arg_response,
                           GVariant *arg_options,
                           Request *request)
{
  const char *uris[2] = { NULL, NULL };

  g_variant_lookup (arg_options, "&s", "uri", &uris[0]);

  emit_response (request, FALSE, arg_response, uris, arg_options);
}

static void
handle_open_files_response (XdpImplFileChooser *object,
                            guint arg_response,
                            GVariant *arg_options,
                            Request *request)
{
  const char **uris;

  g_variant_lookup (arg_options, "^a&s", "uris", &uris);

  emit_response (request, FALSE, arg_response, uris, arg_options);
}

static void
handle_save_file_response (XdpImplFileChooser *object,
                           guint arg_response,
                           GVariant *arg_options,
                           Request *request)
{
  const char *uris[] = { NULL, NULL };

  g_variant_lookup (arg_options, "&s", "uri", &uris[0]);

  emit_response (request, TRUE, arg_response, uris, arg_options);
}

static gboolean
handle_close (XdpRequest *object,
              GDBusMethodInvocation *invocation,
              Request *request)
{
  g_autoptr(GError) error = NULL;

  REQUEST_AUTOLOCK (request);

  if (request->exported)
    {
      XdpImplRequest *impl_request = g_object_get_data (G_OBJECT (request), "impl-request");

      if (!xdp_impl_request_call_close_sync (impl_request, NULL, &error))
        {
          g_dbus_method_invocation_return_gerror (invocation, error);
          return TRUE;
        }

      request_unexport (request);
    }

  xdp_request_complete_close (XDP_REQUEST (request), invocation);

  return TRUE;
}

typedef struct {
  const char *key;
  const GVariantType *type;
} OptionKey;

static OptionKey open_file_options[] = {
  { "accept_label", G_VARIANT_TYPE_STRING },
  { "filters", (const GVariantType *)"a(sa(us))" },
  { "choices", (const GVariantType *)"a(ssa(ss)s)" }
};

void
copy_options (GVariant *arg_options,
              GVariantBuilder *options,
              OptionKey *supported_options,
              int n_supported_options)
{
  GVariant *value;
  int i;

  for (i = 0; i < n_supported_options; i++)
    {
      value = g_variant_lookup_value (arg_options,
                                      supported_options[i].key,
                                      supported_options[i].type);
      if (value)
         g_variant_builder_add (options, "{sv}", supported_options[i].key, value);
    }
}

static gboolean
handle_open_file (XdpFileChooser *object,
                  GDBusMethodInvocation *invocation,
                  const gchar *arg_parent_window,
                  const gchar *arg_title,
                  GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  const char *app_id = request->app_id;
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);
  g_autoptr(GError) error = NULL;
  g_autoptr(XdpImplRequest) impl_request = NULL;
  GVariantBuilder options;

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  copy_options (arg_options, &options, open_file_options, G_N_ELEMENTS (open_file_options));

  if (!xdp_impl_file_chooser_call_open_file_sync (impl,
                                                  sender, app_id,
                                                  arg_parent_window,
                                                  arg_title,
                                                  g_variant_builder_end (&options),
                                                  request->id,
                                                  NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  impl_request = xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                                  G_DBUS_PROXY_FLAGS_NONE,
                                                  g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                                  request->id,
                                                  NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  g_signal_connect (impl_request, "response", (GCallback)handle_open_file_response, request);

  g_object_set_data_full (G_OBJECT (request), "impl-request", g_object_ref (impl_request), g_object_unref);

  g_signal_connect (request, "handle-close", (GCallback)handle_close, request);

  REQUEST_AUTOLOCK (request);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_file_chooser_complete_open_file (object, invocation, request->id);

  return TRUE;
}

static gboolean
handle_open_files (XdpFileChooser *object,
                   GDBusMethodInvocation *invocation,
                   const gchar *arg_parent_window,
                   const gchar *arg_title,
                   GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  const char *app_id = request->app_id;
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);
  g_autoptr(GError) error = NULL;
  XdpImplRequest *impl_request;
  GVariantBuilder options;

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  copy_options (arg_options, &options, open_file_options, G_N_ELEMENTS (open_file_options));

  if (!xdp_impl_file_chooser_call_open_files_sync (impl,
                                                   sender, app_id,
                                                   arg_parent_window,
                                                   arg_title,
                                                   g_variant_builder_end (&options),
                                                   request->id,
                                                   NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  impl_request = xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                                  G_DBUS_PROXY_FLAGS_NONE,
                                                  g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                                  request->id,
                                                  NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  g_signal_connect (impl_request, "response", (GCallback)handle_open_files_response, request);

  g_object_set_data_full (G_OBJECT (request), "impl-request", impl_request, g_object_unref);

  g_signal_connect (request, "handle-close", (GCallback)handle_close, request);

  REQUEST_AUTOLOCK (request);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_file_chooser_complete_open_files (object, invocation, request->id);
  return TRUE;
}

static OptionKey save_file_options[] = {
  { "accept_label", G_VARIANT_TYPE_STRING },
  { "filters", (const GVariantType *)"a(sa(us))" },
  { "current_name", G_VARIANT_TYPE_STRING },
  { "current_folder", G_VARIANT_TYPE_BYTESTRING },
  { "current_file", G_VARIANT_TYPE_BYTESTRING },
  { "choices", (const GVariantType *)"a(ssa(ss)s)" }
};

static gboolean
handle_save_file (XdpFileChooser *object,
                  GDBusMethodInvocation *invocation,
                  const gchar *arg_parent_window,
                  const gchar *arg_title,
                  GVariant *arg_options)
{
  Request *request = request_from_invocation (invocation);
  const char *app_id = request->app_id;
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);
  g_autoptr(GError) error = NULL;
  XdpImplRequest *impl_request;
  GVariantBuilder options;

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  copy_options (arg_options, &options, save_file_options, G_N_ELEMENTS (save_file_options));

  if (!xdp_impl_file_chooser_call_save_file_sync (impl,
                                                  sender, app_id,
                                                  arg_parent_window,
                                                  arg_title,
                                                  g_variant_builder_end (&options),
                                                  request->id,
                                                  NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  impl_request = xdp_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (impl)),
                                                  G_DBUS_PROXY_FLAGS_NONE,
                                                  g_dbus_proxy_get_name (G_DBUS_PROXY (impl)),
                                                  request->id,
                                                  NULL, &error);
  if (!impl_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  g_signal_connect (impl_request, "response", (GCallback)handle_save_file_response, request);

  g_object_set_data_full (G_OBJECT (request), "impl-request", impl_request, g_object_unref);

  g_signal_connect (request, "handle-close", (GCallback)handle_close, request);

  REQUEST_AUTOLOCK (request);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_file_chooser_complete_open_file (object, invocation, request->id);
  return TRUE;
}

static void
file_chooser_iface_init (XdpFileChooserIface *iface)
{
  iface->handle_open_file = handle_open_file;
  iface->handle_open_files = handle_open_files;
  iface->handle_save_file = handle_save_file;
}

static void
file_chooser_init (FileChooser *fc)
{
}

static void
file_chooser_class_init (FileChooserClass *klass)
{
}

GDBusInterfaceSkeleton *
file_chooser_create (GDBusConnection *connection,
                     const char      *dbus_name)
{
  g_autoptr(GError) error = NULL;

  impl = xdp_impl_file_chooser_proxy_new_sync (connection,
                                               G_DBUS_PROXY_FLAGS_NONE,
                                               dbus_name,
                                               "/org/freedesktop/portal/desktop",
                                               NULL, &error);
  if (impl == NULL)
    {
      g_warning ("Failed to create file chooser proxy: %s\n", error->message);
      return NULL;
    }

  set_proxy_use_threads (G_DBUS_PROXY (impl));

  file_chooser = g_object_new (file_chooser_get_type (), NULL);

  g_signal_connect (impl, "open-files-response", (GCallback)handle_open_files_response, NULL);
  g_signal_connect (impl, "save-file-response", (GCallback)handle_save_file_response, NULL);

  return G_DBUS_INTERFACE_SKELETON (file_chooser);
}
