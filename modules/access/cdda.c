/*****************************************************************************
 * cdda.c : CD digital audio input module for vlc
 *****************************************************************************
 * Copyright (C) 2000, 2003 the VideoLAN team
 * $Id: accc7a4f93c8af59c30e1639fbd8b42be3fde7c4 $
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Gildas Bazin <gbazin@netcourrier.com>
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

/**
 * Todo:
 *   - Improve CDDB support (non-blocking, cache, ...)
 *   - Fix tracknumber in MRL
 */

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_input.h>
#include <vlc_access.h>
#include <vlc_meta.h>
#include <vlc_charset.h>

#include <vlc_codecs.h> /* For WAVEHEADER */
#include "vcd/cdrom.h"

#warning playlist code must not be used here.
#include <vlc_playlist.h>

#ifdef HAVE_LIBCDDB
#include <cddb/cddb.h>
#endif

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

/*****************************************************************************
 * Module descriptior
 *****************************************************************************/
static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );

#define CACHING_TEXT N_("Caching value in ms")
#define CACHING_LONGTEXT N_( \
    "Default caching value for Audio CDs. This " \
    "value should be set in milliseconds." )

vlc_module_begin();
    set_shortname( N_("Audio CD"));
    set_description( N_("Audio CD input") );
    set_capability( "access", 10 );
    set_category( CAT_INPUT );
    set_subcategory( SUBCAT_INPUT_ACCESS );
    set_callbacks( Open, Close );

    add_usage_hint( N_("[cdda:][device][@[track]]") );
    add_integer( "cdda-caching", DEFAULT_PTS_DELAY / 1000, NULL, CACHING_TEXT,
                 CACHING_LONGTEXT, true );

    add_integer( "cdda-track", -1 , NULL, NULL, NULL, true );
        change_internal();
    add_integer( "cdda-first-sector", -1, NULL, NULL, NULL, true );
        change_internal();
    add_integer( "cdda-last-sector", -1, NULL, NULL, NULL, true );
        change_internal();

    add_string( "cddb-server", "freedb.freedb.org", NULL,
                N_( "CDDB Server" ), N_( "Address of the CDDB server to use." ),
                true );
    add_integer( "cddb-port", 8880, NULL,
                N_( "CDDB port" ), N_( "CDDB Server port to use." ),
                true );
    add_shortcut( "cdda" );
    add_shortcut( "cddasimple" );
vlc_module_end();


/* how many blocks VCDRead will read in each loop */
#define CDDA_BLOCKS_ONCE 20
#define CDDA_DATA_ONCE   (CDDA_BLOCKS_ONCE * CDDA_DATA_SIZE)

/*****************************************************************************
 * Access: local prototypes
 *****************************************************************************/
struct access_sys_t
{
    vcddev_t    *vcddev;                            /* vcd device descriptor */

    /* Current position */
    int         i_sector;                                  /* Current Sector */
    int *       p_sectors;                                  /* Track sectors */

    /* Wave header for the output data */
    WAVEHEADER  waveheader;
    bool  b_header;

    int         i_track;
    int         i_first_sector;
    int         i_last_sector;

#ifdef HAVE_LIBCDDB
    cddb_disc_t *p_disc;
#endif
};

static block_t *Block( access_t * );
static int      Seek( access_t *, int64_t );
static int      Control( access_t *, int, va_list );

static int GetTracks( access_t *p_access, playlist_t *p_playlist,
                      playlist_item_t *p_parent );

#ifdef HAVE_LIBCDDB
static void GetCDDBInfo( access_t *p_access, int i_titles, int *p_sectors );
#endif

/*****************************************************************************
 * Open: open cdda
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    access_t     *p_access = (access_t*)p_this;
    access_sys_t *p_sys;
    vcddev_t *vcddev;
    char *psz_name;
    int i_mrl_tracknum = -1;
    int i_ret;

    if( !p_access->psz_path || !*p_access->psz_path )
    {
        /* Only when selected */
        if( !p_this->b_force ) return VLC_EGENERIC;

        psz_name = var_CreateGetString( p_this, "cd-audio" );
        if( !psz_name || !*psz_name )
        {
            free( psz_name );
            return VLC_EGENERIC;
        }
    }
    else psz_name = ToLocaleDup( p_access->psz_path );

