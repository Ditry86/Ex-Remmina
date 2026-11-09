/*
 * Remmina - The GTK+ Remote Desktop Client
 * Copyright (C) 2010-2011 Vic Lee
 * Copyright (C) 2014-2015 Antenore Gatta, Fabio Castelli, Giovanni Panozzo
 * Copyright (C) 2016-2022 Antenore Gatta, Giovanni Panozzo
 * Copyright (C) 2022-2023 Antenore Gatta, Giovanni Panozzo, Hiroyuki Tanaka
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 *  In addition, as a special exception, the copyright holders give
 *  permission to link the code of portions of this program with the
 *  OpenSSL library under certain conditions as described in each
 *  individual source file, and distribute linked combinations
 *  including the two.
 *  You must obey the GNU General Public License in all respects
 *  for all of the code used other than OpenSSL. *  If you modify
 *  file(s) with this exception, you may extend this exception to your
 *  version of the file(s), but you are not obligated to do so. *  If you
 *  do not wish to do so, delete this exception statement from your
 *  version. *  If you delete this exception statement from all source
 *  files in the program, then also delete it here.
 *
 */


#define _GNU_SOURCE

#include "remmina/plugin.h"
#include "rdp_plugin.h"
#include "rdp_event.h"
#include "rdp_graphics.h"
#include "rdp_file.h"
#include "rdp_settings.h"
#include "rdp_cliprdr.h"
#include "rdp_monitor.h"
#include "rdp_channels.h"

#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#ifdef GDK_WINDOWING_X11
#include <cairo/cairo-xlib.h>
#else
#include <cairo/cairo.h>
#endif
#include <ctype.h>
#include <freerdp/addin.h>
#include <freerdp/assistance.h>
#if FREERDP_VERSION_MAJOR >= 3
#include <freerdp/channels/rdp2tcp.h>
#else
#define RDP2TCP_DVC_CHANNEL_NAME "rdp2tcp"
#endif
#include <freerdp/client/channels.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/constants.h>
#include <freerdp/error.h>
#include <freerdp/event.h>
#include <freerdp/freerdp.h>
#include <freerdp/settings.h>
#include <winpr/cmdline.h>
#include <winpr/memory.h>

#ifdef HAVE_CUPS
#include <cups/cups.h>
#endif

#include <unistd.h>
#include <string.h>

#ifdef GDK_WINDOWING_X11
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <gdk/gdkx.h>
#elif defined(GDK_WINDOWING_WAYLAND)
#include <gdk/gdkwayland.h>
#endif

#if defined(__FreeBSD__)
#include <pthread_np.h>
#endif

#include <freerdp/locale/keyboard.h>

#define REMMINA_RDP_FEATURE_TOOL_REFRESH         1
#define REMMINA_RDP_FEATURE_SCALE                2
#define REMMINA_RDP_FEATURE_UNFOCUS              3
#define REMMINA_RDP_FEATURE_TOOL_SENDCTRLALTDEL  4
#define REMMINA_RDP_FEATURE_DYNRESUPDATE         5
#define REMMINA_RDP_FEATURE_MULTIMON             6
#define REMMINA_RDP_FEATURE_VIEWONLY             7

#define REMMINA_CONNECTION_TYPE_NONE             0

#if FREERDP_VERSION_MAJOR >= 3
	#define CLPARAM const char
#else
	#define CLPARAM char
#endif

#if FREERDP_VERSION_MAJOR < 3
static HANDLE freerdp_abort_event(rdpContext* context) {
#ifdef WINPR_ASSERT
	WINPR_ASSERT(context);
#endif
	return context->abortEvent;
}

struct rdp_remap_table
{
	DWORD table[0x10000];
};

static BOOL freerdp_settings_set_pointer_len(rdpSettings* settings, size_t id, const void* data, size_t len)
{
	switch(id) {
	case FreeRDP_LoadBalanceInfo:
		free(settings->LoadBalanceInfo);
		settings->LoadBalanceInfo = (BYTE *)data;
		freerdp_settings_set_uint32(settings, FreeRDP_LoadBalanceInfoLength, strlen(data));

		return TRUE;
	default:
		return FALSE;
	}
}

static void freerdp_abort_connect_context(rdpContext* context) {
#ifdef WINPR_ASSERT
	WINPR_ASSERT(context);
#endif
	freerdp_abort_connect(context->instance);
}
#endif

RemminaPluginService *remmina_plugin_service = NULL;

static BOOL gfx_h264_available = FALSE;

/* Compatibility: these functions have been introduced with https://github.com/FreeRDP/FreeRDP/commit/8c5d96784d
 * and are missing on older FreeRDP, so we add them here.
 * They should be removed from here after all distributed versions of FreeRDP (libwinpr) will have
 * CommandLineParseCommaSeparatedValuesEx() onboard.
 *
 * (C) Copyright goes to the FreeRDP authors.
 */
static CLPARAM **remmina_rdp_CommandLineParseCommaSeparatedValuesEx(const char *name, const char *list, size_t *count)
{
	TRACE_CALL(__func__);
#if FREERDP_CHECK_VERSION(2, 0, 0)
	return (CLPARAM **)CommandLineParseCommaSeparatedValuesEx(name, list, count);
#else
	char **p;
	char *str;
	size_t nArgs;
	size_t index;
	size_t nCommas;
	size_t prefix, len;

	nCommas = 0;

	if (count == NULL)
		return NULL;

	*count = 0;

	if (!list) {
		if (name) {
			size_t len = strlen(name);
			p = (char **)calloc(2UL + len, sizeof(char *));

			if (p) {
				char *dst = (char *)&p[1];
				p[0] = dst;
				sprintf_s(dst, len + 1, "%s", name);
				*count = 1;
				return p;
			}
		}

		return NULL;
	}

	{
		const char *it = list;

		while ((it = strchr(it, ',')) != NULL) {
			it++;
			nCommas++;
		}
	}

	nArgs = nCommas + 1;

	if (name)
		nArgs++;

	prefix = (nArgs + 1UL) * sizeof(char *);
	len = strlen(list);
	p = (char **)calloc(len + prefix + 1, sizeof(char *));

	if (!p)
		return NULL;

	str = &((char *)p)[prefix];
	memcpy(str, list, len);

	if (name)
		p[0] = (char *)name;

	for (index = name ? 1 : 0; index < nArgs; index++) {
		char *comma = strchr(str, ',');
		p[index] = str;

		if (comma) {
			str = comma + 1;
			*comma = '\0';
		}
	}

	*count = nArgs;
	return p;
#endif
}

static CLPARAM **remmina_rdp_CommandLineParseCommaSeparatedValues(const char *list, size_t *count)
{
	TRACE_CALL(__func__);
	return remmina_rdp_CommandLineParseCommaSeparatedValuesEx(NULL, list, count);
}

/*
 * End of CommandLineParseCommaSeparatedValuesEx() compatibility and copyright
 */
static BOOL rf_process_event_queue(RemminaProtocolWidget *gp)
{
	TRACE_CALL(__func__);
	UINT16 flags;
	rdpInput *input;
	rfContext *rfi = GET_PLUGIN_DATA(gp);
	RemminaPluginRdpEvent *event;
	DISPLAY_CONTROL_MONITOR_LAYOUT *dcml;
	CLIPRDR_FORMAT_DATA_RESPONSE response = { 0 };
	RemminaFile *remminafile;

	if (rfi->event_queue == NULL)
		return true;

	input = rfi->clientContext.context.input;

	remminafile = remmina_plugin_service->protocol_plugin_get_file(gp);

	while ((event = (RemminaPluginRdpEvent *)g_async_queue_try_pop(rfi->event_queue)) != NULL) {
		time(&(rfi->last_time)); //update last user interaction time
		time(&(rfi->last_time_idle_keypress));
		switch (event->type) {
		case REMMINA_RDP_EVENT_TYPE_SCANCODE:
			
			if (event->key_event.extended1){
				flags = KBD_FLAGS_EXTENDED1;
			}
			else{
				flags = event->key_event.extended ? KBD_FLAGS_EXTENDED : 0;
			}
			flags |= event->key_event.up ? KBD_FLAGS_RELEASE : KBD_FLAGS_DOWN;
			input->KeyboardEvent(input, flags, event->key_event.key_code);
			break;

		case REMMINA_RDP_EVENT_TYPE_SCANCODE_UNICODE:
			/*
			 * TS_UNICODE_KEYBOARD_EVENT RDP message, see https://msdn.microsoft.com/en-us/library/cc240585.aspx
			 */
			flags = event->key_event.up ? KBD_FLAGS_RELEASE : KBD_FLAGS_DOWN;
			input->UnicodeKeyboardEvent(input, flags, event->key_event.unicode_code);
			break;

		case REMMINA_RDP_EVENT_TYPE_MOUSE:
			if (event->mouse_event.extended)
				input->ExtendedMouseEvent(input, event->mouse_event.flags,
							  event->mouse_event.x, event->mouse_event.y);
			else
				input->MouseEvent(input, event->mouse_event.flags,
						  event->mouse_event.x, event->mouse_event.y);
			break;

		case REMMINA_RDP_EVENT_TYPE_CLIPBOARD_SEND_CLIENT_FORMAT_LIST:
			if(rfi->clipboard.context != NULL){
				rfi->clipboard.context->ClientFormatList(rfi->clipboard.context, event->clipboard_formatlist.pFormatList);
			}
			
			free(event->clipboard_formatlist.pFormatList);
			break;

		case REMMINA_RDP_EVENT_TYPE_CLIPBOARD_SEND_CLIENT_FORMAT_DATA_RESPONSE:
		{
			UINT32 msgFlags = (event->clipboard_formatdataresponse.data) ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
#if FREERDP_VERSION_MAJOR >= 3
			response.common.msgFlags = msgFlags;
			response.common.dataLen = event->clipboard_formatdataresponse.size;
#else
			response.msgFlags = msgFlags;
			response.dataLen = event->clipboard_formatdataresponse.size;
#endif
			response.requestedFormatData = event->clipboard_formatdataresponse.data;
			rfi->clipboard.context->ClientFormatDataResponse(rfi->clipboard.context, &response);
		}
			break;

		case REMMINA_RDP_EVENT_TYPE_CLIPBOARD_SEND_CLIENT_FORMAT_DATA_REQUEST:
			REMMINA_PLUGIN_DEBUG("Sending client FormatDataRequest to server");
			gettimeofday(&(rfi->clipboard.clientformatdatarequest_tv), NULL);
			rfi->clipboard.context->ClientFormatDataRequest(rfi->clipboard.context, event->clipboard_formatdatarequest.pFormatDataRequest);
			free(event->clipboard_formatdatarequest.pFormatDataRequest);
			break;

		case REMMINA_RDP_EVENT_TYPE_SEND_MONITOR_LAYOUT:
			if (remmina_plugin_service->file_get_int(remminafile, "multimon", FALSE)) {
				freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_UseMultimon, TRUE);
				if (remmina_plugin_service->file_get_int(remminafile, "force_multimon", FALSE)) {
					freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_ForceMultimon, TRUE);
				}	
				freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_Fullscreen, TRUE);
				/* got some crashes with g_malloc0, to be investigated */
				dcml = calloc(freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_MonitorCount), sizeof(DISPLAY_CONTROL_MONITOR_LAYOUT));
				REMMINA_PLUGIN_DEBUG("REMMINA_RDP_EVENT_TYPE_SEND_MONITOR_LAYOUT:");
				if (!dcml)
					break;

				const rdpMonitor *base = freerdp_settings_get_pointer(rfi->clientContext.context.settings, FreeRDP_MonitorDefArray);
				for (gint i = 0; i < freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_MonitorCount); ++i) {
					const rdpMonitor *current = &base[i];
					REMMINA_PLUGIN_DEBUG("Sending display layout for monitor n° %d", i);
					dcml[i].Flags = (current->is_primary ? DISPLAY_CONTROL_MONITOR_PRIMARY : 0);
					REMMINA_PLUGIN_DEBUG("Monitor %d is primary: %d", i, dcml[i].Flags);
					dcml[i].Left = current->x;
					REMMINA_PLUGIN_DEBUG("Monitor %d x: %d", i, dcml[i].Left);
					dcml[i].Top = current->y;
					REMMINA_PLUGIN_DEBUG("Monitor %d y: %d", i, dcml[i].Top);
					dcml[i].Width = current->width;
					REMMINA_PLUGIN_DEBUG("Monitor %d width: %d", i, dcml[i].Width);
					dcml[i].Height = current->height;
					REMMINA_PLUGIN_DEBUG("Monitor %d height: %d", i, dcml[i].Height);
					dcml[i].PhysicalWidth = current->attributes.physicalWidth;
					REMMINA_PLUGIN_DEBUG("Monitor %d physical width: %d", i, dcml[i].PhysicalWidth);
					dcml[i].PhysicalHeight = current->attributes.physicalHeight;
					REMMINA_PLUGIN_DEBUG("Monitor %d physical height: %d", i, dcml[i].PhysicalHeight);
					if (current->attributes.orientation)
						dcml[i].Orientation = current->attributes.orientation;
					else
						dcml[i].Orientation = event->monitor_layout.desktopOrientation;
					REMMINA_PLUGIN_DEBUG("Monitor %d orientation: %d", i, dcml[i].Orientation);
					dcml[i].DesktopScaleFactor = event->monitor_layout.desktopScaleFactor;
					dcml[i].DeviceScaleFactor = event->monitor_layout.deviceScaleFactor;
				}
				rfi->dispcontext->SendMonitorLayout(rfi->dispcontext, freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_MonitorCount), dcml);
				g_free(dcml);
			} else {
				dcml = g_malloc0(sizeof(DISPLAY_CONTROL_MONITOR_LAYOUT));
				if (dcml) {
					dcml->Flags = DISPLAY_CONTROL_MONITOR_PRIMARY;
					dcml->Width = event->monitor_layout.width;
					dcml->Height = event->monitor_layout.height;
					dcml->Orientation = event->monitor_layout.desktopOrientation;
					dcml->DesktopScaleFactor = event->monitor_layout.desktopScaleFactor;
					dcml->DeviceScaleFactor = event->monitor_layout.deviceScaleFactor;
					rfi->dispcontext->SendMonitorLayout(rfi->dispcontext, 1, dcml);
					g_free(dcml); \
				}
			}
			break;
		case REMMINA_RDP_EVENT_DISCONNECT:
			/* Disconnect requested via GUI (i.e: tab destroy/close) */
			freerdp_abort_connect_context(&rfi->clientContext.context);
			break;
		}

		g_free(event);
	}

	return true;
}

