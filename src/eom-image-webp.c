/* Eye Of MATE - WebP image support
 *
 * Copyright (C) 2026 Perberos
 *
 * Author: Perberos
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "config.h"

#ifdef HAVE_WEBP

#define GDK_PIXBUF_ENABLE_BACKEND
#define GLIB_DISABLE_DEPRECATION_WARNINGS

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk-pixbuf/gdk-pixbuf-animation.h>

#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>
#include <string.h>
#include <errno.h>

#include "eom-image-webp.h"
#include "eom-image-private.h"
#include "eom-transform.h"

/* Callback used to free the WebP-decoded pixel buffer when the GdkPixbuf
 * is finalized. */
static void
webp_pixels_free (guchar *pixels, gpointer data)
{
	WebPFree (pixels);
}

/* Callback for pixel buffers copied from an animated frame (malloc'd by us). */
static void
g_free_pixels (guchar *pixels, gpointer data)
{
	g_free (pixels);
}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS

/* -------------------------------------------------------------------------
 * EomWebpAnim / EomWebpAnimIter implementation
 * ------------------------------------------------------------------------- */

typedef struct _EomWebpFrame EomWebpFrame;
struct _EomWebpFrame {
	GdkPixbuf *pixbuf;
	gint       delay_time; /* in milliseconds */
	gint       elapsed;    /* cumulative ms before this frame */
};

typedef struct _EomWebpAnim EomWebpAnim;
typedef struct _EomWebpAnimClass EomWebpAnimClass;

#define EOM_TYPE_WEBP_ANIM (eom_webp_anim_get_type ())
#define EOM_WEBP_ANIM(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), EOM_TYPE_WEBP_ANIM, EomWebpAnim))
#define EOM_IS_WEBP_ANIM(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), EOM_TYPE_WEBP_ANIM))

struct _EomWebpAnim {
	GdkPixbufAnimation parent_instance;

	gint     width;
	gint     height;
	gint     total_time;
	gboolean loop;
	GList   *frames; /* list of EomWebpFrame* */
};

struct _EomWebpAnimClass {
	GdkPixbufAnimationClass parent_class;
};

GType eom_webp_anim_get_type (void) G_GNUC_CONST;

typedef struct _EomWebpAnimIter EomWebpAnimIter;
typedef struct _EomWebpAnimIterClass EomWebpAnimIterClass;

#define EOM_TYPE_WEBP_ANIM_ITER (eom_webp_anim_iter_get_type ())
#define EOM_WEBP_ANIM_ITER(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), EOM_TYPE_WEBP_ANIM_ITER, EomWebpAnimIter))
#define EOM_IS_WEBP_ANIM_ITER(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), EOM_TYPE_WEBP_ANIM_ITER))

struct _EomWebpAnimIter {
	GdkPixbufAnimationIter parent_instance;

	EomWebpAnim *anim;
	GTimeVal     start_time;
	GTimeVal     current_time;
	gint         position;
	GList       *current_frame;
};

struct _EomWebpAnimIterClass {
	GdkPixbufAnimationIterClass parent_class;
};

GType eom_webp_anim_iter_get_type (void) G_GNUC_CONST;

/* EomWebpAnim methods */
static void                    eom_webp_anim_finalize         (GObject            *object);
static gboolean                eom_webp_anim_is_static_image  (GdkPixbufAnimation *animation);
static GdkPixbuf              *eom_webp_anim_get_static_image (GdkPixbufAnimation *animation);
static void                    eom_webp_anim_get_size         (GdkPixbufAnimation *animation,
                                                               int                *width,
                                                               int                *height);
static GdkPixbufAnimationIter *eom_webp_anim_get_iter         (GdkPixbufAnimation *animation,
                                                               const GTimeVal     *start_time);

G_DEFINE_TYPE (EomWebpAnim, eom_webp_anim, GDK_TYPE_PIXBUF_ANIMATION)

static void
eom_webp_anim_init (EomWebpAnim *anim)
{
}

static void
eom_webp_anim_class_init (EomWebpAnimClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GdkPixbufAnimationClass *anim_class = GDK_PIXBUF_ANIMATION_CLASS (klass);

	object_class->finalize = eom_webp_anim_finalize;

	anim_class->is_static_image = eom_webp_anim_is_static_image;
	anim_class->get_static_image = eom_webp_anim_get_static_image;
	anim_class->get_size = eom_webp_anim_get_size;
	anim_class->get_iter = eom_webp_anim_get_iter;
}