#ifdef WIN32
    if( psz_name[0] && psz_name[1] == ':' &&
        psz_name[2] == '\\' && psz_name[3] == '\0' ) psz_name[2] = '\0';
#endif

    /* Open CDDA */
    if( (vcddev = ioctl_Open( VLC_OBJECT(p_access), psz_name )) == NULL )
    {
        msg_Warn( p_access, "could not open %s", psz_name );
        free( psz_name );
        return VLC_EGENERIC;
    }
    free( psz_name );

    /* Set up p_access */
    STANDARD_BLOCK_ACCESS_INIT
    p_sys->vcddev = vcddev;

   /* Do we play a single track ? */
   p_sys->i_track = var_CreateGetInteger( p_access, "cdda-track" );

   if( p_sys->i_track < 0 && i_mrl_tracknum <= 0 )
   {
        /* We only do separate items if the whole disc is requested */
        playlist_t *p_playlist = pl_Yield( p_access );

        i_ret = -1;
        if( p_playlist )
        {
            input_thread_t *p_input = (input_thread_t*)vlc_object_find( p_access, VLC_OBJECT_INPUT, FIND_PARENT );
            if( p_input )
            {
                input_item_t *p_current = input_GetItem( p_input );
                playlist_item_t *p_item;

                if( p_playlist->status.p_item->p_input == p_current )
                    p_item = p_playlist->status.p_item;
                else
                    p_item = playlist_ItemGetByInput( p_playlist, p_current, pl_Unlocked );

                if( p_item )
                    i_ret = GetTracks( p_access, p_playlist, p_item );
                else
                    msg_Dbg( p_playlist, "unable to find item in playlist");
                vlc_object_release( p_input );
            }
            pl_Release( p_access );
        }
        if( i_ret < 0 )
            goto error;
    }
    else
    {
        /* Build a WAV header for the output data */
        memset( &p_sys->waveheader, 0, sizeof(WAVEHEADER) );
        SetWLE( &p_sys->waveheader.Format, 1 ); /*WAVE_FORMAT_PCM*/
        SetWLE( &p_sys->waveheader.BitsPerSample, 16);
        p_sys->waveheader.MainChunkID = VLC_FOURCC('R', 'I', 'F', 'F');
        p_sys->waveheader.Length = 0;               /* we just don't know */
        p_sys->waveheader.ChunkTypeID = VLC_FOURCC('W', 'A', 'V', 'E');
        p_sys->waveheader.SubChunkID = VLC_FOURCC('f', 'm', 't', ' ');
        SetDWLE( &p_sys->waveheader.SubChunkLength, 16);
        SetWLE( &p_sys->waveheader.Modus, 2);
        SetDWLE( &p_sys->waveheader.SampleFreq, 44100);
        SetWLE( &p_sys->waveheader.BytesPerSample,
                    2 /*Modus*/ * 16 /*BitsPerSample*/ / 8 );
        SetDWLE( &p_sys->waveheader.BytesPerSec,
                    2*16/8 /*BytesPerSample*/ * 44100 /*SampleFreq*/ );
        p_sys->waveheader.DataChunkID = VLC_FOURCC('d', 'a', 't', 'a');
        p_sys->waveheader.DataLength = 0;           /* we just don't know */

        p_sys->i_first_sector = var_CreateGetInteger( p_access,
                                                      "cdda-first-sector" );
        p_sys->i_last_sector  = var_CreateGetInteger( p_access,
                                                      "cdda-last-sector" );
        /* Tracknumber in MRL */
        if( p_sys->i_first_sector < 0 || p_sys->i_last_sector < 0 )
        {
            int i_titles;
            if( i_mrl_tracknum <= 0 )
            {
                msg_Err( p_access, "wrong sector information" );
                goto error;
            }
            i_titles = ioctl_GetTracksMap( VLC_OBJECT(p_access),
                                            p_sys->vcddev, &p_sys->p_sectors );
        }


        p_sys->i_sector = p_sys->i_first_sector;
        p_access->info.i_size = (p_sys->i_last_sector - p_sys->i_first_sector)
                                     * (int64_t)CDDA_DATA_SIZE;
    }

    /* PTS delay */
    var_Create( p_access, "cdda-caching", VLC_VAR_INTEGER|VLC_VAR_DOINHERIT );

    return VLC_SUCCESS;