static gboolean remmina_rdp_tunnel_init(RemminaProtocolWidget *gp)
{
	TRACE_CALL(__func__);

	/* Opens the optional SSH tunnel if needed.
	 * Used also when reopening the same tunnel for a FreeRDP reconnect.
	 * Returns TRUE if all OK, and setups correct rfi->Settings values
	 * with connection and certificate parameters */

	gchar *hostport;
	gchar *s;
	gchar *host;
	gchar *cert_host;
	gint cert_port;
	gint port;

	rfContext *rfi = GET_PLUGIN_DATA(gp);

	REMMINA_PLUGIN_DEBUG("Tunnel init");
	hostport = remmina_plugin_service->protocol_plugin_start_direct_tunnel(gp, 3389, FALSE);
	if (hostport == NULL)
		return FALSE;

	remmina_plugin_service->get_server_port(hostport, 3389, &host, &port);

	if (host[0] == 0)
		return FALSE;

	REMMINA_PLUGIN_DEBUG("protocol_plugin_start_direct_tunnel() returned %s", hostport);

	cert_host = host;
	cert_port = port;

	if (!rfi->is_reconnecting) {
		/* settings->CertificateName and settings->ServerHostname is created
		 * only on 1st connect, not on reconnections */

		freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_ServerHostname, host);

		if (cert_port == 3389) {
			freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_CertificateName, cert_host);
		} else {
			s = g_strdup_printf("%s:%d", cert_host, cert_port);
			freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_CertificateName, s);
			g_free(s);
		}
	}

	REMMINA_PLUGIN_DEBUG("Tunnel has been optionally initialized. Now connecting to %s:%d", host, port);

	if (cert_host != host) g_free(cert_host);
	g_free(host);
	g_free(hostport);

	freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_ServerPort, port);

	return TRUE;
}

static BOOL rf_auto_reconnect(rfContext *rfi)
{
	TRACE_CALL(__func__);
	rdpSettings *settings = rfi->clientContext.context.settings;
	RemminaPluginRdpUiObject *ui;
	time_t treconn;
	gchar *cval;
	gint maxattempts;

	RemminaProtocolWidget *gp = rfi->protocol_widget;
	RemminaFile *remminafile = remmina_plugin_service->protocol_plugin_get_file(gp);

	rfi->is_reconnecting = TRUE;
	rfi->stop_reconnecting_requested = FALSE;

	/* Get the value set in FreeRDP_AutoReconnectMaxRetries (20) */
	maxattempts = freerdp_settings_get_uint32(settings, FreeRDP_AutoReconnectMaxRetries);
	REMMINA_PLUGIN_DEBUG("maxattempts from default: %d", maxattempts);
	/* Get the value from the global preferences if any */
	if ((cval = remmina_plugin_service->pref_get_value("rdp_reconnect_attempts")) != NULL)
		maxattempts = atoi(cval);
	REMMINA_PLUGIN_DEBUG("maxattempts from general preferences: %d", maxattempts);
	/* Get the value from the profile if any, otherwise uses the value of maxattempts */
	maxattempts = remmina_plugin_service->file_get_int(remminafile, "rdp_reconnect_attempts", maxattempts);
	REMMINA_PLUGIN_DEBUG("maxattempts from general plugin: %d", maxattempts);
	/* If maxattemps is <= 0, we get the value from FreeRDP_AutoReconnectMaxRetries (20) */
	if (maxattempts <= 0)
		maxattempts = freerdp_settings_get_uint32(settings, FreeRDP_AutoReconnectMaxRetries);
	freerdp_settings_set_uint32(settings, FreeRDP_AutoReconnectMaxRetries, maxattempts);
	REMMINA_PLUGIN_DEBUG("maxattempts set to: %d", maxattempts);

	rfi->reconnect_maxattempts = maxattempts;
	rfi->reconnect_nattempt = 0;

	/* Only auto reconnect on network disconnects. */
	switch (freerdp_error_info(rfi->clientContext.context.instance)) {
	case ERRINFO_GRAPHICS_SUBSYSTEM_FAILED:
		/* Disconnected by server hitting a bug or resource limit */
		break;
	case ERRINFO_SUCCESS:
		/* A network disconnect was detected */
		break;
	default:
		rfi->is_reconnecting = FALSE;
		return FALSE;
	}

	if (!freerdp_settings_get_bool(settings, FreeRDP_AutoReconnectionEnabled)) {
		/* No auto-reconnect - just quit */
		rfi->is_reconnecting = FALSE;
		return FALSE;
	}

	/* A network disconnect was detected and we should try to reconnect */
	REMMINA_PLUGIN_DEBUG("[%s] network disconnection detected, initiating reconnection attempt",
		freerdp_settings_get_string(rfi->clientContext.context.settings, FreeRDP_ServerHostname));

	ui = g_new0(RemminaPluginRdpUiObject, 1);
	ui->type = REMMINA_RDP_UI_RECONNECT_PROGRESS;
	remmina_rdp_event_queue_ui_async(rfi->protocol_widget, ui);

	/* Sleep half a second to allow:
	 *  - processing of the UI event we just pushed on the queue
	 *  - better network conditions
	 *  Remember: We hare on a thread, so the main gui won’t lock */
	usleep(500000);

	/* Perform an auto-reconnect. */
	while (TRUE) {
		/* Quit retrying if max retries has been exceeded */
		if (rfi->reconnect_nattempt++ >= rfi->reconnect_maxattempts) {
			REMMINA_PLUGIN_DEBUG("[%s] maximum number of reconnection attempts exceeded.",
				freerdp_settings_get_string(rfi->clientContext.context.settings, FreeRDP_ServerHostname));
			break;
		}

		if (rfi->stop_reconnecting_requested) {
			REMMINA_PLUGIN_DEBUG("[%s] reconnect request loop interrupted by user.",
						 freerdp_settings_get_string(rfi->clientContext.context.settings, FreeRDP_ServerHostname));
			break;
		}

		/* Attempt the next reconnect */
		REMMINA_PLUGIN_DEBUG("[%s] reconnection, attempt #%d of %d",
					 freerdp_settings_get_string(rfi->clientContext.context.settings, FreeRDP_ServerHostname), rfi->reconnect_nattempt, rfi->reconnect_maxattempts);

		ui = g_new0(RemminaPluginRdpUiObject, 1);
		ui->type = REMMINA_RDP_UI_RECONNECT_PROGRESS;
		remmina_rdp_event_queue_ui_async(rfi->protocol_widget, ui);

		treconn = time(NULL);

		/* Reconnect the SSH tunnel, if needed */
		if (!remmina_rdp_tunnel_init(rfi->protocol_widget)) {
			REMMINA_PLUGIN_DEBUG("[%s] unable to recreate tunnel with remmina_rdp_tunnel_init.",
						 freerdp_settings_get_string(rfi->clientContext.context.settings, FreeRDP_ServerHostname));
		} else {
			if (freerdp_reconnect(rfi->clientContext.context.instance)) {
				/* Reconnection is successful */
				REMMINA_PLUGIN_DEBUG("[%s] reconnected.", freerdp_settings_get_string(rfi->clientContext.context.settings, FreeRDP_ServerHostname));
				rfi->is_reconnecting = FALSE;
				return TRUE;
			}
		}

		/* Wait until 5 secs have elapsed from last reconnect attempt, while checking for rfi->stop_reconnecting_requested */
		while (time(NULL) - treconn < 5) {
			if (rfi->stop_reconnecting_requested)
				break;
			usleep(200000); // 200ms sleep
		}
	}

	rfi->is_reconnecting = FALSE;
	return FALSE;
}

static BOOL rf_begin_paint(rdpContext *context)
{
	TRACE_CALL(__func__);
	rdpGdi *gdi;

	if (!context)
		return FALSE;

	gdi = context->gdi;
	if (!gdi || !gdi->primary || !gdi->primary->hdc || !gdi->primary->hdc->hwnd)
		return FALSE;

	return TRUE;
}

static BOOL rf_end_paint(rdpContext *context)
{
	TRACE_CALL(__func__);
	rdpGdi *gdi;
	rfContext *rfi;
	RemminaPluginRdpUiObject *ui;
	int i, ninvalid;
	region *reg;
	HGDI_RGN cinvalid;

	gdi = context->gdi;
	rfi = (rfContext *)context;

	if (gdi == NULL || gdi->primary == NULL || gdi->primary->hdc == NULL || gdi->primary->hdc->hwnd == NULL)
		return TRUE;

	if (gdi->primary->hdc->hwnd->invalid->null)
		return TRUE;

	if (gdi->primary->hdc->hwnd->ninvalid < 1)
		return TRUE;

	ninvalid = gdi->primary->hdc->hwnd->ninvalid;
	cinvalid = gdi->primary->hdc->hwnd->cinvalid;
	reg = (region *)g_malloc(sizeof(region) * ninvalid);
	for (i = 0; i < ninvalid; i++) {
		reg[i].x = cinvalid[i].x;
		reg[i].y = cinvalid[i].y;
		reg[i].w = cinvalid[i].w;
		reg[i].h = cinvalid[i].h;
	}

	ui = g_new0(RemminaPluginRdpUiObject, 1);
	ui->type = REMMINA_RDP_UI_UPDATE_REGIONS;
	ui->reg.ninvalid = ninvalid;
	ui->reg.ureg = reg;

	remmina_rdp_event_queue_ui_async(rfi->protocol_widget, ui);


	gdi->primary->hdc->hwnd->invalid->null = TRUE;
	gdi->primary->hdc->hwnd->ninvalid = 0;


	return TRUE;
}

static BOOL rf_desktop_resize(rdpContext *context)
{
	TRACE_CALL(__func__);
	rfContext *rfi;
	RemminaProtocolWidget *gp;
	RemminaPluginRdpUiObject *ui;
	UINT32 w, h;

	rfi = (rfContext *)context;
	gp = rfi->protocol_widget;

	w = freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_DesktopWidth);
	h = freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_DesktopHeight);
	remmina_plugin_service->protocol_plugin_set_width(gp, w);
	remmina_plugin_service->protocol_plugin_set_height(gp, h);

	ui = g_new0(RemminaPluginRdpUiObject, 1);
	ui->type = REMMINA_RDP_UI_EVENT;
	ui->event.type = REMMINA_RDP_UI_EVENT_DESTROY_CAIRO_SURFACE;
	remmina_rdp_event_queue_ui_sync_retint(gp, ui);

	/* Tell libfreerdp to change its internal GDI bitmap width and height,
	 * this will also destroy gdi->primary_buffer, making our rfi->surface invalid */
	gdi_resize(((rdpContext *)rfi)->gdi, w, h);

	/* Call to remmina_rdp_event_update_scale(gp) on the main UI thread,
	 * this will recreate rfi->surface from gdi->primary_buffer */

	ui = g_new0(RemminaPluginRdpUiObject, 1);
	ui->type = REMMINA_RDP_UI_EVENT;
	ui->event.type = REMMINA_RDP_UI_EVENT_UPDATE_SCALE;
	remmina_rdp_event_queue_ui_sync_retint(gp, ui);

	remmina_plugin_service->protocol_plugin_desktop_resize(gp);

	return TRUE;
}

static BOOL rf_play_sound(rdpContext *context, const PLAY_SOUND_UPDATE *play_sound)
{
	TRACE_CALL(__func__);
	rfContext *rfi;
	RemminaProtocolWidget *gp;
	GdkDisplay *disp;

	rfi = (rfContext *)context;
	gp = rfi->protocol_widget;

	disp = gtk_widget_get_display(GTK_WIDGET(gp));
	gdk_display_beep(disp);

	return TRUE;
}

static BOOL rf_keyboard_set_indicators(rdpContext *context, UINT16 led_flags)
{
	TRACE_CALL(__func__);
	rfContext *rfi;
	RemminaProtocolWidget *gp;
	GdkDisplay *disp;

	rfi = (rfContext *)context;
	gp = rfi->protocol_widget;
	disp = gtk_widget_get_display(GTK_WIDGET(gp));

#ifdef GDK_WINDOWING_X11
	if (GDK_IS_X11_DISPLAY(disp)) {
		/* TODO: We are not on the main thread. Will X.Org complain? */
		Display *x11_display;
		x11_display = gdk_x11_display_get_xdisplay(disp);
		XkbLockModifiers(x11_display, XkbUseCoreKbd,
				 LockMask | Mod2Mask,
				 ((led_flags & KBD_SYNC_CAPS_LOCK) ? LockMask : 0) |
				 ((led_flags & KBD_SYNC_NUM_LOCK) ? Mod2Mask : 0)
				 );

		/* TODO: Add support to KANA_LOCK and SCROLL_LOCK */
	}
#endif

	return TRUE;
}

