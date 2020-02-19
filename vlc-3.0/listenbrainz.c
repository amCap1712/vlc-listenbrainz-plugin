/*****************************************************************************
 * listenbrainz.c : listenbrainz submission plugin
 *****************************************************************************
 *
 * Author: Kartik Ohri <kartikohri13 at gmail com>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/* ListenBrainz Submit Listens API 1
 * https://api.listenbrainz.org/1/submit-listens
 *
 */
/*****************************************************************************
 * Preamble
 *****************************************************************************/


#define _GNU_SOURCE
#define VLC_MODULE_LICENSE VLC_LICENSE_GPL_2_PLUS

#define HAVE_POLL_H 1
#include <poll.h>
#include <stdio.h>
#include <assert.h>
#include <time.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_interface.h>
#include <vlc_input.h>
#include <vlc_dialog.h>
#include <vlc_meta.h>
#include <vlc_memstream.h>
#include <vlc_stream.h>
#include <vlc_url.h>
#include <vlc_network.h>
#include <vlc_tls.h>
#include <vlc_playlist.h>

#define N_(str) (str)
#define VLC_TICK_INVALID INT64_C(0)

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/

#define QUEUE_MAX 50

/* Keeps track of metadata to be submitted */
typedef struct listenbrainz_song_t
{
    char        *psz_a;             /**< track artist     */
    char        *psz_t;             /**< track title      */
    char        *psz_b;             /**< track album      */
    char        *psz_n;             /**< track number     */
    int         i_l;                /**< track length     */
    char        *psz_m;             /**< musicbrainz id   */
    time_t      date;               /**< date since epoch */
    mtime_t     i_start;            /**< playing start    */
} listenbrainz_song_t;

struct intf_sys_t
{
    listenbrainz_song_t     p_queue[QUEUE_MAX]; /**< songs not submitted yet*/
    int                     i_songs;            /**< number of songs        */

    input_thread_t         *p_input;            /**< current input thread   */
    vlc_mutex_t             lock;               /**< p_sys mutex            */
    vlc_cond_t              wait;               /**< song to submit event   */
    vlc_thread_t            thread;             /**< thread to submit song  */

    /* submission of played songs */
    vlc_url_t               p_submit_url;       /**< where to submit data   */

    char                    *psz_user_token;    /**< Authentication token */

    /* data about song currently playing */
    listenbrainz_song_t     p_current_song;     /**< song being played      */

    mtime_t                 time_pause;         /**< time when vlc paused   */
    mtime_t                 time_total_pauses;  /**< total time in pause    */

    bool                    b_submit_nowp;      /**< do we have to submit ? */

    bool                    b_meta_read;        /**< if we read the song's
                                                 * metadata already         */
};

static int  Open            (vlc_object_t *);
static void Close           (vlc_object_t *);
static void *Run            (void *);

#define USERTOKEN_TEXT      N_("User token")
#define USERTOKEN_LONGTEXT  N_("The user token of your listenbrainz account")
#define URL_TEXT            N_("Submission URL")
#define URL_LONGTEXT        N_("The URL set for an alternative ListenBrainz instance")

/* This error value is used when listenbrainz plugin has to be unloaded. */
#define VLC_LISTENBRAINZ_EFATAL -72

/* listenbrainz client identifier */
#define CLIENT_NAME     PACKAGE
#define CLIENT_VERSION  VERSION
#define PACKAGE_NAME    "vlc"
#define PACKAGE_VERSION "3.0.*"

/*****************************************************************************
 * Module descriptor
 ****************************************************************************/

vlc_module_begin ()
    set_category( CAT_INTERFACE )
    set_subcategory( SUBCAT_INTERFACE_CONTROL )
    set_shortname( N_("Listenbrainz") )
    set_description( N_("Submission of played songs to listenbrainz") )
    add_string( "listenbrainz-usertoken", "", USERTOKEN_TEXT, USERTOKEN_LONGTEXT, false )
    add_string( "submission-url", "api.listenbrainz.org", URL_TEXT, URL_LONGTEXT, false )
    set_capability( "interface", 0 )
    set_callbacks( Open, Close )
vlc_module_end ()

/*****************************************************************************
 * DeleteSong : Delete the char pointers in a song
 *****************************************************************************/
static void DeleteSong(listenbrainz_song_t* p_song)
{
    FREENULL(p_song->psz_a);
    FREENULL(p_song->psz_b);
    FREENULL(p_song->psz_t);
    FREENULL(p_song->psz_m);
    FREENULL(p_song->psz_n);
}

/*****************************************************************************
 * ReadMetaData : Read meta data when parsed by vlc
 *****************************************************************************/
