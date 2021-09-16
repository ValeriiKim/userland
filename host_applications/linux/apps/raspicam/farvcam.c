/*
Copyright (c) 2018, Raspberry Pi (Trading) Ltd.
Copyright (c) 2013, Broadcom Europe Ltd.
Copyright (c) 2013, James Hughes
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/**
 * \file farvcam.c
 * Программа для Raspberry Pi Zero для работы с терминалом CAN-WAY. Сырой вариант, в котором пока реализовано
 * получение фотографии и снятие видео. Видео и фотографии получаются одного разрешения,
 * поскольку используется сплиттер, подключенный к камере. Сплиттер имеет два выхода.
 * Один выход подключается к энкодеру для получения видео в формате H264,
 * второй выход подключается к энкодеру для получения фотографий в формате JPEG. 
 */

// We use some GNU extensions (basename)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <memory.h>
#include <sysexits.h>
#include <math.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include "bcm_host.h"
#include "interface/vcos/vcos.h"

#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_logging.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/util/mmal_default_components.h"
#include "interface/mmal/util/mmal_connection.h"
#include "interface/mmal/mmal_parameters_camera.h"

#include "RaspiCommonSettings.h"
#include "RaspiCamControl.h"
#include "RaspiPreview.h"
#include "RaspiCLI.h"
#include "RaspiHelpers.h"
#include "RaspiGPS.h"

#include <semaphore.h>
#include <threads.h>

#include <stdbool.h>
#include <pthread.h>
#include <termios.h>
#include <fcntl.h>
#include "pigpio.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize.h"

// Standard port setting for the camera component
#define MMAL_CAMERA_PREVIEW_PORT 0
#define MMAL_CAMERA_VIDEO_PORT 1
#define MMAL_CAMERA_CAPTURE_PORT 2

// Port configuration for the splitter component
#define SPLITTER_OUTPUT_PORT 0
#define SPLITTER_PREVIEW_PORT 1

// Video format information
// 0 implies variable
#define VIDEO_FRAME_RATE_NUM 30
#define VIDEO_FRAME_RATE_DEN 1

/// Video render needs at least 2 buffers.
#define VIDEO_OUTPUT_BUFFERS_NUM 3

#define MAX_USER_EXIF_TAGS 32
#define MAX_EXIF_PAYLOAD_LENGTH 128

// Max bitrate we allow for recording
const int MAX_BITRATE_MJPEG = 25000000;   // 25Mbits/s
const int MAX_BITRATE_LEVEL4 = 25000000;  // 25Mbits/s
const int MAX_BITRATE_LEVEL42 = 62500000; // 62.5Mbits/s

const int max_filename_length = 30;

/// Interval at which we check for an failure abort during capture
const int ABORT_INTERVAL = 100; // ms

/// Capture/Pause switch method
/// Simply capture for time specified
enum
{
    WAIT_METHOD_NONE,     /// Simply capture for time specified
    WAIT_METHOD_TIMED,    /// Cycle between capture and pause for times specified
    WAIT_METHOD_KEYPRESS, /// Switch between capture and pause on keypress
    WAIT_METHOD_SIGNAL,   /// Switch between capture and pause on signal
    WAIT_METHOD_FOREVER   /// Run/record forever
};

/// Frame advance method
enum
{
    FRAME_NEXT_SINGLE,
    FRAME_NEXT_TIMELAPSE,
    FRAME_NEXT_KEYPRESS,
    FRAME_NEXT_FOREVER,
    FRAME_NEXT_GPIO,
    FRAME_NEXT_SIGNAL,
    FRAME_NEXT_IMMEDIATELY
};

// Forward
typedef struct RASPIVID_STATE_S RASPIVID_STATE;

/** Struct used to pass information in encoder port userdata to callback
 */
typedef struct
{
    FILE *file_handle;      /// File handle to write buffer data to.
    RASPIVID_STATE *pstate; /// pointer to our state in case required in callback
    int abort;              /// Set to 1 in callback if an error occurs to attempt to abort the capture
    char *cb_buff;          /// Circular buffer
    int cb_len;             /// Length of buffer
    int cb_wptr;            /// Current write pointer
    int cb_wrap;            /// Has buffer wrapped at least once?
    int cb_data;            /// Valid bytes in buffer
#define IFRAME_BUFSIZE (60 * 1000)
    int iframe_buff[IFRAME_BUFSIZE]; /// buffer of iframe pointers
    int iframe_buff_wpos;
    int iframe_buff_rpos;
    char header_bytes[29];
    int header_wptr;
    FILE *imv_file_handle; /// File handle to write inline motion vectors to.
    FILE *raw_file_handle; /// File handle to write raw data to.
    int flush_buffers;
    FILE *pts_file_handle; /// File timestamps
} PORT_USERDATA;

/** Struct used to pass image information in encoder port userdata to callback
 */
typedef struct
{
    FILE *file_handle;                   /// File handle to write buffer data to.
    VCOS_SEMAPHORE_T complete_semaphore; /// semaphore which is posted when we reach end of frame (indicates end of capture or fault)
    RASPIVID_STATE *pstate;              /// pointer to our state in case required in callback
    MMAL_POOL_T *encoderPool;
    unsigned char *data;
    unsigned int bufferPosition;
    unsigned int startingOffset;
    unsigned int offset;
    unsigned int length;
    unsigned int lengthActual;
} PORT_USERDATA_IMAGE;

/** Possible raw output formats
 */
typedef enum
{
    RAW_OUTPUT_FMT_YUV = 0,
    RAW_OUTPUT_FMT_RGB,
    RAW_OUTPUT_FMT_GRAY,
} RAW_OUTPUT_FMT;

/** Structure containing all state information for the current run
 */
struct RASPIVID_STATE_S
{
    RASPICOMMONSETTINGS_PARAMETERS common_settings; /// Common settings
    int timeout;                                    /// Video record duration. Units are milliseconds
    int timeout_image;                              /// Time taken before frame is grabbed and app then shuts down. Units are milliseconds
    MMAL_FOURCC_T encoding;                         /// Requested codec video encoding (MJPEG or H264)
    MMAL_FOURCC_T encoding_image;                   /// Encoding to use for the output image file.
    int bitrate;                                    /// Requested bitrate
    int framerate;                                  /// Requested frame rate (fps)
    int intraperiod;                                /// Intra-refresh period (key frame rate)
    int quantisationParameter;                      /// Quantisation parameter - quality. Set bitrate 0 and set this for variable bitrate
    int bInlineHeaders;                             /// Insert inline headers to stream (SPS, PPS)
    int demoMode;                                   /// Run app in demo mode
    int demoInterval;                               /// Interval between camera settings changes
    int immutableInput;                             /// Flag to specify whether encoder works in place or creates a new buffer. Result is preview can display either
    /// the camera output or the encoder output (with compression artifacts)
    int profile;    /// H264 profile to use for encoding
    int level;      /// H264 level to use for encoding
    int waitMethod; /// Method for switching between pause and capture

    int onTime;  /// In timed cycle mode, the amount of time the capture is on per cycle
    int offTime; /// In timed cycle mode, the amount of time the capture is off per cycle

    int segmentSize;   /// Segment mode In timed cycle mode, the amount of time the capture is off per cycle
    int segmentWrap;   /// Point at which to wrap segment counter
    int segmentNumber; /// Current segment counter
    int splitNow;      /// Split at next possible i-frame if set to 1.
    int splitWait;     /// Switch if user wants splited files

    RASPIPREVIEW_PARAMETERS preview_parameters;   /// Preview setup parameters
    RASPICAM_CAMERA_PARAMETERS camera_parameters; /// Camera setup parameters

    MMAL_COMPONENT_T *camera_component;        // Pointer to the camera component
    MMAL_COMPONENT_T *splitter_component;      // Pointer to the splitter component
    MMAL_COMPONENT_T *encoder_component;       // Pointer to the encoder component
    MMAL_COMPONENT_T *encoder_component_image; // Pointer to the encoder component for image
    MMAL_COMPONENT_T *resize_component;        // Pointer to the resize component

    MMAL_CONNECTION_T *preview_connection;       // Pointer to the connection from camera or splitter to preview
    MMAL_CONNECTION_T *splitter_connection;      // Pointer to the connection from camera to splitter
    MMAL_CONNECTION_T *encoder_connection;       // Pointer to the connection from camera to encoder
    MMAL_CONNECTION_T *encoder_connection_image; // Pointer to the connection from splitter to encoder
    MMAL_CONNECTION_T *resizer_connection;       // Pointer to the connection from splitter to encoder

    MMAL_POOL_T *splitter_pool;       /// Pointer to the pool of buffers used by splitter output port 0
    MMAL_POOL_T *splitter_pool_image; // Pointer to the pool of buffers used by splitter output port 1

    MMAL_POOL_T *encoder_pool; /// Pointer to the pool of buffers used by encoder output port
    MMAL_POOL_T *encoder_pool_image;

    PORT_USERDATA callback_data; /// Used to move data to the encoder callback

    int bCapturing;      /// State of capture/pause
    int bCircularBuffer; /// Whether we are writing to a circular buffer

    int inlineMotionVectors;       /// Encoder outputs inline Motion Vectors
    char *imv_filename;            /// filename of inline Motion Vectors output
    int raw_output;                /// Output raw video from camera as well
    RAW_OUTPUT_FMT raw_output_fmt; /// The raw video format
    char *raw_filename;            /// Filename for raw video output
    char *jpeg_filename;           // Filename for jpeg image
    int intra_refresh_type;        /// What intra refresh type to use. -1 to not set.
    int frame;
    char *pts_filename;
    int save_pts;
    int64_t starttime;
    int64_t lasttime;

    bool netListen;
    MMAL_BOOL_T addSPSTiming;
    int slices;

    /* Parameters for Still pictures
    */
    int quality;    /// JPEG quality setting (1-100)
    int wantRAW;    /// Flag for whether the JPEG metadata also contains the RAW bayer image
    char *linkname; /// filename of output file
    int frameStart; /// First number of frame output counter
    MMAL_PARAM_THUMBNAIL_CONFIG_T thumbnailConfig;
    const char *exifTags[MAX_USER_EXIF_TAGS]; /// Array of pointers to tags supplied from the command line
    int numExifTags;                          /// Number of supplied tags
    int enableExifTags;                       /// Enable/Disable EXIF tags in output
    int timelapse;                            /// Delay between each picture in timelapse mode. If 0, disable timelapse
    int fullResPreview;                       /// If set, the camera preview port runs at capture resolution. Reduces fps.
    int frameNextMethod;                      /// Which method to use to advance to next frame
    int useGL;                                /// Render preview using OpenGL
    int glCapture;                            /// Save the GL frame-buffer instead of camera output
    int burstCaptureMode;                     /// Enable burst mode
    int datetime;                             /// Use DateTime instead of frame#
    int timestamp;                            /// Use timestamp instead of frame#
    int restart_interval;                     /// JPEG restart interval. 0 for none.
};

/// Structure to cross reference H264 profile strings against the MMAL parameter equivalent
static XREF_T profile_map[] =
    {
        {"baseline", MMAL_VIDEO_PROFILE_H264_BASELINE},
        {"main", MMAL_VIDEO_PROFILE_H264_MAIN},
        {"high", MMAL_VIDEO_PROFILE_H264_HIGH},
        //   {"constrained",  MMAL_VIDEO_PROFILE_H264_CONSTRAINED_BASELINE} // Does anyone need this?
};

static int profile_map_size = sizeof(profile_map) / sizeof(profile_map[0]);

/// Structure to cross reference H264 level strings against the MMAL parameter equivalent
static XREF_T level_map[] =
    {
        {"4", MMAL_VIDEO_LEVEL_H264_4},
        {"4.1", MMAL_VIDEO_LEVEL_H264_41},
        {"4.2", MMAL_VIDEO_LEVEL_H264_42},
};

static int level_map_size = sizeof(level_map) / sizeof(level_map[0]);

static XREF_T initial_map[] =
    {
        {"record", 0},
        {"pause", 1},
};

static int initial_map_size = sizeof(initial_map) / sizeof(initial_map[0]);

static XREF_T intra_refresh_map[] =
    {
        {"cyclic", MMAL_VIDEO_INTRA_REFRESH_CYCLIC},
        {"adaptive", MMAL_VIDEO_INTRA_REFRESH_ADAPTIVE},
        {"both", MMAL_VIDEO_INTRA_REFRESH_BOTH},
        {"cyclicrows", MMAL_VIDEO_INTRA_REFRESH_CYCLIC_MROWS},
        //   {"random",       MMAL_VIDEO_INTRA_REFRESH_PSEUDO_RAND} Cannot use random, crashes the encoder. No idea why.
};

static int intra_refresh_map_size = sizeof(intra_refresh_map) / sizeof(intra_refresh_map[0]);

static XREF_T raw_output_fmt_map[] =
    {
        {"yuv", RAW_OUTPUT_FMT_YUV},
        {"rgb", RAW_OUTPUT_FMT_RGB},
        {"gray", RAW_OUTPUT_FMT_GRAY},
};

static int raw_output_fmt_map_size = sizeof(raw_output_fmt_map) / sizeof(raw_output_fmt_map[0]);

static struct
{
    char *description;
    int nextWaitMethod;
} wait_method_description[] =
    {
        {"Simple capture", WAIT_METHOD_NONE},
        {"Capture forever", WAIT_METHOD_FOREVER},
        {"Cycle on time", WAIT_METHOD_TIMED},
        {"Cycle on keypress", WAIT_METHOD_KEYPRESS},
        {"Cycle on signal", WAIT_METHOD_SIGNAL},
};