static BOOL rf_keyboard_set_ime_status(rdpContext *context, UINT16 imeId, UINT32 imeState,
				UINT32 imeConvMode)
{
	TRACE_CALL(__func__);
	if (!context)
		return FALSE;

	/* Unimplemented, we ignore it */

	return TRUE;
}

static BOOL remmina_rdp_pre_connect(freerdp *instance)
{
	TRACE_CALL(__func__);
	rdpChannels *channels;
	rdpSettings *settings;
	rdpContext *context = instance->context;

	settings = context->settings;
	channels = context->channels;
	freerdp_settings_set_uint32(settings, FreeRDP_OsMajorType, OSMAJORTYPE_UNIX);
	freerdp_settings_set_uint32(settings, FreeRDP_OsMinorType, OSMINORTYPE_UNSPECIFIED);
	freerdp_settings_set_bool(settings, FreeRDP_BitmapCacheEnabled, TRUE);
	freerdp_settings_set_uint32(settings, FreeRDP_OffscreenSupportLevel, 1);

	PubSub_SubscribeChannelConnected(instance->context->pubSub,
					 remmina_rdp_OnChannelConnectedEventHandler);
	PubSub_SubscribeChannelDisconnected(instance->context->pubSub,
						remmina_rdp_OnChannelDisconnectedEventHandler);

	if (!freerdp_client_load_addins(channels, settings))
		return FALSE;

	return true;
}

static BOOL remmina_rdp_post_connect(freerdp *instance)
{
	TRACE_CALL(__func__);
	rfContext *rfi;
	RemminaProtocolWidget *gp;
	RemminaPluginRdpUiObject *ui;
	UINT32 freerdp_local_color_format;

	rfi = (rfContext *)instance->context;
	gp = rfi->protocol_widget;
	rfi->postconnect_error = REMMINA_POSTCONNECT_ERROR_OK;

	rfi->attempt_interactive_authentication = FALSE; // We authenticated!

	rfi->srcBpp = freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_ColorDepth);

	if (freerdp_settings_get_bool(rfi->clientContext.context.settings, FreeRDP_RemoteFxCodec) == FALSE)
		rfi->sw_gdi = TRUE;

	rf_register_graphics(instance->context->graphics);

	REMMINA_PLUGIN_DEBUG("bpp: %d", rfi->bpp);
	switch (rfi->bpp) {
	case 24:
		REMMINA_PLUGIN_DEBUG("CAIRO_FORMAT_RGB24");
		freerdp_local_color_format = PIXEL_FORMAT_BGRX32;
		rfi->cairo_format = CAIRO_FORMAT_RGB24;
		break;
	case 32:
		/** Do not use alpha as it's not used with the desktop
		 * CAIRO_FORMAT_ARGB32
		 * See https://gitlab.com/Remmina/Remmina/-/issues/2456
		 */
		REMMINA_PLUGIN_DEBUG("CAIRO_FORMAT_RGB24");
		freerdp_local_color_format = PIXEL_FORMAT_BGRA32;
		rfi->cairo_format = CAIRO_FORMAT_RGB24;
		break;
	default:
		REMMINA_PLUGIN_DEBUG("CAIRO_FORMAT_RGB16_565");
		freerdp_local_color_format = PIXEL_FORMAT_RGB16;
		rfi->cairo_format = CAIRO_FORMAT_RGB16_565;
		break;
	}

	if (!gdi_init(instance, freerdp_local_color_format)) {
		rfi->postconnect_error = REMMINA_POSTCONNECT_ERROR_GDI_INIT;
		return FALSE;
	}

	if (instance->context->codecs->h264 == NULL && freerdp_settings_get_bool(rfi->clientContext.context.settings, FreeRDP_GfxH264)) {
		gdi_free(instance);
		rfi->postconnect_error = REMMINA_POSTCONNECT_ERROR_NO_H264;
		return FALSE;
	}

	rdpUpdate *update = instance->context->update;
	update->BeginPaint = rf_begin_paint;
	update->EndPaint = rf_end_paint;
	update->DesktopResize = rf_desktop_resize;

	update->PlaySound = rf_play_sound;
	update->SetKeyboardIndicators = rf_keyboard_set_indicators;
	update->SetKeyboardImeStatus = rf_keyboard_set_ime_status;

	remmina_rdp_clipboard_init(rfi);
	rfi->connected = true;

	ui = g_new0(RemminaPluginRdpUiObject, 1);
	ui->type = REMMINA_RDP_UI_CONNECTED;
	remmina_rdp_event_queue_ui_async(gp, ui);

	const char* KeyboardRemappingList =
	    freerdp_settings_get_string(instance->context->settings, FreeRDP_KeyboardRemappingList);


#if FREERDP_CHECK_VERSION(3, 11, 0)
	rfi->remap_table = freerdp_keyboard_remap_string_to_list(KeyboardRemappingList);
	if (!rfi->remap_table)
		return FALSE;
#else
	rfi->remap_table = NULL;
#endif

	return TRUE;
}

#if !defined(FREERDP_VERSION_MAJOR) || (FREERDP_VERSION_MAJOR < 3)
static BOOL remmina_rdp_authenticate(freerdp *instance, char **username, char **password, char **domain)
{
	TRACE_CALL(__func__);
	gchar *s_username, *s_password, *s_domain;
	gint ret;
	rfContext *rfi;
	RemminaProtocolWidget *gp;
	gboolean save;
	gboolean disablepasswordstoring;
	RemminaFile *remminafile;

	rfi = (rfContext *)instance->context;
	gp = rfi->protocol_widget;
	remminafile = remmina_plugin_service->protocol_plugin_get_file(gp);
	disablepasswordstoring = remmina_plugin_service->file_get_int(remminafile, "disablepasswordstoring", FALSE);

	ret = remmina_plugin_service->protocol_plugin_init_auth(gp,
								(disablepasswordstoring ? 0 : REMMINA_MESSAGE_PANEL_FLAG_SAVEPASSWORD) | REMMINA_MESSAGE_PANEL_FLAG_USERNAME | REMMINA_MESSAGE_PANEL_FLAG_DOMAIN,
								_("Enter RDP authentication credentials"),
								remmina_plugin_service->file_get_string(remminafile, "username"),
								remmina_plugin_service->file_get_string(remminafile, "password"),
								remmina_plugin_service->file_get_string(remminafile, "domain"),
								NULL);
	if (ret == GTK_RESPONSE_OK) {
		s_username = remmina_plugin_service->protocol_plugin_init_get_username(gp);
		if (s_username) freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_Username, s_username);

		s_password = remmina_plugin_service->protocol_plugin_init_get_password(gp);
		if (s_password) freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_Password, s_password);

		s_domain = remmina_plugin_service->protocol_plugin_init_get_domain(gp);
		if (s_domain) freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_Domain, s_domain);

		remmina_plugin_service->file_set_string(remminafile, "username", s_username);
		remmina_plugin_service->file_set_string(remminafile, "domain", s_domain);

		save = remmina_plugin_service->protocol_plugin_init_get_savepassword(gp);
		if (save) {
			// User has requested to save credentials. We put the password
			// into remminafile->settings. It will be saved later, on successful connection, by
			// rcw.c
			remmina_plugin_service->file_set_string(remminafile, "password", s_password);
		} else {
			remmina_plugin_service->file_set_string(remminafile, "password", NULL);
		}


		if (s_username) g_free(s_username);
		if (s_password) g_free(s_password);
		if (s_domain) g_free(s_domain);

		return TRUE;
	} else {
		return FALSE;
	}

	return TRUE;
}

static BOOL remmina_rdp_gw_authenticate(freerdp *instance, char **username, char **password, char **domain)
{
	TRACE_CALL(__func__);
	gchar *s_username, *s_password, *s_domain;
	gint ret;
	rfContext *rfi;
	RemminaProtocolWidget *gp;
	gboolean save;
	gboolean disablepasswordstoring;
	gboolean basecredforgw;
	RemminaFile *remminafile;

	rfi = (rfContext *)instance->context;
	gp = rfi->protocol_widget;
	remminafile = remmina_plugin_service->protocol_plugin_get_file(gp);

	if (!remmina_plugin_service->file_get_string(remminafile, "gateway_server"))
		return false;
	disablepasswordstoring = remmina_plugin_service->file_get_int(remminafile, "disablepasswordstoring", FALSE);
	basecredforgw = remmina_plugin_service->file_get_int(remminafile, "base-cred-for-gw", FALSE);

	if (basecredforgw) {
		ret = remmina_plugin_service->protocol_plugin_init_auth(gp,
									(disablepasswordstoring ? 0 : REMMINA_MESSAGE_PANEL_FLAG_SAVEPASSWORD) | REMMINA_MESSAGE_PANEL_FLAG_USERNAME | REMMINA_MESSAGE_PANEL_FLAG_DOMAIN,
									_("Enter RDP authentication credentials"),
									remmina_plugin_service->file_get_string(remminafile, "username"),
									remmina_plugin_service->file_get_string(remminafile, "password"),
									remmina_plugin_service->file_get_string(remminafile, "domain"),
									NULL);
	} else {
		ret = remmina_plugin_service->protocol_plugin_init_auth(gp,
									(disablepasswordstoring ? 0 : REMMINA_MESSAGE_PANEL_FLAG_SAVEPASSWORD) | REMMINA_MESSAGE_PANEL_FLAG_USERNAME | REMMINA_MESSAGE_PANEL_FLAG_DOMAIN,
									_("Enter RDP gateway authentication credentials"),
									remmina_plugin_service->file_get_string(remminafile, "gateway_username"),
									remmina_plugin_service->file_get_string(remminafile, "gateway_password"),
									remmina_plugin_service->file_get_string(remminafile, "gateway_domain"),
									NULL);
	}


	if (ret == GTK_RESPONSE_OK) {
		s_username = remmina_plugin_service->protocol_plugin_init_get_username(gp);
		if (s_username) freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_GatewayUsername, s_username);

		s_password = remmina_plugin_service->protocol_plugin_init_get_password(gp);
		if (s_password) freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_GatewayPassword, s_password);

		s_domain = remmina_plugin_service->protocol_plugin_init_get_domain(gp);
		if (s_domain) freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_GatewayDomain, s_domain);

		save = remmina_plugin_service->protocol_plugin_init_get_savepassword(gp);

		if (basecredforgw) {
			remmina_plugin_service->file_set_string(remminafile, "username", s_username);
			remmina_plugin_service->file_set_string(remminafile, "domain", s_domain);
			if (save)
				remmina_plugin_service->file_set_string(remminafile, "password", s_password);
			else
				remmina_plugin_service->file_set_string(remminafile, "password", NULL);
		} else {
			remmina_plugin_service->file_set_string(remminafile, "gateway_username", s_username);
			remmina_plugin_service->file_set_string(remminafile, "gateway_domain", s_domain);
			if (save)
				remmina_plugin_service->file_set_string(remminafile, "gateway_password", s_password);
			else
				remmina_plugin_service->file_set_string(remminafile, "gateway_password", NULL);
		}

		if (s_username) g_free(s_username);
		if (s_password) g_free(s_password);
		if (s_domain) g_free(s_domain);

		return true;
	} else {
		return false;
	}

	return true;
}
#else
static BOOL remmina_rdp_authenticate_ex(freerdp* instance, char** username, char** password,
                                char** domain, rdp_auth_reason reason)
{
	TRACE_CALL(__func__);
	const gchar *s_username = NULL;
	const gchar *s_password = NULL;
	const gchar *s_domain = NULL;
	const gchar* key_user = NULL;
	const gchar* key_domain = NULL;
	const gchar* key_password = NULL;
	const gchar* key_title = NULL;
	gint ret;
	rfContext *rfi;
	RemminaProtocolWidget *gp;
	gboolean save;
	gboolean disablepasswordstoring;
	RemminaFile *remminafile;
	RemminaMessagePanelFlags flags =  REMMINA_MESSAGE_PANEL_FLAG_USERNAME | REMMINA_MESSAGE_PANEL_FLAG_DOMAIN;

	rfi = (rfContext *)instance->context;
	gp = rfi->protocol_widget;
	remminafile = remmina_plugin_service->protocol_plugin_get_file(gp);
	disablepasswordstoring = remmina_plugin_service->file_get_int(remminafile, "disablepasswordstoring", FALSE);

	FreeRDP_Settings_Keys_String cfg_key_user = FreeRDP_STRING_UNUSED;
	FreeRDP_Settings_Keys_String cfg_key_domain = FreeRDP_STRING_UNUSED;
	FreeRDP_Settings_Keys_String cfg_key_password = FreeRDP_STRING_UNUSED;
	switch(reason) {
		case AUTH_NLA:
		case AUTH_TLS:
		case AUTH_RDP:
			if ((*username) && (*password)){
				return true;
			}
			key_title = _("Enter RDP authentication credentials");
			key_user = "username";
			key_domain = "domain";
			key_password = "password";
			cfg_key_user = FreeRDP_Username;
			cfg_key_domain = FreeRDP_Domain;
			cfg_key_password = FreeRDP_Password;
			break;
		case GW_AUTH_HTTP:
		case GW_AUTH_RDG:
		case GW_AUTH_RPC:
			key_title = _("Enter RDP gateway authentication credentials");
			key_user = "gateway_username";
			key_domain = "gateway_domain";
			key_password = "gateway_password";
			cfg_key_user = FreeRDP_GatewayUsername;
			cfg_key_domain = FreeRDP_GatewayDomain;
			cfg_key_password = FreeRDP_GatewayPassword;
			break;
		case AUTH_SMARTCARD_PIN:
			key_title = _("Enter RDP SmartCard PIN");
			key_password = "smartcard_pin";
			cfg_key_password = FreeRDP_Password;
			flags = 0;
			break;
		default:
			// TODO: Display an error dialog informing the user that the remote requires some mechanism FreeRDP or Remmina currently do not support
			g_fprintf(stderr, "[authentication] unsupported type %d, access denied", reason);
			return FALSE;
	}

	if (!disablepasswordstoring)
		flags |= REMMINA_MESSAGE_PANEL_FLAG_SAVEPASSWORD;

	if(key_user)
		s_username = remmina_plugin_service->file_get_string(remminafile, key_user);

	if(key_password)
		s_password = remmina_plugin_service->file_get_string(remminafile, key_password);

	if(key_domain)
		s_domain = remmina_plugin_service->file_get_string(remminafile, key_domain);

	ret = remmina_plugin_service->protocol_plugin_init_auth(gp, flags,
								key_title,
								s_username,
								s_password,
								s_domain,
								NULL);
	BOOL rc = FALSE;
	if (ret == GTK_RESPONSE_OK) {
		if (cfg_key_user != FreeRDP_STRING_UNUSED)
		{
			gchar* s_username = remmina_plugin_service->protocol_plugin_init_get_username(gp);
			if (s_username)
				freerdp_settings_set_string(rfi->clientContext.context.settings, cfg_key_user, s_username);
			remmina_plugin_service->file_set_string(remminafile, key_user, s_username);
			g_free(s_username);
		}

		gchar* s_pwd_copy = NULL;
		if (s_password)
			s_pwd_copy= strdup(s_password);
		if (cfg_key_password != FreeRDP_STRING_UNUSED)
		{
			g_free(s_pwd_copy);
			s_pwd_copy = remmina_plugin_service->protocol_plugin_init_get_password(gp);
			if (s_pwd_copy)
				freerdp_settings_set_string(rfi->clientContext.context.settings, cfg_key_password, s_pwd_copy);
		}

		if (cfg_key_domain != FreeRDP_STRING_UNUSED) {
			gchar* s_domain = remmina_plugin_service->protocol_plugin_init_get_domain(gp);
			if (s_domain)
				freerdp_settings_set_string(rfi->clientContext.context.settings, cfg_key_domain, s_domain);
			remmina_plugin_service->file_set_string(remminafile, key_domain, s_domain);
			g_free(s_domain);
		}

		save = remmina_plugin_service->protocol_plugin_init_get_savepassword(gp);
		if (save) {
			// User has requested to save credentials. We put the password
			// into remminafile->settings. It will be saved later, on successful connection, by
			// rcw.c
			remmina_plugin_service->file_set_string(remminafile, key_password, s_pwd_copy);
		} else {
			remmina_plugin_service->file_set_string(remminafile, key_password, NULL);
		}
		g_free(s_pwd_copy);

		rc = TRUE;
	}

	return rc;
}