static void ReadMetaData(intf_thread_t *p_this, input_thread_t *p_input)
{
    intf_sys_t *p_sys = p_this->p_sys;

    assert(p_input != NULL);

    input_item_t *p_item = input_GetItem(p_input);
    if (p_item == NULL)
        return;

#define ALLOC_ITEM_META(a, b) do { \
        char *psz_meta = input_item_Get##b(p_item); \
        if (psz_meta && *psz_meta) \
            a = vlc_uri_encode(psz_meta); \
        free(psz_meta); \
    } while (0)

    vlc_mutex_lock(&p_sys->lock);

    p_sys->b_meta_read = true;

    ALLOC_ITEM_META(p_sys->p_current_song.psz_a, Artist);
    if (!p_sys->p_current_song.psz_a)
    {
        msg_Dbg(p_this, "No artist..");
        DeleteSong(&p_sys->p_current_song);
        goto end;
    }

    ALLOC_ITEM_META(p_sys->p_current_song.psz_t, Title);
    if (!p_sys->p_current_song.psz_t)
    {
        msg_Dbg(p_this, "No track name..");
        DeleteSong(&p_sys->p_current_song);
        goto end;
    }

    /* Now we have read the mandatory meta data, so we can submit that info */
    p_sys->b_submit_nowp = true;

    ALLOC_ITEM_META(p_sys->p_current_song.psz_b, Album);
    ALLOC_ITEM_META(p_sys->p_current_song.psz_m, TrackID);
    ALLOC_ITEM_META(p_sys->p_current_song.psz_n, TrackNum);

    p_sys->p_current_song.i_l = input_item_GetDuration(p_item) / 1000000;

#undef ALLOC_ITEM_META

    msg_Dbg(p_this, "Meta data registered");

    vlc_cond_signal(&p_sys->wait);

    end:
    vlc_mutex_unlock(&p_sys->lock);
}

/*****************************************************************************
 * AddToQueue: Add the played song to the queue to be submitted
 *****************************************************************************/
static void AddToQueue (intf_thread_t *p_this)
{
    mtime_t                     played_time;
    intf_sys_t                  *p_sys = p_this->p_sys;

    vlc_mutex_lock(&p_sys->lock);

    /* Check that we have the mandatory meta data */
    if (!p_sys->p_current_song.psz_t || !p_sys->p_current_song.psz_a)
        goto end;

    /* wait for the user to listen enough before submitting */
    played_time = mdate() - p_sys->p_current_song.i_start -
                  p_sys->time_total_pauses;
    played_time /= 1000000; 

    /*HACK: it seam that the preparsing sometime fail,
            so use the playing time as the song length */
    if (p_sys->p_current_song.i_l == 0)
        p_sys->p_current_song.i_l = played_time;

    /* Don't send song shorter than 30s */
    if (p_sys->p_current_song.i_l < 30)
    {
        msg_Dbg(p_this, "Song too short (< 30s), not submitting");
        goto end;
    }

    /* Send if the user had listen more than 240s OR half the track length */
    if ((played_time < 240) &&
        (played_time < (p_sys->p_current_song.i_l / 2)))
    {
        msg_Dbg(p_this, "Song not listened long enough, not submitting");
        goto end;
    }

    /* Check that all meta are present */
    if (!p_sys->p_current_song.psz_a || !*p_sys->p_current_song.psz_a ||
        !p_sys->p_current_song.psz_t || !*p_sys->p_current_song.psz_t)
    {
        msg_Dbg(p_this, "Missing artist or title, not submitting");
        goto end;
    }

    if (p_sys->i_songs >= QUEUE_MAX)
    {
        msg_Warn(p_this, "Submission queue is full, not submitting");
        goto end;
    }

    msg_Dbg(p_this, "Song will be submitted.");

#define QUEUE_COPY(a) \
    p_sys->p_queue[p_sys->i_songs].a = p_sys->p_current_song.a

#define QUEUE_COPY_NULL(a) \
    QUEUE_COPY(a); \
    p_sys->p_current_song.a = NULL

    QUEUE_COPY(i_l);
    QUEUE_COPY_NULL(psz_n);
    QUEUE_COPY_NULL(psz_a);
    QUEUE_COPY_NULL(psz_t);
    QUEUE_COPY_NULL(psz_b);
    QUEUE_COPY_NULL(psz_m);
    QUEUE_COPY(date);
#undef QUEUE_COPY_NULL
#undef QUEUE_COPY

    p_sys->i_songs++;

    /* signal the main loop we have something to submit */
    vlc_cond_signal(&p_sys->wait);

    end:
    DeleteSong(&p_sys->p_current_song);
    vlc_mutex_unlock(&p_sys->lock);
}