static void
eom_webp_anim_finalize (GObject *object)
{
	EomWebpAnim *anim = EOM_WEBP_ANIM (object);
	GList *l;

	for (l = anim->frames; l != NULL; l = l->next) {
		EomWebpFrame *frame = (EomWebpFrame *) l->data;
		if (frame->pixbuf)
			g_object_unref (frame->pixbuf);
		g_free (frame);
	}
	g_list_free (anim->frames);
	anim->frames = NULL;

	G_OBJECT_CLASS (eom_webp_anim_parent_class)->finalize (object);
}

static gboolean
eom_webp_anim_is_static_image (GdkPixbufAnimation *animation)
{
	EomWebpAnim *anim = EOM_WEBP_ANIM (animation);
	return (anim->frames != NULL && anim->frames->next == NULL);
}

static GdkPixbuf *
eom_webp_anim_get_static_image (GdkPixbufAnimation *animation)
{
	EomWebpAnim *anim = EOM_WEBP_ANIM (animation);
	if (anim->frames != NULL) {
		EomWebpFrame *frame = (EomWebpFrame *) anim->frames->data;
		return frame->pixbuf;
	}
	return NULL;
}

static void
eom_webp_anim_get_size (GdkPixbufAnimation *animation,
                        int                *width,
                        int                *height)
{
	EomWebpAnim *anim = EOM_WEBP_ANIM (animation);
	if (width)
		*width = anim->width;
	if (height)
		*height = anim->height;
}

static GdkPixbufAnimationIter *
eom_webp_anim_get_iter (GdkPixbufAnimation *animation,
                        const GTimeVal     *start_time)
{
	EomWebpAnim *anim = EOM_WEBP_ANIM (animation);
	EomWebpAnimIter *iter = g_object_new (EOM_TYPE_WEBP_ANIM_ITER, NULL);

	iter->anim = g_object_ref (anim);
	iter->current_frame = anim->frames;
	if (start_time) {
		iter->start_time = *start_time;
		iter->current_time = *start_time;
	} else {
		g_get_current_time (&iter->start_time);
		iter->current_time = iter->start_time;
	}
	iter->position = 0;

	return GDK_PIXBUF_ANIMATION_ITER (iter);
}

/* EomWebpAnimIter methods */
static void                    eom_webp_anim_iter_finalize               (GObject                *object);
static int                     eom_webp_anim_iter_get_delay_time         (GdkPixbufAnimationIter *anim_iter);
static GdkPixbuf              *eom_webp_anim_iter_get_pixbuf             (GdkPixbufAnimationIter *anim_iter);
static gboolean                eom_webp_anim_iter_on_currently_loading_frame (GdkPixbufAnimationIter *anim_iter);
static gboolean                eom_webp_anim_iter_advance                (GdkPixbufAnimationIter *anim_iter,
                                                                          const GTimeVal         *current_time);

G_DEFINE_TYPE (EomWebpAnimIter, eom_webp_anim_iter, GDK_TYPE_PIXBUF_ANIMATION_ITER)

static void
eom_webp_anim_iter_init (EomWebpAnimIter *iter)
{
}

static void
eom_webp_anim_iter_class_init (EomWebpAnimIterClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GdkPixbufAnimationIterClass *anim_iter_class = GDK_PIXBUF_ANIMATION_ITER_CLASS (klass);

	object_class->finalize = eom_webp_anim_iter_finalize;

	anim_iter_class->get_delay_time = eom_webp_anim_iter_get_delay_time;
	anim_iter_class->get_pixbuf = eom_webp_anim_iter_get_pixbuf;
	anim_iter_class->on_currently_loading_frame = eom_webp_anim_iter_on_currently_loading_frame;
	anim_iter_class->advance = eom_webp_anim_iter_advance;
}

static void
eom_webp_anim_iter_finalize (GObject *object)
{
	EomWebpAnimIter *iter = EOM_WEBP_ANIM_ITER (object);

	if (iter->anim) {
		g_object_unref (iter->anim);
		iter->anim = NULL;
	}

	G_OBJECT_CLASS (eom_webp_anim_iter_parent_class)->finalize (object);
}