static struct
{
    char *description;
    int nextFrameMethod;
} next_frame_description[] =
    {
        {"Single capture", FRAME_NEXT_SINGLE},
        {"Capture on timelapse", FRAME_NEXT_TIMELAPSE},
        {"Capture on keypress", FRAME_NEXT_KEYPRESS},
        {"Run forever", FRAME_NEXT_FOREVER},
        {"Capture on GPIO", FRAME_NEXT_GPIO},
        {"Capture on signal", FRAME_NEXT_SIGNAL},
};

static int wait_method_description_size = sizeof(wait_method_description) / sizeof(wait_method_description[0]);

typedef enum OV528_Command
{
    INIT = 0x01,
    GET_PICTURE = 0x04,
    SNAPSHOT = 0x05,
    SET_PACKAGE_SIZE = 0x06,
    SET_BAUD_RATE = 0x07,
    RESET = 0x08,
    POWER_DOWN = 0x09,
    DATA = 0x0A,
    SYNC = 0x0D,
    ACK = 0x0E,
    NAK = 0x0F
};

enum ColorSetting
{
    SET_2_BIT_GRAY_SCALE = 0x01,
    SET_4_BIT_GRAY_SCALE = 0x02,
    SET_8_BIT_GRAY_SCALE = 0x03,
    SET_2_BIT_COLOR = 0x05,
    SET_16_BIT_COLOR = 0x06,
    SET_JPEG = 0x07
};

enum JPEG_Resolution
{
    RES_160x128 = 0x03,
    RES_320x240 = 0x05,
    RES_640x480 = 0x07
};

enum GetPicture
{
    GET_SNAPSHOT = 0x01,
    GET_PREVIEW_PICTURE = 0x02,
    GET_JPEG_PREVIEW_PICTURE = 0x03
};

enum Snapshot
{
    SNAPSHOT_COMPRESSED_PICTURE = 0x00,
    SNAPSHOT_UNCOMPRESSED_PICTURE = 0x01
};

enum Data
{
    DATA_SNAPSHOT_PICTURE = 0x01,
    DATA_PREVIEW_PICTURE = 0x02,
    DATA_JPEG_PICTURE = 0x05
};

/**
 * Assign a default set of parameters to the state passed in
 *
 * @param state Pointer to state structure to assign defaults to
 */
static void default_status(RASPIVID_STATE *state)
{
    if (!state)
    {
        vcos_assert(0);
        return;
    }

    // Default everything to zero
    memset(state, 0, sizeof(RASPIVID_STATE));

    raspicommonsettings_set_defaults(&state->common_settings);

    // Video capture default parameters
    state->timeout = 5000;               // replaced with 5000ms later if unset
    state->common_settings.width = 1920; // Default to 1080p
    state->common_settings.height = 1080;
    state->encoding = MMAL_ENCODING_H264;
    state->bitrate = 17000000; // This is a decent default bitrate for 1080p
    state->framerate = VIDEO_FRAME_RATE_NUM;
    state->intraperiod = -1; // Not set
    state->quantisationParameter = 0;
    state->demoMode = 0;
    state->demoInterval = 250; // ms
    state->immutableInput = 1;
    state->profile = MMAL_VIDEO_PROFILE_H264_HIGH;
    state->level = MMAL_VIDEO_LEVEL_H264_4;
    state->waitMethod = WAIT_METHOD_NONE;
    state->onTime = 5000;
    state->offTime = 5000;
    state->bCapturing = 0;
    state->bInlineHeaders = 0;
    state->segmentSize = 0; // 0 = not segmenting the file.
    state->segmentNumber = 1;
    state->segmentWrap = 0; // Point at which to wrap segment number back to 1. 0 = no wrap
    state->splitNow = 0;
    state->splitWait = 0;
    state->inlineMotionVectors = 0;
    state->intra_refresh_type = -1;
    state->frame = 0;
    state->save_pts = 0;
    state->netListen = false;
    state->addSPSTiming = MMAL_FALSE;
    state->slices = 1;

    // Image capture default parameters
    state->quality = 85;
    state->wantRAW = 0;
    state->linkname = NULL;
    state->frameStart = 0;
    state->thumbnailConfig.enable = 1;
    state->thumbnailConfig.width = 64;
    state->thumbnailConfig.height = 48;
    state->thumbnailConfig.quality = 35;
    state->demoMode = 0;
    state->demoInterval = 250; // ms
    state->camera_component = NULL;
    state->encoder_component_image = NULL;
    state->preview_connection = NULL;
    state->encoder_connection_image = NULL;
    state->encoder_pool_image = NULL;
    state->encoding_image = MMAL_ENCODING_JPEG;
    state->numExifTags = 0;
    state->enableExifTags = 1;
    state->timelapse = 0;
    state->fullResPreview = 0;
    state->frameNextMethod = FRAME_NEXT_SINGLE;
    state->useGL = 0;
    state->glCapture = 0;
    state->burstCaptureMode = 0;
    state->datetime = 0;
    state->timestamp = 0;
    state->restart_interval = 0;

    // Setup preview window defaults
    raspipreview_set_defaults(&state->preview_parameters);

    // Set up the camera_parameters to default
    raspicamcontrol_set_defaults(&state->camera_parameters);
}

static void check_camera_model(int cam_num)
{
    MMAL_COMPONENT_T *camera_info;
    MMAL_STATUS_T status;

    // Try to get the camera name
    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA_INFO, &camera_info);
    if (status == MMAL_SUCCESS)
    {
        MMAL_PARAMETER_CAMERA_INFO_T param;
        param.hdr.id = MMAL_PARAMETER_CAMERA_INFO;
        param.hdr.size = sizeof(param) - 4; // Deliberately undersize to check firmware version
        status = mmal_port_parameter_get(camera_info->control, &param.hdr);

        if (status != MMAL_SUCCESS)
        {
            // Running on newer firmware
            param.hdr.size = sizeof(param);
            status = mmal_port_parameter_get(camera_info->control, &param.hdr);
            if (status == MMAL_SUCCESS && param.num_cameras > cam_num)
            {
                if (!strncmp(param.cameras[cam_num].camera_name, "toshh2c", 7))
                {
                    fprintf(stderr, "The driver for the TC358743 HDMI to CSI2 chip you are using is NOT supported.\n");
                    fprintf(stderr, "They were written for a demo purposes only, and are in the firmware on an as-is\n");
                    fprintf(stderr, "basis and therefore requests for support or changes will not be acted on.\n\n");
                }
            }
        }

        mmal_component_destroy(camera_info);
    }
}

/**
 * Dump image state parameters to stderr.
 *
 * @param state Pointer to state structure to assign defaults to
 */
static void dump_status(RASPIVID_STATE *state)
{
    int i;

    if (!state)
    {
        vcos_assert(0);
        return;
    }

    raspicommonsettings_dump_parameters(&state->common_settings);

    fprintf(stderr, "bitrate %d, framerate %d, time delay %d\n", state->bitrate, state->framerate, state->timeout);
    fprintf(stderr, "H264 Profile %s\n", raspicli_unmap_xref(state->profile, profile_map, profile_map_size));
    fprintf(stderr, "H264 Level %s\n", raspicli_unmap_xref(state->level, level_map, level_map_size));
    fprintf(stderr, "H264 Quantisation level %d, Inline headers %s\n", state->quantisationParameter, state->bInlineHeaders ? "Yes" : "No");
    fprintf(stderr, "H264 Fill SPS Timings %s\n", state->addSPSTiming ? "Yes" : "No");
    fprintf(stderr, "H264 Intra refresh type %s, period %d\n", raspicli_unmap_xref(state->intra_refresh_type, intra_refresh_map, intra_refresh_map_size), state->intraperiod);
    fprintf(stderr, "H264 Slices %d\n", state->slices);

    // Not going to display segment data unless asked for it.
    if (state->segmentSize)
        fprintf(stderr, "Segment size %d, segment wrap value %d, initial segment number %d\n", state->segmentSize, state->segmentWrap, state->segmentNumber);

    if (state->raw_output)
        fprintf(stderr, "Raw output enabled, format %s\n", raspicli_unmap_xref(state->raw_output_fmt, raw_output_fmt_map, raw_output_fmt_map_size));

    fprintf(stderr, "Wait method : ");
    for (i = 0; i < wait_method_description_size; i++)
    {
        if (state->waitMethod == wait_method_description[i].nextWaitMethod)
            fprintf(stderr, "%s", wait_method_description[i].description);
    }
    fprintf(stderr, "\nInitial state '%s'\n", raspicli_unmap_xref(state->bCapturing, initial_map, initial_map_size));
    fprintf(stderr, "\n\n");

    raspipreview_dump_parameters(&state->preview_parameters);
    raspicamcontrol_dump_parameters(&state->camera_parameters);
}

/**
 * Open a file based on the settings in state
 *
 * @param state Pointer to state
 */
static FILE *open_filename(RASPIVID_STATE *pState, char *filename)
{
    FILE *new_handle = NULL;
    char *tempname = NULL;

    if (pState->segmentSize || pState->splitWait)
    {
        // Create a new filename string

        //If %d/%u or any valid combination e.g. %04d is specified, assume segment number.
        bool bSegmentNumber = false;
        const char *pPercent = strchr(filename, '%');
        if (pPercent)
        {
            pPercent++;
            while (isdigit(*pPercent))
                pPercent++;
            if (*pPercent == 'u' || *pPercent == 'd')
                bSegmentNumber = true;
        }

        if (bSegmentNumber)
        {
            asprintf(&tempname, filename, pState->segmentNumber);
        }
        else
        {
            char temp_ts_str[100];
            time_t t = time(NULL);
            struct tm *tm = localtime(&t);
            strftime(temp_ts_str, 100, filename, tm);
            asprintf(&tempname, "%s", temp_ts_str);
        }

        filename = tempname;
    }

    if (filename)
    {
        bool bNetwork = false;
        int sfd = -1, socktype;

        if (!strncmp("tcp://", filename, 6))
        {
            bNetwork = true;
            socktype = SOCK_STREAM;
        }
        else if (!strncmp("udp://", filename, 6))
        {
            if (pState->netListen)
            {
                fprintf(stderr, "No support for listening in UDP mode\n");
                exit(131);
            }
            bNetwork = true;
            socktype = SOCK_DGRAM;
        }

        if (bNetwork)
        {
            unsigned short port;
            filename += 6;
            char *colon;
            if (NULL == (colon = strchr(filename, ':')))
            {
                fprintf(stderr, "%s is not a valid IPv4:port, use something like tcp://1.2.3.4:1234 or udp://1.2.3.4:1234\n",
                        filename);
                exit(132);
            }
            if (1 != sscanf(colon + 1, "%hu", &port))
            {
                fprintf(stderr,
                        "Port parse failed. %s is not a valid network file name, use something like tcp://1.2.3.4:1234 or udp://1.2.3.4:1234\n",
                        filename);
                exit(133);
            }
            char chTmp = *colon;
            *colon = 0;

            struct sockaddr_in saddr = {};
            saddr.sin_family = AF_INET;
            saddr.sin_port = htons(port);
            if (0 == inet_aton(filename, &saddr.sin_addr))
            {
                fprintf(stderr, "inet_aton failed. %s is not a valid IPv4 address\n",
                        filename);
                exit(134);
            }
            *colon = chTmp;

            if (pState->netListen)
            {
                int sockListen = socket(AF_INET, SOCK_STREAM, 0);
                if (sockListen >= 0)
                {
                    int iTmp = 1;
                    setsockopt(sockListen, SOL_SOCKET, SO_REUSEADDR, &iTmp, sizeof(int)); //no error handling, just go on
                    if (bind(sockListen, (struct sockaddr *)&saddr, sizeof(saddr)) >= 0)
                    {
                        while ((-1 == (iTmp = listen(sockListen, 0))) && (EINTR == errno))
                            ;
                        if (-1 != iTmp)
                        {
                            fprintf(stderr, "Waiting for a TCP connection on %s:%" SCNu16 "...",
                                    inet_ntoa(saddr.sin_addr), ntohs(saddr.sin_port));
                            struct sockaddr_in cli_addr;
                            socklen_t clilen = sizeof(cli_addr);
                            while ((-1 == (sfd = accept(sockListen, (struct sockaddr *)&cli_addr, &clilen))) && (EINTR == errno))
                                ;
                            if (sfd >= 0)
                                fprintf(stderr, "Client connected from %s:%" SCNu16 "\n", inet_ntoa(cli_addr.sin_addr), ntohs(cli_addr.sin_port));
                            else
                                fprintf(stderr, "Error on accept: %s\n", strerror(errno));
                        }
                        else //if (-1 != iTmp)
                        {
                            fprintf(stderr, "Error trying to listen on a socket: %s\n", strerror(errno));
                        }
                    }
                    else //if (bind(sockListen, (struct sockaddr *) &saddr, sizeof(saddr)) >= 0)
                    {
                        fprintf(stderr, "Error on binding socket: %s\n", strerror(errno));
                    }
                }
                else //if (sockListen >= 0)
                {
                    fprintf(stderr, "Error creating socket: %s\n", strerror(errno));
                }

                if (sockListen >= 0)   //regardless success or error
                    close(sockListen); //do not listen on a given port anymore
            }
            else //if (pState->netListen)
            {
                if (0 <= (sfd = socket(AF_INET, socktype, 0)))
                {
                    fprintf(stderr, "Connecting to %s:%hu...", inet_ntoa(saddr.sin_addr), port);

                    int iTmp = 1;
                    while ((-1 == (iTmp = connect(sfd, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in)))) && (EINTR == errno))
                        ;
                    if (iTmp < 0)
                        fprintf(stderr, "error: %s\n", strerror(errno));
                    else
                        fprintf(stderr, "connected, sending video...\n");
                }
                else
                    fprintf(stderr, "Error creating socket: %s\n", strerror(errno));
            }

            if (sfd >= 0)
                new_handle = fdopen(sfd, "w");
        }
        else
        {
            new_handle = fopen(filename, "wb");
        }
    }

    if (pState->common_settings.verbose)
    {
        if (new_handle)
            fprintf(stderr, "Opening output file \"%s\"\n", filename);
        else
            fprintf(stderr, "Failed to open new file \"%s\"\n", filename);
    }

    if (tempname)
        free(tempname);

    return new_handle;
}