/*****************************************************************************
 * PlayingChange: Playing status change callback
 *****************************************************************************/
static int PlayingChange(vlc_object_t *p_this, const char *psz_var,
                         vlc_value_t oldval, vlc_value_t newval, void *p_data)
{
    VLC_UNUSED(oldval);

    intf_thread_t   *p_intf = (intf_thread_t*) p_data;
    intf_sys_t      *p_sys  = p_intf->p_sys;
    input_thread_t  *p_input = (input_thread_t*)p_this;
    int             state;

    VLC_UNUSED(psz_var);

    if (newval.i_int != INPUT_EVENT_STATE) return VLC_SUCCESS;

    if (var_CountChoices(p_input, "video-es"))
    {
        msg_Dbg(p_this, "Not an audio-only input, not submitting");
        return VLC_SUCCESS;
    }

    state = var_GetInteger(p_input, "state");

    if (!p_sys->b_meta_read && state >= PLAYING_S)
    {
        ReadMetaData(p_intf, p_input);
        return VLC_SUCCESS;
    }


    if (state >= END_S)
        AddToQueue(p_intf);
    else if (state == PAUSE_S)
        p_sys->time_pause = mdate();
    else if (state == PLAYING_S) {
        if (p_sys->time_pause > 0) {
            mtime_t current_time = mdate();
            mtime_t time_paused = current_time - p_sys->time_pause;
            p_sys->time_total_pauses += time_paused;

            msg_Dbg(p_intf, "Pause duration: %"PRIu64, (time_paused / 1000000));
            //check whether duration of pause is more than 60s
            if ((time_paused / 1000000) > 60) {
                int64_t played_time = current_time - p_sys->p_current_song.i_start - p_sys->time_total_pauses;
                played_time /= 1000000;
                //check whether the item as of now qualifies as a listen
                if ((played_time > 30) &&
                    (played_time > 240 || played_time >= p_sys->p_current_song.i_l / 2)) {
                    AddToQueue(p_intf);
                    ReadMetaData(p_intf, p_input);
                    p_sys->p_current_song.i_start = current_time;
                    time(&p_sys->p_current_song.date);
                    p_sys->time_total_pauses = 0;
                }
            }
            p_sys->time_pause = 0;
        }
    }

    return VLC_SUCCESS;
}

/*****************************************************************************
 * ItemChange: Playlist item change callback
 *****************************************************************************/
