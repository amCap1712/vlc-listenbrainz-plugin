/*****************************************************************************
 * listenbrainz.c : ListenBrainz submission plugin
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

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>
#include <time.h>

#define VLC_MODULE_LICENSE VLC_LICENSE_GPL_2_PLUS
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_interface.h>
#include <vlc_input_item.h>
#include <vlc_dialog.h>
#include <vlc_meta.h>
#include <vlc_memstream.h>
#include <vlc_stream.h>
#include <vlc_url.h>
#include <vlc_tls.h>
#include <vlc_player.h>
#include <vlc_playlist.h>

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
    vlc_tick_t  i_start;            /**< playing start    */
} listenbrainz_song_t;

struct intf_sys_t
{
    listenbrainz_song_t     p_queue[QUEUE_MAX]; /**< songs not submitted yet*/
    int                     i_songs;            /**< number of songs        */

    vlc_playlist_t                  *playlist;
    struct vlc_playlist_listener_id *playlist_listener;
    struct vlc_player_listener_id   *player_listener;

    vlc_mutex_t             lock;               /**< p_sys mutex            */
    vlc_cond_t              wait;               /**< song to submit event   */
    vlc_thread_t            thread;             /**< thread to submit song  */

    /* submission of played songs */
    vlc_url_t               p_submit_url;       /**< where to submit data   */

    char                    *psz_user_token;    /**< Authentication token */

    /* data about song currently playing */
    listenbrainz_song_t     p_current_song;       /**< song being played      */

    vlc_tick_t              time_pause;         /**< time when vlc paused   */
    vlc_tick_t              time_total_pauses;  /**< total time in pause    */

    bool                    b_meta_read;        /**< if we read the song's
                                                 * metadata already         */
};

static int  Open            (vlc_object_t *);
static void Close           (vlc_object_t *);
static void *Run            (void *);

#define USERTOKEN_TEXT      N_("User token")
#define USERTOKEN_LONGTEXT  N_("The user token of your ListenBrainz account")
#define URL_TEXT            N_("Submission URL")
#define URL_LONGTEXT        N_("The URL set for an alternative ListenBrainz instance")

/* This error value is used when ListenBrainz plugin has to be unloaded. */
#define VLC_LISTENBRAINZ_EFATAL -72

/* ListenBrainz client identifier */
#define CLIENT_NAME     PACKAGE
#define CLIENT_VERSION  VERSION

/*****************************************************************************
 * Module descriptor
 ****************************************************************************/