error:
    ioctl_Close( VLC_OBJECT(p_access), p_sys->vcddev );
    free( p_sys );
    return VLC_EGENERIC;
}

/*****************************************************************************
 * Close: closes cdda
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    access_t     *p_access = (access_t *)p_this;
    access_sys_t *p_sys = p_access->p_sys;
    ioctl_Close( p_this, p_sys->vcddev );
    free( p_sys );
}

/*****************************************************************************
 * Block: read data (CDDA_DATA_ONCE)
 *****************************************************************************/
static block_t *Block( access_t *p_access )
{
    access_sys_t *p_sys = p_access->p_sys;
    int i_blocks = CDDA_BLOCKS_ONCE;
    block_t *p_block;

    if( p_sys->i_track < 0 ) p_access->info.b_eof = true;

    /* Check end of file */
    if( p_access->info.b_eof ) return NULL;

    if( !p_sys->b_header )
    {
        /* Return only the header */
        p_block = block_New( p_access, sizeof( WAVEHEADER ) );
        memcpy( p_block->p_buffer, &p_sys->waveheader, sizeof(WAVEHEADER) );
        p_sys->b_header = true;
        return p_block;
    }

    if( p_sys->i_sector >= p_sys->i_last_sector )
    {
        p_access->info.b_eof = true;
        return NULL;
    }

    /* Don't read too far */
    if( p_sys->i_sector + i_blocks >= p_sys->i_last_sector )
        i_blocks = p_sys->i_last_sector - p_sys->i_sector;

    /* Do the actual reading */
    if( !( p_block = block_New( p_access, i_blocks * CDDA_DATA_SIZE ) ) )
    {
        msg_Err( p_access, "cannot get a new block of size: %i",
                 i_blocks * CDDA_DATA_SIZE );
        return NULL;
    }

    if( ioctl_ReadSectors( VLC_OBJECT(p_access), p_sys->vcddev,
            p_sys->i_sector, p_block->p_buffer, i_blocks, CDDA_TYPE ) < 0 )
    {
        msg_Err( p_access, "cannot read sector %i", p_sys->i_sector );
        block_Release( p_block );

        /* Try to skip one sector (in case of bad sectors) */
        p_sys->i_sector++;
        p_access->info.i_pos += CDDA_DATA_SIZE;
        return NULL;
    }

    /* Update a few values */
    p_sys->i_sector += i_blocks;
    p_access->info.i_pos += p_block->i_buffer;

    return p_block;
}

/****************************************************************************
 * Seek
 ****************************************************************************/
