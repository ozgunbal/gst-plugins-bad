/*
 * GStreamer
 * Copyright (C) 2016 Matthew Waters <matthew@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-glvideo_flip
 *
 * Transforms video on the GPU.
 *
 * <refsect2>
 * <title>Examples</title>
 * |[
 * gst-launch-1.0 videotestsrc ! glupload ! glvideoflip method=clockwise ! glimagesinkelement
 * ]| This pipeline flips the test image 90 degrees clockwise.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstglvideoflip.h"

#define GST_CAT_DEFAULT gst_gl_video_flip_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define DEFAULT_METHOD GST_GL_VIDEO_FLIP_METHOD_IDENTITY

enum
{
  PROP_0,
  PROP_METHOD,
};

static GstStaticPadTemplate _sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw(" GST_CAPS_FEATURE_MEMORY_GL_MEMORY "), "
        "format = (string) RGBA, "
        "width = " GST_VIDEO_SIZE_RANGE ", "
        "height = " GST_VIDEO_SIZE_RANGE ", "
        "framerate = " GST_VIDEO_FPS_RANGE ", "
        "texture-target = (string) 2D"));

static GstStaticPadTemplate _src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw(" GST_CAPS_FEATURE_MEMORY_GL_MEMORY "), "
        "format = (string) RGBA, "
        "width = " GST_VIDEO_SIZE_RANGE ", "
        "height = " GST_VIDEO_SIZE_RANGE ", "
        "framerate = " GST_VIDEO_FPS_RANGE ", "
        "texture-target = (string) 2D"));

#define GST_TYPE_GL_VIDEO_FLIP_METHOD (gst_video_flip_method_get_type())
static const GEnumValue video_flip_methods[] = {
  {GST_GL_VIDEO_FLIP_METHOD_IDENTITY, "Identity (no rotation)", "none"},
  {GST_GL_VIDEO_FLIP_METHOD_90R, "Rotate clockwise 90 degrees", "clockwise"},
  {GST_GL_VIDEO_FLIP_METHOD_180, "Rotate 180 degrees", "rotate-180"},
  {GST_GL_VIDEO_FLIP_METHOD_90L, "Rotate counter-clockwise 90 degrees",
      "counterclockwise"},
  {GST_GL_VIDEO_FLIP_METHOD_FLIP_HORIZ, "Flip horizontally", "horizontal-flip"},
  {GST_GL_VIDEO_FLIP_METHOD_FLIP_VERT, "Flip vertically", "vertical-flip"},
  {GST_GL_VIDEO_FLIP_METHOD_FLIP_UL_LR,
      "Flip across upper left/lower right diagonal", "upper-left-diagonal"},
  {GST_GL_VIDEO_FLIP_METHOD_FLIP_UR_LL,
      "Flip across upper right/lower left diagonal", "upper-right-diagonal"},
  {GST_GL_VIDEO_FLIP_METHOD_AUTO,
      "Select flip method based on image-orientation tag", "automatic"},
  {0, NULL, NULL},
};

static GType
gst_video_flip_method_get_type (void)
{
  static GType video_flip_method_type = 0;

  if (!video_flip_method_type) {
    video_flip_method_type = g_enum_register_static ("GstGLVideoFlipMethod",
        video_flip_methods);
  }
  return video_flip_method_type;
}

#define gst_gl_video_flip_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstGLVideoFlip, gst_gl_video_flip,
    GST_TYPE_BIN, GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT,
        "glvideoflip", 0, "glvideoflip element");
    );

static void gst_gl_video_flip_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_gl_video_flip_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static GstPadProbeReturn _input_sink_probe (GstPad * pad,
    GstPadProbeInfo * info, gpointer user_data);
static GstPadProbeReturn _trans_src_probe (GstPad * pad, GstPadProbeInfo * info,
    gpointer user_data);

static void
gst_gl_video_flip_class_init (GstGLVideoFlipClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;

  gobject_class = (GObjectClass *) klass;
  element_class = GST_ELEMENT_CLASS (klass);

  gobject_class->set_property = gst_gl_video_flip_set_property;
  gobject_class->get_property = gst_gl_video_flip_get_property;

  g_object_class_install_property (gobject_class, PROP_METHOD,
      g_param_spec_enum ("method", "method", "method",
          GST_TYPE_GL_VIDEO_FLIP_METHOD, DEFAULT_METHOD,
          GST_PARAM_CONTROLLABLE | G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
          G_PARAM_STATIC_STRINGS));

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&_src_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&_sink_template));

  gst_element_class_set_metadata (element_class, "OpenGL video flip filter",
      "Filter/Effect/Video", "Flip video on the GPU",
      "Matthew Waters <matthew@centricular.com>");
}

static void
gst_gl_video_flip_init (GstGLVideoFlip * flip)
{
  gboolean res = TRUE;
  GstPad *pad;

  flip->input_capsfilter = gst_element_factory_make ("capsfilter", NULL);
  res &= gst_bin_add (GST_BIN (flip), flip->input_capsfilter);

  flip->transformation = gst_element_factory_make ("gltransformation", NULL);
  g_object_set (flip->transformation, "ortho", TRUE, NULL);
  res &= gst_bin_add (GST_BIN (flip), flip->transformation);

  flip->output_capsfilter = gst_element_factory_make ("capsfilter", NULL);
  res &= gst_bin_add (GST_BIN (flip), flip->output_capsfilter);

  res &=
      gst_element_link_pads (flip->input_capsfilter, "src",
      flip->transformation, "sink");
  res &=
      gst_element_link_pads (flip->transformation, "src",
      flip->output_capsfilter, "sink");

  pad = gst_element_get_static_pad (flip->input_capsfilter, "sink");
  if (!pad) {
    res = FALSE;
  } else {
    GST_DEBUG_OBJECT (flip, "setting target sink pad %" GST_PTR_FORMAT, pad);
    flip->sinkpad = gst_ghost_pad_new ("sink", pad);
    flip->sink_probe = gst_pad_add_probe (flip->sinkpad,
        GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM |
        GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
        (GstPadProbeCallback) _input_sink_probe, flip, NULL);
    gst_element_add_pad (GST_ELEMENT_CAST (flip), flip->sinkpad);
    gst_object_unref (pad);
  }

  pad = gst_element_get_static_pad (flip->transformation, "src");
  flip->src_probe = gst_pad_add_probe (pad,
      GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
      (GstPadProbeCallback) _trans_src_probe, flip, NULL);
  gst_object_unref (pad);

  pad = gst_element_get_static_pad (flip->output_capsfilter, "src");
  if (!pad) {
    res = FALSE;
  } else {
    GST_DEBUG_OBJECT (flip, "setting target sink pad %" GST_PTR_FORMAT, pad);
    flip->srcpad = gst_ghost_pad_new ("src", pad);
    gst_element_add_pad (GST_ELEMENT_CAST (flip), flip->srcpad);
    gst_object_unref (pad);
  }

  if (!res) {
    GST_WARNING_OBJECT (flip, "Failed to add/connect the necessary machinery");
  }
}

static void
gst_gl_video_flip_set_method (GstGLVideoFlip * vf, GstGLVideoFlipMethod method,
    gboolean from_tag)
{
  GST_OBJECT_LOCK (vf);
  /* Store updated method */
  if (from_tag)
    vf->tag_method = method;
  else
    vf->method = method;

  /* Get the new method */
  if (vf->method == GST_GL_VIDEO_FLIP_METHOD_AUTO)
    method = vf->tag_method;
  else
    method = vf->method;

  if (method != vf->active_method) {
    vf->active_method = method;
    GST_OBJECT_UNLOCK (vf);

    g_object_set (vf->transformation, "rotation-x", 0., "rotation-y", 0.,
        "rotation-z", 0., "scale-x", 1., "scale-y", 1., NULL);
    switch (method) {
      case GST_GL_VIDEO_FLIP_METHOD_IDENTITY:
        break;
      case GST_GL_VIDEO_FLIP_METHOD_90R:
        g_object_set (vf->transformation, "rotation-z", 90., NULL);
        break;
      case GST_GL_VIDEO_FLIP_METHOD_180:
        g_object_set (vf->transformation, "rotation-z", 180., NULL);
        break;
      case GST_GL_VIDEO_FLIP_METHOD_90L:
        g_object_set (vf->transformation, "rotation-z", 270., NULL);
        break;
      case GST_GL_VIDEO_FLIP_METHOD_FLIP_HORIZ:
        g_object_set (vf->transformation, "scale-x", -1., NULL);
        break;
      case GST_GL_VIDEO_FLIP_METHOD_FLIP_UR_LL:
        g_object_set (vf->transformation, "scale-x", -1., "rotation-z", 90.,
            NULL);
        break;
      case GST_GL_VIDEO_FLIP_METHOD_FLIP_VERT:
        g_object_set (vf->transformation, "scale-x", -1., "rotation-z", 180.,
            NULL);
        break;
      case GST_GL_VIDEO_FLIP_METHOD_FLIP_UL_LR:
        g_object_set (vf->transformation, "scale-x", -1., "rotation-z", 270.,
            NULL);
        break;
      default:
        break;
    }
  } else {
    GST_OBJECT_UNLOCK (vf);
  }
}