vlc_module_begin ()
    set_category(CAT_INTERFACE)
    set_subcategory(SUBCAT_INTERFACE_CONTROL)
    set_shortname(N_("Listenbrainz"))
    set_description(N_("Submission of played songs to ListenBrainz"))
    add_string("listenbrainz-usertoken", "", USERTOKEN_TEXT, USERTOKEN_LONGTEXT, false)
    add_string("submission-url", "api.listenbrainz.org", URL_TEXT, URL_LONGTEXT, false)
    set_capability("interface", 0)
    set_callbacks(Open, Close)
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
static void ReadMetaData(intf_thread_t *p_this)
{
    intf_sys_t *p_sys = p_this->p_sys;

    vlc_player_t *player = vlc_playlist_GetPlayer(p_sys->playlist);
    input_item_t *item = vlc_player_GetCurrentMedia(player);
    if (item == NULL)
        return;

#define ALLOC_ITEM_META(a, b) do { \
        char *psz_meta = input_item_Get##b(item); \
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

    ALLOC_ITEM_META(p_sys->p_current_song.psz_b, Album);
    ALLOC_ITEM_META(p_sys->p_current_song.psz_m, TrackID);
    ALLOC_ITEM_META(p_sys->p_current_song.psz_n, TrackNum);

    p_sys->p_current_song.i_l = SEC_FROM_VLC_TICK(input_item_GetDuration(item));

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
    int64_t                     played_time;
    intf_sys_t                  *p_sys = p_this->p_sys;

    vlc_mutex_lock(&p_sys->lock);

    /* Check that we have the mandatory meta data */
    if (!p_sys->p_current_song.psz_t || !p_sys->p_current_song.psz_a)
        goto end;

    /* wait for the user to listen enough before submitting */
    played_time = SEC_FROM_VLC_TICK(vlc_tick_now() - p_sys->p_current_song.i_start -
                                    p_sys->time_total_pauses);

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

static void player_on_state_changed(vlc_player_t *player,
                                    enum vlc_player_state state, void *data)
{
    intf_thread_t *intf = data;
    intf_sys_t *sys = intf->p_sys;

    if (vlc_player_GetVideoTrackCount(player))
    {
        msg_Dbg(intf, "Not an audio-only input, not submitting");
        return;
    }

    if (!sys->b_meta_read && state >= VLC_PLAYER_STATE_PLAYING)
    {
        ReadMetaData(intf);
        return;
    }

    switch (state)
    {
        case VLC_PLAYER_STATE_STOPPED:
            AddToQueue(intf);
            break;
        case VLC_PLAYER_STATE_PAUSED:
            sys->time_pause = vlc_tick_now();
            break;
        case VLC_PLAYER_STATE_PLAYING:
            if (sys->time_pause > 0)
            {
                vlc_tick_t current_time = vlc_tick_now();
                vlc_tick_t time_paused = current_time - sys->time_pause;
                sys->time_total_pauses += time_paused;

                msg_Dbg(intf, "Pause duration: %ld",SEC_FROM_VLC_TICK(time_paused));
                //check whether duration of pause is more than 60s
                if(SEC_FROM_VLC_TICK(time_paused) > 60)
                {
                    int64_t played_time = SEC_FROM_VLC_TICK(current_time
                            - sys->p_current_song.i_start - sys->time_total_pauses);

                    //check whether the item as of now qualifies as a listen
                    if((played_time > 30) &&
                        (played_time > 240 || played_time >= sys->p_current_song.i_l / 2))
                    {
                        AddToQueue(intf);
                        ReadMetaData(intf);
                        sys->p_current_song.i_start = current_time;
                        time(&sys->p_current_song.date);
                        sys->time_total_pauses = 0;
                    }
                }
                sys->time_pause = 0;
            }
            break;
        default:
            break;
    }
}

/*****************************************************************************
 * ItemChange: Playlist item change callback
 *****************************************************************************/
static void playlist_on_current_index_changed(vlc_playlist_t *playlist,
                                              ssize_t index, void *userdata)
{
    VLC_UNUSED(index);

    intf_thread_t *intf = userdata;
    if(index > 0)
        AddToQueue(intf);

    intf_sys_t *sys = intf->p_sys;
    sys->b_meta_read = false;

    vlc_player_t *player = vlc_playlist_GetPlayer(playlist);
    input_item_t *item = vlc_player_GetCurrentMedia(player);

    if (!item)
        return;

    if (vlc_player_GetVideoTrackCount(player))
    {
        msg_Dbg(intf, "Not an audio-only input, not submitting");
        return;
    }

    sys->time_total_pauses = 0;
    time(&sys->p_current_song.date);                /* to be sent to ListenBrainz */
    sys->p_current_song.i_start = vlc_tick_now();   /* only used locally */

    if (input_item_IsPreparsed(item))
        ReadMetaData(intf);
    /* if the input item was not preparsed, we'll do it in player_on_state_changed()
     * callback, when "state" == VLC_PLAYER_STATE_PLAYING */
}

/*****************************************************************************
 * Open: initialize and create stuff
 *****************************************************************************/
static int Open(vlc_object_t *p_this)
{
    intf_thread_t   *p_intf     = (intf_thread_t*) p_this;
    intf_sys_t      *p_sys      = calloc(1, sizeof(intf_sys_t));
    int             retval      = VLC_EGENERIC;

    if (!p_sys)
        return VLC_ENOMEM;

    p_intf->p_sys = p_sys;

    static struct vlc_playlist_callbacks const playlist_cbs =
            {
                    .on_current_index_changed = playlist_on_current_index_changed,
            };
    static struct vlc_player_cbs const player_cbs =
            {
                    .on_state_changed = player_on_state_changed,
            };

    vlc_playlist_t *playlist = p_sys->playlist = vlc_intf_GetMainPlaylist(p_intf);
    vlc_player_t *player = vlc_playlist_GetPlayer(playlist);

    vlc_playlist_Lock(playlist);
    p_sys->playlist_listener =
            vlc_playlist_AddListener(playlist, &playlist_cbs, p_intf, false);
    if (!p_sys->playlist_listener)
    {
        vlc_playlist_Unlock(playlist);
        goto fail;
    }

    p_sys->player_listener =
            vlc_player_AddListener(player, &player_cbs, p_intf);
    vlc_playlist_Unlock(playlist);
    if (!p_sys->player_listener)
        goto fail;

    vlc_mutex_init(&p_sys->lock);
    vlc_cond_init(&p_sys->wait);

    if (vlc_clone(&p_sys->thread, Run, p_intf, VLC_THREAD_PRIORITY_LOW))
    {
        retval = VLC_ENOMEM;
        goto fail;
    }

    retval = VLC_SUCCESS;
    goto ret;
    fail:
    if (p_sys->playlist_listener)
    {
        vlc_playlist_Lock(playlist);
        if (p_sys->player_listener)
        {
            vlc_cond_destroy(&p_sys->wait);
            vlc_mutex_destroy(&p_sys->lock);
            vlc_player_RemoveListener(player, p_sys->player_listener);
        }
        vlc_playlist_RemoveListener(playlist, p_sys->playlist_listener);
        vlc_playlist_Unlock(playlist);
    }
    free(p_sys);
    ret:
    return retval;
}

/*****************************************************************************
 * Close: destroy interface stuff
 *****************************************************************************/
static void Close(vlc_object_t *p_this)
{
    intf_thread_t *p_intf = (intf_thread_t*) p_this;
    intf_sys_t *p_sys = p_intf->p_sys;
    vlc_playlist_t *playlist = p_sys->playlist;

    vlc_cancel(p_sys->thread);
    vlc_join(p_sys->thread, NULL);

    int i;
    for (i = 0; i < p_sys->i_songs; i++)
        DeleteSong(&p_sys->p_queue[i]);
    vlc_UrlClean(&p_sys->p_submit_url);

    vlc_cond_destroy(&p_sys->wait);
    vlc_mutex_destroy(&p_sys->lock);

    vlc_playlist_Lock(playlist);
    vlc_player_RemoveListener(
            vlc_playlist_GetPlayer(playlist), p_sys->player_listener);
    vlc_playlist_RemoveListener(playlist, p_sys->playlist_listener);
    vlc_playlist_Unlock(playlist);

    free(p_sys);
}

static void HandleInterval(vlc_tick_t *next, unsigned int *i_interval)
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
    *next = vlc_tick_now() + (*i_interval * VLC_TICK_FROM_SEC(60));
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
    /* data about ListenBrainz session */
    vlc_tick_t              next_exchange = VLC_TICK_INVALID; /**< when can we send data  */
    unsigned int            i_interval = 0;     /**< waiting interval (secs)*/

    intf_sys_t *p_sys = p_intf->p_sys;
    vlc_tick_wait(vlc_tick_now() +   VLC_TICK_FROM_SEC(60));
    /* main loop */
    for (;;)
    {
        vlc_restorecancel(canc);
        if (next_exchange != VLC_TICK_INVALID)
            vlc_tick_wait(next_exchange);

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
                                     _("Listenbrainz usertoken not set"),
                                     "%s", _("Please set a user token or disable the "
                                             "ListenBrainz plugin, and restart VLC.\n"
                                             "Visit https://listenbrainz.org/profile/ to get a user token."));
            goto out;
        }

        time(&timestamp);

        psz_submission_url = var_InheritString(p_intf, "submission-url");
        if (!psz_submission_url)
            goto out;

        i_ret = asprintf(&psz_url, "https://%s/1/submit-listens", psz_submission_url);

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
                                 " "PACKAGE_NAME"/"PACKAGE_VERSION"\r\n");
        vlc_memstream_puts(&req, "Connection: close\r\n");
        vlc_memstream_puts(&req, "Accept-Encoding: identity\r\n");
        vlc_memstream_printf(&req, "Content-Length: %zu\r\n", payload.length);
        vlc_memstream_puts(&req, "Content-Type: application/json\r\n");
        vlc_memstream_puts(&req, "\r\n");
        /* Could avoid copying payload with iovec... but efforts */
        vlc_memstream_write(&req, payload.ptr, payload.length);
        vlc_memstream_puts(&req, "\r\n\r\n");
        free(payload.ptr);

        if (vlc_memstream_close(&req)) /* Out of memory */
            goto out;

        msg_Dbg(p_intf, "%s", req.ptr);

        msg_Dbg(p_intf, "Open socket");
        vlc_tls_client_t *creds = vlc_tls_ClientCreate(VLC_OBJECT(p_intf));
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