static int
eom_webp_anim_iter_get_delay_time (GdkPixbufAnimationIter *anim_iter)
{
	EomWebpAnimIter *iter = EOM_WEBP_ANIM_ITER (anim_iter);

	if (iter->current_frame) {
		EomWebpFrame *frame = (EomWebpFrame *) iter->current_frame->data;
		gint delay = frame->delay_time - (iter->position - frame->elapsed);
		if (delay <= 0)
			return 10;
		return delay;
	}
	return -1;
}

static GdkPixbuf *
eom_webp_anim_iter_get_pixbuf (GdkPixbufAnimationIter *anim_iter)
{
	EomWebpAnimIter *iter = EOM_WEBP_ANIM_ITER (anim_iter);

	if (iter->current_frame) {
		EomWebpFrame *frame = (EomWebpFrame *) iter->current_frame->data;
		return frame->pixbuf;
	} else if (iter->anim && iter->anim->frames) {
		EomWebpFrame *frame = (EomWebpFrame *) iter->anim->frames->data;
		return frame->pixbuf;
	}
	return NULL;
}

static gboolean
eom_webp_anim_iter_on_currently_loading_frame (GdkPixbufAnimationIter *anim_iter)
{
	return FALSE;
}

static gboolean
eom_webp_anim_iter_advance (GdkPixbufAnimationIter *anim_iter,
                            const GTimeVal         *current_time)
{
	EomWebpAnimIter *iter = EOM_WEBP_ANIM_ITER (anim_iter);
	gint elapsed;
	gint loop_count;
	GList *tmp;
	GList *old;

	if (!iter->anim || iter->anim->total_time <= 0 || !iter->anim->frames)
		return FALSE;

	if (current_time)
		iter->current_time = *current_time;
	else
		g_get_current_time (&iter->current_time);

	elapsed = (((iter->current_time.tv_sec - iter->start_time.tv_sec) * G_USEC_PER_SEC +
	            iter->current_time.tv_usec - iter->start_time.tv_usec)) / 1000;

	if (elapsed < 0) {
		iter->start_time = iter->current_time;
		elapsed = 0;
	}

	loop_count = elapsed / iter->anim->total_time;
	elapsed = elapsed % iter->anim->total_time;
	iter->position = elapsed;

	if (loop_count < 1 || iter->anim->loop)
		tmp = iter->anim->frames;
	else
		tmp = NULL;

	while (tmp != NULL) {
		EomWebpFrame *frame = (EomWebpFrame *) tmp->data;
		if (iter->position >= frame->elapsed &&
		    iter->position < (frame->elapsed + frame->delay_time))
			break;
		tmp = tmp->next;
	}

	old = iter->current_frame;

	if (tmp != NULL) {
		iter->current_frame = tmp;
	} else if (iter->anim->loop && iter->anim->frames) {
		iter->current_frame = iter->anim->frames;
	}

	/* Fallback: if timer precision or rounding keeps current_frame identical,
	 * advance to next frame so EOM's advance loop does not spin. */
	if (iter->current_frame == old && iter->anim->frames && iter->anim->frames->next) {
		if (old && old->next)
			iter->current_frame = old->next;
		else
			iter->current_frame = iter->anim->frames;

		if (iter->current_frame) {
			EomWebpFrame *frame = (EomWebpFrame *) iter->current_frame->data;
			iter->position = frame->elapsed;
			iter->start_time.tv_sec = iter->current_time.tv_sec - (frame->elapsed / 1000);
			iter->start_time.tv_usec = iter->current_time.tv_usec - ((frame->elapsed % 1000) * 1000);
		}
	}

	return (iter->current_frame != old);
}

/* -------------------------------------------------------------------------
 * WebP Decoding helpers
 * ------------------------------------------------------------------------- */

/**
 * load_static_webp:
 *
 * Decode a non-animated WebP using WebPDecodeRGB(A). Returns a new
 * GdkPixbuf on success, or %NULL with @error set on failure.
 */