/**
 * Update any annotation data specific to the video.
 * This simply passes on the setting from cli, or
 * if application defined annotate requested, updates
 * with the H264 parameters
 *
 * @param state Pointer to state control struct
 *
 */
static void update_annotation_data(RASPIVID_STATE *state)
{
    // So, if we have asked for a application supplied string, set it to the H264 or GPS parameters
    if (state->camera_parameters.enable_annotate & ANNOTATE_APP_TEXT)
    {
        char *text;

        if (state->common_settings.gps)
        {
            text = raspi_gps_location_string();
        }
        else
        {
            const char *refresh = raspicli_unmap_xref(state->intra_refresh_type, intra_refresh_map, intra_refresh_map_size);

            asprintf(&text, "%dk,%df,%s,%d,%s,%s",
                     state->bitrate / 1000, state->framerate,
                     refresh ? refresh : "(none)",
                     state->intraperiod,
                     raspicli_unmap_xref(state->profile, profile_map, profile_map_size),
                     raspicli_unmap_xref(state->level, level_map, level_map_size));
        }

        raspicamcontrol_set_annotate(state->camera_component, state->camera_parameters.enable_annotate, text,
                                     state->camera_parameters.annotate_text_size,
                                     state->camera_parameters.annotate_text_colour,
                                     state->camera_parameters.annotate_bg_colour,
                                     state->camera_parameters.annotate_justify,
                                     state->camera_parameters.annotate_x,
                                     state->camera_parameters.annotate_y);

        free(text);
    }
    else
    {
        raspicamcontrol_set_annotate(state->camera_component, state->camera_parameters.enable_annotate, state->camera_parameters.annotate_string,
                                     state->camera_parameters.annotate_text_size,
                                     state->camera_parameters.annotate_text_colour,
                                     state->camera_parameters.annotate_bg_colour,
                                     state->camera_parameters.annotate_justify,
                                     state->camera_parameters.annotate_x,
                                     state->camera_parameters.annotate_y);
    }
}

/**
 *  buffer header callback function for encoder
 *
 *  Callback will dump buffer data to the specific file
 *
 * @param port Pointer to port from which callback originated
 * @param buffer mmal buffer header pointer
 */
static void encoder_buffer_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    MMAL_BUFFER_HEADER_T *new_buffer;
    static int64_t base_time = -1;
    static int64_t last_second = -1;

    // All our segment times based on the receipt of the first encoder callback
    if (base_time == -1)
        base_time = get_microseconds64() / 1000;

    // We pass our file handle and other stuff in via the userdata field.

    PORT_USERDATA *pData = (PORT_USERDATA *)port->userdata;

    if (pData)
    {
        int bytes_written = buffer->length;
        // printf("Buffer length in callback: %d \n", bytes_written);
        int64_t current_time = get_microseconds64() / 1000;

        vcos_assert(pData->file_handle);
        if (pData->pstate->inlineMotionVectors)
            vcos_assert(pData->imv_file_handle);

        if (pData->cb_buff)
        {
            int space_in_buff = pData->cb_len - pData->cb_wptr;
            int copy_to_end = space_in_buff > buffer->length ? buffer->length : space_in_buff;
            int copy_to_start = buffer->length - copy_to_end;

            if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG)
            {
                if (pData->header_wptr + buffer->length > sizeof(pData->header_bytes))
                {
                    vcos_log_error("Error in header bytes\n");
                }
                else
                {
                    // These are the header bytes, save them for final output
                    mmal_buffer_header_mem_lock(buffer);
                    memcpy(pData->header_bytes + pData->header_wptr, buffer->data, buffer->length);
                    mmal_buffer_header_mem_unlock(buffer);
                    pData->header_wptr += buffer->length;
                }
            }
            else if ((buffer->flags & MMAL_BUFFER_HEADER_FLAG_CODECSIDEINFO))
            {
                // Do something with the inline motion vectors...
            }
            else
            {
                static int frame_start = -1;
                int i;

                if (frame_start == -1)
                    frame_start = pData->cb_wptr;

                if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_KEYFRAME)
                {
                    pData->iframe_buff[pData->iframe_buff_wpos] = frame_start;
                    pData->iframe_buff_wpos = (pData->iframe_buff_wpos + 1) % IFRAME_BUFSIZE;
                }

                if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END)
                    frame_start = -1;

                // If we overtake the iframe rptr then move the rptr along
                if ((pData->iframe_buff_rpos + 1) % IFRAME_BUFSIZE != pData->iframe_buff_wpos)
                {
                    while (
                        (
                            pData->cb_wptr <= pData->iframe_buff[pData->iframe_buff_rpos] &&
                            (pData->cb_wptr + buffer->length) > pData->iframe_buff[pData->iframe_buff_rpos]) ||
                        ((pData->cb_wptr > pData->iframe_buff[pData->iframe_buff_rpos]) &&
                         (pData->cb_wptr + buffer->length) > (pData->iframe_buff[pData->iframe_buff_rpos] + pData->cb_len)))
                        pData->iframe_buff_rpos = (pData->iframe_buff_rpos + 1) % IFRAME_BUFSIZE;
                }

                mmal_buffer_header_mem_lock(buffer);
                // We are pushing data into a circular buffer
                memcpy(pData->cb_buff + pData->cb_wptr, buffer->data, copy_to_end);
                memcpy(pData->cb_buff, buffer->data + copy_to_end, copy_to_start);
                mmal_buffer_header_mem_unlock(buffer);

                if ((pData->cb_wptr + buffer->length) > pData->cb_len)
                    pData->cb_wrap = 1;

                pData->cb_wptr = (pData->cb_wptr + buffer->length) % pData->cb_len;

                for (i = pData->iframe_buff_rpos; i != pData->iframe_buff_wpos; i = (i + 1) % IFRAME_BUFSIZE)
                {
                    int p = pData->iframe_buff[i];
                    if (pData->cb_buff[p] != 0 || pData->cb_buff[p + 1] != 0 || pData->cb_buff[p + 2] != 0 || pData->cb_buff[p + 3] != 1)
                    {
                        vcos_log_error("Error in iframe list\n");
                    }
                }
            }
        }
        else
        {
            // For segmented record mode, we need to see if we have exceeded our time/size,
            // but also since we have inline headers turned on we need to break when we get one to
            // ensure that the new stream has the header in it. If we break on an I-frame, the
            // SPS/PPS header is actually in the previous chunk.
            if ((buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG) &&
                ((pData->pstate->segmentSize && current_time > base_time + pData->pstate->segmentSize) ||
                 (pData->pstate->splitWait && pData->pstate->splitNow)))
            {
                FILE *new_handle;

                base_time = current_time;

                pData->pstate->splitNow = 0;
                pData->pstate->segmentNumber++;

                // Only wrap if we have a wrap point set
                if (pData->pstate->segmentWrap && pData->pstate->segmentNumber > pData->pstate->segmentWrap)
                    pData->pstate->segmentNumber = 1;

                if (pData->pstate->common_settings.filename && pData->pstate->common_settings.filename[0] != '-')
                {
                    new_handle = open_filename(pData->pstate, pData->pstate->common_settings.filename);

                    if (new_handle)
                    {
                        fclose(pData->file_handle);
                        pData->file_handle = new_handle;
                    }
                }

                if (pData->pstate->imv_filename && pData->pstate->imv_filename[0] != '-')
                {
                    new_handle = open_filename(pData->pstate, pData->pstate->imv_filename);

                    if (new_handle)
                    {
                        fclose(pData->imv_file_handle);
                        pData->imv_file_handle = new_handle;
                    }
                }

                if (pData->pstate->pts_filename && pData->pstate->pts_filename[0] != '-')
                {
                    new_handle = open_filename(pData->pstate, pData->pstate->pts_filename);

                    if (new_handle)
                    {
                        fclose(pData->pts_file_handle);
                        pData->pts_file_handle = new_handle;
                    }
                }
            }
            if (buffer->length)
            {
                // printf("DEBUG_BUF\n");
                mmal_buffer_header_mem_lock(buffer);
                if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CODECSIDEINFO)
                {
                    if (pData->pstate->inlineMotionVectors)
                    {
                        bytes_written = fwrite(buffer->data, 1, buffer->length, pData->imv_file_handle);
                        if (pData->flush_buffers)
                            fflush(pData->imv_file_handle);
                    }
                    else
                    {
                        //We do not want to save inlineMotionVectors...
                        bytes_written = buffer->length;
                    }
                }
                else
                {
                    bytes_written = fwrite(buffer->data, 1, buffer->length, pData->file_handle);
                    if (pData->flush_buffers)
                    {
                        fflush(pData->file_handle);
                        fdatasync(fileno(pData->file_handle));
                    }

                    if (pData->pstate->save_pts &&
                        !(buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG) &&
                        buffer->pts != MMAL_TIME_UNKNOWN &&
                        buffer->pts != pData->pstate->lasttime)
                    {
                        int64_t pts;
                        if (pData->pstate->frame == 0)
                            pData->pstate->starttime = buffer->pts;
                        pData->pstate->lasttime = buffer->pts;
                        pts = buffer->pts - pData->pstate->starttime;
                        fprintf(pData->pts_file_handle, "%lld.%03lld\n", pts / 1000, pts % 1000);
                        pData->pstate->frame++;
                    }
                }

                mmal_buffer_header_mem_unlock(buffer);

                if (bytes_written != buffer->length)
                {
                    vcos_log_error("Failed to write buffer data (%d from %d)- aborting", bytes_written, buffer->length);
                    pData->abort = 1;
                }
            }
        }

        // See if the second count has changed and we need to update any annotation
        if (current_time / 1000 != last_second)
        {
            update_annotation_data(pData->pstate);
            last_second = current_time / 1000;
        }
    }
    else
    {
        vcos_log_error("Received a encoder buffer callback with no state");
    }

    // release buffer back to the pool
    mmal_buffer_header_release(buffer);

    // and send one back to the port (if still open)
    if (port->is_enabled)
    {
        MMAL_STATUS_T status;

        new_buffer = mmal_queue_get(pData->pstate->encoder_pool->queue);

        if (new_buffer)
            status = mmal_port_send_buffer(port, new_buffer);

        if (!new_buffer || status != MMAL_SUCCESS)
            vcos_log_error("Unable to return a buffer to the encoder port");
    }
}

/**
 *  buffer header callback function for splitter
 *
 *  Callback will dump buffer data to the specific file
 *
 * @param port Pointer to port from which callback originated
 * @param buffer mmal buffer header pointer
 */
static void splitter_buffer_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    MMAL_BUFFER_HEADER_T *new_buffer;
    PORT_USERDATA *pData = (PORT_USERDATA *)port->userdata;

    if (pData)
    {
        int bytes_written = 0;
        int bytes_to_write = buffer->length;

        /* Write only luma component to get grayscale image: */
        if (buffer->length && pData->pstate->raw_output_fmt == RAW_OUTPUT_FMT_GRAY)
            bytes_to_write = port->format->es->video.width * port->format->es->video.height;

        vcos_assert(pData->raw_file_handle);

        if (bytes_to_write)
        {
            mmal_buffer_header_mem_lock(buffer);
            bytes_written = fwrite(buffer->data, 1, bytes_to_write, pData->raw_file_handle);
            mmal_buffer_header_mem_unlock(buffer);

            if (bytes_written != bytes_to_write)
            {
                vcos_log_error("Failed to write raw buffer data (%d from %d)- aborting", bytes_written, bytes_to_write);
                pData->abort = 1;
            }
        }
    }
    else
    {
        vcos_log_error("Received a camera buffer callback with no state");
    }

    // release buffer back to the pool
    mmal_buffer_header_release(buffer);

    // and send one back to the port (if still open)
    if (port->is_enabled)
    {
        MMAL_STATUS_T status;

        new_buffer = mmal_queue_get(pData->pstate->splitter_pool->queue);

        if (new_buffer)
            status = mmal_port_send_buffer(port, new_buffer);

        if (!new_buffer || status != MMAL_SUCCESS)
            vcos_log_error("Unable to return a buffer to the splitter port");
    }
}

/**
 * Create the camera component, set up its ports
 *
 * @param state Pointer to state control struct
 *
 * @return MMAL_SUCCESS if all OK, something else otherwise
 *
 */
