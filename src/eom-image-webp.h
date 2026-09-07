#ifndef _EOM_IMAGE_WEBP_H_
#define _EOM_IMAGE_WEBP_H_

#include <glib.h>
#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk-pixbuf/gdk-pixbuf-animation.h>
#include "eom-image.h"
#include "eom-image-save-info.h"

G_BEGIN_DECLS

#if HAVE_WEBP

gboolean            eom_image_webp_save_file                 (EomImage         *image,
                                                              const char       *file,
                                                              EomImageSaveInfo *source,
                                                              EomImageSaveInfo *target,
                                                              GError          **error);

GdkPixbuf          *eom_image_webp_load_file                 (GFile            *file,
                                                              GError          **error);

GdkPixbufAnimation *eom_image_webp_load_animation            (GFile            *file,
                                                              GError          **error);

gboolean            eom_image_webp_get_dimension_from_buffer (const guchar     *buffer,
                                                              gsize             size,
                                                              gint             *width,
                                                              gint             *height);

#endif /* HAVE_WEBP */

G_END_DECLS

#endif /* _EOM_IMAGE_WEBP_H_ */
