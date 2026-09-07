#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define GDK_PIXBUF_ENABLE_BACKEND
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk-pixbuf/gdk-pixbuf-io.h>
#include <string.h>
#include "eom-pixbuf-util.h"

#ifdef HAVE_WEBP
static char *webp_mime_types[] = { "image/webp", NULL };
static char *webp_extensions[] = { "webp", NULL };

static GdkPixbufFormat webp_format = {
	.name = "webp",
	.signature = NULL,
	.domain = NULL,
	.description = "WebP",
	.mime_types = webp_mime_types,
	.extensions = webp_extensions,
	.flags = GDK_PIXBUF_FORMAT_WRITABLE | GDK_PIXBUF_FORMAT_THREADSAFE,
	.disabled = FALSE,
	.license = "LGPL"
};

GdkPixbufFormat*
eom_pixbuf_get_webp_format (void)
{
	return &webp_format;
}
#endif

GSList*
eom_pixbuf_get_savable_formats (void)
{
	GSList *list;
	GSList *write_list = NULL;
	GSList *it;

	list = gdk_pixbuf_get_formats ();

	for (it = list; it != NULL; it = it->next) {
		GdkPixbufFormat *format;

		format = (GdkPixbufFormat*) it->data;
		if (gdk_pixbuf_format_is_writable (format)) {
			write_list = g_slist_prepend (write_list, format);
		}
	}

	g_slist_free (list);

#ifdef HAVE_WEBP
	gboolean has_webp = FALSE;
	for (it = write_list; it != NULL; it = it->next) {
		gchar *name = gdk_pixbuf_format_get_name ((GdkPixbufFormat*) it->data);
		if (name && g_ascii_strcasecmp (name, "webp") == 0) {
			has_webp = TRUE;
			g_free (name);
			break;
		}
		g_free (name);
	}
	if (!has_webp) {
		write_list = g_slist_prepend (write_list, eom_pixbuf_get_webp_format ());
	}
#endif

	write_list = g_slist_reverse (write_list);

	return write_list;
}

GdkPixbufFormat*
eom_pixbuf_get_format_by_suffix (const char *suffix)
{
	GSList *list;
	GSList *it;
	GdkPixbufFormat *result = NULL;

	g_return_val_if_fail (suffix != NULL, NULL);

	list = gdk_pixbuf_get_formats ();

	for (it = list; (it != NULL) && (result == NULL); it = it->next) {
		GdkPixbufFormat *format;
		gchar **extensions;
		int i;

		format = (GdkPixbufFormat*) it->data;

		extensions = gdk_pixbuf_format_get_extensions (format);
		for (i = 0; extensions[i] != NULL; i++) {
			/* g_print ("check extension: %s against %s\n", extensions[i], suffix); */
			if (g_ascii_strcasecmp (suffix, extensions[i]) == 0) {
				result = format;
				break;
			}
		}

		g_strfreev (extensions);
	}

	g_slist_free (list);

#ifdef HAVE_WEBP
	if (result == NULL && g_ascii_strcasecmp (suffix, "webp") == 0) {
		result = eom_pixbuf_get_webp_format ();
	}
#endif

	return result;
}

char*
eom_pixbuf_get_common_suffix (GdkPixbufFormat *format)
{
	char **extensions;
	int i;
	char *result = NULL;

	if (format == NULL) return NULL;

	extensions = gdk_pixbuf_format_get_extensions (format);
	if (extensions[0] == NULL) return NULL;

	/* try to find 3-char suffix first, use the last occurence */
	for (i = 0; extensions [i] != NULL; i++) {
		if (strlen (extensions[i]) <= 3) {
			g_free (result);
			result = g_ascii_strdown (extensions[i], -1);
		}
	}

	/* otherwise take the first one */
	if (result == NULL) {
		result = g_ascii_strdown (extensions[0], -1);
	}

	g_strfreev (extensions);

	return result;
}

static char*
get_suffix_from_basename (const char *basename)
{
	char *suffix;
	char *suffix_start;
	guint len;

	/* FIXME: does this work for all locales? */
	suffix_start = g_utf8_strrchr (basename, -1, '.');

	if (suffix_start == NULL)
		return NULL;

	len = strlen (suffix_start) - 1;
	suffix = g_strndup (suffix_start+1, len);

	return suffix;

}

GdkPixbufFormat *
eom_pixbuf_get_format (GFile *file)
{
	GdkPixbufFormat *format;
	char *basename, *suffix;
	g_return_val_if_fail (file != NULL, NULL);

	basename = g_file_get_basename (file);
	suffix = get_suffix_from_basename (basename);

	format = eom_pixbuf_get_format_by_suffix (suffix);

	g_free (basename);
	g_free (suffix);

	return format;
}