static MMAL_STATUS_T create_camera_component(RASPIVID_STATE *state)
{
    MMAL_COMPONENT_T *camera = 0;
    MMAL_ES_FORMAT_T *format;
    MMAL_PORT_T *preview_port = NULL, *video_port = NULL, *still_port = NULL;
    MMAL_STATUS_T status;

    /* Create the component */
    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &camera);

    if (status != MMAL_SUCCESS)
    {
        vcos_log_error("Failed to create camera component");
        goto error;
    }

    status = raspicamcontrol_set_stereo_mode(camera->output[0], &state->camera_parameters.stereo_mode);
    status += raspicamcontrol_set_stereo_mode(camera->output[1], &state->camera_parameters.stereo_mode);
    status += raspicamcontrol_set_stereo_mode(camera->output[2], &state->camera_parameters.stereo_mode);

    if (status != MMAL_SUCCESS)
    {
        vcos_log_error("Could not set stereo mode : error %d", status);
        goto error;
    }

    MMAL_PARAMETER_INT32_T camera_num =
        {{MMAL_PARAMETER_CAMERA_NUM, sizeof(camera_num)}, state->common_settings.cameraNum};

    status = mmal_port_parameter_set(camera->control, &camera_num.hdr);

    if (status != MMAL_SUCCESS)
    {
        vcos_log_error("Could not select camera : error %d", status);
        goto error;
    }

    if (!camera->output_num)
    {
        status = MMAL_ENOSYS;
        vcos_log_error("Camera doesn't have output ports");
        goto error;
    }

    status = mmal_port_parameter_set_uint32(camera->control, MMAL_PARAMETER_CAMERA_CUSTOM_SENSOR_CONFIG, state->common_settings.sensor_mode);

    if (status != MMAL_SUCCESS)
    {
        vcos_log_error("Could not set sensor mode : error %d", status);
        goto error;
    }

    preview_port = camera->output[MMAL_CAMERA_PREVIEW_PORT];
    video_port = camera->output[MMAL_CAMERA_VIDEO_PORT];
    still_port = camera->output[MMAL_CAMERA_CAPTURE_PORT];

    // Enable the camera, and tell it its control callback function
    status = mmal_port_enable(camera->control, default_camera_control_callback);

    if (status != MMAL_SUCCESS)
    {
        vcos_log_error("Unable to enable control port : error %d", status);
        goto error;
    }

    //  set up the camera configuration
    {
        MMAL_PARAMETER_CAMERA_CONFIG_T cam_config =
            {
                {MMAL_PARAMETER_CAMERA_CONFIG, sizeof(cam_config)},
                .max_stills_w = state->common_settings.width,
                .max_stills_h = state->common_settings.height,
                .stills_yuv422 = 0,
                .one_shot_stills = 0,
                .max_preview_video_w = state->common_settings.width,
                .max_preview_video_h = state->common_settings.height,
                .num_preview_video_frames = 3 + vcos_max(0, (state->framerate - 30) / 10),
                .stills_capture_circular_buffer_height = 0,
                .fast_preview_resume = 0,
                .use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RAW_STC};
        mmal_port_parameter_set(camera->control, &cam_config.hdr);
    }

    // Now set up the port formats

    // Set the encode format on the Preview port
    // HW limitations mean we need the preview to be the same size as the required recorded output

    format = preview_port->format;

    format->encoding = MMAL_ENCODING_OPAQUE;
    format->encoding_variant = MMAL_ENCODING_I420;

    if (state->camera_parameters.shutter_speed > 6000000)
    {
        MMAL_PARAMETER_FPS_RANGE_T fps_range = {{MMAL_PARAMETER_FPS_RANGE, sizeof(fps_range)},
                                                {5, 1000},
                                                {166, 1000}};
        mmal_port_parameter_set(preview_port, &fps_range.hdr);
    }
    else if (state->camera_parameters.shutter_speed > 1000000)
    {
        MMAL_PARAMETER_FPS_RANGE_T fps_range = {{MMAL_PARAMETER_FPS_RANGE, sizeof(fps_range)},
                                                {166, 1000},
                                                {999, 1000}};
        mmal_port_parameter_set(preview_port, &fps_range.hdr);
    }

    //enable dynamic framerate if necessary
    if (state->camera_parameters.shutter_speed)
    {
        if (state->framerate > 1000000. / state->camera_parameters.shutter_speed)
        {
            state->framerate = 0;
            if (state->common_settings.verbose)
                fprintf(stderr, "Enable dynamic frame rate to fulfil shutter speed requirement\n");
        }
    }

    format->encoding = MMAL_ENCODING_OPAQUE;
    format->es->video.width = VCOS_ALIGN_UP(state->common_settings.width, 32);
    format->es->video.height = VCOS_ALIGN_UP(state->common_settings.height, 16);
    format->es->video.crop.x = 0;
    format->es->video.crop.y = 0;
    format->es->video.crop.width = state->common_settings.width;
    format->es->video.crop.height = state->common_settings.height;
    format->es->video.frame_rate.num = state->framerate;
    format->es->video.frame_rate.den = VIDEO_FRAME_RATE_DEN;

    status = mmal_port_format_commit(preview_port);

    if (status != MMAL_SUCCESS)
    {
        vcos_log_error("camera viewfinder format couldn't be set");
        goto error;
    }

    // Set the encode format on the video  port

    format = video_port->format;
    format->encoding_variant = MMAL_ENCODING_I420;

    if (state->camera_parameters.shutter_speed > 6000000)
    {
        MMAL_PARAMETER_FPS_RANGE_T fps_range = {{MMAL_PARAMETER_FPS_RANGE, sizeof(fps_range)},
                                                {5, 1000},
                                                {166, 1000}};
        mmal_port_parameter_set(video_port, &fps_range.hdr);
    }
    else if (state->camera_parameters.shutter_speed > 1000000)
    {
        MMAL_PARAMETER_FPS_RANGE_T fps_range = {{MMAL_PARAMETER_FPS_RANGE, sizeof(fps_range)},
                                                {167, 1000},
                                                {999, 1000}};
        mmal_port_parameter_set(video_port, &fps_range.hdr);
    }

    format->encoding = MMAL_ENCODING_OPAQUE;
    format->es->video.width = VCOS_ALIGN_UP(state->common_settings.width, 32);
    format->es->video.height = VCOS_ALIGN_UP(state->common_settings.height, 16);
    format->es->video.crop.x = 0;
    format->es->video.crop.y = 0;
    format->es->video.crop.width = state->common_settings.width;
    format->es->video.crop.height = state->common_settings.height;
    format->es->video.frame_rate.num = state->framerate;
    format->es->video.frame_rate.den = VIDEO_FRAME_RATE_DEN;

    status = mmal_port_format_commit(video_port);

    if (status != MMAL_SUCCESS)
    {
        vcos_log_error("camera video format couldn't be set");
        goto error;
    }

    // Ensure there are enough buffers to avoid dropping frames
    if (video_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM)
        video_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;

    // Set the encode format on the still  port

    format = still_port->format;

    format->encoding = MMAL_ENCODING_OPAQUE;
    format->encoding_variant = MMAL_ENCODING_I420;

    format->es->video.width = VCOS_ALIGN_UP(state->common_settings.width, 32);
    format->es->video.height = VCOS_ALIGN_UP(state->common_settings.height, 16);
    format->es->video.crop.x = 0;
    format->es->video.crop.y = 0;
    format->es->video.crop.width = state->common_settings.width;
    format->es->video.crop.height = state->common_settings.height;
    format->es->video.frame_rate.num = 0;
    format->es->video.frame_rate.den = 1;

    status = mmal_port_format_commit(still_port);

    if (status != MMAL_SUCCESS)
    {
        vcos_log_error("camera still format couldn't be set");
        goto error;
    }

    /* Ensure there are enough buffers to avoid dropping frames */
    if (still_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM)
        still_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;

    /* Enable component */
    status = mmal_component_enable(camera);

    if (status != MMAL_SUCCESS)
    {
        vcos_log_error("camera component couldn't be enabled");
        goto error;
    }

    // Note: this sets lots of parameters that were not individually addressed before.
    raspicamcontrol_set_all_parameters(camera, &state->camera_parameters);

    state->camera_component = camera;

    update_annotation_data(state);

    if (state->common_settings.verbose)
        fprintf(stderr, "Camera component done\n");

    return status;

error:

    if (camera)
        mmal_component_destroy(camera);

    return status;
}

/**
 * Destroy the camera component
 *
 * @param state Pointer to state control struct
 *
 */
static void destroy_camera_component(RASPIVID_STATE *state)
{
    if (state->camera_component)
    {
        mmal_component_destroy(state->camera_component);
        state->camera_component = NULL;
    }
}

/**
 * Create the splitter component, set up its ports
 *
 * @param state Pointer to state control struct
 *
 * @return MMAL_SUCCESS if all OK, something else otherwise
 *
 */
static MMAL_STATUS_T create_splitter_component(RASPIVID_STATE *state)
{
    MMAL_COMPONENT_T *splitter = 0;
    MMAL_PORT_T *splitter_output = NULL;
    MMAL_PORT_T *splitter_output_image = NULL;
    MMAL_ES_FORMAT_T *format;
    MMAL_STATUS_T status;
    MMAL_POOL_T *pool, *pool_image;
    int i;

    if (state->camera_component == NULL)
    {
        status = MMAL_ENOSYS;
        vcos_log_error("Camera component must be created before splitter");
        goto error;
    }

    /* Create the component */
    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_SPLITTER, &splitter);

    if (status != MMAL_SUCCESS)
    {
        vcos_log_error("Failed to create splitter component");
        goto error;
    }

    if (!splitter->input_num)
    {
        status = MMAL_ENOSYS;
        vcos_log_error("Splitter doesn't have any input port");
        goto error;
    }

    if (splitter->output_num < 2)
    {
        status = MMAL_ENOSYS;
        vcos_log_error("Splitter doesn't have enough output ports");
        goto error;
    }

    /* Ensure there are enough buffers to avoid dropping frames: */
    mmal_format_copy(splitter->input[0]->format, state->camera_component->output[MMAL_CAMERA_VIDEO_PORT]->format);

    if (splitter->input[0]->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM)
        splitter->input[0]->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;

    status = mmal_port_format_commit(splitter->input[0]);

    if (status != MMAL_SUCCESS)
    {
        vcos_log_error("Unable to set format on splitter input port");
        goto error;
    }

    /* Splitter can do format conversions, configure format for its output port: */
    for (i = 0; i < splitter->output_num; i++)
    {
        if (i == 1)
        {
            splitter->output[i]->buffer_num = 1;
        }
        else
        {
            splitter->output[i]->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;
        }
        mmal_format_copy(splitter->output[i]->format, splitter->input[0]->format);
        format = splitter->output[i]->format;
        format->encoding = MMAL_ENCODING_I420;
        format->encoding_variant = MMAL_ENCODING_I420;

        status = mmal_port_format_commit(splitter->output[i]);

        if (status != MMAL_SUCCESS)
        {
            vcos_log_error("Unable to set format on splitter output port %d", i);
            goto error;
        }
    }
    format = splitter->output[1]->format;
    format->encoding = MMAL_ENCODING_RGB24;
    format->encoding_variant = 0;

    splitter->output[1]->buffer_size = splitter->output[1]->buffer_size_min;
    splitter->output[1]->buffer_num = splitter->output[1]->buffer_num_recommended;
    status = mmal_port_format_commit(splitter->output[1]);
    if (status != MMAL_SUCCESS)
    {
        vcos_log_error("Unable to set format on splitter output port %d", 1);
        goto error;
    }
    status = mmal_component_enable(splitter);
    if (status != MMAL_SUCCESS)
    {
        vcos_log_error("splitter component couldn't be enabled");
        goto error;
    }
    pool = mmal_port_pool_create(splitter->output[1], splitter->output[1]->buffer_num, splitter->output[1]->buffer_size);
    if (!pool)
    {
        vcos_log_error("Failed to create buffer header pool for splitter output port %s", splitter->output[1]->name);
    }
    state->encoder_pool_image = pool;
    state->splitter_component = splitter;

    if (state->common_settings.verbose)
        fprintf(stderr, "Splitter component done\n");

    return status;

error:

    if (splitter)
        mmal_component_destroy(splitter);

    return status;
}

/**
 * Destroy the splitter component
 *
 * @param state Pointer to state control struct
 *
 */
static void destroy_splitter_component(RASPIVID_STATE *state)
{
    // Get rid of any port buffers first
    if (state->splitter_pool)
    {
        mmal_port_pool_destroy(state->splitter_component->output[SPLITTER_OUTPUT_PORT], state->splitter_pool);
    }

    if (state->splitter_component)
    {
        mmal_component_destroy(state->splitter_component);
        state->splitter_component = NULL;
    }
}

/**
 * Create the encoder component, set up its ports
 *
 * @param state Pointer to state control struct
 *
 * @return MMAL_SUCCESS if all OK, something else otherwise
 *
 */