static GdkPixbuf *
load_static_webp (const uint8_t *data, gsize data_size,
                  gboolean has_alpha, GError **error)
{
	int      width = 0, height = 0;
	int      stride;
	uint8_t *pixels = NULL;

	if (has_alpha) {
		pixels = WebPDecodeRGBA (data, data_size, &width, &height);
		stride = width * 4;
	} else {
		pixels = WebPDecodeRGB (data, data_size, &width, &height);
		stride = width * 3;
	}

	if (pixels == NULL || width <= 0 || height <= 0) {
		g_set_error_literal (error,
		                     GDK_PIXBUF_ERROR,
		                     GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
		                     _("Failed to decode WebP image."));
		if (pixels)
			WebPFree (pixels);
		return NULL;
	}

	GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data (pixels,
	                                               GDK_COLORSPACE_RGB,
	                                               has_alpha,
	                                               8,
	                                               width, height,
	                                               stride,
	                                               webp_pixels_free,
	                                               NULL);
	if (pixbuf == NULL) {
		g_set_error_literal (error,
		                     GDK_PIXBUF_ERROR,
		                     GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY,
		                     _("Failed to create pixbuf for WebP image."));
		WebPFree (pixels);
	}

	return pixbuf;
}

/**
 * load_animated_webp:
 *
 * Decodes all frames of an animated WebP using WebPAnimDecoder.
 * Returns a new #GdkPixbufAnimation on success, or %NULL with @error set on failure.
 */
static GdkPixbufAnimation *
load_animated_webp (const uint8_t *data, gsize data_size, GError **error)
{
	WebPAnimDecoderOptions dec_options;
	WebPAnimDecoder       *dec = NULL;
	WebPAnimInfo           anim_info;
	WebPData               webp_data;
	GList                 *frames = NULL;
	int                    prev_timestamp = 0;
	int                    total_time = 0;
	int                    canvas_width;
	int                    canvas_height;
	int                    stride;

	WebPAnimDecoderOptionsInit (&dec_options);
	dec_options.color_mode = MODE_RGBA;
	dec_options.use_threads = 1;

	webp_data.bytes = data;
	webp_data.size  = data_size;

	dec = WebPAnimDecoderNew (&webp_data, &dec_options);
	if (dec == NULL) {
		g_set_error_literal (error,
		                     GDK_PIXBUF_ERROR,
		                     GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
		                     _("Failed to create WebP animation decoder."));
		return NULL;
	}

	if (!WebPAnimDecoderGetInfo (dec, &anim_info)) {
		g_set_error_literal (error,
		                     GDK_PIXBUF_ERROR,
		                     GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
		                     _("Failed to get WebP animation info."));
		WebPAnimDecoderDelete (dec);
		return NULL;
	}

	canvas_width = (int) anim_info.canvas_width;
	canvas_height = (int) anim_info.canvas_height;
	stride = canvas_width * 4;

	while (WebPAnimDecoderHasMoreFrames (dec)) {
		uint8_t      *frame_rgba = NULL;
		int           timestamp_ms = 0;
		int           duration;
		guchar       *pixels_copy;
		GdkPixbuf    *pixbuf;
		EomWebpFrame *frame;

		if (!WebPAnimDecoderGetNext (dec, &frame_rgba, &timestamp_ms))
			break;

		duration = timestamp_ms - prev_timestamp;
		if (duration <= 0)
			duration = 100;
		prev_timestamp = timestamp_ms;

		pixels_copy = g_memdup2 (frame_rgba, (gsize) stride * (gsize) canvas_height);
		if (pixels_copy == NULL) {
			g_set_error_literal (error,
			                     GDK_PIXBUF_ERROR,
			                     GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY,
			                     _("Failed to allocate memory for WebP animation frame."));
			break;
		}

		pixbuf = gdk_pixbuf_new_from_data (pixels_copy,
		                                   GDK_COLORSPACE_RGB,
		                                   TRUE,
		                                   8,
		                                   canvas_width,
		                                   canvas_height,
		                                   stride,
		                                   g_free_pixels,
		                                   NULL);
		if (pixbuf == NULL) {
			g_free (pixels_copy);
			g_set_error_literal (error,
			                     GDK_PIXBUF_ERROR,
			                     GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY,
			                     _("Failed to create pixbuf for WebP animation frame."));
			break;
		}

		frame = g_new0 (EomWebpFrame, 1);
		frame->pixbuf = pixbuf;
		frame->delay_time = duration;
		frame->elapsed = total_time;
		total_time += duration;

		frames = g_list_prepend (frames, frame);
	}

	WebPAnimDecoderDelete (dec);

	if (frames == NULL) {
		if (error && *error == NULL) {
			g_set_error_literal (error,
			                     GDK_PIXBUF_ERROR,
			                     GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
			                     _("Failed to decode any frames from WebP animation."));
		}
		return NULL;
	}

	frames = g_list_reverse (frames);

	EomWebpAnim *anim = g_object_new (EOM_TYPE_WEBP_ANIM, NULL);
	anim->width = canvas_width;
	anim->height = canvas_height;
	anim->total_time = total_time;
	anim->loop = TRUE;
	anim->frames = frames;

	return GDK_PIXBUF_ANIMATION (anim);
}