static BOOL remmina_rdp_choose_smartcard(freerdp* instance, SmartcardCertInfo** cert_list, DWORD count,
                                 DWORD* choice, BOOL gateway)
{
	// TODO: Display a simple list of smartcards/certs to choose from.
	// See client_cli_choose_smartcard for a sample
	return client_cli_choose_smartcard(instance, cert_list, count, choice, gateway);
}

static BOOL remmina_rdp_get_access_token(freerdp* instance, AccessTokenType tokenType, char** token,
                                    size_t count, ...)
{
	// TODO: Open a (currently hard coded) URL, authenticate in a webview/browser, return the access token.
	// See client_cli_get_access_token or sdl_webview_get_access_token for implementations
	return client_cli_get_access_token(instance, tokenType, token, count);
}

static BOOL remmina_rdp_present_gateway_message(freerdp* instance, UINT32 type, BOOL isDisplayMandatory,
                                           BOOL isConsentMandatory, size_t length,
                                           const WCHAR* message)
{
	if (!isConsentMandatory && !isDisplayMandatory){
		return TRUE;
	}

	rfContext *rfi;
	RemminaProtocolWidget *gp;
	rfi = (rfContext *)instance->context;
	gp = rfi->protocol_widget;
	int ret = remmina_plugin_service->protocol_widget_panel_accept(gp, (char*)message);
	if (ret == GTK_RESPONSE_YES){
		return TRUE;
	}
	else{
		return FALSE;
	}
}

static int remmina_rdp_logon_error_info(freerdp* instance, UINT32 data, UINT32 type)
{
	// TODO: Display the reason for connection termination
	// See client_cli_logon_error_info or sdl_logon_error_info
	return client_cli_logon_error_info(instance, data, type);
}

static SSIZE_T remmina_rdp_retry_dialog(freerdp* instance, const char* what, size_t current,
                               void* userarg)
{
	// TODO:
	// See client_common_retry_dialog or
	return client_common_retry_dialog(instance, what, current, userarg);
}

static void remmina_rdp_post_final_disconnect(freerdp* instance)
{
	// Clean up resources allocated in PreConnect
}
#endif

static DWORD remmina_rdp_verify_certificate_ex(freerdp *instance, const char *host, UINT16 port,
						   const char *common_name, const char *subject,
						   const char *issuer, const char *fingerprint, DWORD flags)
{
	TRACE_CALL(__func__);
	gint status;
	rfContext *rfi;
	RemminaProtocolWidget *gp;

	rfi = (rfContext *)instance->context;
	gp = rfi->protocol_widget;

	status = remmina_plugin_service->protocol_plugin_init_certificate(gp, subject, issuer, fingerprint);

	if (status == GTK_RESPONSE_OK)
		return 1;

	return 0;
}

static DWORD
remmina_rdp_verify_certificate(freerdp *instance, const char *common_name, const char *subject, const char *issuer, const char *fingerprint, BOOL host_mismatch) __attribute__ ((unused));
static DWORD
remmina_rdp_verify_certificate(freerdp *instance, const char *common_name, const char *subject,
				   const char *issuer, const char *fingerprint, BOOL host_mismatch)
{
	TRACE_CALL(__func__);
	gint status;
	rfContext *rfi;
	RemminaProtocolWidget *gp;

	rfi = (rfContext *)instance->context;
	gp = rfi->protocol_widget;

	status = remmina_plugin_service->protocol_plugin_init_certificate(gp, subject, issuer, fingerprint);

	if (status == GTK_RESPONSE_OK)
		return 1;

	return 0;
}

static DWORD remmina_rdp_verify_changed_certificate_ex(freerdp *instance, const char *host, UINT16 port,
							   const char *common_name, const char *subject,
							   const char *issuer, const char *fingerprint,
							   const char *old_subject, const char *old_issuer,
							   const char *old_fingerprint, DWORD flags)
{
	TRACE_CALL(__func__);
	gint status;
	rfContext *rfi;
	RemminaProtocolWidget *gp;

	rfi = (rfContext *)instance->context;
	gp = rfi->protocol_widget;

	status = remmina_plugin_service->protocol_plugin_changed_certificate(gp, subject, issuer, fingerprint, old_fingerprint);

	if (status == GTK_RESPONSE_OK)
		return 1;

	return 0;
}

static void remmina_rdp_post_disconnect(freerdp *instance)
{
	TRACE_CALL(__func__);
	rfContext *rfi;

	if (!instance || !instance->context)
		return;
	rfi = (rfContext *)instance->context;
	PubSub_UnsubscribeChannelConnected(instance->context->pubSub,
					   remmina_rdp_OnChannelConnectedEventHandler);
	PubSub_UnsubscribeChannelDisconnected(instance->context->pubSub,
						  remmina_rdp_OnChannelDisconnectedEventHandler);

	/* The remaining cleanup will be continued on main thread by complete_cleanup_on_main_thread() */

	// With FreeRDP3 only resources allocated in PostConnect and later are cleaned up here.
#if FREERDP_CHECK_VERSION(3, 11, 0)
	freerdp_keyboard_remap_free(rfi->remap_table);
	rfi->remap_table = NULL;
#endif
}

static void remmina_rdp_main_loop(RemminaProtocolWidget *gp)
{
	TRACE_CALL(__func__);
	DWORD status;
	gchar buf[100];
	rfContext *rfi = GET_PLUGIN_DATA(gp);
	RemminaFile *remminafile = remmina_plugin_service->protocol_plugin_get_file(gp);
	time_t cur_time, time_diff_jitter, time_diff_keypress;

	int jitter_time = remmina_plugin_service->file_get_int(remminafile, "rdp_mouse_jitter", 0);
 	int keypress_time = remmina_plugin_service->file_get_int(remminafile, "rdp_idle_keypress_time", 0);
	int keypress_opts = remmina_plugin_service->file_get_int(remminafile, "rdp_idle_keypress_combo", 0);
	time(&(rfi->last_time));
	time(&(rfi->last_time_idle_keypress));
#if FREERDP_VERSION_MAJOR >= 3
	while (!freerdp_shall_disconnect_context(&rfi->clientContext.context)) {
#else
	while (!freerdp_shall_disconnect(rfi->clientContext.context.instance)) {
#endif
		// move mouse if we've been idle and option is selected
		time(&cur_time);
		time_diff_jitter = cur_time - rfi->last_time;
		if (jitter_time > 0 && time_diff_jitter > jitter_time){
			rfi->last_time = cur_time;
			remmina_rdp_mouse_jitter(gp);
		}
		// press key(s) if we've been idle and option is selected
		time(&cur_time);
		time_diff_keypress = cur_time - rfi->last_time_idle_keypress;		
		if (keypress_time > 0 && time_diff_keypress > keypress_time){
			rfi->last_time_idle_keypress = cur_time;
			remmina_rdp_idle_keypress(gp, &keypress_opts);
		}

		HANDLE handles[MAXIMUM_WAIT_OBJECTS] = {0};
		DWORD nCount = freerdp_get_event_handles(&rfi->clientContext.context, &handles[0], ARRAYSIZE(handles));
		if (rfi->event_handle)
			handles[nCount++] = rfi->event_handle;

		handles[nCount++] = freerdp_abort_event(&rfi->clientContext.context);

		if (nCount == 0) {
			fprintf(stderr, "freerdp_get_event_handles failed\n");
			break;
		}

		status = WaitForMultipleObjects(nCount, handles, FALSE, 100);

		if (status == WAIT_FAILED) {
			fprintf(stderr, "WaitForMultipleObjects failed with %lu\n", (unsigned long)status);
			break;
		}

		if (rfi->event_handle && WaitForSingleObject(rfi->event_handle, 0) == WAIT_OBJECT_0) {
			if (!rf_process_event_queue(gp)) {
				fprintf(stderr, "Could not process local keyboard/mouse event queue\n");
				break;
			}
			if (read(rfi->event_pipe[0], buf, sizeof(buf))) {
			}
		}

		/* Check if a processed event called freerdp_abort_connect() and exit if true */
		if (WaitForSingleObject(freerdp_abort_event(&rfi->clientContext.context), 0) == WAIT_OBJECT_0)
			/* Session disconnected by local user action */
			break;

		if (!freerdp_check_event_handles(&rfi->clientContext.context)) {
			if (rf_auto_reconnect(rfi)) {
				/* Reset the possible reason/error which made us doing many reconnection reattempts and continue */
				remmina_plugin_service->protocol_plugin_set_error(gp, NULL);
				continue;
			}
			if (freerdp_get_last_error(&rfi->clientContext.context) == FREERDP_ERROR_SUCCESS)
				fprintf(stderr, "Could not check FreeRDP file descriptor\n");
			break;
		}
	}
	const gchar *host = freerdp_settings_get_string (rfi->clientContext.context.settings, FreeRDP_ServerHostname);
	// TRANSLATORS: the placeholder may be either an IP/FQDN or a server hostname
	REMMINA_PLUGIN_AUDIT(_("Disconnected from %s via RDP"), host);
	freerdp_disconnect(rfi->clientContext.context.instance);
	REMMINA_PLUGIN_DEBUG("RDP client disconnected");
}

static int remmina_rdp_load_static_channel_addin(rdpChannels *channels, rdpSettings *settings, char *name, void *data)
{
	TRACE_CALL(__func__);
	PVIRTUALCHANNELENTRY entry = NULL;
	PVIRTUALCHANNELENTRYEX entryEx = NULL;

	entryEx = (PVIRTUALCHANNELENTRYEX)(void *)freerdp_load_channel_addin_entry(
		name, NULL, NULL, FREERDP_ADDIN_CHANNEL_STATIC | FREERDP_ADDIN_CHANNEL_ENTRYEX);

	if (!entryEx)
		entry = freerdp_load_channel_addin_entry(name, NULL, NULL, FREERDP_ADDIN_CHANNEL_STATIC);

	if (entryEx) {
		if (freerdp_channels_client_load_ex(channels, settings, entryEx, data) == 0) {
			fprintf(stderr, "loading channel %s\n", name);
			return TRUE;
		}
	} else if (entry) {
		if (freerdp_channels_client_load(channels, settings, entry, data) == 0) {
			fprintf(stderr, "loading channel %s\n", name);
			return TRUE;
		}
	}

	return FALSE;
}

static gchar *remmina_rdp_find_prdriver(char *smap, char *prn)
{
	char c, *p, *dr;
	int matching;
	size_t sz;

	enum { S_WAITPR,
	       S_INPRINTER,
	       S_WAITCOLON,
	       S_WAITDRIVER,
	       S_INDRIVER,
	       S_WAITSEMICOLON } state = S_WAITPR;

	matching = 0;
	while ((c = *smap++) != 0) {
		switch (state) {
		case S_WAITPR:
			if (c != '\"') return NULL;
			state = S_INPRINTER;
			p = prn;
			matching = 1;
			break;
		case S_INPRINTER:
			if (matching && c == *p && *p != 0) {
				p++;
			} else if (c == '\"') {
				if (*p != 0)
					matching = 0;
				state = S_WAITCOLON;
			} else {
				matching = 0;
			}
			break;
		case S_WAITCOLON:
			if (c != ':')
				return NULL;
			state = S_WAITDRIVER;
			break;
		case S_WAITDRIVER:
			if (c != '\"')
				return NULL;
			state = S_INDRIVER;
			dr = smap;
			break;
		case S_INDRIVER:
			if (c == '\"') {
				if (matching)
					goto found;
				else
					state = S_WAITSEMICOLON;
			}
			break;
		case S_WAITSEMICOLON:
			if (c != ';')
				return NULL;
			state = S_WAITPR;
			break;
		}
	}
	return NULL;

found:
	sz = smap - dr;
	p = (char *)malloc(sz);
	memcpy(p, dr, sz);
	p[sz - 1] = 0;
	return p;
}

#ifdef HAVE_CUPS
/**
 * Callback function used by cupsEnumDests
 *   - For each enumerated local printer tries to set the Printer Name and Driver.
 * @return 1 if there are other printers to scan or 0 when it's done.
 */
static int remmina_rdp_set_printers(void *user_data, unsigned flags, cups_dest_t *dest)
{
	rfContext *rfi = (rfContext *)user_data;
	RemminaProtocolWidget *gp = rfi->protocol_widget;

	/** @warning printer-make-and-model is not always the same as on the Windows,
	 * therefore it fails finding to right one and it fails to add
	 * the printer.
	 *
	 * We pass NULL and we do not check for errors. The following code is
	 * how it is supposed to work. @todo Ask CUPS mailing list for help.
	 *
	 * @code
	 * const char *model = cupsGetOption("printer-make-and-model",
	 * 		dest->num_options,
	 * 		dest->options);
	 * @endcode
	 */

	RemminaFile *remminafile = remmina_plugin_service->protocol_plugin_get_file(gp);
	const gchar *s = remmina_plugin_service->file_get_string(remminafile, "printer_overrides");

	/* Devices must be allocated with freerdp_device_new(): FreeRDP clones
	 * them internally (freerdp_settings_clone -> freerdp_device_clone) and
	 * a plain calloc()'d RDPDR_PRINTER is too small for that, causing a
	 * heap out-of-bounds read and a crash in freerdp_connect(). */
	REMMINA_PLUGIN_DEBUG("Destination: %s", dest->name);

	const char *driver = NULL;
	gchar *d = NULL;
	if (s) {
		d = remmina_rdp_find_prdriver(strdup(s), dest->name);
		if (!d) {
			/**
			 * When remmina_rdp_find_prdriver doesn't return a DriverName
			 * it means that we don't want to share that printer
			 */
			return 1;
		}
		driver = d;
	} else {
		/* We set to a default driver*/
		driver = "MS Publisher Imagesetter";
	}

	const char *args[] = { "printer", dest->name, driver };
	RDPDR_DEVICE *pdev = freerdp_device_new(RDPDR_DTYP_PRINT, 3, args);
	g_free(d);

	if (!pdev) {
		REMMINA_PLUGIN_DEBUG("Could not allocate printer device");
		return 1;
	}

	REMMINA_PLUGIN_DEBUG("Printer Name: %s", pdev->Name);
	REMMINA_PLUGIN_DEBUG("Printer Driver: %s", ((RDPDR_PRINTER *)pdev)->DriverName);

	freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_RedirectPrinters, TRUE);
	freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_DeviceRedirection, TRUE);

	if (!freerdp_device_collection_add(rfi->clientContext.context.settings, pdev)) {
		REMMINA_PLUGIN_DEBUG("Could not add printer device");
		freerdp_device_free(pdev);
		return 1;
	}

	return 1;
}
#endif /* HAVE_CUPS */