static MMAL_STATUS_T create_encoder_component(RASPIVID_STATE *state)
{
    MMAL_COMPONENT_T *encoder = 0;
    MMAL_PORT_T *encoder_input = NULL, *encoder_output = NULL;
    MMAL_STATUS_T status;
    MMAL_POOL_T *pool;

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_ENCODER, &encoder);

    if (status != MMAL_SUCCESS)
    {
        vcos_log_error("Unable to create video encoder component");
        goto error;
    }

    if (!encoder->input_num || !encoder->output_num)
    {
        status = MMAL_ENOSYS;
        vcos_log_error("Video encoder doesn't have input/output ports");
        goto error;
    }

    encoder_input = encoder->input[0];
    encoder_output = encoder->output[0];

    // We want same format on input and output
    mmal_format_copy(encoder_output->format, encoder_input->format);

    // Only supporting H264 at the moment
    encoder_output->format->encoding = state->encoding;

    if (state->encoding == MMAL_ENCODING_H264)
    {
        if (state->level == MMAL_VIDEO_LEVEL_H264_4)
        {
            if (state->bitrate > MAX_BITRATE_LEVEL4)
            {
                fprintf(stderr, "Bitrate too high: Reducing to 25MBit/s\n");
                state->bitrate = MAX_BITRATE_LEVEL4;
            }
        }
        else
        {
            if (state->bitrate > MAX_BITRATE_LEVEL42)
            {
                fprintf(stderr, "Bitrate too high: Reducing to 62.5MBit/s\n");
                state->bitrate = MAX_BITRATE_LEVEL42;
            }
        }
    }
    else if (state->encoding == MMAL_ENCODING_MJPEG)
    {
        if (state->bitrate > MAX_BITRATE_MJPEG)
        {
            fprintf(stderr, "Bitrate too high: Reducing to 25MBit/s\n");
            state->bitrate = MAX_BITRATE_MJPEG;
        }
    }

    encoder_output->format->bitrate = state->bitrate;

    if (state->encoding == MMAL_ENCODING_H264)
        encoder_output->buffer_size = encoder_output->buffer_size_recommended;
    else
        encoder_output->buffer_size = 256 << 10;

    if (encoder_output->buffer_size < encoder_output->buffer_size_min)
        encoder_output->buffer_size = encoder_output->buffer_size_min;

    encoder_output->buffer_num = encoder_output->buffer_num_recommended;

    if (encoder_output->buffer_num < encoder_output->buffer_num_min)
        encoder_output->buffer_num = encoder_output->buffer_num_min;

    // We need to set the frame rate on output to 0, to ensure it gets
    // updated correctly from the input framerate when port connected
    encoder_output->format->es->video.frame_rate.num = 0;
    encoder_output->format->es->video.frame_rate.den = 1;

    // Commit the port changes to the output port
    status = mmal_port_format_commit(encoder_output);

    if (status != MMAL_SUCCESS)
    {
        vcos_log_error("Unable to set format on video encoder output port");
        goto error;
    }

    // Set the rate control parameter
    if (0)
    {
        MMAL_PARAMETER_VIDEO_RATECONTROL_T param = {{MMAL_PARAMETER_RATECONTROL, sizeof(param)}, MMAL_VIDEO_RATECONTROL_DEFAULT};
        status = mmal_port_parameter_set(encoder_output, &param.hdr);
        if (status != MMAL_SUCCESS)
        {
            vcos_log_error("Unable to set ratecontrol");
            goto error;
        }
    }

    if (state->encoding == MMAL_ENCODING_H264 &&
        state->intraperiod != -1)
    {
        MMAL_PARAMETER_UINT32_T param = {{MMAL_PARAMETER_INTRAPERIOD, sizeof(param)}, state->intraperiod};
        status = mmal_port_parameter_set(encoder_output, &param.hdr);
        if (status != MMAL_SUCCESS)
        {
            vcos_log_error("Unable to set intraperiod");
            goto error;
        }
    }

    if (state->encoding == MMAL_ENCODING_H264 && state->slices > 1 && state->common_settings.width <= 1280)
    {
        int frame_mb_rows = VCOS_ALIGN_UP(state->common_settings.height, 16) >> 4;

        if (state->slices > frame_mb_rows) //warn user if too many slices selected
        {
            fprintf(stderr, "H264 Slice count (%d) exceeds number of macroblock rows (%d). Setting slices to %d.\n", state->slices, frame_mb_rows, frame_mb_rows);
            // Continue rather than abort..
        }
        int slice_row_mb = frame_mb_rows / state->slices;
        if (frame_mb_rows - state->slices * slice_row_mb)
            slice_row_mb++; //must round up to avoid extra slice if not evenly divided

        status = mmal_port_parameter_set_uint32(encoder_output, MMAL_PARAMETER_MB_ROWS_PER_SLICE, slice_row_mb);
        if (status != MMAL_SUCCESS)
        {
            vcos_log_error("Unable to set number of slices");
            goto error;
        }
    }

    if (state->encoding == MMAL_ENCODING_H264 &&
        state->quantisationParameter)
    {
        MMAL_PARAMETER_UINT32_T param = {{MMAL_PARAMETER_VIDEO_ENCODE_INITIAL_QUANT, sizeof(param)}, state->quantisationParameter};
        status = mmal_port_parameter_set(encoder_output, &param.hdr);
        if (status != MMAL_SUCCESS)
        {
            vcos_log_error("Unable to set initial QP");
            goto error;
        }

        MMAL_PARAMETER_UINT32_T param2 = {{MMAL_PARAMETER_VIDEO_ENCODE_MIN_QUANT, sizeof(param)}, state->quantisationParameter};
        status = mmal_port_parameter_set(encoder_output, &param2.hdr);
        if (status != MMAL_SUCCESS)
        {
            vcos_log_error("Unable to set min QP");
            goto error;
        }

        MMAL_PARAMETER_UINT32_T param3 = {{MMAL_PARAMETER_VIDEO_ENCODE_MAX_QUANT, sizeof(param)}, state->quantisationParameter};
        status = mmal_port_parameter_set(encoder_output, &param3.hdr);
        if (status != MMAL_SUCCESS)
        {
            vcos_log_error("Unable to set max QP");
            goto error;
        }
    }

    if (state->encoding == MMAL_ENCODING_H264)
    {
        MMAL_PARAMETER_VIDEO_PROFILE_T param;
        param.hdr.id = MMAL_PARAMETER_PROFILE;
        param.hdr.size = sizeof(param);

        param.profile[0].profile = state->profile;

        if ((VCOS_ALIGN_UP(state->common_settings.width, 16) >> 4) * (VCOS_ALIGN_UP(state->common_settings.height, 16) >> 4) * state->framerate > 245760)
        {
            if ((VCOS_ALIGN_UP(state->common_settings.width, 16) >> 4) * (VCOS_ALIGN_UP(state->common_settings.height, 16) >> 4) * state->framerate <= 522240)
            {
                fprintf(stderr, "Too many macroblocks/s: Increasing H264 Level to 4.2\n");
                state->level = MMAL_VIDEO_LEVEL_H264_42;
            }
            else
            {
                vcos_log_error("Too many macroblocks/s requested");
                status = MMAL_EINVAL;
                goto error;
            }
        }

        param.profile[0].level = state->level;

        status = mmal_port_parameter_set(encoder_output, &param.hdr);
        if (status != MMAL_SUCCESS)
        {
            vcos_log_error("Unable to set H264 profile");
            goto error;
        }
    }

    if (mmal_port_parameter_set_boolean(encoder_input, MMAL_PARAMETER_VIDEO_IMMUTABLE_INPUT, state->immutableInput) != MMAL_SUCCESS)
    {
        vcos_log_error("Unable to set immutable input flag");
        // Continue rather than abort..
    }

    if (state->encoding == MMAL_ENCODING_H264)
    {
        //set INLINE HEADER flag to generate SPS and PPS for every IDR if requested
        if (mmal_port_parameter_set_boolean(encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_INLINE_HEADER, state->bInlineHeaders) != MMAL_SUCCESS)
        {
            vcos_log_error("failed to set INLINE HEADER FLAG parameters");
            // Continue rather than abort..
        }

        //set flag for add SPS TIMING
        if (mmal_port_parameter_set_boolean(encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_SPS_TIMING, state->addSPSTiming) != MMAL_SUCCESS)
        {
            vcos_log_error("failed to set SPS TIMINGS FLAG parameters");
            // Continue rather than abort..
        }

        //set INLINE VECTORS flag to request motion vector estimates
        if (mmal_port_parameter_set_boolean(encoder_output, MMAL_PARAMETER_VIDEO_ENCODE_INLINE_VECTORS, state->inlineMotionVectors) != MMAL_SUCCESS)
        {
            vcos_log_error("failed to set INLINE VECTORS parameters");
            // Continue rather than abort..
        }

        // Adaptive intra refresh settings
        if (state->intra_refresh_type != -1)
        {
            MMAL_PARAMETER_VIDEO_INTRA_REFRESH_T param;
            param.hdr.id = MMAL_PARAMETER_VIDEO_INTRA_REFRESH;
            param.hdr.size = sizeof(param);

            // Get first so we don't overwrite anything unexpectedly
            status = mmal_port_parameter_get(encoder_output, &param.hdr);
            if (status != MMAL_SUCCESS)
            {
                vcos_log_warn("Unable to get existing H264 intra-refresh values. Please update your firmware");
                // Set some defaults, don't just pass random stack data
                param.air_mbs = param.air_ref = param.cir_mbs = param.pir_mbs = 0;
            }

            param.refresh_mode = state->intra_refresh_type;

            //if (state->intra_refresh_type == MMAL_VIDEO_INTRA_REFRESH_CYCLIC_MROWS)
            //   param.cir_mbs = 10;

            status = mmal_port_parameter_set(encoder_output, &param.hdr);
            if (status != MMAL_SUCCESS)
            {
                vcos_log_error("Unable to set H264 intra-refresh values");
                goto error;
            }
        }
    }

    //  Enable component
    status = mmal_component_enable(encoder);

    if (status != MMAL_SUCCESS)
    {
        vcos_log_error("Unable to enable video encoder component");
        goto error;
    }

    /* Create pool of buffer headers for the output port to consume */
    pool = mmal_port_pool_create(encoder_output, encoder_output->buffer_num, encoder_output->buffer_size);

    if (!pool)
    {
        vcos_log_error("Failed to create buffer header pool for encoder output port %s", encoder_output->name);
    }

    state->encoder_pool = pool;
    state->encoder_component = encoder;

    if (state->common_settings.verbose)
        fprintf(stderr, "Encoder component done\n");

    return status;

error:
    if (encoder)
        mmal_component_destroy(encoder);

    state->encoder_component = NULL;

    return status;
}

/**
 * Destroy the encoder component
 *
 * @param state Pointer to state control struct
 *
 */
static void destroy_encoder_component(RASPIVID_STATE *state)
{
    // Get rid of any port buffers first
    if (state->encoder_pool)
    {
        mmal_port_pool_destroy(state->encoder_component->output[0], state->encoder_pool);
    }

    if (state->encoder_component)
    {
        mmal_component_destroy(state->encoder_component);
        state->encoder_component = NULL;
    }
}

/**
 * Pause for specified time, but return early if detect an abort request
 *
 * @param state Pointer to state control struct
 * @param pause Time in ms to pause
 * @param callback Struct contain an abort flag tested for early termination
 *
 */
static int pause_and_test_abort(RASPIVID_STATE *state, int pause)
{
    int wait;

    if (!pause)
        return 0;

    // Going to check every ABORT_INTERVAL milliseconds
    for (wait = 0; wait < pause; wait += ABORT_INTERVAL)
    {
        vcos_sleep(ABORT_INTERVAL);
        if (state->callback_data.abort)
            return 1;
    }

    return 0;
}

/**
 * Create the encoder component, set up its ports
 *
 * @param state Pointer to state control struct. encoder_component member set to the created camera_component if successful.
 *
 * @return a MMAL_STATUS, MMAL_SUCCESS if all OK, something else otherwise
 */
static MMAL_STATUS_T create_encoder_component_image(RASPIVID_STATE *state)
{
    MMAL_COMPONENT_T *encoder = 0;
    MMAL_PORT_T *encoder_input = NULL, *encoder_output = NULL;
    MMAL_STATUS_T status;
    MMAL_POOL_T *pool;

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_IMAGE_ENCODER, &encoder);

    if (status != MMAL_SUCCESS)
    {
        vcos_log_error("Unable to create JPEG encoder component");
        goto error;
    }

    if (!encoder->input_num || !encoder->output_num)
    {
        status = MMAL_ENOSYS;
        vcos_log_error("JPEG encoder doesn't have input/output ports");
        goto error;
    }

    encoder_input = encoder->input[0];
    encoder_output = encoder->output[0];

    // We want same format on input and output
    mmal_format_copy(encoder_output->format, encoder_input->format);

    // Specify out output format
    encoder_output->format->encoding = state->encoding_image;

    encoder_output->buffer_size = encoder_output->buffer_size_recommended;

    if (encoder_output->buffer_size < encoder_output->buffer_size_min)
        encoder_output->buffer_size = encoder_output->buffer_size_min;

    encoder_output->buffer_num = encoder_output->buffer_num_recommended;

    if (encoder_output->buffer_num < encoder_output->buffer_num_min)
        encoder_output->buffer_num = encoder_output->buffer_num_min;

    // Commit the port changes to the output port
    status = mmal_port_format_commit(encoder_output);

    if (status != MMAL_SUCCESS)
    {
        vcos_log_error("Unable to set format on image encoder output port");
        goto error;
    }

    // Set the JPEG quality level
    status = mmal_port_parameter_set_uint32(encoder_output, MMAL_PARAMETER_JPEG_Q_FACTOR, state->quality);

    if (status != MMAL_SUCCESS)
    {
        vcos_log_error("Unable to set JPEG quality");
        goto error;
    }

    // Set the JPEG restart interval
    status = mmal_port_parameter_set_uint32(encoder_output, MMAL_PARAMETER_JPEG_RESTART_INTERVAL, state->restart_interval);

    if (state->restart_interval && status != MMAL_SUCCESS)
    {
        vcos_log_error("Unable to set JPEG restart interval");
        goto error;
    }

    // Set up any required thumbnail
    {
        MMAL_PARAMETER_THUMBNAIL_CONFIG_T param_thumb = {{MMAL_PARAMETER_THUMBNAIL_CONFIGURATION, sizeof(MMAL_PARAMETER_THUMBNAIL_CONFIG_T)}, 0, 0, 0, 0};

        if (state->thumbnailConfig.enable &&
            state->thumbnailConfig.width > 0 && state->thumbnailConfig.height > 0)
        {
            // Have a valid thumbnail defined
            param_thumb.enable = 1;
            param_thumb.width = state->thumbnailConfig.width;
            param_thumb.height = state->thumbnailConfig.height;
            param_thumb.quality = state->thumbnailConfig.quality;
        }
        status = mmal_port_parameter_set(encoder->control, &param_thumb.hdr);
    }

    //  Enable component
    status = mmal_component_enable(encoder);

    if (status != MMAL_SUCCESS)
    {
        vcos_log_error("Unable to enable video encoder component");
        goto error;
    }

    /* Create pool of buffer headers for the output port to consume */
    pool = mmal_port_pool_create(encoder_output, encoder_output->buffer_num, encoder_output->buffer_size);

    if (!pool)
    {
        vcos_log_error("Failed to create buffer header pool for encoder output port %s", encoder_output->name);
    }

    state->encoder_pool_image = pool;
    state->encoder_component_image = encoder;

    if (state->common_settings.verbose)
        fprintf(stderr, "Encoder component done\n");

    return status;

