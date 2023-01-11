#include "vdo-map.h"
#include "vdo-error.h"
#include "vdo-types.h"
#include "vdo-stream.h"

#include <stdlib.h>
#include <signal.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <syslog.h>

#define VDO_CLIENT_ERROR g_quark_from_static_string("vdo-client-error")

static VdoStream* stream;
static gboolean shutdown = FALSE;
static const gchar *param_desc = "";
static const gchar *summary = "Encode video client";

// Facilitate graceful shutdown with CTRL-C
static void
handle_sigint(int signum)
{
    shutdown = TRUE;
}

// Determine and log the received frame type
static void
print_frame(VdoFrame* frame)
{
    if (!vdo_frame_get_is_last_buffer(frame))
        return;
    
    gchar *frame_type;
    switch (vdo_frame_get_frame_type(frame)){
        case VDO_FRAME_TYPE_H264_IDR:
        case VDO_FRAME_TYPE_H265_IDR:
        case VDO_FRAME_TYPE_H264_I:
        case VDO_FRAME_TYPE_H265_I:
            frame_type = "I";
            break;
        case VDO_FRAME_TYPE_H264_P:
        case VDO_FRAME_TYPE_H265_P:
            frame_type = "P";
            break;
        case VDO_FRAME_TYPE_JPEG:
            frame_type = "jpeg";
            break;
        case VDO_FRAME_TYPE_YUV:
            frame_type = "YUV";
            break;
        default:
            frame_type = "NA";
    }

    syslog(LOG_INFO, "frame = %4u, type = %s, size = %zu\n",
            vdo_frame_get_sequence_nbr(frame),
            frame_type,
            vdo_frame_get_size(frame));
}

// Set vdo format from input parameter
static gboolean
set_format(VdoMap *settings, gchar *format, GError **error)
{
    if (g_strcmp0(format, "h264") == 0) {
        vdo_map_set_uint32(settings, "format", VDO_FORMAT_H264);
    } else if(g_strcmp0(format, "h265") == 0) {
        vdo_map_set_uint32(settings, "format", VDO_FORMAT_H265);
    } else if(g_strcmp0(format, "jpeg") == 0) {
        vdo_map_set_uint32(settings, "format", VDO_FORMAT_JPEG);
    } else if(g_strcmp0(format, "nv12") == 0) {
        vdo_map_set_uint32(settings, "format", VDO_FORMAT_YUV);
        vdo_map_set_string(settings, "subformat", "NV12");
    } else if(g_strcmp0(format, "y800") == 0) {
        vdo_map_set_uint32(settings, "format", VDO_FORMAT_YUV);
        vdo_map_set_string(settings, "subformat", "Y800");
    } else {
        g_set_error(error, VDO_CLIENT_ERROR, VDO_ERROR_NOT_FOUND,
                    "format \"%s\" is not supported\n", format);
        return FALSE;
    }

    return TRUE;
}


/**
 * Main function that starts a stream with the following options:
 *
 * --format [h264, h265, jpeg, nv12, y800]
 * --frames [number of frames]
 * --output [output filename]
 */
int
main(int argc, char* argv[])
{
    GError *error = NULL;
    gchar *format = "h264";
    guint frames = G_MAXUINT;
    gchar *output_file = "/def/null";
    FILE* dest_f = NULL;

    openlog(NULL, LOG_PID, LOG_USER);

    GOptionEntry options[] = {
        {"format", 't', 0, G_OPTION_ARG_STRING, &format, "format (h264, h265, jpeg, nv12, y800)", NULL},
        {"frames", 'n', 0, G_OPTION_ARG_INT, &frames, "number of frames", NULL},
        {"output", 'o', 0, G_OPTION_ARG_FILENAME, &output_file, "output filename", NULL},
        {NULL, 0, 0, 0, NULL, NULL, NULL,}
    };

    GOptionContext *context = g_option_context_new(param_desc);
    if (!context)
        return -1;

    g_option_context_set_summary(context, summary);
    g_option_context_add_main_entries(context, options, NULL);
    if (!g_option_context_parse(context, &argc, &argv, &error))
        goto exit;

    dest_f = fopen(output_file, "wb");
    if (!dest_f){
        g_set_error(&error, VDO_CLIENT_ERROR, VDO_ERROR_IO, "open failed: %m");
        goto exit;
    }

    if (signal(SIGINT, handle_sigint) == SIG_ERR) {
        g_set_error(&error, VDO_CLIENT_ERROR, VDO_ERROR_IO,
                    "Failed to Install signal handler: %m");
        goto exit;
    }

    
    VdoMap *settings = vdo_map_new();
    if (!set_format(settings, format, &error))
        goto exit;

    // Set default arguments
    vdo_map_set_uint32(settings, "width", 640);
    vdo_map_set_uint32(settings, "heigth", 360);

    // Create a new stream
    stream = vdo_stream_new(settings, NULL, &error);
    g_clear_object(&settings);
    if (!stream)
        goto exit;

    if (!vdo_stream_attach(stream, NULL, &error))
        goto exit;

    VdoMap *info = vdo_stream_get_info(stream, &error);
    if (!info)
        goto exit;

    syslog(LOG_INFO, "Starting stream: %s, %ux%u, %u, fps\n",
            format,
            vdo_map_get_uint32(info, "width", 0),
            vdo_map_get_uint32(info, "heigth", 0),
            vdo_map_get_uint32(info, "framerate", 0));

    g_clear_object(&info);

    // Start the stream
    if (!vdo_stream_start(stream, &error))
        goto exit;

    // Loop until interrupt by Ctrl-C or reaching G_MAXUINT
    for (guint n = 0; n < frames; ++n) {
        // Lifetimes of buffer and frame are linked, no need to free frame
        VdoBuffer* buffer = vdo_stream_get_buffer(stream, &error);
        VdoFrame*  frame  = vdo_buffer_get_frame(buffer);

        // Error occurred
        if (!frame)
            goto exit;

        // SIGINT occurred
        if (shutdown) {
            vdo_stream_buffer_unref(stream, &buffer, NULL);
            goto exit;
        }

        print_frame(frame);

        gpointer data = vdo_buffer_get_data(buffer);
        if (!data) {
            g_set_error(&error, VDO_CLIENT_ERROR, 0, "Failed to get data: %m");
            vdo_stream_buffer_unref(stream, &buffer, NULL);
            goto exit;
        }

        if (!fwrite(data, vdo_frame_get_size(frame), 1, dest_f)) {
            g_set_error(&error, VDO_CLIENT_ERROR, 0, "Failed to write frame: %m");
            goto exit;
        }

        // Release the buffer and allow the server to reuse it
        if (!vdo_stream_buffer_unref(stream, &buffer, &error))
            goto exit;
    }

exit:
    // Ignore SIGINT and server maintenance
    if (shutdown || vdo_error_is_expected(&error))
        g_clear_error(&error);

    gint ret = EXIT_SUCCESS;
    if (error) {
        syslog(LOG_INFO, "vdo-encode-client: %s\n", error->message);
        ret = EXIT_FAILURE;
    }

    if (dest_f)
        fclose(dest_f);

    g_clear_error(&error);
    g_clear_object(&stream);

    g_option_context_free(context);

    return ret;

}