/* Send Ctrl+Alt+Del keystrokes to the plugin drawing_area widget */
static void remmina_rdp_send_ctrlaltdel(RemminaProtocolWidget *gp)
{
	TRACE_CALL(__func__);
	guint keys[] = { GDK_KEY_Control_L, GDK_KEY_Alt_L, GDK_KEY_Delete };
	rfContext *rfi = GET_PLUGIN_DATA(gp);

	remmina_plugin_service->protocol_plugin_send_keys_signals(rfi->drawing_area,
								  keys, G_N_ELEMENTS(keys), GDK_KEY_PRESS | GDK_KEY_RELEASE);
}

static gboolean remmina_rdp_set_connection_type(rdpSettings *settings, guint32 type)
{
	freerdp_settings_set_uint32(settings, FreeRDP_ConnectionType, type);

	if (type == CONNECTION_TYPE_MODEM) {
		freerdp_settings_set_bool(settings, FreeRDP_DisableWallpaper, TRUE);
		freerdp_settings_set_bool(settings, FreeRDP_AllowFontSmoothing, FALSE);
		freerdp_settings_set_bool(settings, FreeRDP_AllowDesktopComposition, FALSE);
		freerdp_settings_set_bool(settings, FreeRDP_DisableFullWindowDrag, TRUE);
		freerdp_settings_set_bool(settings, FreeRDP_DisableMenuAnims, TRUE);
		freerdp_settings_set_bool(settings, FreeRDP_DisableThemes, TRUE);
	} else if (type == CONNECTION_TYPE_BROADBAND_LOW) {
		freerdp_settings_set_bool(settings, FreeRDP_DisableWallpaper, TRUE);
		freerdp_settings_set_bool(settings, FreeRDP_AllowFontSmoothing, FALSE);
		freerdp_settings_set_bool(settings, FreeRDP_AllowDesktopComposition, FALSE);
		freerdp_settings_set_bool(settings, FreeRDP_DisableFullWindowDrag, TRUE);
		freerdp_settings_set_bool(settings, FreeRDP_DisableMenuAnims, TRUE);
		freerdp_settings_set_bool(settings, FreeRDP_DisableThemes, FALSE);
	} else if (type == CONNECTION_TYPE_SATELLITE) {
		freerdp_settings_set_bool(settings, FreeRDP_DisableWallpaper, TRUE);
		freerdp_settings_set_bool(settings, FreeRDP_AllowFontSmoothing, FALSE);
		freerdp_settings_set_bool(settings, FreeRDP_AllowDesktopComposition, TRUE);
		freerdp_settings_set_bool(settings, FreeRDP_DisableFullWindowDrag, TRUE);
		freerdp_settings_set_bool(settings, FreeRDP_DisableMenuAnims, TRUE);
		freerdp_settings_set_bool(settings, FreeRDP_DisableThemes, FALSE);
	} else if (type == CONNECTION_TYPE_BROADBAND_HIGH) {
		freerdp_settings_set_bool(settings, FreeRDP_DisableWallpaper, TRUE);
		freerdp_settings_set_bool(settings, FreeRDP_AllowFontSmoothing, FALSE);
		freerdp_settings_set_bool(settings, FreeRDP_AllowDesktopComposition, TRUE);
		freerdp_settings_set_bool(settings, FreeRDP_DisableFullWindowDrag, TRUE);
		freerdp_settings_set_bool(settings, FreeRDP_DisableMenuAnims, TRUE);
		freerdp_settings_set_bool(settings, FreeRDP_DisableThemes, FALSE);
	} else if (type == CONNECTION_TYPE_WAN) {
		freerdp_settings_set_bool(settings, FreeRDP_DisableWallpaper, FALSE);
		freerdp_settings_set_bool(settings, FreeRDP_AllowFontSmoothing, TRUE);
		freerdp_settings_set_bool(settings, FreeRDP_AllowDesktopComposition, TRUE);
		freerdp_settings_set_bool(settings, FreeRDP_DisableFullWindowDrag, FALSE);
		freerdp_settings_set_bool(settings, FreeRDP_DisableMenuAnims, FALSE);
		freerdp_settings_set_bool(settings, FreeRDP_DisableThemes, FALSE);
	} else if (type == CONNECTION_TYPE_LAN) {
		freerdp_settings_set_bool(settings, FreeRDP_DisableWallpaper, FALSE);
		freerdp_settings_set_bool(settings, FreeRDP_AllowFontSmoothing, TRUE);
		freerdp_settings_set_bool(settings, FreeRDP_AllowDesktopComposition, TRUE);
		freerdp_settings_set_bool(settings, FreeRDP_DisableFullWindowDrag, FALSE);
		freerdp_settings_set_bool(settings, FreeRDP_DisableMenuAnims, FALSE);
		freerdp_settings_set_bool(settings, FreeRDP_DisableThemes, FALSE);
	} else if (type == CONNECTION_TYPE_AUTODETECT) {
		freerdp_settings_set_bool(settings, FreeRDP_DisableWallpaper, FALSE);
		freerdp_settings_set_bool(settings, FreeRDP_AllowFontSmoothing, TRUE);
		freerdp_settings_set_bool(settings, FreeRDP_AllowDesktopComposition, TRUE);
		freerdp_settings_set_bool(settings, FreeRDP_DisableFullWindowDrag, FALSE);
		freerdp_settings_set_bool(settings, FreeRDP_DisableMenuAnims, FALSE);
		freerdp_settings_set_bool(settings, FreeRDP_DisableThemes, FALSE);
		freerdp_settings_set_bool(settings, FreeRDP_NetworkAutoDetect, TRUE);

		/* Automatically activate GFX and RFX codec support */
#ifdef WITH_GFX_H264
		freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, gfx_h264_available);
		freerdp_settings_set_bool(settings, FreeRDP_GfxH264, gfx_h264_available);
#endif
		freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, TRUE);
		freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, TRUE);
	} else if (type == REMMINA_CONNECTION_TYPE_NONE) {
		return FALSE;
	} else {
		return FALSE;
	}

	return TRUE;
}

#ifdef GDK_WINDOWING_X11
#if FREERDP_CHECK_VERSION(2, 3, 0)
static gchar *remmina_get_rdp_kbd_remap(const gchar *keymap)
{
	TRACE_CALL(__func__);
	guint *table;
	gchar keys[20];
	gchar *rdp_kbd_remap = NULL;
	gint i;
	Display *display;

	table = remmina_plugin_service->pref_keymap_get_table(keymap);
	if (!table)
		return rdp_kbd_remap;
	rdp_kbd_remap = g_malloc0(512);
	display = XOpenDisplay(0);
	for (i = 0; table[i] > 0; i += 2) {
#if !defined(WITHOUT_FREERDP_3x_DEPRECATED)
		g_snprintf(keys, sizeof(keys), "0x%02x=0x%02x", freerdp_keyboard_get_rdp_scancode_from_x11_keycode(XKeysymToKeycode(display, table[i])),
			freerdp_keyboard_get_rdp_scancode_from_x11_keycode(XKeysymToKeycode(display, table[i + 1])));
#else
#warning "TODO: freerdp_keyboard_get_rdp_scancode_from_x11_keycode not implemented!"
#endif
		if (i > 0)
			g_strlcat(rdp_kbd_remap, ",", 512);
		g_strlcat(rdp_kbd_remap, keys, 512);
	}
	XCloseDisplay(display);

	return rdp_kbd_remap;
}
#endif
#endif

static gboolean remmina_rdp_main(RemminaProtocolWidget *gp)
{
	TRACE_CALL(__func__);
	const gchar *s;
	gchar *sm;
	gchar *value;
	const gchar *cs;
	RemminaFile *remminafile;
	rfContext *rfi = GET_PLUGIN_DATA(gp);
	rdpChannels *channels;
	gchar *gateway_host;
	gint gateway_port;
	gchar *datapath = NULL;
	gboolean status = TRUE;
#ifdef GDK_WINDOWING_X11
	gchar *rdp_kbd_remap;
#endif
	gint i;

	gint desktopOrientation, desktopScaleFactor, deviceScaleFactor;

	channels = rfi->clientContext.context.channels;

	remminafile = remmina_plugin_service->protocol_plugin_get_file(gp);

	gchar* ddir = remmina_plugin_service->file_get_user_datadir();
	datapath = g_build_path("/",
				ddir,
				"RDP",
				NULL);
	g_free(ddir);

	REMMINA_PLUGIN_DEBUG("RDP data path is %s", datapath);

	if ((datapath != NULL) && (datapath[0] != '\0'))
		if (access(datapath, W_OK) == 0)
			freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_ConfigPath, datapath);
	g_free(datapath);

	if (remmina_plugin_service->file_get_int(remminafile, "assistance_mode", 0)){
		rdpAssistanceFile* file = freerdp_assistance_file_new();
		if (!file){
			REMMINA_PLUGIN_DEBUG("Could not allocate assistance file structure");
			return FALSE;
		}
		
		if (remmina_plugin_service->file_get_string(remminafile, "assistance_file") == NULL || 
				remmina_plugin_service->file_get_string(remminafile, "assistance_pass") == NULL ){

			REMMINA_PLUGIN_DEBUG("Assistance file and password are not set while assistance mode is on");
			return FALSE;
		}

		status = freerdp_assistance_parse_file(file, 
			remmina_plugin_service->file_get_string(remminafile, "assistance_file"), 
			remmina_plugin_service->file_get_string(remminafile, "assistance_pass"));

		if (status < 0){
			REMMINA_PLUGIN_DEBUG("Could not parse assistance file");
			return FALSE;
		}
			

		if (!freerdp_assistance_populate_settings_from_assistance_file(file, rfi->clientContext.context.settings)){
			REMMINA_PLUGIN_DEBUG("Could not populate settings from assistance file");
			return FALSE;
		}		
	}