error:

    if (encoder)
        mmal_component_destroy(encoder);

    return status;
}

/**
 * Destroy the encoder component
 *
 * @param state Pointer to state control struct
 *
 */
static void destroy_encoder_component_image(RASPIVID_STATE *state)
{
    // Get rid of any port buffers first
    if (state->encoder_pool_image)
    {
        mmal_port_pool_destroy(state->encoder_component_image->output[0], state->encoder_pool_image);
    }

    if (state->encoder_component_image)
    {
        mmal_component_destroy(state->encoder_component_image);
        state->encoder_component_image = NULL;
    }
}

MMAL_STATUS_T create_resizer_component(RASPIVID_STATE *state)
{
    MMAL_COMPONENT_T *resizer = 0;
    MMAL_PORT_T *input_port = NULL;
    MMAL_PORT_T *output_port = NULL;
    MMAL_ES_FORMAT_T *format;
    MMAL_STATUS_T status;

    if (state->camera_component == NULL)
    {
        status = MMAL_ENOSYS;
        vcos_log_error("Camera component must be created before splitter");
        goto error;
    }
    status = mmal_component_create("vc.ril.isp", &resizer);
    if (status != MMAL_SUCCESS)
    {
        printf("Failed to create resize component\n");
        goto error;
    }
    // проверяем наличие портов у компонента
    if (resizer->input_num < 1 || resizer->output_num < 1)
    {
        vcos_log_error("Resizer doesn't have enough ports");
    }

    mmal_format_copy(resizer->input[0]->format, state->splitter_component->output[1]->format);

    resizer->input[0]->buffer_num = 1;
    status = mmal_port_format_commit(resizer->input[0]);
    if (status != MMAL_SUCCESS)
    {
        vcos_log_error("Unable to set format on resizer input port");
        goto error;
    }
    resizer->output[0]->buffer_num = 1;
    mmal_format_copy(resizer->output[0]->format, resizer->input[0]->format);
    resizer->output[0]->format->es->video.width = 640;
    resizer->output[0]->format->es->video.height = 480;
    resizer->output[0]->format->es->video.crop.x = 0;
    resizer->output[0]->format->es->video.crop.y = 0;
    resizer->output[0]->format->es->video.crop.width = 640;
    resizer->output[0]->format->es->video.crop.height = 480;
    // resizer->output[0]->format->es->video.frame_rate.num = 0;
    // resizer->output[0]->format->es->video.frame_rate.den = 1;

    status = mmal_port_format_commit(resizer->output[0]);
    if (status != MMAL_SUCCESS)
    {
        vcos_log_error("Unable to set format on resizer output port");
        goto error;
    }
    state->resize_component = resizer;
    return status;

error:
    if (resizer)
        mmal_component_destroy(resizer);
    return status;
}

void destroy_resize_component(RASPIVID_STATE *state)
{
    if (state->resize_component)
    {
        mmal_component_destroy(state->resize_component);
        state->resize_component = NULL;
    }
}

/**
 *  buffer header callback function for encoder
 *
 *  Callback will dump buffer data to the specific file
 *
 * @param port Pointer to port from which callback originated
 * @param buffer mmal buffer header pointer
 */
static void encoder_buffer_callback_image(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    int complete = 0;

    // We pass our file handle and other stuff in via the userdata field.

    PORT_USERDATA_IMAGE *pData = (PORT_USERDATA_IMAGE *)port->userdata;

    if (pData)
    {
        unsigned int flags = buffer->flags;
        pData->lengthActual += buffer->length;
        mmal_buffer_header_mem_lock(buffer);
        // printf("buffer->length: %d\n", buffer->length);
        // printf("pData->offset: %d\n", pData->offset);
        for (unsigned int i = 0; i < buffer->length; i++, pData->bufferPosition++)
        {
            if (pData->offset >= pData->length)
            {
                printf("pData->offset: %d\n", pData->offset);
                printf("pData->length: %d\n", pData->length);
                printf("buffer->length: %d\n", buffer->length);
                printf("Buffer provided was too small! Failed to copy data into buffer\n");
                break;
            }
            else
            {
                // if (pData->bufferPosition >= 54)
                // {

                pData->data[pData->offset] = buffer->data[i];
                pData->offset++;
                // }
            }
        }
        mmal_buffer_header_mem_unlock(buffer);
        // int bytes_written = buffer->length;

        // if (buffer->length && pData->file_handle)
        // {
        //     mmal_buffer_header_mem_lock(buffer);

        //     bytes_written = fwrite(buffer->data, 1, buffer->length, pData->file_handle);

        //     mmal_buffer_header_mem_unlock(buffer);
        // }

        // // We need to check we wrote what we wanted - it's possible we have run out of storage.
        // if (bytes_written != buffer->length)
        // {
        //     vcos_log_error("Unable to write buffer to file - aborting!!!");
        //     complete = 1;
        // }

        // // Now flag if we have completed
        if (buffer->flags & (MMAL_BUFFER_HEADER_FLAG_FRAME_END | MMAL_BUFFER_HEADER_FLAG_TRANSMISSION_FAILED))
        {
            printf("HERE\n");
            // Перенёс функцию увеличения семафора сюда из конца функции
            // после этого перестала возникать ошибка с недостаточным объёмом буфера
            vcos_semaphore_post(&(pData->complete_semaphore));
        }
    }
    else
    {
        vcos_log_error("Received a encoder buffer callback with no state");
    }
    // release buffer back to the pool
    mmal_buffer_header_release(buffer);
    // and send one back to the port (if still open)
    if (port->is_enabled)
    {
        MMAL_STATUS_T status = MMAL_SUCCESS;
        MMAL_BUFFER_HEADER_T *new_buffer;

        new_buffer = mmal_queue_get(pData->pstate->encoder_pool_image->queue);

        if (new_buffer)
        {
            status = mmal_port_send_buffer(port, new_buffer);
        }
        if (!new_buffer || status != MMAL_SUCCESS)
            vcos_log_error("Unable to return a buffer to the encoder port");
    }
    // if (complete)
    //     vcos_semaphore_post(&(pData->complete_semaphore));
}

int start_recording(RASPIVID_STATE *state)
{
    int status;
    MMAL_PORT_T *camera_video_port = state->camera_component->output[MMAL_CAMERA_VIDEO_PORT];
    MMAL_PORT_T *encoder_output_port = state->encoder_component->output[0];
    MMAL_PORT_T *splitter_output_port = state->splitter_component->output[SPLITTER_OUTPUT_PORT];
    MMAL_PORT_T *encoder_input_port = state->encoder_component->input[0];
    if (!state->encoder_connection->is_enabled)
    {
        printf("Enable the encoder connection!\n");
        status = connect_ports(splitter_output_port, encoder_input_port, &state->encoder_connection);
        if (status != MMAL_SUCCESS)
        {
            state->encoder_connection = NULL;
            vcos_log_error("%s: Failed to connect splitter output port 0 to video encoder input", __func__);
            // goto error;
        }
    }
    else
        printf("Encoder connection is already enabled\n");
    // Set up our video userdata - this is passed through to the callback where we need the information.
    state->callback_data.pstate = state;
    state->callback_data.abort = 0;
    state->callback_data.file_handle = NULL;
    // state->common_settings.filename = malloc(max_filename_length);
    // strncpy(state->common_settings.filename, "video1.h264", max_filename_length);
    state->callback_data.file_handle = fopen(state->common_settings.filename, "wb");
    if (!state->callback_data.file_handle)
    {
        // Notify user, carry on but discarding encoded output buffers
        vcos_log_error("%s: Error opening output file: %s\nNo output file will be generated\n", __func__, state->common_settings.filename);
        return -1;
    }
    // Set up our userdata - this is passed through to the callback where we need the information.
    encoder_output_port->userdata = (struct MMAL_PORT_USERDATA_T *)&state->callback_data;
    if (encoder_output_port->is_enabled)
    {
        printf("Could not enable encoder output port. Try waiting longer before attempting to take another record\n");
        return -1;
    }
    // Enable the encoder output port and tell it its callback function
    status = mmal_port_enable(encoder_output_port, encoder_buffer_callback);
    if (status != MMAL_SUCCESS)
        return -1;
    // Send all the buffers to the encoder output port
    if (state->callback_data.file_handle)
    {
        int num = mmal_queue_length(state->encoder_pool->queue);
        int q;
        for (q = 0; q < num; q++)
        {
            MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(state->encoder_pool->queue);
            if (!buffer)
            {
                vcos_log_error("Unable to get a required buffer %d from pool queue", q);
                return -1;
            }

            if (mmal_port_send_buffer(encoder_output_port, buffer) != MMAL_SUCCESS)
            {
                vcos_log_error("Unable to send a buffer to encoder output port (%d)", q);
                return -1;
            }
        }
    }
    // Enable Capture parameter of camera video port for starting video capture
    mmal_port_parameter_set_boolean(camera_video_port, MMAL_PARAMETER_CAPTURE, 1);
    printf("Starting video capture\n");
    return 0;
}

int stop_recording(RASPIVID_STATE *state)
{
    int status;
    MMAL_PORT_T *camera_video_port = state->camera_component->output[MMAL_CAMERA_VIDEO_PORT];
    MMAL_PORT_T *encoder_output_port = state->encoder_component->output[0];
    mmal_port_parameter_set_boolean(camera_video_port, MMAL_PARAMETER_CAPTURE, 0);
    fprintf(stderr, "Finished capture\n");
    check_disable_port(encoder_output_port);
    if (state->encoder_connection->is_enabled)
    {
        status = mmal_connection_destroy(state->encoder_connection);
        if (status == MMAL_SUCCESS)
            printf("Encoder connection was destroyed\n");
        else
            printf("Encoder connection was not destroyed\n");
    }
    fclose(state->callback_data.file_handle);
    // Clear callback userdata
    // encoder_output_port->userdata = (struct MMAL_PORT_USERDATA_T *){0};
    state->callback_data = (const PORT_USERDATA){0};
    return 0;
}

int take_picture(RASPIVID_STATE *state, unsigned char *preallocated_data, unsigned int length, unsigned int *lengthActual)
{
    int status;
    VCOS_STATUS_T vcos_status;
    MMAL_PORT_T *camera_video_port = state->camera_component->output[MMAL_CAMERA_VIDEO_PORT];
    MMAL_PORT_T *splitter_image_port = state->splitter_component->output[1];
    // MMAL_PORT_T *encoder_input_port_image = state->encoder_component_image->input[0];
    // MMAL_PORT_T *encoder_output_port_image = state->encoder_component_image->output[0];

    // Восстанавливаем соединение энкодера, если оно было разорвано (после получения очередной фотографии)
    // if (!state->encoder_connection_image->is_enabled)
    // {
    //     status = connect_ports(splitter_image_port, encoder_input_port_image, &state->encoder_connection_image);
    //     if (status != MMAL_SUCCESS)
    //     {
    //         state->encoder_connection_image = NULL;
    //         vcos_log_error("%s: Failed to connect splitter output port 1 to image encoder input", __func__);
    //         return -1;
    //     }
    //     printf("Encoder connection for still image is ready\n");
    // }
    PORT_USERDATA_IMAGE userdata;
    userdata.pstate = state;
    vcos_status = vcos_semaphore_create(&userdata.complete_semaphore, "Farvcam-sem", 0);
    userdata.encoderPool = state->encoder_pool_image;
    userdata.data = preallocated_data;
    userdata.bufferPosition = 0;
    userdata.offset = 0;
    userdata.startingOffset = 0;
    userdata.length = length;
    userdata.lengthActual = 0;

    splitter_image_port->userdata = (struct MMAL_PORT_USERDATA_T *)&userdata;
    if (splitter_image_port->is_enabled)
    {
        printf("Could not enable encoder output port. Try waiting longer before attempting to take another picture\n");
        return -1;
    }
    status = mmal_port_enable(splitter_image_port, encoder_buffer_callback_image);

    int num = mmal_queue_length(state->encoder_pool_image->queue);
    for (int q = 0; q < num; q++)
    {
        MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(state->encoder_pool_image->queue);

        if (!buffer)
            vcos_log_error("Unable to get a required buffer %d from pool queue", q);

        if (mmal_port_send_buffer(splitter_image_port, buffer) != MMAL_SUCCESS)
            vcos_log_error("Unable to send a buffer to encoder output port (%d)", q);
    }
    if (mmal_port_parameter_set_boolean(camera_video_port, MMAL_PARAMETER_CAPTURE, 1) != MMAL_SUCCESS)
    {
        vcos_log_error("Failed to start capture");
        return -1;
    }
    vcos_semaphore_wait(&userdata.complete_semaphore);
    vcos_semaphore_delete(&userdata.complete_semaphore);
    // mmal_port_parameter_set_boolean(camera_video_port, MMAL_PARAMETER_CAPTURE, 0);
    printf("Actual image size: %d\n", userdata.lengthActual);
    *lengthActual = userdata.lengthActual;

    // Выключаем порт и разрываем соединение, чтобы происходила очистка буферов перед следующим захватом
    check_disable_port(splitter_image_port);

    splitter_image_port->userdata = (const struct MMAL_PORT_USERDATA_T *){0};
    userdata = (const PORT_USERDATA_IMAGE){0};
    return 0;
}