static int Seek( access_t *p_access, int64_t i_pos )
{
    access_sys_t *p_sys = p_access->p_sys;

    /* Next sector to read */
    p_sys->i_sector = p_sys->i_first_sector + i_pos / CDDA_DATA_SIZE;
    p_access->info.i_pos = i_pos;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Control:
 *****************************************************************************/
static int Control( access_t *p_access, int i_query, va_list args )
{
    bool   *pb_bool;
    int          *pi_int;
    int64_t      *pi_64;

    switch( i_query )
    {
        case ACCESS_CAN_SEEK:
        case ACCESS_CAN_FASTSEEK:
        case ACCESS_CAN_PAUSE:
        case ACCESS_CAN_CONTROL_PACE:
            pb_bool = (bool*)va_arg( args, bool* );
            *pb_bool = true;
            break;

        case ACCESS_GET_MTU:
            pi_int = (int*)va_arg( args, int * );
            *pi_int = CDDA_DATA_ONCE;
            break;

        case ACCESS_GET_PTS_DELAY:
            pi_64 = (int64_t*)va_arg( args, int64_t * );
            *pi_64 = var_GetInteger( p_access, "cdda-caching" ) * 1000;
            break;

        case ACCESS_SET_PAUSE_STATE:
            break;

        case ACCESS_GET_TITLE_INFO:
        case ACCESS_SET_TITLE:
        case ACCESS_GET_META:
        case ACCESS_SET_SEEKPOINT:
        case ACCESS_SET_PRIVATE_ID_STATE:
        case ACCESS_GET_CONTENT_TYPE:
            return VLC_EGENERIC;

        default:
            msg_Warn( p_access, "unimplemented query in control" );
            return VLC_EGENERIC;

    }
    return VLC_SUCCESS;
}

static int GetTracks( access_t *p_access,
                      playlist_t *p_playlist, playlist_item_t *p_parent )
{
    access_sys_t *p_sys = p_access->p_sys;
    int i, i_titles;
    input_item_t *p_input_item;
    playlist_item_t *p_item_in_category;
    char *psz_name;
    i_titles = ioctl_GetTracksMap( VLC_OBJECT(p_access),
                                   p_sys->vcddev, &p_sys->p_sectors );
    if( i_titles < 0 )
    {
        msg_Err( p_access, "unable to count tracks" );
        return VLC_EGENERIC;;
    }
    else if( i_titles <= 0 )
    {
        msg_Err( p_access, "no audio tracks found" );
        return VLC_EGENERIC;
    }

    p_item_in_category = playlist_ItemToNode( p_playlist, p_parent, pl_Unlocked );
    playlist_ItemSetName( p_parent, "Audio CD" );
    var_SetInteger( p_playlist, "item-change", p_parent->p_input->i_id );

#ifdef HAVE_LIBCDDB
    GetCDDBInfo( p_access, i_titles, p_sys->p_sectors );
    if( p_sys->p_disc )
    {
        if( cddb_disc_get_title( p_sys->p_disc ) )
        {
            const char *psz_name = cddb_disc_get_title( p_sys->p_disc );
            playlist_ItemSetName( p_parent, psz_name );
            var_SetInteger( p_playlist, "item-change",
                            p_parent->p_input->i_id );
        }
    }
#endif

    /* Build title table */
    for( i = 0; i < i_titles; i++ )
    {
        msg_Dbg( p_access, "track[%d] start=%d", i, p_sys->p_sectors[i] );
        char *psz_uri, *psz_opt, *psz_first, *psz_last;

        if( asprintf( &psz_uri, "cdda://%s", p_access->psz_path ? p_access->psz_path : "" ) == -1 )
            psz_uri = NULL;
        if( asprintf( &psz_opt, "cdda-track=%i", i+1 ) == -1 )
            psz_opt = NULL;
        if( asprintf( &psz_first, "cdda-first-sector=%i",p_sys->p_sectors[i] ) == -1 )
            psz_first = NULL;

//        if( i != i_titles -1 )
//        {
            if( asprintf( &psz_last, "cdda-last-sector=%i", p_sys->p_sectors[i+1] ) == -1 )
                psz_last = NULL;
//        }
//        else
//        {
//            if( asprintf( &psz_last, "cdda-last-sector=%i", 1242 /* FIXME */) == -1 )
//                psz_last = NULL;
//        }

        /* Define a "default name" */
        if( asprintf( &psz_name, _("Audio CD - Track %i"), (i+1) ) == -1 )
            psz_name = NULL;

        /* Create playlist items */
        p_input_item = input_item_NewWithType( VLC_OBJECT( p_playlist ),
                                              psz_uri, psz_name, 0, NULL, -1,
                                              ITEM_TYPE_DISC );
        input_item_AddOption( p_input_item, psz_first );
        input_item_AddOption( p_input_item, psz_last );
        input_item_AddOption( p_input_item, psz_opt );

#ifdef HAVE_LIBCDDB
        /* If we have CDDB info, change the name */
        if( p_sys->p_disc )
        {
            cddb_track_t *t = cddb_disc_get_track( p_sys->p_disc, i );
            if( t!= NULL )
            {
                if( cddb_track_get_title( t )  != NULL )
                {
                    free( p_input_item->psz_name );
                    p_input_item->psz_name = strdup( cddb_track_get_title( t ) );
                    input_item_SetTitle( p_input_item, cddb_track_get_title( t ) );
                }
                if( cddb_track_get_artist( t ) != NULL )
                {
                    input_item_SetArtist( p_input_item, cddb_track_get_artist( t ) );
                }
            }
        }
#endif
        int i_ret = playlist_BothAddInput( p_playlist, p_input_item,
                               p_item_in_category,
                               PLAYLIST_APPEND, PLAYLIST_END, NULL, NULL,
                               pl_Unlocked );
        vlc_gc_decref( p_input_item );
        free( psz_uri ); free( psz_opt ); free( psz_name );
        free( psz_first ); free( psz_last );
        if( i_ret != VLC_SUCCESS )
            return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

#ifdef HAVE_LIBCDDB
static void GetCDDBInfo( access_t *p_access, int i_titles, int *p_sectors )
{
    int i, i_matches;
    int64_t  i_length = 0, i_size = 0;
    cddb_conn_t  *p_cddb = cddb_new();

    if( !p_cddb )
    {
        msg_Warn( p_access, "unable to use CDDB" );
        goto cddb_destroy;
    }

    char* psz_tmp = config_GetPsz( p_access, "cddb-server" );
    cddb_set_email_address( p_cddb, "vlc@videolan.org" );
    cddb_set_server_name( p_cddb, psz_tmp );
    cddb_set_server_port( p_cddb, config_GetInt( p_access, "cddb-port" ) );
    free( psz_tmp );

    /// \todo
    cddb_cache_disable( p_cddb );

//    cddb_cache_set_dir( p_cddb,
//                     config_GetPsz( p_access,
//                                    MODULE_STRING "-cddb-cachedir") );

    cddb_set_timeout( p_cddb, 10 );

    /// \todo
    cddb_http_disable( p_cddb);

    p_access->p_sys->p_disc = cddb_disc_new();

    if(! p_access->p_sys->p_disc )
    {
        msg_Err( p_access, "unable to create CDDB disc structure." );
        goto cddb_end;
    }

    for(i = 0; i < i_titles ; i++ )
    {
        cddb_track_t *t = cddb_track_new();
        cddb_track_set_frame_offset(t, p_sectors[i] );
        cddb_disc_add_track( p_access->p_sys->p_disc, t );
        i_size = ( p_sectors[i+1] - p_sectors[i] ) *
                   (int64_t)CDDA_DATA_SIZE;
        i_length += INT64_C(1000000) * i_size / 44100 / 4  ;
    }

    cddb_disc_set_length( p_access->p_sys->p_disc, (int)(i_length/1000000) );

    if (!cddb_disc_calc_discid(p_access->p_sys->p_disc ))
    {
        msg_Err( p_access, "CDDB disc ID calculation failed" );
        goto cddb_destroy;
    }

    i_matches = cddb_query( p_cddb, p_access->p_sys->p_disc);

    if (i_matches > 0)
    {
        if (i_matches > 1)
             msg_Warn( p_access, "found %d matches in CDDB. Using first one.",
                                 i_matches);
        cddb_read( p_cddb, p_access->p_sys->p_disc );
    }
    else
        msg_Warn( p_access, "CDDB error: %s", cddb_error_str(errno));

cddb_destroy:
    cddb_destroy( p_cddb);

cddb_end: ;
}
#endif /*HAVE_LIBCDDB*/