#if defined(PROXY_TYPE_IGNORE)
	if (!remmina_plugin_service->file_get_int(remminafile, "useproxyenv", FALSE) ? TRUE : FALSE) {
		REMMINA_PLUGIN_DEBUG("Not using system proxy settings");
		freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_ProxyType, PROXY_TYPE_IGNORE);
	}
#endif

	if (!remmina_rdp_tunnel_init(gp))
		return FALSE;

	freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_AutoReconnectionEnabled, (remmina_plugin_service->file_get_int(remminafile, "disableautoreconnect", FALSE) ? FALSE : TRUE));
	/* Disable RDP auto reconnection when SSH tunnel is enabled */
	if (remmina_plugin_service->file_get_int(remminafile, "ssh_tunnel_enabled", FALSE))
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_AutoReconnectionEnabled, FALSE);

	freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_ColorDepth, remmina_plugin_service->file_get_int(remminafile, "colordepth", 99));

	freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_SoftwareGdi, TRUE);
	REMMINA_PLUGIN_DEBUG("gfx_h264_available: %d", gfx_h264_available);

	/* Avoid using H.264 modes if they are not available on libfreerdp */
	if (!gfx_h264_available &&
	    (freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_ColorDepth) == 65 ||
	     freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_ColorDepth) == 66))
		freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_ColorDepth, 64); // Fallback to GFX RFX

	if (freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_ColorDepth) == 0) {
		/* RFX (Win7)*/
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_RemoteFxCodec, TRUE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_SupportGraphicsPipeline, FALSE);
		freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_ColorDepth, 32);
	} else if (freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_ColorDepth) == 63) {
		/* /gfx (RFX Progressive) (Win8) */
		freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_ColorDepth, 32);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_SupportGraphicsPipeline, TRUE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_GfxH264, FALSE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_GfxAVC444, FALSE);
	} else if (freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_ColorDepth) == 64) {
		/* /gfx:rfx (Win8) */
		freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_ColorDepth, 32);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_RemoteFxCodec, TRUE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_SupportGraphicsPipeline, TRUE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_GfxH264, FALSE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_GfxAVC444, FALSE);
	} else if (freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_ColorDepth) == 65) {
		/* /gfx:avc420 (Win8.1) */
		freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_ColorDepth, 32);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_SupportGraphicsPipeline, TRUE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_GfxH264, gfx_h264_available);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_GfxAVC444, FALSE);
	} else if (freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_ColorDepth) == 66) {
		/* /gfx:avc444 (Win10) */
		freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_ColorDepth, 32);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_SupportGraphicsPipeline, TRUE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_GfxH264, gfx_h264_available);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_GfxAVC444, gfx_h264_available);
	} else if (freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_ColorDepth) == 99) {
		/* Automatic (Let the server choose its best format) */
		freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_ColorDepth, 32);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_RemoteFxCodec, TRUE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_SupportGraphicsPipeline, TRUE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_GfxH264, gfx_h264_available);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_GfxAVC444, gfx_h264_available);
	}

	if (freerdp_settings_get_bool(rfi->clientContext.context.settings, FreeRDP_RemoteFxCodec) ||
		freerdp_settings_get_bool(rfi->clientContext.context.settings, FreeRDP_NSCodec) ||
		freerdp_settings_get_bool(rfi->clientContext.context.settings, FreeRDP_SupportGraphicsPipeline)) {
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_FastPathOutput, TRUE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_FrameMarkerCommandEnabled, TRUE);
		freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_ColorDepth, 32);
		rfi->bpp = 32;
	}

	gint w = remmina_plugin_service->get_profile_remote_width(gp);
	gint h = remmina_plugin_service->get_profile_remote_height(gp);
	/* multiple of 4 */
	w = (w + 3) & ~0x3;
	h = (h + 3) & ~0x3;
	freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_DesktopWidth, w);
	freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_DesktopHeight, h);
	REMMINA_PLUGIN_DEBUG("Resolution set by the user: %dx%d", w, h);

	/* Workaround for FreeRDP issue #5417: in GFX AVC modes we can't go under
	 * AVC_MIN_DESKTOP_WIDTH x AVC_MIN_DESKTOP_HEIGHT */
	if (freerdp_settings_get_bool(rfi->clientContext.context.settings, FreeRDP_SupportGraphicsPipeline) &&
		freerdp_settings_get_bool(rfi->clientContext.context.settings, FreeRDP_GfxH264)) {
		if (freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_DesktopWidth) <
			AVC_MIN_DESKTOP_WIDTH)
			freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_DesktopWidth,
							AVC_MIN_DESKTOP_WIDTH);
		if (freerdp_settings_get_uint32(rfi->clientContext.context.settings,
						FreeRDP_DesktopHeight) <
			AVC_MIN_DESKTOP_HEIGHT)
			freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_DesktopHeight,
							AVC_MIN_DESKTOP_HEIGHT);
	}

	/* Workaround for FreeRDP issue #5119. This will make our horizontal resolution
	 * an even value, but it will add a vertical black 1 pixel line on the
	 * right of the desktop */
	if ((freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_DesktopWidth) & 1) != 0) {
		UINT32 tmp = freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_DesktopWidth);
		freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_DesktopWidth, tmp - 1);
	}

	remmina_plugin_service->protocol_plugin_set_width(gp, freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_DesktopWidth));
	remmina_plugin_service->protocol_plugin_set_height(gp, freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_DesktopHeight));

	w = freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_DesktopWidth);
	h = freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_DesktopHeight);
	REMMINA_PLUGIN_DEBUG("Resolution set after workarounds: %dx%d", w, h);


	if (remmina_plugin_service->file_get_string(remminafile, "username"))
		freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_Username, remmina_plugin_service->file_get_string(remminafile, "username"));

	if (remmina_plugin_service->file_get_string(remminafile, "domain"))
		freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_Domain, remmina_plugin_service->file_get_string(remminafile, "domain"));

	s = remmina_plugin_service->file_get_string(remminafile, "password");
	if (s){
		freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_Password, s);
	} 
	else {
		i = remmina_plugin_service->file_get_int(remminafile, "allow_empty_pass", 0);
		if (i){
			freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_Password, "");
		}
		else{
			freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_Password, s);
		}
		
	}

	freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_AutoLogonEnabled, TRUE);

	/**
	 * Proxy support
	 * Proxy settings are hidden at the moment as an advanced feature
	 */
	gchar *proxy_type = g_strdup(remmina_plugin_service->file_get_string(remminafile, "proxy_type"));
	gchar *proxy_username = g_strdup(remmina_plugin_service->file_get_string(remminafile, "proxy_username"));
	gchar *proxy_password = g_strdup(remmina_plugin_service->file_get_string(remminafile, "proxy_password"));
	gchar *proxy_hostname = g_strdup(remmina_plugin_service->file_get_string(remminafile, "proxy_hostname"));
	gint proxy_port = remmina_plugin_service->file_get_int(remminafile, "proxy_port", 80);
	REMMINA_PLUGIN_DEBUG("proxy_type: %s", proxy_type);
	REMMINA_PLUGIN_DEBUG("proxy_username: %s", proxy_username);
	REMMINA_PLUGIN_DEBUG("proxy_password: %s", proxy_password);
	REMMINA_PLUGIN_DEBUG("proxy_hostname: %s", proxy_hostname);
	REMMINA_PLUGIN_DEBUG("proxy_port: %d", proxy_port);
	if (proxy_type && proxy_hostname) {
		if (g_strcmp0(proxy_type, "no_proxy") == 0)
			freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_ProxyType, PROXY_TYPE_IGNORE);
		else if (g_strcmp0(proxy_type, "http") == 0)
			freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_ProxyType, PROXY_TYPE_HTTP);
		else if (g_strcmp0(proxy_type, "socks5") == 0)
			freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_ProxyType, PROXY_TYPE_SOCKS);
		else
			g_warning("Invalid proxy protocol, at the moment only no_proxy, HTTP and SOCKS5 are supported");
		REMMINA_PLUGIN_DEBUG("ProxyType set to: %" PRIu32, freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_ProxyType));
		freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_ProxyHostname, proxy_hostname);
		if (proxy_username)
			freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_ProxyUsername, proxy_username);
		if (proxy_password)
			freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_ProxyPassword, proxy_password);
		if (proxy_port)
			freerdp_settings_set_uint16(rfi->clientContext.context.settings, FreeRDP_ProxyPort, proxy_port);
	}
	g_free(proxy_hostname);
	g_free(proxy_username);
	g_free(proxy_password);

	if (remmina_plugin_service->file_get_int(remminafile, "base-cred-for-gw", FALSE)) {
		// Reset gateway credentials
		remmina_plugin_service->file_set_string(remminafile, "gateway_username", NULL);
		remmina_plugin_service->file_set_string(remminafile, "gateway_domain", NULL);
		remmina_plugin_service->file_set_string(remminafile, "gateway_password", NULL);
	}

	/* Remote Desktop Gateway server address */
	freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_GatewayEnabled, FALSE);
	s = remmina_plugin_service->file_get_string(remminafile, "gateway_server");
	if (s) {
		cs = remmina_plugin_service->file_get_string(remminafile, "gwtransp");
#if FREERDP_CHECK_VERSION(2, 3, 1)
		if (remmina_plugin_service->file_get_int(remminafile, "websockets", FALSE))
			freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_GatewayHttpUseWebsockets, TRUE);
		else
			freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_GatewayHttpUseWebsockets, FALSE);