size_t getImageBufferSize(RASPIVID_STATE *state)
{
    //oversize the buffer so to fit BMP images
    //oversize the buffer so to fit BMP images
    int width = VCOS_ALIGN_UP(1920, 32);
    int height = VCOS_ALIGN_UP(1080, 16);
    size_t buffer_size = width * height * 3 + 54;
    return buffer_size;
}

int serial_setup(int fd, struct termios *old_serial, struct termios *new_serial)
{
    tcgetattr(fd, old_serial);
    bzero(new_serial, sizeof(*new_serial));
    /*
      BAUDRATE: 115200 baud.
      CRTSCTS : no output hardware flow control
      CLOCAL  : do not change "owner" of the port
      CS8     : 8n1 (8bit,no parity,1 stopbit)
      CREAD   : enable receiving
    */
    cfsetispeed(new_serial, B115200);
    cfsetospeed(new_serial, B115200);
    new_serial->c_cflag |= (CLOCAL | CREAD);
    new_serial->c_cflag &= ~PARENB;
    new_serial->c_cflag &= ~CSTOPB;
    new_serial->c_cflag &= ~CSIZE;
    new_serial->c_cflag |= CS8;
    new_serial->c_cflag &= ~CRTSCTS;
    /* Do not map CR to NL and vice versa for input */
    new_serial->c_iflag &= ~(ICRNL | INLCR);
    new_serial->c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    new_serial->c_cc[VTIME] = 1; // межсимвольный таймер 0.1 sec
    new_serial->c_cc[VMIN] = 6;  // блокирующее чтение до тех пор, пока не получим 6 символов
    new_serial->c_oflag &= ~OPOST;
    tcflush(fd, TCIFLUSH);
    tcsetattr(fd, TCSANOW, new_serial);
    return 0;
}

bool write_serial_command(int fd, uint8_t *args, uint8_t argn)
{
    int w = write(fd, args, argn);
    //tcdrain(fd);
    if (w < 0)
        return false;
    else
        return true;
}

void cropRGB(uint8_t *input_image, int input_w, int input_h,
             uint8_t *output_image, int output_w, int output_h,
             int cropX, int cropY)
{
    unsigned int row;
    uint8_t *rowBuffer = malloc(3 * output_w * sizeof(uint8_t));
    uint8_t *out_ptr = output_image;
    for (row = cropY; row < cropY + output_h; ++row)
    {
        memcpy(out_ptr, input_image + 3 * (row * input_w + cropX), 3 * output_w);
        if (row != cropY + output_h - 1)
            out_ptr += 3 * output_w;
    }
    free(rowBuffer);
}

/** Функция вызываемая потоком для записи и сохранения видео.
 * @param state_arg указатель на структуру state
 * @return NULL
 * */
void *video_routine(void *state_arg)
{
    RASPIVID_STATE *state = (RASPIVID_STATE *)state_arg;
    char video_name[max_filename_length];
    int video_num = 0;
    // gpioInitialise();
    // gpioSetMode(23, PI_INPUT);
    // gpioSetPullUpDown(23, PI_PUD_UP);
    state->common_settings.filename = malloc(max_filename_length);
    sprintf(video_name, "video_%d.h264", video_num);
    strncpy(state->common_settings.filename, video_name, max_filename_length);
    start_recording(state);
    pause_and_test_abort(state, 1 * 10 * 1000);
    stop_recording(state);
    // В терминале включённое состояние порта соответствует PI_OFF!
    // while (gpioRead(23) == PI_OFF)
    // {
    //     // Ничего не делаем. Ждём пока пользователь не выключит выход
    //     // Запись должна начинаться после перехода из состояния ВЫКЛ в состояние ВКЛ
    // }
    // while (1)
    // {
    //     // пока выход снова не включен, ничего не делаем
    //     if (gpioRead(23) == PI_OFF)
    //     {
    //         sprintf(video_name, "video_%d.h264", video_num);
    //         strncpy(state->common_settings.filename, video_name, max_filename_length);
    //         start_recording(state);
    //         while (gpioRead(23) == PI_OFF)
    //         {
    //             // ждём пока выход не отключится
    //         }
    //         stop_recording(state);
    //         video_num++;
    //     }
    // }
    free(state->common_settings.filename);
    return NULL;
}

typedef struct
{
    int last_pos;
    void *context;
} custom_stbi_mem_context;

static void custom_stbi_write_mem(void *context, void *data, int size)
{
    custom_stbi_mem_context *c = (custom_stbi_mem_context *)context;
    char *dst = (char *)c->context;
    char *src = (char *)data;
    int cur_pos = c->last_pos;
    for (int i = 0; i < size; i++)
    {
        dst[cur_pos++] = src[i];
    }
    c->last_pos = cur_pos;
}

/** Функция вызываемая потоком для получения фото и его отправки в терминал CAN-WAY
 * по протоколу OV528
 * @param state_arg указатель на структуру state
 * @return NULL
 * */
void *photo_routine(void *state_arg)
{
    RASPIVID_STATE *state = (RASPIVID_STATE *)state_arg;
    MMAL_STATUS_T status = MMAL_SUCCESS;
    // uint8_t commID, param1, param2, param3, param4;
    // uint8_t ACK_counter = 0x00;
    // // Буфер, в котором хранится принятое сообщение
    // uint8_t buffer[12];
    // /** Переменная, хранящая размер буфера изображения,
    //  * вычисляемый в callback-функции, т.е. это реальный размер изображения,
    //  * не взятый с запасом
    //  */
    // unsigned int data_length;
    // size_t package_size;
    // /** буфер для хранения данных изображения, получаемых динамически. В этот
    //  * буфер будут копироваться данные после захвата изображения
    //  */
    // uint8_t *data;
    // /** указатель на data для отправки данных буфера в последовательный порт
    //  */
    // uint8_t *ptr;
    // // буфер для отправки пакета данных (пока что ограничен размером 512)
    // uint8_t package[512];
    // // ID текущего пакета, который необходимо отправить
    // uint32_t package_id = 0, package_id_prev = 0;
    // uint32_t package_max_id;
    // uint32_t verify_checksum = 0;
    // int im_width, im_height;
    // float w_to_h;
    // // переменная, запускающая цикл ожидания и обработки запроса
    // int running = 1;

    // FILE *output_file = NULL;
    // state->jpeg_filename = malloc(max_filename_length);

    // // Открытие и настройка последовательного порта
    // struct termios old_serial, new_serial;
    // int fd = open("/dev/serial0", O_RDWR | O_NOCTTY);
    // if (fd == -1)
    // {
    //     fprintf(stderr, "Unable to open serial device\n");
    //     return -1;
    // }
    // serial_setup(fd, &old_serial, &new_serial);
    // printf("Starting communication routine\n");
    // while (running)
    // {
    //     int res = read(fd, buffer, 6);
    //     printf("Number of received bytes: %d \n", res);
    //     commID = buffer[1];
    //     param1 = buffer[2];
    //     param2 = buffer[3];
    //     param3 = buffer[4];
    //     param4 = buffer[5];
    //     switch (commID)
    //     {
    //     case SYNC:
    //     {
    //         uint8_t args[] = {0xAA, ACK, commID, ACK_counter, 0x00, 0x00};
    //         printf("Received SYNC command \n");
    //         write_serial_command(fd, args, 6);
    //         break;
    //     }
    //     case INIT:
    //     {
    //         /**
    //          * изменение разрешения фотографии пока не работает
    //          */
    //         switch (param4)
    //         {
    //         case RES_160x128:
    //         {
    //             // Это разрешение не поддерживается терминалом CAN-WAY
    //             im_width = 160;
    //             im_height = 128;
    //             break;
    //         }
    //         case RES_320x240:
    //         {
    //             im_width = 320;
    //             im_height = 240;
    //             break;
    //         }
    //         case RES_640x480:
    //         {
    //             im_width = 640;
    //             im_height = 480;
    //             break;
    //         }
    //         }
    //         printf("Received Initial command\n");
    //         w_to_h = (float)im_width / (float)im_height;
    //         unsigned int crop_width = ceil(state->common_settings.height * w_to_h);
    //         MMAL_PARAMETER_CROP_T crop = {{MMAL_PARAMETER_CROP, sizeof(MMAL_PARAMETER_CROP_T)}, {0, 0, 0, 0}};
    //         /** Терминал CAN-WAY отправляет запрос на получения фотографий, у которых отношение
    //          * ширина/высота равно 5:4 или 4:3. При этом для разрешения видео 1920х1080 это отношение равно
    //          * 16:9. Поэтому сначала видеокадр обрезается так, чтобы сохранить максимально возможное исходное
    //          * разрешение, но при этом уменьшить ширину кадра для соответствия отношению 1.25 или 1.33. После
    //          * этого разрешение обрезанного кадра можно уменьшить до заданного без искажения фото.
    //          * */
    //         crop.rect.x = state->common_settings.width / 2 - crop_width / 2;
    //         crop.rect.y = 0;
    //         crop.rect.width = crop_width;
    //         crop.rect.height = state->common_settings.height;
    //         // Пока что изменение разрешения плохо работает вместе с resizer
    //         // status = mmal_port_parameter_set(state->resize_component->input[0], &crop.hdr);
    //         // if (status != MMAL_SUCCESS)
    //         // {
    //         //     mmal_status_to_int(status);
    //         //     return NULL;
    //         // }
    //         uint8_t args[] = {0xAA, ACK, commID, ACK_counter, 0x00, 0x00};
    //         write_serial_command(fd, args, 6);
    //         break;
    //     }
    //     case SET_PACKAGE_SIZE:
    //     {
    //         printf("Receieved Set Package Command\n");
    //         package_size = (param3 << 8U) | param2;
    //         uint8_t args[] = {0xAA, ACK, commID, ACK_counter, 0x00, 0x00};
    //         write_serial_command(fd, args, 6);
    //         printf("Package size is: %d\n", package_size);
    //         break;
    //     }
    //     case SNAPSHOT:
    //     {
    //         printf("Received Snapshot command\n");
    //         // unsigned int length_oversized = getImageBufferSize(state);
    //         // // Выделяем память для изображения
    //         // data = malloc(length_oversized * sizeof(uint8_t));
    //         // // Делаем снимок
    //         // take_picture(state, data, length_oversized, &data_length);
    //         // // рассчитываем максимальный ID отправляемого пакета
    //         // package_max_id = floor(data_length / (package_size - 6));
    //         // // ptr теперь указывает на первый элемент буфера data
    //         // ptr = data;
    //         uint8_t args[] = {0xAA, ACK, commID, ACK_counter, 0x00, 0x00};
    //         write_serial_command(fd, args, 6);
    //         unsigned int length_oversized = getImageBufferSize(state);
    //         // Выделяем память для изображения
    //         data = malloc(length_oversized * sizeof(uint8_t));
    //         // Делаем снимок
    //         take_picture(state, data, length_oversized, &data_length);
    //         // рассчитываем максимальный ID отправляемого пакета
    //         package_max_id = floor(data_length / (package_size - 6));
    //         // ptr теперь указывает на первый элемент буфера data
    //         ptr = data;
    //         break;
    //     }
    //     case GET_PICTURE:
    //     {
    //         printf("Received Get picture command\n");
    //         uint8_t args_1[] = {0xAA, ACK, commID, ACK_counter, 0x00, 0x00};
    //         write_serial_command(fd, args_1, 6);
    //         param1 = data_length & 0x000000FFU;
    //         param2 = (data_length & 0x0000FF00U) >> 8U;
    //         param3 = (data_length & 0x00FF0000U) >> 16U;
    //         uint8_t args_2[] = {0xAA, DATA, 0x01, param1, param2, param3};
    //         write_serial_command(fd, args_2, 6);
    //         break;
    //     }
    //     case ACK:
    //     {
    //         printf("Received ACK command\n");
    //         if ((param3 == 0xF0) && (param4 == 0xF0))
    //         {
    //             printf("All Image data was sent!\n");
    //         }
    //         else
    //         {
    //             /** Отправляем целый пакет данных, если текущий ID требуемого
    //              * пакета меньше максимального
    //             */
    //             printf("Package maximum ID: %d \n", package_max_id);
    //             package_id = (param4 << 8U) | param3;
    //             printf("Package ID: %d \n", package_id);
    //             if ((package_id < package_max_id) && (package_id != package_id_prev))
    //             {
    //                 package[0] = param3;
    //                 package[1] = param4;
    //                 package[2] = (package_size - 6) & 0x000000FFU;
    //                 package[3] = ((package_size - 6) & 0x0000FF00U) >> 8U;
    //                 memcpy(package + 4, ptr, package_size - 6);
    //                 for (int i = 0; i < package_size - 2; i++)
    //                 {
    //                     verify_checksum += package[i];
    //                 }
    //                 package[package_size - 2] = verify_checksum & 0x000000FFU;
    //                 package[package_size - 1] = 0x00;
    //                 int w = write(fd, package, package_size);
    //                 printf("Bytes written: %d \n", w);
    //                 ptr += package_size - 6;
    //                 verify_checksum = 0;
    //                 package_id_prev = package_id;
    //             }
    //             /** Отправляем остаток данных в пакете меньшего размера - это пакет
    //              * с максимальным идентификатором
    //             */
    //             else if (package_id != package_id_prev)
    //             {
    //                 package[0] = param3;
    //                 package[1] = param4;
    //                 package[2] = (data_length - package_max_id * (package_size - 6)) & 0x000000FFU;
    //                 package[3] = ((data_length - package_max_id * (package_size - 6)) & 0x0000FF00U) >> 8U;
    //                 memcpy(package + 4, ptr, data_length - package_max_id * (package_size - 6));
    //                 for (int i = 0; i < 4 + data_length - package_max_id * (package_size - 6); i++)
    //                 {
    //                     verify_checksum += package[i];
    //                 }
    //                 package[4 + data_length - package_max_id * (package_size - 6)] = verify_checksum & 0x000000FFU;
    //                 package[4 + data_length - package_max_id * (package_size - 6) + 1] = 0x00;

    //                 int w = write(fd, package, 6 + data_length - package_max_id * (package_size - 6));
    //                 // tcdrain(fd);
    //                 package_id = 0;
    //                 verify_checksum = 0;
    //                 package_id_prev = package_id;

    //                 // сохраняем фотографию на диске для отладки
    //                 strncpy(state->jpeg_filename, "test_pic.jpeg", max_filename_length);
    //                 output_file = fopen(state->jpeg_filename, "wb");
    //                 fwrite(data, 1, data_length, output_file); // здесь записываем только реальное количество байт
    //                 fclose(output_file);
    //                 output_file = NULL;
    //                 // сбрасываем указатель
    //                 ptr = NULL;
    //                 // освобождаем память, динамически выделенную для хранения изображения
    //                 memset(data, 0, data_length);
    //                 free(data);
    //             }
    //         }
    //     }

    //     break;
    //     }
    //     if (buffer[0] == 'Z')
    //         running = 0;
    // }
    // tcsetattr(fd, TCSANOW, &old_serial);
    // close(fd);

    FILE *output_file = NULL;
    state->jpeg_filename = malloc(max_filename_length);
    unsigned int length_oversized = getImageBufferSize(state);
    uint8_t *data = malloc(length_oversized * sizeof(uint8_t));
    uint8_t *cropped_data = malloc(length_oversized * sizeof(uint8_t));
    uint8_t *resized_data = malloc(length_oversized * sizeof(uint8_t));
    uint8_t *buffer = malloc(length_oversized * sizeof(uint8_t));
    unsigned int length_actual;

    printf("START\n");
    usleep(2 * 1000000);
    clock_t tic = clock();
    take_picture(state, data, length_oversized, &length_actual);
    cropRGB(data, 1920, 1080, cropped_data, 1440, 1080, 240, 0);
    stbir_resize_uint8(cropped_data, 1440, 1080, 1440 * 3, resized_data, 640, 480, 640 * 3, 3);
    custom_stbi_mem_context ct;
    ct.last_pos = 0;
    ct.context = (void *)buffer;
    int result = stbi_write_jpg_to_func(custom_stbi_write_mem, &ct, 640, 480, 3, resized_data, 90);
    clock_t toc = clock();
    printf("Elapsed: %f seconds\n", (double)(toc - tic) / CLOCKS_PER_SEC);
    stbi_write_jpg("t1_resized.jpeg", 640, 480, 3, resized_data, 90);
    printf("Last pos: %i", ct.last_pos);

    // strncpy(state->jpeg_filename, "t1.jpeg", max_filename_length);
    // output_file = fopen(state->jpeg_filename, "wb");
    // fwrite(buffer, 1, length_actual, output_file); // здесь записываем только реальное количество байт
    // fclose(output_file);
    // output_file = NULL;
    // stbi_write_jpg("t1.jpeg", 1440, 1080, 3, cropped_data, 90);

    printf("START\n");
    memset(data, 0, length_oversized);
    usleep(2 * 1000000);
    take_picture(state, data, length_oversized, &length_actual);
    stbi_write_jpg("t2.jpeg", 1920, 1080, 3, data, 90);
    // освобождаем память, динамически выделенную для хранения имени изображения
    free(state->jpeg_filename);
    free(data);
    free(cropped_data);
    free(resized_data);
    free(buffer);
    // free(resized_data);
    return NULL;
}

