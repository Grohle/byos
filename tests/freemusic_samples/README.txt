Sample responses for tests/freemusic_test.c (Free Music app).
ia_*.json   recorded with curl from archive.org on 2026-11-10 (advancedsearch, /metadata/<id>,
            /metadata/<id>/files).
flac_head_*.bin  first 8192 bytes (curl -r 0-8191) of real FLAC files from archive.org.
jm_err_*.json    recorded from api.jamendo.com without a key / with an invalid key.
jm_*_doc.json    SYNTHETIC: built by hand from the documented v3.0 schema
                 (developer.jamendo.com/v3.0/tracks, /albums/tracks) because a real response needs a
                 client_id. Replace with a real capture once a key is available
                 (see docs/FREEMUSIC.md, "Jamendo").

Vinyl user sources (tests/freemusic_test.c t_detect/t_catalog/t_rss/t_subsonic):
vinyl_catalog.json  EXAMPLE of the open Vinyl catalog format (docs/FREEMUSIC.md): relative,
                    absolute and ../ URLs, track-level files, rate in kHz, an AAC-only album.
rss_podcast.xml     SYNTHETIC podcast (itunes tags, CDATA, entities, comment, video/AAC skipped).
rss_netlabel.xml    SYNTHETIC netlabel feed with Media RSS (FLAC + MP3 per item, copyright).
atom_feed.xml       SYNTHETIC Atom feed with an enclosure link.
feeds.opml          SYNTHETIC OPML (nested folder, duplicate feed, relative xmlUrl).
sub_*.json          SYNTHETIC Subsonic answers built from the documented API (getAlbumList2,
                    getAlbum with OpenSubsonic bitDepth/samplingRate, error 40).
ia_coll_*.json      recorded with curl from archive.org on 2026-11-10: /metadata/bad-panda/metadata,
                    a missing item, and the rows=0 count of collection:(bad-panda).