#endif
		if (g_strcmp0(cs, "http") == 0) {
			freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_GatewayRpcTransport, FALSE);
			freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_GatewayHttpTransport, TRUE);
		} else if (g_strcmp0(cs, "rpc") == 0) {
			freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_GatewayRpcTransport, TRUE);
			freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_GatewayHttpTransport, FALSE);
		} else if (g_strcmp0(cs, "auto") == 0) {
			freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_GatewayRpcTransport, TRUE);
			freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_GatewayHttpTransport, TRUE);
		}
		remmina_plugin_service->get_server_port(s, 443, &gateway_host, &gateway_port);
		freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_GatewayHostname, gateway_host);
		freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_GatewayPort, gateway_port);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_GatewayEnabled, TRUE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_GatewayUseSameCredentials, TRUE);
	}
	/* Remote Desktop Gateway domain */
	if (remmina_plugin_service->file_get_string(remminafile, "gateway_domain")) {
		freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_GatewayDomain, remmina_plugin_service->file_get_string(remminafile, "gateway_domain"));
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_GatewayUseSameCredentials, FALSE);
	}
	/* Remote Desktop Gateway username */
	if (remmina_plugin_service->file_get_string(remminafile, "gateway_username")) {
		freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_GatewayUsername, remmina_plugin_service->file_get_string(remminafile, "gateway_username"));
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_GatewayUseSameCredentials, FALSE);
	}
	/* Remote Desktop Gateway password */
	s = remmina_plugin_service->file_get_string(remminafile, "gateway_password");
	if (s) {
		freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_GatewayPassword, s);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_GatewayUseSameCredentials, FALSE);
	}
	/* If no different credentials were provided for the Remote Desktop Gateway
	 * use the same authentication credentials for the host */
	if (freerdp_settings_get_bool(rfi->clientContext.context.settings, FreeRDP_GatewayEnabled) && freerdp_settings_get_bool(rfi->clientContext.context.settings, FreeRDP_GatewayUseSameCredentials)) {
		freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_GatewayDomain, freerdp_settings_get_string(rfi->clientContext.context.settings, FreeRDP_Domain));
		freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_GatewayUsername, freerdp_settings_get_string(rfi->clientContext.context.settings, FreeRDP_Username));
		freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_GatewayPassword, freerdp_settings_get_string(rfi->clientContext.context.settings, FreeRDP_Password));
	}
	/* Remote Desktop Gateway usage */
	if (freerdp_settings_get_bool(rfi->clientContext.context.settings, FreeRDP_GatewayEnabled))
		freerdp_set_gateway_usage_method(rfi->clientContext.context.settings,
						 remmina_plugin_service->file_get_int(remminafile, "gateway_usage", FALSE) ? TSC_PROXY_MODE_DETECT : TSC_PROXY_MODE_DIRECT);

	freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_GatewayAccessToken,
					remmina_plugin_service->file_get_string(remminafile, "gatewayaccesstoken"));

	freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_AuthenticationLevel, remmina_plugin_service->file_get_int(
						remminafile, "authentication level", freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_AuthenticationLevel)));

	/* Certificate ignore */
	freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_IgnoreCertificate, remmina_plugin_service->file_get_int(remminafile, "cert_ignore", 0));
	freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_OldLicenseBehaviour, remmina_plugin_service->file_get_int(remminafile, "old-license", 0));
	freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_AllowUnanouncedOrdersFromServer, remmina_plugin_service->file_get_int(remminafile, "relax-order-checks", 0));
	freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_GlyphSupportLevel, (remmina_plugin_service->file_get_int(remminafile, "glyph-cache", 0) ? GLYPH_SUPPORT_FULL : GLYPH_SUPPORT_NONE));

	if ((cs = remmina_plugin_service->file_get_string(remminafile, "clientname")))
		freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_ClientHostname, cs);
	else
		freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_ClientHostname, g_get_host_name());

	/* Client Build number is optional, if not specified defaults to 0, allow for comments to appear after number */
	if ((cs = remmina_plugin_service->file_get_string(remminafile, "clientbuild"))) {
		if (*cs) {
			UINT32 val = strtoul(cs, NULL, 0);
			freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_ClientBuild, val);
		}
	}


	if (remmina_plugin_service->file_get_string(remminafile, "loadbalanceinfo")) {
		const gchar *tmp = strdup(remmina_plugin_service->file_get_string(remminafile, "loadbalanceinfo"));
		freerdp_settings_set_pointer_len(rfi->clientContext.context.settings, FreeRDP_LoadBalanceInfo, tmp, strlen(tmp));
	}

	if (remmina_plugin_service->file_get_string(remminafile, "exec"))
		freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_AlternateShell, remmina_plugin_service->file_get_string(remminafile, "exec"));

	if (remmina_plugin_service->file_get_string(remminafile, "execpath"))
		freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_ShellWorkingDirectory, remmina_plugin_service->file_get_string(remminafile, "execpath"));

	sm = g_strdup_printf("rdp_quality_%i", remmina_plugin_service->file_get_int(remminafile, "quality", DEFAULT_QUALITY_0));
	value = remmina_plugin_service->pref_get_value(sm);
	g_free(sm);

	if (value && value[0]) {
		freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_PerformanceFlags, strtoul(value, NULL, 16));
	} else {
		switch (remmina_plugin_service->file_get_int(remminafile, "quality", DEFAULT_QUALITY_0)) {
		case 9:
			freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_PerformanceFlags, DEFAULT_QUALITY_9);
			break;

		case 2:
			freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_PerformanceFlags, DEFAULT_QUALITY_2);
			break;

		case 1:
			freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_PerformanceFlags, DEFAULT_QUALITY_1);
			break;

		case 0:
		default:
			freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_PerformanceFlags, DEFAULT_QUALITY_0);
			break;
		}
	}
	g_free(value);

	if ((cs = remmina_plugin_service->file_get_string(remminafile, "network"))) {
		guint32 type = 0;

		if (g_strcmp0(cs, "modem") == 0)
			type = CONNECTION_TYPE_MODEM;
		else if (g_strcmp0(cs, "broadband") == 0)
			type = CONNECTION_TYPE_BROADBAND_HIGH;
		else if (g_strcmp0(cs, "broadband-low") == 0)
			type = CONNECTION_TYPE_BROADBAND_LOW;
		else if (g_strcmp0(cs, "broadband-high") == 0)
			type = CONNECTION_TYPE_BROADBAND_HIGH;
		else if (g_strcmp0(cs, "wan") == 0)
			type = CONNECTION_TYPE_WAN;
		else if (g_strcmp0(cs, "lan") == 0)
			type = CONNECTION_TYPE_LAN;
		else if ((g_strcmp0(cs, "autodetect") == 0))
			type = CONNECTION_TYPE_AUTODETECT;
		else if ((g_strcmp0(cs, "none") == 0))
			type = REMMINA_CONNECTION_TYPE_NONE;
		else
			type = REMMINA_CONNECTION_TYPE_NONE;

		if (!remmina_rdp_set_connection_type(rfi->clientContext.context.settings, type))
			REMMINA_PLUGIN_DEBUG("Network settings not set");
	}

	/* PerformanceFlags bitmask need also to be split into BOOL variables
	 * like freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_DisableWallpaper, freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_AllowFontSmoothing…
	 * or freerdp_get_param_bool() function will return the wrong value
	 */
	freerdp_performance_flags_split(rfi->clientContext.context.settings);

#ifdef GDK_WINDOWING_X11
#if FREERDP_CHECK_VERSION(2, 3, 0)
	rdp_kbd_remap = remmina_get_rdp_kbd_remap(remmina_plugin_service->file_get_string(remminafile, "keymap"));
	if (rdp_kbd_remap != NULL) {
		freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_KeyboardRemappingList, rdp_kbd_remap);
				REMMINA_PLUGIN_DEBUG(
					"rdp_keyboard_remapping_list: %s",
					freerdp_settings_get_string(rfi->clientContext.context.settings,
												FreeRDP_KeyboardRemappingList));
				g_free(rdp_kbd_remap);
	}
	else {
		freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_KeyboardRemappingList, remmina_plugin_service->pref_get_value("rdp_kbd_remap"));
				REMMINA_PLUGIN_DEBUG(
					"rdp_keyboard_remapping_list: %s",
					freerdp_settings_get_string(rfi->clientContext.context.settings,
												FreeRDP_KeyboardRemappingList));
		}
#endif
#endif

	freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_KeyboardLayout, remmina_rdp_settings_get_keyboard_layout());

	if (remmina_plugin_service->file_get_int(remminafile, "console", FALSE))
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_ConsoleSession, TRUE);

	if (remmina_plugin_service->file_get_int(remminafile, "restricted-admin", FALSE)) {
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_ConsoleSession, TRUE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_RestrictedAdminModeRequired, TRUE);
	}

	if (remmina_plugin_service->file_get_string(remminafile, "pth")) {
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_ConsoleSession, TRUE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_RestrictedAdminModeRequired, TRUE);
		freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_PasswordHash, remmina_plugin_service->file_get_string(remminafile, "pth"));
		remmina_plugin_service->file_set_int(remminafile, "restricted-admin", TRUE);
	}

	cs = remmina_plugin_service->file_get_string(remminafile, "security");
	if (g_strcmp0(cs, "rdp") == 0) {
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_RdpSecurity, TRUE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_TlsSecurity, FALSE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_NlaSecurity, FALSE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_ExtSecurity, FALSE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_UseRdpSecurityLayer, TRUE);
	} else if (g_strcmp0(cs, "tls") == 0) {
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_RdpSecurity, FALSE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_TlsSecurity, TRUE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_NlaSecurity, FALSE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_ExtSecurity, FALSE);
	} else if (g_strcmp0(cs, "nla") == 0) {
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_RdpSecurity, FALSE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_TlsSecurity, FALSE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_NlaSecurity, TRUE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_ExtSecurity, FALSE);
	} else if (g_strcmp0(cs, "ext") == 0) {
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_RdpSecurity, FALSE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_TlsSecurity, FALSE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_NlaSecurity, FALSE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_ExtSecurity, TRUE);
	} else {
		/* This is "-nego" switch of xfreerdp */
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_NegotiateSecurityLayer, TRUE);
	}

	cs = remmina_plugin_service->file_get_string(remminafile, "tls-seclevel");
	if (cs && g_strcmp0(cs,"")!=0) {
		i = atoi(cs);
		freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_TlsSecLevel, i);
	}

	if (remmina_plugin_service->file_get_int(remminafile, "smartcard-logon", FALSE)) {
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_SmartcardLogon, TRUE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_RedirectSmartCards, TRUE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_PasswordIsSmartcardPin, TRUE);
	}

	freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_CompressionEnabled, TRUE);
	if (remmina_plugin_service->file_get_int(remminafile, "disable_fastpath", FALSE)) {
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_FastPathInput, FALSE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_FastPathOutput, FALSE);
	} else {
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_FastPathInput, TRUE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_FastPathOutput, TRUE);
	}

	/* Orientation and scaling settings */
	remmina_rdp_settings_get_orientation_scale_prefs(&desktopOrientation, &desktopScaleFactor, &deviceScaleFactor);

	freerdp_settings_set_uint16(rfi->clientContext.context.settings, FreeRDP_DesktopOrientation, desktopOrientation);
	if (desktopScaleFactor != 0 && deviceScaleFactor != 0) {
		freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_DesktopScaleFactor, desktopScaleFactor);
		freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_DeviceScaleFactor, deviceScaleFactor);
	}

	/* Try to enable "Display Control Virtual Channel Extension", needed to
	 * dynamically resize remote desktop. This will automatically open
	 * the "disp" dynamic channel, if available */
	freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_SupportDisplayControl, TRUE);

	if (freerdp_settings_get_bool(rfi->clientContext.context.settings, FreeRDP_SupportDisplayControl)) {
		CLPARAM *d[1];
		int dcount;

		dcount = 1;
		d[0] = "disp";
		freerdp_client_add_dynamic_channel(rfi->clientContext.context.settings, dcount, d);
	}

	if (freerdp_settings_get_bool(rfi->clientContext.context.settings, FreeRDP_SupportGraphicsPipeline)) {
		CLPARAM *d[1];

		int dcount;

		dcount = 1;
		d[0] = "rdpgfx";
		freerdp_client_add_dynamic_channel(rfi->clientContext.context.settings, dcount, d);
	}

	/* Sound settings */
	cs = remmina_plugin_service->file_get_string(remminafile, "sound");
	if (g_strcmp0(cs, "remote") == 0) {
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_RemoteConsoleAudio, TRUE);
	} else if ((cs != NULL && cs[0] != '\0') && g_str_has_prefix(cs, "local")) {
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_AudioPlayback, TRUE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_AudioCapture, TRUE);
	} else {
		/* Disable sound */
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_AudioPlayback, FALSE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_RemoteConsoleAudio, FALSE);
	}

	cs = remmina_plugin_service->file_get_string(remminafile, "microphone");
	if (cs != NULL && cs[0] != '\0') {
		if (g_strcmp0(cs, "0") == 0) {
			REMMINA_PLUGIN_DEBUG("“microphone” was set to 0, setting to \"\"");
			remmina_plugin_service->file_set_string(remminafile, "microphone", "");
		} else {
			freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_AudioCapture, TRUE);
			REMMINA_PLUGIN_DEBUG("“microphone” set to “%s”", cs);
			CLPARAM **p;
			size_t count;

			p = remmina_rdp_CommandLineParseCommaSeparatedValuesEx("audin", cs, &count);

			freerdp_client_add_dynamic_channel(rfi->clientContext.context.settings, count, p);
			g_free(p);
		}
	}

	cs = remmina_plugin_service->file_get_string(remminafile, "audio-output");
	if (cs != NULL && cs[0] != '\0') {
		REMMINA_PLUGIN_DEBUG("audio output set to %s", cs);
		CLPARAM **p;
		size_t count;

		p = remmina_rdp_CommandLineParseCommaSeparatedValuesEx("rdpsnd", g_strdup(cs), &count);
		status = freerdp_client_add_static_channel(rfi->clientContext.context.settings, count, p);
		if (status)
			status = freerdp_client_add_dynamic_channel(rfi->clientContext.context.settings, count, p);
		g_free(p);
	}


	cs = remmina_plugin_service->file_get_string(remminafile, "freerdp_log_level");
	if (cs != NULL && cs[0] != '\0')
		REMMINA_PLUGIN_DEBUG("Log level set to to %s", cs);
	else
		cs = "INFO";
	wLog *root = WLog_GetRoot();
	WLog_SetStringLogLevel(root, cs);

	cs = remmina_plugin_service->file_get_string(remminafile, "freerdp_log_filters");
	if (cs != NULL && cs[0] != '\0') {
		REMMINA_PLUGIN_DEBUG("Log filters set to to %s", cs);
		WLog_AddStringLogFilters(cs);
	} else {
		WLog_AddStringLogFilters(NULL);
	}


	cs = remmina_plugin_service->file_get_string(remminafile, "usb");
	if (cs != NULL && cs[0] != '\0') {
		CLPARAM **p;
		size_t count;
		p = remmina_rdp_CommandLineParseCommaSeparatedValuesEx("urbdrc", g_strdup(cs), &count);
		freerdp_client_add_dynamic_channel(rfi->clientContext.context.settings, count, p);
		g_free(p);
	}

	cs = remmina_plugin_service->file_get_string(remminafile, "vc");
	if (cs != NULL && cs[0] != '\0') {
		CLPARAM **p;
		size_t count;
		p = remmina_rdp_CommandLineParseCommaSeparatedValues(g_strdup(cs), &count);
		freerdp_client_add_static_channel(rfi->clientContext.context.settings, count, p);
		g_free(p);
	}

	cs = remmina_plugin_service->file_get_string(remminafile, "dvc");
	if (cs != NULL && cs[0] != '\0') {
		CLPARAM **p;
		size_t count;
		p = remmina_rdp_CommandLineParseCommaSeparatedValues(g_strdup(cs), &count);
		freerdp_client_add_dynamic_channel(rfi->clientContext.context.settings, count, p);
		g_free(p);
	}

	cs = remmina_plugin_service->file_get_string(remminafile, RDP2TCP_DVC_CHANNEL_NAME);
	if (cs != NULL && cs[0] != '\0') {
		freerdp_settings_set_string(rfi->clientContext.context.settings, FreeRDP_RDP2TCPArgs, cs);

#if FREERDP_VERSION_MAJOR >= 3
		char* args = freerdp_settings_get_string_writable(rfi->clientContext.context.settings, FreeRDP_RDP2TCPArgs);
#else
		char* args = freerdp_settings_get_string(rfi->clientContext.context.settings, FreeRDP_RDP2TCPArgs);
#endif
		REMMINA_PLUGIN_DEBUG(RDP2TCP_DVC_CHANNEL_NAME " set to %s",args);
		remmina_rdp_load_static_channel_addin(channels, rfi->clientContext.context.settings, RDP2TCP_DVC_CHANNEL_NAME, args);
	}

	int vermaj, vermin, verrev;
	freerdp_get_version(&vermaj, &vermin, &verrev);