/**
 * main
 */
int main(int argc, const char **argv)
{
    // Основная структура, хранящая практически все данные о камере и прочих компонентах
    RASPIVID_STATE state;
    PORT_USERDATA_IMAGE callback_data_image;

    // int exit_code = EX_OK;
    MMAL_STATUS_T status = MMAL_SUCCESS;
    MMAL_PORT_T *camera_preview_port = NULL;
    MMAL_PORT_T *camera_video_port = NULL;
    MMAL_PORT_T *camera_still_port = NULL;
    MMAL_PORT_T *preview_input_port = NULL;

    MMAL_PORT_T *encoder_input_port = NULL;
    MMAL_PORT_T *encoder_output_port = NULL;
    MMAL_PORT_T *encoder_input_port_image = NULL;
    MMAL_PORT_T *encoder_output_port_image = NULL;

    MMAL_PORT_T *splitter_input_port = NULL;
    MMAL_PORT_T *splitter_output_port = NULL;
    MMAL_PORT_T *splitter_image_port = NULL;
    MMAL_PORT_T *splitter_preview_port = NULL;

    MMAL_PORT_T *resizer_input_port = NULL;
    MMAL_PORT_T *resizer_output_port = NULL;

    bcm_host_init();
    // Register our application with the logging system
    vcos_log_register("Farvcamera", VCOS_LOG_CATEGORY);
    signal(SIGINT, default_signal_handler);
    // Disable USR1 for the moment - may be reenabled if go in to signal capture mode
    signal(SIGUSR1, SIG_IGN);
    default_status(&state);

    state.timeout = 5000;
    state.timeout_image = 3000;

    get_sensor_defaults(state.common_settings.cameraNum, state.common_settings.camera_name,
                        &state.common_settings.width, &state.common_settings.height);
    check_camera_model(state.common_settings.cameraNum);

    if ((status = create_camera_component(&state)) != MMAL_SUCCESS)
    {
        vcos_log_error("%s: Failed to create camera component", __func__);
        // exit_code = EX_SOFTWARE;
    }
    if ((status = create_encoder_component(&state)) != MMAL_SUCCESS)
    {
        vcos_log_error("%s: Failed to create encoder component", __func__);
        raspipreview_destroy(&state.preview_parameters);
        destroy_camera_component(&state);
        // exit_code = EX_SOFTWARE;
    }
    if ((status = create_splitter_component(&state)) != MMAL_SUCCESS)
    {
        vcos_log_error("%s: Failed to create splitter component", __func__);
        raspipreview_destroy(&state.preview_parameters);
        destroy_camera_component(&state);
        destroy_encoder_component(&state);
    }

    // if ((status = create_encoder_component_image(&state)) != MMAL_SUCCESS)
    // {
    //     vcos_log_error("%s: Failed to create encoder component for image capture", __func__);
    //     // exit_code = EX_SOFTWARE;
    // }

    if ((status = create_resizer_component(&state)) != MMAL_SUCCESS)
    {
        vcos_log_error("%s: Failed to create resize component for image capture", __func__);
    }

    printf("Starting component connection stage\n");
    camera_video_port = state.camera_component->output[MMAL_CAMERA_VIDEO_PORT];
    camera_still_port = state.camera_component->output[MMAL_CAMERA_CAPTURE_PORT];
    encoder_input_port = state.encoder_component->input[0];
    encoder_output_port = state.encoder_component->output[0];

    splitter_input_port = state.splitter_component->input[0];
    splitter_output_port = state.splitter_component->output[SPLITTER_OUTPUT_PORT];
    splitter_image_port = state.splitter_component->output[1];

    // resizer_input_port = state.resize_component->input[0];
    // resizer_output_port = state.resize_component->output[0];

    // encoder_input_port_image = state.encoder_component_image->input[0];
    // encoder_output_port_image = state.encoder_component_image->output[0];

    VCOS_STATUS_T vcos_status;

    printf("Connecting camera video port to splitter input port\n");
    status = connect_ports(camera_video_port, splitter_input_port, &state.splitter_connection);
    if (status != MMAL_SUCCESS)
    {
        state.encoder_connection = NULL;
        vcos_log_error("%s: Failed to connect camera video port to splitter input", __func__);
        // goto error;
    }
    printf("Connecting splitter output port 0 to video encoder input port\n");
    status = connect_ports(splitter_output_port, encoder_input_port, &state.encoder_connection);
    if (status != MMAL_SUCCESS)
    {
        state.encoder_connection = NULL;
        vcos_log_error("%s: Failed to connect splitter output port 0 to video encoder input", __func__);
        // goto error;
    }
    // printf("Connecting splitter output port 1 to image encoder input port\n");
    // status = connect_ports(splitter_image_port, encoder_input_port_image, &state.encoder_connection_image);
    // if (status != MMAL_SUCCESS)
    // {
    //     state.encoder_connection_image = NULL;
    //     vcos_log_error("%s: Failed to connect splitter output port 1 to image encoder input", __func__);
    //     // goto error;
    // }
    // printf("Connecting splitter output port 1 to resizer input port\n");
    // status = connect_ports(splitter_image_port, resizer_input_port, &state.resizer_connection);
    // if (status != MMAL_SUCCESS)
    // {
    //     state.resizer_connection = NULL;
    //     vcos_log_error("%s: Failed to connect splitter output port 1 to resizer input port", __func__);
    //     // goto error;
    // }

    // printf("Connecting resizer output port to encoder input port\n");
    // status = connect_ports(resizer_output_port, encoder_input_port_image, &state.encoder_connection_image);
    // if (status != MMAL_SUCCESS)
    // {
    //     state.resizer_connection = NULL;
    //     vcos_log_error("%s: Failed to connect resizer output port to encoder input port", __func__);
    //     // goto error;
    // }
    // printf("Connecting camera still port to image encoder input port\n");
    // status = connect_ports(camera_still_port, encoder_input_port_image, &state.encoder_connection_image);
    // if (status != MMAL_SUCCESS)
    // {
    //     state.encoder_connection_image = NULL;
    //     vcos_log_error("%s: Failed to connect splitter output port 1 to image encoder input", __func__);
    //     // goto error;
    // }

    printf("Camera, splitter and encoder components are created and connected!\n");
    // Ждём некоторое время для стабилизации камеры после соединений
    vcos_sleep(state.timeout_image);
    // Настройка потоков для работы с видео и фотографиями
    int rc1, rc2;
    pthread_t video_thread, photo_thread;
    // Создаём два потока - один для снятия и сохранения видео, второй - для получения фото и отправки по RS-232
    if (rc1 = pthread_create(&video_thread, NULL, &video_routine, &state))
    {
        printf("Video thread creation failed: %d\n", rc1);
    }
    if (rc2 = pthread_create(&photo_thread, NULL, &photo_routine, &state))
    {
        printf("Photo thread creation failed: %d\n", rc2);
    }

    pthread_join(video_thread, NULL);
    pthread_join(photo_thread, NULL);

    fprintf(stderr, "Closing down\n");

    // Disable all our ports that are not handled by connections
    check_disable_port(camera_still_port);
    check_disable_port(encoder_output_port);
    // check_disable_port(encoder_output_port_image);
    check_disable_port(splitter_output_port);

    if (state.encoder_connection)
        mmal_connection_destroy(state.encoder_connection);
    if (state.resizer_connection)
        mmal_connection_destroy(state.resizer_connection);
    if (state.splitter_connection)
        mmal_connection_destroy(state.splitter_connection);
    // if (state.encoder_connection_image)
    //     mmal_connection_destroy(state.encoder_connection_image);

    /* Disable components */
    if (state.encoder_component)
        mmal_component_disable(state.encoder_component);

    // if (state.encoder_component_image)
    //     mmal_component_disable(state.encoder_component_image);

    if (state.resize_component)
        mmal_component_disable(state.resize_component);

    if (state.splitter_component)
        mmal_component_disable(state.splitter_component);

    if (state.camera_component)
        mmal_component_disable(state.camera_component);

    destroy_encoder_component(&state);
    // destroy_encoder_component_image(&state);
    raspipreview_destroy(&state.preview_parameters);
    destroy_resize_component(&state);
    destroy_splitter_component(&state);
    destroy_camera_component(&state);

    gpioTerminate();

    fprintf(stderr, "Close down completed, all components disconnected, disabled and destroyed\n\n");
    return 0;
}
