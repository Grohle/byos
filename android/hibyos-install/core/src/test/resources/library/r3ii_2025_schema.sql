-- table BOOK_TABLE BOOK_TABLE
CREATE TABLE BOOK_TABLE(id INT,path TEXT COLLATE NOCASE,name TEXT COLLATE NOCASE,character TEXT COLLATE NOCASE,PRIMARY KEY(path));
-- index sqlite_autoindex_BOOK_TABLE_1 BOOK_TABLE
;
-- table BOOK_COLLECT_TABLE BOOK_COLLECT_TABLE
CREATE TABLE BOOK_COLLECT_TABLE(id INT,path TEXT COLLATE NOCASE,name TEXT COLLATE NOCASE,character TEXT COLLATE NOCASE,PRIMARY KEY(path));
-- index sqlite_autoindex_BOOK_COLLECT_TABLE_1 BOOK_COLLECT_TABLE
;
-- table BOOK_RECENT_TABLE BOOK_RECENT_TABLE
CREATE TABLE BOOK_RECENT_TABLE(id INT,path TEXT COLLATE NOCASE,name TEXT COLLATE NOCASE,character TEXT COLLATE NOCASE,PRIMARY KEY(path));
-- index sqlite_autoindex_BOOK_RECENT_TABLE_1 BOOK_RECENT_TABLE
;
-- table BOOKMARK_TABLE BOOKMARK_TABLE
CREATE TABLE BOOKMARK_TABLE(id INT,path TEXT COLLATE NOCASE,name TEXT COLLATE NOCASE, filepos INT, type INT, PRIMARY KEY(path, filepos, type));
-- index sqlite_autoindex_BOOKMARK_TABLE_1 BOOKMARK_TABLE
;
-- table MEDIA_TABLE MEDIA_TABLE
CREATE TABLE MEDIA_TABLE(id INT,path TEXT COLLATE NOCASE,name TEXT COLLATE NOCASE,album TEXT COLLATE NOCASE,artist TEXT COLLATE NOCASE,genre TEXT COLLATE NOCASE,year INT,dis_id INT,ck_id INT,has_child_file INT,begin_time INT,end_time INT,cue_id INT,character TEXT COLLATE NOCASE,size INT,sample_rate INT,bit_rate INT,bit INT,channel INT,format INT,quality TEXT COLLATE NOCASE,album_pic_path TEXT COLLATE NOCASE,lrc_path TEXT COLLATE NOCASE,track_gain REAL,track_peak REAL, ctime INT, mtime INT, pinyin_charater TEXT, album_artist TEXT,PRIMARY KEY(id, path, cue_id));
-- index sqlite_autoindex_MEDIA_TABLE_1 MEDIA_TABLE
;
-- table MEDIA2_TABLE MEDIA2_TABLE
CREATE TABLE MEDIA2_TABLE(id INT,path TEXT COLLATE NOCASE,name TEXT COLLATE NOCASE,album TEXT COLLATE NOCASE,artist TEXT COLLATE NOCASE,genre TEXT COLLATE NOCASE,year INT,dis_id INT,ck_id INT,has_child_file INT,begin_time INT,end_time INT,cue_id INT,character TEXT COLLATE NOCASE,size INT,sample_rate INT,bit_rate INT,bit INT,channel INT,format INT,quality TEXT COLLATE NOCASE,album_pic_path TEXT COLLATE NOCASE,lrc_path TEXT COLLATE NOCASE,track_gain REAL,track_peak REAL, ctime INT, mtime INT, pinyin_charater TEXT, album_artist TEXT,PRIMARY KEY(id, path, cue_id));
-- index sqlite_autoindex_MEDIA2_TABLE_1 MEDIA2_TABLE
;
-- table MEDIA3_TABLE MEDIA3_TABLE
CREATE TABLE MEDIA3_TABLE(id INT,path TEXT COLLATE NOCASE,name TEXT COLLATE NOCASE,album TEXT COLLATE NOCASE,artist TEXT COLLATE NOCASE,genre TEXT COLLATE NOCASE,year INT,dis_id INT,ck_id INT,has_child_file INT,begin_time INT,end_time INT,cue_id INT,character TEXT COLLATE NOCASE,size INT,sample_rate INT,bit_rate INT,bit INT,channel INT,format INT,quality TEXT COLLATE NOCASE,album_pic_path TEXT COLLATE NOCASE,lrc_path TEXT COLLATE NOCASE,track_gain REAL,track_peak REAL, ctime INT, mtime INT, pinyin_charater TEXT, album_artist TEXT,PRIMARY KEY(id, path, cue_id));
-- index sqlite_autoindex_MEDIA3_TABLE_1 MEDIA3_TABLE
;
-- table ALBUM_TABLE ALBUM_TABLE
CREATE TABLE ALBUM_TABLE(id INT, album TEXT COLLATE NOCASE,character TEXT COLLATE NOCASE, cn INT, ctime INT, mtime INT, mqa INT, pinyin_charater TEXT, PRIMARY KEY(album));
-- index sqlite_autoindex_ALBUM_TABLE_1 ALBUM_TABLE
;
-- table ARTIST_TABLE ARTIST_TABLE
CREATE TABLE ARTIST_TABLE(id INT, artist TEXT COLLATE NOCASE,character TEXT COLLATE NOCASE, cn INT, ctime INT, mtime INT, pinyin_charater TEXT, PRIMARY KEY(artist));
-- index sqlite_autoindex_ARTIST_TABLE_1 ARTIST_TABLE
;
-- table ALBUM_ARTIST_TABLE ALBUM_ARTIST_TABLE
CREATE TABLE ALBUM_ARTIST_TABLE(id INT, album_artist TEXT COLLATE NOCASE,character TEXT COLLATE NOCASE, cn INT, ctime INT, mtime INT,mqa INT,pinyin_charater TEXT,PRIMARY KEY(album_artist));
-- index sqlite_autoindex_ALBUM_ARTIST_TABLE_1 ALBUM_ARTIST_TABLE
;
-- table GENRE_TABLE GENRE_TABLE
CREATE TABLE GENRE_TABLE( id INT,genre TEXT COLLATE NOCASE,character TEXT COLLATE NOCASE, cn INT, ctime INT, mtime INT, pinyin_charater TEXT, PRIMARY KEY(genre));
-- index sqlite_autoindex_GENRE_TABLE_1 GENRE_TABLE
;
-- table COLLECT_TABLE COLLECT_TABLE
CREATE TABLE COLLECT_TABLE(id INT,path TEXT COLLATE NOCASE,name TEXT COLLATE NOCASE,album TEXT COLLATE NOCASE,artist TEXT COLLATE NOCASE,genre TEXT COLLATE NOCASE,year INT,dis_id INT,ck_id INT,has_child_file INT,begin_time INT,end_time INT,cue_id INT,character TEXT COLLATE NOCASE,size INT,sample_rate INT,bit_rate INT,bit INT,channel INT,format INT,quality TEXT COLLATE NOCASE,album_pic_path TEXT COLLATE NOCASE,lrc_path TEXT COLLATE NOCASE,track_gain REAL,track_peak REAL, ctime INT, mtime INT, pinyin_charater TEXT, album_artist TEXT,PRIMARY KEY(id, path, cue_id));
-- index sqlite_autoindex_COLLECT_TABLE_1 COLLECT_TABLE
;
-- table COLLECT_OPERATE_TABLE COLLECT_OPERATE_TABLE
CREATE TABLE COLLECT_OPERATE_TABLE(id INT,path TEXT COLLATE NOCASE,name TEXT COLLATE NOCASE,album TEXT COLLATE NOCASE,artist TEXT COLLATE NOCASE,genre TEXT COLLATE NOCASE,year INT,dis_id INT,ck_id INT,has_child_file INT,begin_time INT,end_time INT,cue_id INT,character TEXT COLLATE NOCASE,size INT,sample_rate INT,bit_rate INT,bit INT,channel INT,format INT,quality TEXT COLLATE NOCASE,album_pic_path TEXT COLLATE NOCASE,lrc_path TEXT COLLATE NOCASE,track_gain REAL,track_peak REAL, ctime INT, mtime INT, pinyin_charater TEXT, album_artist TEXT,PRIMARY KEY(id, path, cue_id));
-- index sqlite_autoindex_COLLECT_OPERATE_TABLE_1 COLLECT_OPERATE_TABLE
;
-- table HISTORY_TABLE HISTORY_TABLE
CREATE TABLE HISTORY_TABLE(id INT,path TEXT COLLATE NOCASE,name TEXT COLLATE NOCASE,album TEXT COLLATE NOCASE,artist TEXT COLLATE NOCASE,genre TEXT COLLATE NOCASE,year INT,dis_id INT,ck_id INT,has_child_file INT,begin_time INT,end_time INT,cue_id INT,character TEXT COLLATE NOCASE,size INT,sample_rate INT,bit_rate INT,bit INT,channel INT,format INT,quality TEXT COLLATE NOCASE,album_pic_path TEXT COLLATE NOCASE,lrc_path TEXT COLLATE NOCASE,track_gain REAL,track_peak REAL, ctime INT, mtime INT, pinyin_charater TEXT, album_artist TEXT,PRIMARY KEY(id, path, cue_id));
-- index sqlite_autoindex_HISTORY_TABLE_1 HISTORY_TABLE
;
-- table VERSION_TABLE VERSION_TABLE
CREATE TABLE VERSION_TABLE( version_id INT,PRIMARY KEY(version_id));
-- index sqlite_autoindex_VERSION_TABLE_1 VERSION_TABLE
;
-- table COUNT_TABLE COUNT_TABLE
CREATE TABLE COUNT_TABLE( cn INT);
-- table ALBUM2_TABLE ALBUM2_TABLE
CREATE TABLE ALBUM2_TABLE(id INT,album TEXT COLLATE NOCASE,character TEXT COLLATE NOCASE, cn INT, ctime INT, mtime INT, mqa INT, pinyin_charater TEXT,  PRIMARY KEY(album));
-- index sqlite_autoindex_ALBUM2_TABLE_1 ALBUM2_TABLE
;
-- table ARTIST2_TABLE ARTIST2_TABLE
CREATE TABLE ARTIST2_TABLE(id INT,artist TEXT COLLATE NOCASE,character TEXT COLLATE NOCASE,cn INT, ctime INT, mtime INT, pinyin_charater TEXT, PRIMARY KEY(artist));
-- index sqlite_autoindex_ARTIST2_TABLE_1 ARTIST2_TABLE
;
-- table ALBUM_ARTIST2_TABLE ALBUM_ARTIST2_TABLE
CREATE TABLE ALBUM_ARTIST2_TABLE(id INT,album_artist TEXT COLLATE NOCASE,character TEXT COLLATE NOCASE,cn INT, ctime INT, mtime INT,mqa INT,pinyin_charater TEXT,PRIMARY KEY(album_artist));
-- index sqlite_autoindex_ALBUM_ARTIST2_TABLE_1 ALBUM_ARTIST2_TABLE
;
-- table GENRE2_TABLE GENRE2_TABLE
CREATE TABLE GENRE2_TABLE(id INT,genre TEXT COLLATE NOCASE,character TEXT COLLATE NOCASE,cn INT, ctime INT, mtime INT, pinyin_charater TEXT, PRIMARY KEY(genre));
-- index sqlite_autoindex_GENRE2_TABLE_1 GENRE2_TABLE
;
-- table SEARCH_TABLE SEARCH_TABLE
CREATE TABLE SEARCH_TABLE(id INT,path TEXT COLLATE NOCASE,name TEXT COLLATE NOCASE,album TEXT COLLATE NOCASE,artist TEXT COLLATE NOCASE,genre TEXT COLLATE NOCASE,year INT,dis_id INT,ck_id INT,has_child_file INT,begin_time INT,end_time INT,cue_id INT,character TEXT COLLATE NOCASE,size INT,sample_rate INT,bit_rate INT,bit INT,channel INT,format INT,quality TEXT COLLATE NOCASE,album_pic_path TEXT COLLATE NOCASE,lrc_path TEXT COLLATE NOCASE,track_gain REAL,track_peak REAL, ctime INT, mtime INT, pinyin_charater TEXT, album_artist TEXT,PRIMARY KEY(id, path, cue_id));
-- index sqlite_autoindex_SEARCH_TABLE_1 SEARCH_TABLE
;
-- table M3U_TABLE M3U_TABLE
CREATE TABLE M3U_TABLE(id INT,path TEXT COLLATE NOCASE,name TEXT COLLATE NOCASE,character TEXT COLLATE NOCASE,PRIMARY KEY(path));
-- index sqlite_autoindex_M3U_TABLE_1 M3U_TABLE
;
-- table PLAYLIST_TABLE PLAYLIST_TABLE
CREATE TABLE PLAYLIST_TABLE(id INT,path TEXT COLLATE NOCASE,name TEXT COLLATE NOCASE,character TEXT COLLATE NOCASE,cn INT, PRIMARY KEY(path));
-- index sqlite_autoindex_PLAYLIST_TABLE_1 PLAYLIST_TABLE
;
-- table MANAGER_TABLE MANAGER_TABLE
CREATE TABLE MANAGER_TABLE(id INT,path TEXT COLLATE NOCASE,count INT,music_count INT,dir_count INT,m3u_count INT, PRIMARY KEY(path));
-- index sqlite_autoindex_MANAGER_TABLE_1 MANAGER_TABLE
;
-- table STEP_TABLE STEP_TABLE
CREATE TABLE STEP_TABLE(date_col TEXT COLLATE NOCASE, step INT, PRIMARY KEY(date_col));
-- index sqlite_autoindex_STEP_TABLE_1 STEP_TABLE
;
-- table CTIME_TABLE CTIME_TABLE
CREATE TABLE CTIME_TABLE(media_id INT, PRIMARY KEY(media_id));
-- index sqlite_autoindex_CTIME_TABLE_1 CTIME_TABLE
;
-- table MTIME_TABLE MTIME_TABLE
CREATE TABLE MTIME_TABLE(media_id INT, PRIMARY KEY(media_id));
-- index sqlite_autoindex_MTIME_TABLE_1 MTIME_TABLE
;
-- table SORT_TABLE SORT_TABLE
CREATE TABLE SORT_TABLE(s_id INT, PRIMARY KEY(s_id));
-- index sqlite_autoindex_SORT_TABLE_1 SORT_TABLE
;
-- table FORMAT_TABLE FORMAT_TABLE
CREATE TABLE FORMAT_TABLE( id INT, format TEXT COLLATE NOCASE, character TEXT COLLATE NOCASE, cn INT, PRIMARY KEY(format));
-- index sqlite_autoindex_FORMAT_TABLE_1 FORMAT_TABLE
;
-- table FORMAT2_TABLE FORMAT2_TABLE
CREATE TABLE FORMAT2_TABLE( id INT, format TEXT COLLATE NOCASE, character TEXT COLLATE NOCASE, cn INT, PRIMARY KEY(format));
-- index sqlite_autoindex_FORMAT2_TABLE_1 FORMAT2_TABLE
;
-- table RECENT_TABLE RECENT_TABLE
CREATE TABLE RECENT_TABLE(id INT,path TEXT COLLATE NOCASE,name TEXT COLLATE NOCASE,album TEXT COLLATE NOCASE,artist TEXT COLLATE NOCASE,genre TEXT COLLATE NOCASE,year INT,dis_id INT,ck_id INT,has_child_file INT,begin_time INT,end_time INT,cue_id INT,character TEXT COLLATE NOCASE,size INT,sample_rate INT,bit_rate INT,bit INT,channel INT,format INT,quality TEXT COLLATE NOCASE,album_pic_path TEXT COLLATE NOCASE,lrc_path TEXT COLLATE NOCASE,track_gain REAL,track_peak REAL,ctime INT,mtime INT, pinyin_charater TEXT, album_artist TEXT,PRIMARY KEY(id, path, cue_id));
-- index sqlite_autoindex_RECENT_TABLE_1 RECENT_TABLE
;
-- table INFO_TABLE INFO_TABLE
CREATE TABLE INFO_TABLE(id INT, released_at INT, artists TEXT COLLATE NOCASE, album_id TEXT COLLATE NOCASE, album_description TEXT COLLATE NOCASE, PRIMARY KEY(id));
-- index sqlite_autoindex_INFO_TABLE_1 INFO_TABLE
;
-- index charater_index MEDIA_TABLE
CREATE INDEX charater_index ON MEDIA_TABLE(character);
-- table list_tb_1 list_tb_1
CREATE TABLE list_tb_1(id INT,path TEXT COLLATE NOCASE,name TEXT COLLATE NOCASE,album TEXT COLLATE NOCASE,artist TEXT COLLATE NOCASE,genre TEXT COLLATE NOCASE,year INT,dis_id INT,ck_id INT,has_child_file INT,begin_time INT,end_time INT,cue_id INT,character TEXT COLLATE NOCASE,size INT,sample_rate INT,bit_rate INT,bit INT,channel INT,format INT,quality TEXT COLLATE NOCASE,album_pic_path TEXT COLLATE NOCASE,lrc_path TEXT COLLATE NOCASE,track_gain REAL,track_peak REAL,ctime INT,mtime INT,PRIMARY KEY(id, path, cue_id));
-- index sqlite_autoindex_list_tb_1_1 list_tb_1
;