/**
 * eom_image_webp_load_animation:
 * @file: a #GFile pointing to a WebP image
 * @error: return location for a #GError, or %NULL
 *
 * Loads a WebP file (static or animated) into a #GdkPixbufAnimation.
 *
 * Returns: (transfer full): a new #GdkPixbufAnimation, or %NULL on error.
 */
GdkPixbufAnimation *
eom_image_webp_load_animation (GFile *file, GError **error)
{
	gchar                  *data = NULL;
	gsize                   data_size = 0;
	GdkPixbufAnimation     *anim = NULL;
	WebPBitstreamFeatures   features;
	VP8StatusCode           status;

	g_return_val_if_fail (G_IS_FILE (file), NULL);

	if (!g_file_load_contents (file, NULL, &data, &data_size, NULL, error)) {
		return NULL;
	}

	if (data_size == 0) {
		g_set_error_literal (error,
		                     GDK_PIXBUF_ERROR,
		                     GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
		                     _("WebP file is empty."));
		g_free (data);
		return NULL;
	}

	status = WebPGetFeatures ((const uint8_t *) data, data_size, &features);
	if (status != VP8_STATUS_OK) {
		g_set_error (error,
		             GDK_PIXBUF_ERROR,
		             GDK_PIXBUF_ERROR_CORRUPT_IMAGE,
		             _("Failed to parse WebP bitstream (status %d)."),
		             (int) status);
		g_free (data);
		return NULL;
	}

	if (features.has_animation) {
		anim = load_animated_webp ((const uint8_t *) data, data_size, error);
	} else {
		GdkPixbuf *static_pixbuf = load_static_webp ((const uint8_t *) data, data_size,
		                                             (features.has_alpha != 0), error);
		if (static_pixbuf != NULL) {
			EomWebpFrame *frame = g_new0 (EomWebpFrame, 1);
			frame->pixbuf = static_pixbuf;
			frame->delay_time = 0;
			frame->elapsed = 0;

			EomWebpAnim *webp_anim = g_object_new (EOM_TYPE_WEBP_ANIM, NULL);
			webp_anim->width = features.width;
			webp_anim->height = features.height;
			webp_anim->total_time = 0;
			webp_anim->loop = FALSE;
			webp_anim->frames = g_list_append (NULL, frame);

			anim = GDK_PIXBUF_ANIMATION (webp_anim);
		}
	}

	g_free (data);

	return anim;
}

/**
 * eom_image_webp_load_file:
 * @file: a #GFile pointing to a WebP image
 * @error: return location for a #GError, or %NULL
 *
 * Decodes a WebP file into a single #GdkPixbuf. If the WebP is animated,
 * the first frame is returned.
 *
 * Returns: (transfer full): a new #GdkPixbuf, or %NULL on error.
 */
GdkPixbuf *
eom_image_webp_load_file (GFile *file, GError **error)
{
	GdkPixbufAnimation *anim;
	GdkPixbuf          *pixbuf = NULL;

	anim = eom_image_webp_load_animation (file, error);
	if (anim == NULL)
		return NULL;

	if (gdk_pixbuf_animation_is_static_image (anim)) {
		pixbuf = gdk_pixbuf_animation_get_static_image (anim);
	} else {
		GdkPixbufAnimationIter *iter = gdk_pixbuf_animation_get_iter (anim, NULL);
		if (iter != NULL) {
			pixbuf = gdk_pixbuf_animation_iter_get_pixbuf (iter);
			g_object_unref (iter);
		}
	}

	if (pixbuf != NULL)
		g_object_ref (pixbuf);

	g_object_unref (anim);

	return pixbuf;
}