static int ItemChange(vlc_object_t *p_this, const char *psz_var,
                      vlc_value_t oldval, vlc_value_t newval, void *p_data)
{
    intf_thread_t  *p_intf  = p_data;
    intf_sys_t     *p_sys   = p_intf->p_sys;
    input_thread_t *p_input = newval.p_address;

    VLC_UNUSED(psz_var);
    VLC_UNUSED(oldval);

    p_sys->b_meta_read      = false;

    if (p_sys->p_input != NULL)
    {
        var_DelCallback(p_sys->p_input, "intf-event", PlayingChange, p_intf);
        vlc_object_release(p_sys->p_input);
        p_sys->p_input = NULL;
    }

    if (p_input == NULL)
        return VLC_SUCCESS;

    input_item_t *p_item = input_GetItem(p_input);
    if (p_item == NULL)
        return VLC_SUCCESS;

    if (var_CountChoices(p_input, "video-es"))
    {
        msg_Dbg(p_this, "Not an audio-only input, not submitting");
        return VLC_SUCCESS;
    }

    p_sys->time_total_pauses = 0;
    time(&p_sys->p_current_song.date);        /* to be sent to listenbrainz */
    p_sys->p_current_song.i_start = mdate();    /* only used locally */

    p_sys->p_input = vlc_object_hold(p_input);
    var_AddCallback(p_input, "intf-event", PlayingChange, p_intf);

    if (input_item_IsPreparsed(p_item))
        ReadMetaData(p_intf, p_input);
    /* if the input item was not preparsed, we'll do it in PlayingChange()
     * callback, when "state" == PLAYING_S */

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Open: initialize and create stuff
 *****************************************************************************/
static int Open(vlc_object_t *p_this)
{
    intf_thread_t   *p_intf     = (intf_thread_t*) p_this;
    intf_sys_t      *p_sys      = calloc(1, sizeof(intf_sys_t));

    if (!p_sys)
        return VLC_ENOMEM;

    p_intf->p_sys = p_sys;

    vlc_mutex_init(&p_sys->lock);
    vlc_cond_init(&p_sys->wait);

    if (vlc_clone(&p_sys->thread, Run, p_intf, VLC_THREAD_PRIORITY_LOW))
    {
        vlc_cond_destroy(&p_sys->wait);
        vlc_mutex_destroy(&p_sys->lock);
        free(p_sys);
        return VLC_ENOMEM;
    }

    var_AddCallback(pl_Get(p_intf), "input-current", ItemChange, p_intf);

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close: destroy interface stuff
 *****************************************************************************/
static void Close(vlc_object_t *p_this)
{
    intf_thread_t               *p_intf = (intf_thread_t*) p_this;
    intf_sys_t                  *p_sys  = p_intf->p_sys;

    vlc_cancel(p_sys->thread);
    vlc_join(p_sys->thread, NULL);

    var_DelCallback(pl_Get(p_intf), "input-current", ItemChange, p_intf);

    if (p_sys->p_input != NULL)
    {
        var_DelCallback(p_sys->p_input, "intf-event", PlayingChange, p_intf);
        vlc_object_release(p_sys->p_input);
    }

    int i;
    for (i = 0; i < p_sys->i_songs; i++)
        DeleteSong(&p_sys->p_queue[i]);
    vlc_UrlClean(&p_sys->p_submit_url);
    vlc_cond_destroy(&p_sys->wait);
    vlc_mutex_destroy(&p_sys->lock);
    free(p_sys);
}

static void HandleInterval(mtime_t *next, unsigned int *i_interval)
{
    if (*i_interval == 0)
    {
        /* first interval is 1 minute */
        *i_interval = 1;
    }
    else
    {
        /* else we double the previous interval, up to 120 minutes */
        *i_interval <<= 1;
        if (*i_interval > 120)
            *i_interval = 120;
    }
    *next = mdate() + (*i_interval * 1000000 * 60);
}

/*****************************************************************************
 * Run : submit songs
 *****************************************************************************/
static void *Run(void *data)
{
    intf_thread_t          *p_intf = data;
    uint8_t                 p_buffer[1024];
    int                     canc = vlc_savecancel();
    char                    *psz_url, *psz_submission_url;
    int                     i_ret;
    time_t                  timestamp;

    /* data about listenbrainz session */
    mtime_t                 next_exchange = 0; /**< when can we send data  */
    unsigned int            i_interval = 0;     /**< waiting interval (secs)*/

    intf_sys_t *p_sys = p_intf->p_sys;
    // TODO: Add wait
    
    /* main loop */
    for (;;)
    {
        vlc_restorecancel(canc);
        mwait(next_exchange);

        vlc_mutex_lock(&p_sys->lock);
        mutex_cleanup_push(&p_sys->lock);

        while (p_sys->i_songs == 0)
            vlc_cond_wait(&p_sys->wait, &p_sys->lock);

        vlc_cleanup_pop();
        vlc_mutex_unlock(&p_sys->lock);
        canc = vlc_savecancel();

        p_sys->psz_user_token = var_InheritString(p_intf, "listenbrainz-usertoken");
        msg_Dbg(p_intf, "Begin...");

        /* usertoken have not been setup */
        if (EMPTY_STR(p_sys->psz_user_token))
        {
            free(p_sys->psz_user_token);
            /* usertoken not set */
            vlc_dialog_display_error(p_intf,
                                     "Listenbrainz usertoken not set",
                                     "%s", "Please set a user token or disable the "
                                             "listenbrainz plugin, and restart VLC.\n"
                                             "Visit https://listenbrainz.org/profile/ to get a user token.");
            goto out;
        }

        time(&timestamp);

        psz_submission_url = var_InheritString(p_intf, "submission-url");
        if (!psz_submission_url)
            goto out;

        i_ret = snprintf(psz_url, sizeof(psz_submission_url) * 2, "https://%s/1/submit-listens", psz_submission_url);

        free(psz_submission_url);
        if (i_ret == -1)
            goto out;

        /* parse the submission url */
        vlc_UrlParse(&p_sys->p_submit_url, psz_url);
        free(psz_url);

        msg_Dbg(p_intf, "Going to submit some data...");
        vlc_url_t *url;
        struct vlc_memstream req, payload;

        vlc_memstream_open(&payload);

        /* forge the HTTP POST request */
        vlc_mutex_lock(&p_sys->lock);

        url = &p_sys->p_submit_url;
        i_interval = 0;
        next_exchange = VLC_TICK_INVALID;

        if(p_sys->i_songs == 1)
            vlc_memstream_printf(&payload, "{\"listen_type\":\"single\",\"payload\":[");
        else
            vlc_memstream_printf(&payload, "{\"listen_type\":\"import\",\"payload\":[");

        for (int i_song = 0 ; i_song < p_sys->i_songs ; i_song++)
        {
            listenbrainz_song_t *p_song = &p_sys->p_queue[i_song];

            vlc_memstream_printf(&payload, "{\"listened_at\": %"PRIu64, (uint64_t)p_song->date);
            vlc_memstream_printf(&payload, ", \"track_metadata\": {\"artist_name\": \"%s\", ", vlc_uri_decode(p_song->psz_a));
            vlc_memstream_printf(&payload, " \"track_name\": \"%s\", ", vlc_uri_decode(p_song->psz_t));
            if (p_song->psz_b != NULL)
                vlc_memstream_printf(&payload, " \"release_name\": \"%s\"", vlc_uri_decode(p_song->psz_b));
            if (p_song->psz_m != NULL)
                vlc_memstream_printf(&payload, ", \"additional_info\": {\"recording_mbid\":\"%s\"} ", vlc_uri_decode(p_song->psz_m));
            vlc_memstream_printf(&payload, "}}");
        }

        vlc_memstream_printf(&payload, "]}");
        vlc_mutex_unlock(&p_sys->lock);

        if (vlc_memstream_close(&payload))
            goto out;

        vlc_memstream_open(&req);
        vlc_memstream_printf(&req, "POST %s HTTP/1.1\r\n", url->psz_path);
        vlc_memstream_printf(&req, "Host: %s\r\n", url->psz_host);
        vlc_memstream_printf(&req, "Authorization: Token %s\r\n", p_sys->psz_user_token);
        vlc_memstream_puts(&req, "User-Agent:"
                                 ""PACKAGE_NAME"/"PACKAGE_VERSION"\r\n");
        vlc_memstream_puts(&req, "Connection: close\r\n");
        vlc_memstream_puts(&req, "Accept-Encoding: identity\r\n");
        vlc_memstream_printf(&req, "Content-Length: %zu\r\n", payload.length);
        vlc_memstream_puts(&req, "\r\n");
        /* Could avoid copying payload with iovec... but efforts */
        vlc_memstream_write(&req, payload.ptr, payload.length);
        vlc_memstream_puts(&req, "\r\n\r\n");
        free(payload.ptr);

        if (vlc_memstream_close(&req)) /* Out of memory */
            goto out;

        msg_Dbg(p_intf, "%s", req.ptr);

        msg_Dbg(p_intf, "Open socket");
        vlc_tls_creds_t *creds = vlc_tls_ClientCreate(VLC_OBJECT(p_intf));
        char *alp;
        vlc_tls_t *sock = vlc_tls_SocketOpenTLS(creds, url->psz_host, 443, NULL, NULL, NULL);
        msg_Dbg(p_intf, "Close socket");

        if (sock == NULL)
        {
            /* If connection fails, we assume we must handshake again */
            HandleInterval(&next_exchange, &i_interval);
            free(req.ptr);
            continue;
        }

        /* we transmit the data */
        msg_Warn(p_intf, "Begin transmission");
        int i_net_ret = vlc_tls_Write(sock, req.ptr, req.length);
        msg_Warn(p_intf, "Transmission End");
        free(req.ptr);
        if (i_net_ret == -1)
        {
            /* If connection fails, we assume we must handshake again */
            HandleInterval(&next_exchange, &i_interval);
            vlc_tls_Close(sock);
            continue;
        }

        /* FIXME: this might wait forever */
        /* FIXME: With TCP, you should never assume that a single read will
         * return the entire response... */
        msg_Warn(p_intf, "Checking response");
        i_net_ret = vlc_tls_Read(sock, p_buffer, sizeof(p_buffer) - 1, false);
        msg_Warn(p_intf, "Response: %s", (char *) p_buffer);
        vlc_tls_Close(sock);
        if (i_net_ret <= 0)
        {
            msg_Warn(p_intf, "No response");
            /* if we get no answer, something went wrong : try again */
            continue;
        }
        p_buffer[i_net_ret] = '\0';
        if (strstr((char *) p_buffer, "OK")) {
            for (int i = 0; i < p_sys->i_songs; i++)
                DeleteSong(&p_sys->p_queue[i]);
            p_sys->i_songs = 0;

            i_interval = 0;
            next_exchange = VLC_TICK_INVALID;
            msg_Dbg(p_intf, "Submission successful!");
        } else {
            msg_Warn(p_intf, "Error: %s", strstr((char *) p_buffer, "FAILED"));
            HandleInterval(&next_exchange, &i_interval);
            continue;
        }
    }
    out:
        vlc_restorecancel(canc);
        return NULL;

}