static void
gst_gl_video_flip_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstGLVideoFlip *vf = GST_GL_VIDEO_FLIP (object);

  switch (prop_id) {
    case PROP_METHOD:
      gst_gl_video_flip_set_method (vf, g_value_get_enum (value), FALSE);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_gl_video_flip_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstGLVideoFlip *vf = GST_GL_VIDEO_FLIP (object);

  switch (prop_id) {
    case PROP_METHOD:
      g_value_set_enum (value, vf->method);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* Caps negotiation happens like this:
 *
 * 1. caps/accept-caps queries bypass the capsfilters on either side of the
 *    transformation element so the fixed caps don't get in the way.
 * 2. Receiving a caps event on the sink pad will set fixed caps on either side
 *    of the transformation element.
 */
static GstCaps *
_transform_caps (GstGLVideoFlip * vf, GstPadDirection direction, GstCaps * caps)
{
  GstCaps *output = gst_caps_copy (caps);
  gint i;

  for (i = 0; i < gst_caps_get_size (output); i++) {
    GstStructure *structure = gst_caps_get_structure (output, i);
    gint width, height;
    gint par_n, par_d;

    if (gst_structure_get_int (structure, "width", &width) &&
        gst_structure_get_int (structure, "height", &height)) {

      switch (vf->active_method) {
        case GST_GL_VIDEO_FLIP_METHOD_90R:
        case GST_GL_VIDEO_FLIP_METHOD_90L:
        case GST_GL_VIDEO_FLIP_METHOD_FLIP_UL_LR:
        case GST_GL_VIDEO_FLIP_METHOD_FLIP_UR_LL:
          gst_structure_set (structure, "width", G_TYPE_INT, height,
              "height", G_TYPE_INT, width, NULL);
          if (gst_structure_get_fraction (structure, "pixel-aspect-ratio",
                  &par_n, &par_d)) {
            if (par_n != 1 || par_d != 1) {
              GValue val = { 0, };

              g_value_init (&val, GST_TYPE_FRACTION);
              gst_value_set_fraction (&val, par_d, par_n);
              gst_structure_set_value (structure, "pixel-aspect-ratio", &val);
              g_value_unset (&val);
            }
          }
          break;
        case GST_GL_VIDEO_FLIP_METHOD_IDENTITY:
        case GST_GL_VIDEO_FLIP_METHOD_180:
        case GST_GL_VIDEO_FLIP_METHOD_FLIP_HORIZ:
        case GST_GL_VIDEO_FLIP_METHOD_FLIP_VERT:
          break;
        default:
          g_assert_not_reached ();
          break;
      }
    }
  }

  return output;
}

static GstPadProbeReturn
_input_sink_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstGLVideoFlip *vf = GST_GL_VIDEO_FLIP (user_data);

  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    GstEvent *event = GST_PAD_PROBE_INFO_EVENT (info);

    switch (GST_EVENT_TYPE (event)) {
      case GST_EVENT_TAG:{
        GstTagList *taglist;
        gchar *orientation;

        gst_event_parse_tag (event, &taglist);

        if (gst_tag_list_get_string (taglist, "image-orientation",
                &orientation)) {
          if (!g_strcmp0 ("rotate-0", orientation))
            gst_gl_video_flip_set_method (vf, GST_GL_VIDEO_FLIP_METHOD_IDENTITY,
                TRUE);
          else if (!g_strcmp0 ("rotate-90", orientation))
            gst_gl_video_flip_set_method (vf, GST_GL_VIDEO_FLIP_METHOD_90R,
                TRUE);
          else if (!g_strcmp0 ("rotate-180", orientation))
            gst_gl_video_flip_set_method (vf, GST_GL_VIDEO_FLIP_METHOD_180,
                TRUE);
          else if (!g_strcmp0 ("rotate-270", orientation))
            gst_gl_video_flip_set_method (vf, GST_GL_VIDEO_FLIP_METHOD_90L,
                TRUE);
          else if (!g_strcmp0 ("flip-rotate-0", orientation))
            gst_gl_video_flip_set_method (vf,
                GST_GL_VIDEO_FLIP_METHOD_FLIP_HORIZ, TRUE);
          else if (!g_strcmp0 ("flip-rotate-90", orientation))
            gst_gl_video_flip_set_method (vf,
                GST_GL_VIDEO_FLIP_METHOD_FLIP_UR_LL, TRUE);
          else if (!g_strcmp0 ("flip-rotate-180", orientation))
            gst_gl_video_flip_set_method (vf,
                GST_GL_VIDEO_FLIP_METHOD_FLIP_VERT, TRUE);
          else if (!g_strcmp0 ("flip-rotate-270", orientation))
            gst_gl_video_flip_set_method (vf,
                GST_GL_VIDEO_FLIP_METHOD_FLIP_UL_LR, TRUE);

          g_free (orientation);
        }
        break;
      }
      case GST_EVENT_CAPS:{
        GstCaps *caps, *output, *templ;
        GstPad *srcpad;

        gst_event_parse_caps (event, &caps);

        output = _transform_caps (vf, GST_PAD_SINK, caps);

        /* allow overriding the output caps */
        srcpad = gst_element_get_static_pad (vf->transformation, "src");
        templ = gst_pad_get_pad_template_caps (srcpad);
        gst_caps_append (output, gst_caps_ref (templ));
        gst_object_unref (srcpad);

        g_object_set (vf->input_capsfilter, "caps", gst_caps_copy (caps), NULL);
        g_object_set (vf->output_capsfilter, "caps", output, NULL);
        break;
      }
      default:
        break;
    }
  } else if (GST_PAD_PROBE_INFO_TYPE (info) &
      GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM) {
    GstQuery *query = GST_PAD_PROBE_INFO_QUERY (info);

    switch (GST_QUERY_TYPE (query)) {
        /* bypass the capsfilter */
      case GST_QUERY_CAPS:
      case GST_QUERY_ACCEPT_CAPS:{
        GstPad *pad = gst_element_get_static_pad (vf->transformation, "sink");
        if (gst_pad_query (pad, query)) {
          gst_object_unref (pad);
          return GST_PAD_PROBE_HANDLED;
        } else {
          gst_object_unref (pad);
          return GST_PAD_PROBE_DROP;
        }
      }
      default:
        break;
    }
  }

  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
_trans_src_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstGLVideoFlip *vf = GST_GL_VIDEO_FLIP (user_data);

  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM) {
    GstQuery *query = GST_PAD_PROBE_INFO_QUERY (info);

    switch (GST_QUERY_TYPE (query)) {
        /* bypass the capsfilter */
      case GST_QUERY_CAPS:
      case GST_QUERY_ACCEPT_CAPS:{
        if (gst_pad_peer_query (vf->srcpad, query))
          return GST_PAD_PROBE_HANDLED;
        else
          return GST_PAD_PROBE_DROP;
      }
      default:
        break;
    }
  }

  return GST_PAD_PROBE_OK;
}