#if FREERDP_CHECK_VERSION(2, 1, 0)
	cs = remmina_plugin_service->file_get_string(remminafile, "timeout");
	if (cs != NULL && cs[0] != '\0') {
		const gchar *endptr = NULL;
		guint64 val = g_ascii_strtoull(cs, (gchar **)&endptr, 10);
		if (val > 600000 || val <= 0)
			val = 600000;
		freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_TcpAckTimeout, (UINT32)val);
	}
#endif

#if FREERDP_CHECK_VERSION(3, 6, 2)
	if (remmina_plugin_service->file_get_int(remminafile, "preferipv6", FALSE) ? TRUE : FALSE){
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_PreferIPv6OverIPv4, TRUE);
	        if (remmina_plugin_service->file_get_int(remminafile, "forceipvx", FALSE) ? TRUE : FALSE)
			freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_ForceIPvX, 6);
	}else if (remmina_plugin_service->file_get_int(remminafile, "forceipvx", FALSE) ? TRUE : FALSE)
                freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_ForceIPvX, 4);
#else
	if (remmina_plugin_service->file_get_int(remminafile, "preferipv6", FALSE) ? TRUE : FALSE)
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_PreferIPv6OverIPv4, TRUE);
#endif

	freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_RedirectClipboard, remmina_plugin_service->file_get_int(remminafile, "disableclipboard", FALSE) ? FALSE : TRUE);

	cs = remmina_plugin_service->file_get_string(remminafile, "sharefolder");
	if (cs != NULL && cs[0] != '\0') {
		gchar *ccs = g_strdup(cs);
		REMMINA_PLUGIN_DEBUG("[Deprecated->migrating] - Old sharefolder %s to \"drive \"", ccs);
		if (!remmina_plugin_service->file_get_string(remminafile, "drive")) {
			remmina_plugin_service->file_set_string(remminafile, "drive", g_strdup(ccs));
			remmina_plugin_service->file_set_string(remminafile, "sharefolder", NULL);
			REMMINA_PLUGIN_DEBUG("[Deprecated->migrated] - drive set to %s", g_strdup(ccs));
		}
		g_free(ccs);
	}
	cs = remmina_plugin_service->file_get_string(remminafile, "drive");
	if (cs != NULL && cs[0] != '\0') {
		REMMINA_PLUGIN_DEBUG("Redirect directory set to %s", cs);
		CLPARAM **p;
		size_t count;

		gchar **folders = g_strsplit(cs, ";", -1);
		for (i = 0; folders[i] != NULL; i++) {
			REMMINA_PLUGIN_DEBUG("Parsing folder %s", folders[i]);
			p = remmina_rdp_CommandLineParseCommaSeparatedValuesEx("drive", g_strdup(folders[i]), &count);
			status = freerdp_client_add_device_channel(rfi->clientContext.context.settings, count, p);
			g_free(p);
		}
		g_strfreev(folders);
	}

	if (remmina_plugin_service->file_get_int(remminafile, "shareprinter", FALSE)) {
#ifdef HAVE_CUPS
		REMMINA_PLUGIN_DEBUG("Sharing printers");
		const gchar *po = remmina_plugin_service->file_get_string(remminafile, "printer_overrides");
		if (po && po[0] != 0) {
			/* Fallback to remmina code to override print drivers */
			if (cupsEnumDests(CUPS_DEST_FLAGS_NONE, 1000, NULL, 0, 0, remmina_rdp_set_printers, rfi))
				REMMINA_PLUGIN_DEBUG("All printers have been shared");
			else
				REMMINA_PLUGIN_DEBUG("Cannot share printers, are there any available?");
		} else {
			/* Use libfreerdp code to map all printers */
			CLPARAM *d[1];
			int dcount;
			dcount = 1;
			d[0] = "printer";
			freerdp_client_add_device_channel(rfi->clientContext.context.settings, dcount, d);
		}
#endif /* HAVE_CUPS */
	}

	if (remmina_plugin_service->file_get_int(remminafile, "span", FALSE)) {
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_SpanMonitors, TRUE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_UseMultimon, TRUE);
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_Fullscreen, TRUE);
		remmina_plugin_service->file_set_int(remminafile, "multimon", 1);
	}

	if (remmina_plugin_service->file_get_int(remminafile, "multimon", FALSE)) {
		guint32 maxwidth = 0;
		guint32 maxheight = 0;
		gchar *monitorids;
		guint32 i;
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_UseMultimon, TRUE);
		if (remmina_plugin_service->file_get_int(remminafile, "force_multimon", FALSE)) {
			freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_ForceMultimon, TRUE);
		}	
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_Fullscreen, TRUE);

		gchar *monitorids_string = g_strdup(remmina_plugin_service->file_get_string(remminafile, "monitorids"));
		/* Otherwise we get all the attached monitors
		 * monitorids may contains desktop orientation values.
		 * But before we check if there are orientation attributes
		 */
		if (monitorids_string != NULL && monitorids_string[0] != '\0') {
			if (g_strstr_len(monitorids_string, -1, ",") != NULL) {
				if (g_strstr_len(monitorids_string, -1, ":") != NULL) {
					rdpMonitor *base = (rdpMonitor *)freerdp_settings_get_pointer(rfi->clientContext.context.settings, FreeRDP_MonitorDefArray);
					/* We have an ID and an orientation degree */
					gchar **temp_items;
					gchar **rot_items;
					temp_items = g_strsplit(monitorids_string, ",", -1);
					for (i = 0; i < g_strv_length(temp_items); i++) {
						rot_items = g_strsplit(temp_items[i], ":", -1);
						rdpMonitor *current = &base[atoi(rot_items[0])];
						if (i == 0)
							monitorids = g_strdup(rot_items[0]);
						else
							monitorids = g_strdup_printf("%s,%s", monitorids, rot_items[0]);
						current->attributes.orientation = atoi(rot_items[1]);
						REMMINA_PLUGIN_DEBUG("Monitor n %d orientation: %d", i, current->attributes.orientation);
					}
				} else {
					monitorids = g_strdup(monitorids_string);
				}
			} else {
				monitorids = g_strdup(monitorids_string);
			}
		} else {
			monitorids = g_strdup(monitorids_string);
		}
		remmina_rdp_monitor_get(rfi, &monitorids, &maxwidth, &maxheight);
		if (monitorids != NULL && monitorids[0] != '\0') {
			UINT32 *base = (UINT32 *)freerdp_settings_get_pointer(rfi->clientContext.context.settings, FreeRDP_MonitorIds);
			gchar **items;
			items = g_strsplit(monitorids, ",", -1);
			freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_NumMonitorIds, g_strv_length(items));
			REMMINA_PLUGIN_DEBUG("NumMonitorIds: %d", freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_NumMonitorIds));
			for (i = 0; i < g_strv_length(items); i++) {
				if (base != NULL){
					UINT32 *current = &base[i];
					*current = atoi(items[i]);
					REMMINA_PLUGIN_DEBUG("Added monitor with ID %" PRIu32, *current);
				}
			}
			g_free(monitorids);
			g_strfreev(items);
		}
		if (maxwidth && maxheight && remmina_plugin_service->file_get_int(remminafile, "multimon", FALSE)) {
			REMMINA_PLUGIN_DEBUG("Setting DesktopWidth and DesktopHeight to: %dx%d", maxwidth, maxheight);
			freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_DesktopWidth, maxwidth);
			freerdp_settings_set_uint32(rfi->clientContext.context.settings, FreeRDP_DesktopHeight, maxheight);
			REMMINA_PLUGIN_DEBUG("DesktopWidth and DesktopHeight set to: %dx%d", freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_DesktopWidth), freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_DesktopHeight));
		} else {
			REMMINA_PLUGIN_DEBUG("Cannot set Desktop Size, we are using the previously set values: %dx%d", freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_DesktopWidth), freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_DesktopHeight));
		}
		remmina_plugin_service->protocol_plugin_set_width(gp, freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_DesktopWidth));
		remmina_plugin_service->protocol_plugin_set_height(gp, freerdp_settings_get_uint32(rfi->clientContext.context.settings, FreeRDP_DesktopHeight));
	}

	const gchar *sn = remmina_plugin_service->file_get_string(remminafile, "smartcardname");
	if (remmina_plugin_service->file_get_int(remminafile, "sharesmartcard", FALSE) ||
			(sn != NULL && sn[0] != '\0')) {
		/* Devices must be allocated with freerdp_device_new(): FreeRDP clones
		 * them internally (freerdp_settings_clone -> freerdp_device_clone) and
		 * a plain calloc()'d RDPDR_SMARTCARD is too small for that, causing a
		 * heap out-of-bounds read and a crash in freerdp_connect(). */
		RDPDR_DEVICE *sdev;
		if (sn != NULL && sn[0] != '\0') {
			const char *args[] = { "smartcard", sn };
			sdev = freerdp_device_new(RDPDR_DTYP_SMARTCARD, 2, args);
		} else {
			const char *args[] = { "smartcard" };
			sdev = freerdp_device_new(RDPDR_DTYP_SMARTCARD, 1, args);
		}

		if (!sdev) {
			REMMINA_PLUGIN_DEBUG("Could not allocate smartcard device");
		} else {
			freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_DeviceRedirection, TRUE);
			freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_RedirectSmartCards, TRUE);

			if (!freerdp_device_collection_add(rfi->clientContext.context.settings, sdev)) {
				REMMINA_PLUGIN_DEBUG("Could not add smartcard device");
				freerdp_device_free(sdev);
			}
		}
	}

	if (remmina_plugin_service->file_get_int(remminafile, "passwordispin", FALSE))
		/* Option works only combined with Username and Domain, because FreeRDP
		 * doesn’t know anything about info on smart card */
		freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_PasswordIsSmartcardPin, TRUE);

	/* /serial[:<name>[,<path>[,<driver>[,permissive]]]] */
	if (remmina_plugin_service->file_get_int(remminafile, "shareserial", FALSE)) {
		/* See the smartcard block above: devices must be allocated with
		 * freerdp_device_new() so that FreeRDP can safely clone them in
		 * freerdp_settings_clone(). */
		const gchar *sn = remmina_plugin_service->file_get_string(remminafile, "serialname");
		const gchar *sd = remmina_plugin_service->file_get_string(remminafile, "serialdriver");
		const gchar *sp = remmina_plugin_service->file_get_string(remminafile, "serialpath");
		const gboolean permissive = remmina_plugin_service->file_get_int(remminafile, "serialpermissive", FALSE);

		const char *args[5];
		size_t count = 0;
		args[count++] = "serial";
		args[count++] = (sn != NULL && sn[0] != '\0') ? sn : "";
		args[count++] = (sp != NULL && sp[0] != '\0') ? sp : "";
		args[count++] = (sd != NULL && sd[0] != '\0') ? sd : "";
		if (permissive)
			args[count++] = "permissive";

		RDPDR_DEVICE *sdev = freerdp_device_new(RDPDR_DTYP_SERIAL, count, args);
		if (!sdev) {
			REMMINA_PLUGIN_DEBUG("Could not allocate serial device");
		} else {
			freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_DeviceRedirection, TRUE);
			freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP_RedirectSerialPorts, TRUE);

			if (!freerdp_device_collection_add(rfi->clientContext.context.settings, sdev)) {
				REMMINA_PLUGIN_DEBUG("Could not add serial device");
				freerdp_device_free(sdev);
			}
		}
	}

	if (remmina_plugin_service->file_get_int(remminafile, "shareparallel", FALSE)) {
		/* See the smartcard block above: devices must be allocated with
		 * freerdp_device_new() so that FreeRDP can safely clone them in
		 * freerdp_settings_clone(). */
		const gchar *pn = remmina_plugin_service->file_get_string(remminafile, "parallelname");
		const gchar *dp = remmina_plugin_service->file_get_string(remminafile, "parallelpath");

		const char *args[3];
		size_t count = 0;
		args[count++] = "parallel";
		args[count++] = (pn != NULL && pn[0] != '\0') ? pn : "";
		args[count++] = (dp != NULL && dp[0] != '\0') ? dp : "";

		RDPDR_DEVICE *pdev = freerdp_device_new(RDPDR_DTYP_PARALLEL, count, args);
		if (!pdev) {
			REMMINA_PLUGIN_DEBUG("Could not allocate parallel device");
		} else {
			freerdp_settings_set_bool(rfi->clientContext.context.settings, FreeRDP