/**
 * eom_image_webp_get_dimension_from_buffer:
 * @buffer: buffer containing the start of the WebP file
 * @size: length of @buffer
 * @width: (out) (optional): return location for image width
 * @height: (out) (optional): return location for image height
 *
 * Attempts to extract width and height from a WebP stream prefix.
 *
 * Returns: %TRUE on success, %FALSE otherwise.
 */
gboolean
eom_image_webp_get_dimension_from_buffer (const guchar *buffer,
                                          gsize         size,
                                          gint         *width,
                                          gint         *height)
{
	WebPBitstreamFeatures features;

	if (buffer == NULL || size == 0)
		return FALSE;

	if (WebPGetFeatures ((const uint8_t *) buffer, size, &features) == VP8_STATUS_OK) {
		if (width != NULL)
			*width = features.width;
		if (height != NULL)
			*height = features.height;
		return TRUE;
	}

	return FALSE;
}

/*
 * save_static_webp:
 *
 * Encodes the current pixbuf of @image as a lossless static WebP file.
 */
static gboolean
save_static_webp (EomImage   *image,
                  const char *file,
                  GError    **error)
{
	GdkPixbuf *pixbuf;
	gboolean   success = FALSE;
	uint8_t   *output = NULL;
	size_t     output_size = 0;
	FILE      *out;

	pixbuf = eom_image_get_pixbuf (image);
	if (!pixbuf) {
		g_set_error_literal (error,
		                     G_FILE_ERROR,
		                     G_FILE_ERROR_FAILED,
		                     _("Failed to get pixbuf from image."));
		return FALSE;
	}

	int    width  = gdk_pixbuf_get_width (pixbuf);
	int    height = gdk_pixbuf_get_height (pixbuf);
	int    stride = gdk_pixbuf_get_rowstride (pixbuf);
	guchar *pixels = gdk_pixbuf_get_pixels (pixbuf);

	if (gdk_pixbuf_get_has_alpha (pixbuf)) {
		output_size = WebPEncodeLosslessRGBA (pixels, width, height,
		                                      stride, &output);
	} else {
		output_size = WebPEncodeLosslessRGB (pixels, width, height,
		                                     stride, &output);
	}

	if (output_size > 0 && output != NULL) {
		out = g_fopen (file, "wb");
		if (out) {
			size_t written = fwrite (output, 1, output_size, out);
			if (written == output_size) {
				success = TRUE;
			} else {
				g_set_error_literal (error,
				                     G_FILE_ERROR,
				                     G_FILE_ERROR_FAILED,
				                     _("Failed to write WebP image data."));
			}
			fclose (out);
		} else {
			g_set_error (error,
			             G_FILE_ERROR,
			             g_file_error_from_errno (errno),
			             "%s", g_strerror (errno));
		}
		WebPFree (output);
	} else {
		g_set_error_literal (error,
		                     G_FILE_ERROR,
		                     G_FILE_ERROR_FAILED,
		                     _("Failed to encode WebP image."));
	}

	g_object_unref (pixbuf);

	return success;
}

/**
 * save_animated_webp:
 *
 * Encodes all frames of @image (from an animated WebP, GIF, etc.) into an animated WebP.
 */
