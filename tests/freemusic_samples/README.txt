Sample responses for tests/freemusic_test.c (Free Music app).
ia_*.json   recorded with curl from archive.org on 2026-11-10 (advancedsearch, /metadata/<id>,
            /metadata/<id>/files).
flac_head_*.bin  first 8192 bytes (curl -r 0-8191) of real FLAC files from archive.org.
jm_err_*.json    recorded from api.jamendo.com without a key / with an invalid key.
jm_*_doc.json    SYNTHETIC: built by hand from the documented v3.0 schema
                 (developer.jamendo.com/v3.0/tracks, /albums/tracks) because a real response needs a
                 client_id. Replace with a real capture once a key is available
                 (see docs/FREEMUSIC.md, "Jamendo").