static gboolean
save_animated_webp (EomImage   *image,
                    const char *file,
                    GError    **error)
{
	GdkPixbufAnimation     *anim = NULL;
	GdkPixbufAnimation     *fresh_anim = NULL;
	GList                  *frames = NULL;
	GList                  *allocated_frames = NULL;
	gboolean                success = FALSE;
	int                     width;
	int                     height;
	WebPAnimEncoder        *enc = NULL;
	WebPAnimEncoderOptions  enc_options;
	WebPConfig              config;
	int                     timestamp_ms = 0;
	WebPData                webp_data;
	GList                  *l;
	EomWebpFrame           *first_frame;
	GdkPixbuf              *first_pb;

	if (image->priv->anim == NULL) {
		return save_static_webp (image, file, error);
	}

	if (EOM_IS_WEBP_ANIM (image->priv->anim)) {
		anim = image->priv->anim;
		frames = EOM_WEBP_ANIM (anim)->frames;
	} else {
		/* Load a fresh, independent animation instance to avoid race
		 * conditions with the GUI playback loop on the main thread.
		 */
		if (image->priv->file != NULL) {
			GFileInputStream *stream = g_file_read (image->priv->file, NULL, NULL);
			if (stream != NULL) {
				fresh_anim = gdk_pixbuf_animation_new_from_stream (G_INPUT_STREAM (stream), NULL, NULL);
				g_object_unref (stream);
			}
		}

		anim = (fresh_anim != NULL) ? fresh_anim : image->priv->anim;

		/* Extract frames from GdkPixbufAnimation (e.g. GIF anim) */
		GTimeVal start_time = {0, 0};
		GdkPixbufAnimationIter *iter;
		int total_time = 0;
		GType gif_type = g_type_from_name ("GdkPixbufGifAnim");

		if (gif_type != G_TYPE_INVALID && G_TYPE_CHECK_INSTANCE_TYPE (anim, gif_type)) {
			struct _DummyGifAnim {
				GdkPixbufAnimation parent;
				int total_time;
			} *gif_anim = (void *) anim;
			total_time = gif_anim->total_time;
		}

		iter = gdk_pixbuf_animation_get_iter (anim, &start_time);
		if (iter == NULL) {
			if (fresh_anim != NULL)
				g_object_unref (fresh_anim);
			return save_static_webp (image, file, error);
		}

		GTimeVal cur_time = start_time;
		int elapsed = 0;

		while (TRUE) {
			int delay = gdk_pixbuf_animation_iter_get_delay_time (iter);
			GdkPixbuf *pb = gdk_pixbuf_animation_iter_get_pixbuf (iter);

			if (delay <= 0)
				delay = 100;

			if (pb != NULL) {
				EomWebpFrame *frame = g_new0 (EomWebpFrame, 1);
				frame->pixbuf = gdk_pixbuf_copy (pb);
				frame->delay_time = delay;
				frame->elapsed = elapsed;
				allocated_frames = g_list_prepend (allocated_frames, frame);
			}

			elapsed += delay;
			if (total_time > 0 && elapsed >= total_time)
				break;

			cur_time.tv_sec = elapsed / 1000;
			cur_time.tv_usec = (elapsed % 1000) * 1000;

			if (!gdk_pixbuf_animation_iter_advance (iter, &cur_time))
				break;
		}

		g_object_unref (iter);

		allocated_frames = g_list_reverse (allocated_frames);
		frames = allocated_frames;
	}

	if (frames == NULL) {
		g_set_error_literal (error,
		                     G_FILE_ERROR,
		                     G_FILE_ERROR_FAILED,
		                     _("Failed to extract frames from animated image."));
		return FALSE;
	}

	first_frame = (EomWebpFrame *) frames->data;
	first_pb = first_frame->pixbuf;

	if (image->priv->trans != NULL) {
		GdkPixbuf *t = eom_transform_apply (image->priv->trans, first_pb, NULL);
		if (t != NULL) {
			width = gdk_pixbuf_get_width (t);
			height = gdk_pixbuf_get_height (t);
			g_object_unref (t);
		} else {
			width = gdk_pixbuf_get_width (first_pb);
			height = gdk_pixbuf_get_height (first_pb);
		}
	} else {
		width = gdk_pixbuf_get_width (first_pb);
		height = gdk_pixbuf_get_height (first_pb);
	}

	WebPAnimEncoderOptionsInit (&enc_options);
	enc_options.anim_params.loop_count = 0; /* loop forever */

	enc = WebPAnimEncoderNew (width, height, &enc_options);
	if (enc == NULL) {
		g_set_error_literal (error,
		                     G_FILE_ERROR,
		                     G_FILE_ERROR_FAILED,
		                     _("Failed to create WebP animation encoder."));
		goto cleanup_frames;
	}

	WebPConfigInit (&config);
	config.lossless = 1;

	for (l = frames; l != NULL; l = l->next) {
		EomWebpFrame *frame = (EomWebpFrame *) l->data;
		GdkPixbuf    *pb = frame->pixbuf;
		GdkPixbuf    *transformed = NULL;
		WebPPicture   pic;

		if (image->priv->trans != NULL) {
			transformed = eom_transform_apply (image->priv->trans, pb, NULL);
			if (transformed != NULL)
				pb = transformed;
		}

		if (!WebPPictureInit (&pic)) {
			if (transformed != NULL)
				g_object_unref (transformed);
			continue;
		}

		pic.width = width;
		pic.height = height;
		pic.use_argb = 1;

		if (gdk_pixbuf_get_has_alpha (pb)) {
			WebPPictureImportRGBA (&pic, gdk_pixbuf_get_pixels (pb),
			                       gdk_pixbuf_get_rowstride (pb));
		} else {
			WebPPictureImportRGB (&pic, gdk_pixbuf_get_pixels (pb),
			                      gdk_pixbuf_get_rowstride (pb));
		}

		if (!WebPAnimEncoderAdd (enc, &pic, timestamp_ms, &config)) {
			g_warning ("WebPAnimEncoderAdd failed for timestamp %d", timestamp_ms);
		}

		WebPPictureFree (&pic);
		if (transformed != NULL)
			g_object_unref (transformed);

		timestamp_ms += frame->delay_time;
	}

	/* Add end marker with final timestamp */
	WebPAnimEncoderAdd (enc, NULL, timestamp_ms, NULL);

	WebPDataInit (&webp_data);
	if (!WebPAnimEncoderAssemble (enc, &webp_data)) {
		const char *err = WebPAnimEncoderGetError (enc);
		g_set_error (error,
		             G_FILE_ERROR,
		             G_FILE_ERROR_FAILED,
		             _("Failed to assemble WebP animation: %s"),
		             err ? err : "unknown error");
		goto cleanup_encoder;
	}

	FILE *out = g_fopen (file, "wb");
	if (out == NULL) {
		g_set_error (error,
		             G_FILE_ERROR,
		             g_file_error_from_errno (errno),
		             "%s", g_strerror (errno));
		WebPDataClear (&webp_data);
		goto cleanup_encoder;
	}

	size_t expected_size = webp_data.size;
	size_t written = fwrite (webp_data.bytes, 1, expected_size, out);
	fclose (out);
	WebPDataClear (&webp_data);

	if (written != expected_size) {
		g_set_error_literal (error,
		                     G_FILE_ERROR,
		                     G_FILE_ERROR_FAILED,
		                     _("Failed to write WebP animation data."));
		goto cleanup_encoder;
	}

	success = TRUE;

cleanup_encoder:
	if (enc != NULL)
		WebPAnimEncoderDelete (enc);

cleanup_frames:
	if (allocated_frames != NULL) {
		for (l = allocated_frames; l != NULL; l = l->next) {
			EomWebpFrame *f = (EomWebpFrame *) l->data;
			if (f->pixbuf)
				g_object_unref (f->pixbuf);
			g_free (f);
		}
		g_list_free (allocated_frames);
	}

	if (fresh_anim != NULL) {
		g_object_unref (fresh_anim);
	}

	return success;
}

/**
 * eom_image_webp_save_file:
 * @image: the #EomImage to save
 * @file: destination file path
 * @source: save info for the source image
 * @target: save info for the target (may be %NULL)
 * @error: return location for a #GError, or %NULL
 *
 * Saves @image to @file in WebP format. If @image is animated (WebP, GIF, etc.),
 * all frames are encoded into an animated WebP file; otherwise a static lossless
 * WebP is written.
 *
 * Returns: %TRUE on success, %FALSE on error.
 */
gboolean
eom_image_webp_save_file (EomImage         *image,
                          const char       *file,
                          EomImageSaveInfo *source,
                          EomImageSaveInfo *target,
                          GError          **error)
{
	g_return_val_if_fail (EOM_IS_IMAGE (image), FALSE);
	g_return_val_if_fail (file != NULL, FALSE);

	if (target != NULL && g_ascii_strcasecmp (target->format, EOM_FILE_FORMAT_WEBP) != 0) {
		return FALSE;
	}

	if (eom_image_is_animation (image)) {
		return save_animated_webp (image, file, error);
	} else {
		return save_static_webp (image, file, error);
	}
}

G_GNUC_END_IGNORE_DEPRECATIONS

#endif /* HAVE_WEBP */
