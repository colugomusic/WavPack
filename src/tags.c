////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//              Copyright (c) 1998 - 2009 Conifer Software.               //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// tags.c

// This module provides support for reading and writing metadata tags.

#include <stdlib.h>
#include <string.h>

#include "wavpack_local.h"

#ifdef WIN32
#define stricmp(x,y) _stricmp(x,y)
#define fileno _fileno
#else
#define stricmp(x,y) strcasecmp(x,y)
#endif

#ifdef DEBUG_ALLOC
#define malloc malloc_db
#define realloc realloc_db
#define free free_db
void *malloc_db (uint32_t size);
void *realloc_db (void *ptr, uint32_t size);
void free_db (void *ptr);
int32_t dump_alloc (void);
#endif

#ifndef NO_TAGS

static int get_ape_tag_item (M_Tag *m_tag, const char *item, char *value, int size);
static int get_id3_tag_item (M_Tag *m_tag, const char *item, char *value, int size);
static int get_ape_tag_item_indexed (M_Tag *m_tag, int index, char *item, int size);
static int get_id3_tag_item_indexed (M_Tag *m_tag, int index, char *item, int size);
static int write_tag_blockout (WavpackContext *wpc);
static int write_tag_reader (WavpackContext *wpc);
static void tagcpy (char *dest, char *src, int tag_size);
static int tagdata (char *src, int tag_size);

//////////////////// Global functions part of external API /////////////////////////

// Count and return the total number of tag items in the specified file.

int WavpackGetNumTagItems (WavpackContext *wpc)
{
    int i = 0;

    while (WavpackGetTagItemIndexed (wpc, i, NULL, 0))
        ++i;

    return i;
}

// Attempt to get the specified item from the specified file's ID3v1 or APEv2
// tag. The "size" parameter specifies the amount of space available at "value",
// if the desired item will not fit in this space then ellipses (...) will
// be appended and the string terminated. Only text data are supported. The
// actual length of the string is returned (or 0 if no matching value found).
// Note that with APEv2 tags the length might not be the same as the number of
// characters because UTF-8 encoding is used. Also, APEv2 tags can have multiple
// (NULL separated) strings for a single value (this is why the length is
// returned). If this function is called with a NULL "value" pointer (or a
// zero "length") then only the actual length of the value data is returned
// (not counting the terminating NULL). This can be used to determine the
// actual memory to be allocated beforehand.

int WavpackGetTagItem (WavpackContext *wpc, const char *item, char *value, int size)
{
    M_Tag *m_tag = &wpc->m_tag;

    if (value && size)
        *value = 0;

    if (m_tag->ape_tag_hdr.ID [0] == 'A')
        return get_ape_tag_item (m_tag, item, value, size);
    else if (m_tag->id3_tag.tag_id [0] == 'T')
        return get_id3_tag_item (m_tag, item, value, size);
    else
        return 0;
}

// This function looks up the tag item name by index and is used when the
// application wants to access all the items in the file's ID3v1 or APEv2 tag.
// Note that this function accesses only the item's name; WavpackGetTagItem()
// still must be called to get the actual value. The "size" parameter specifies
// the amount of space available at "item", if the desired item will not fit in
// this space then ellipses (...) will be appended and the string terminated.
// The actual length of the string is returned (or 0 if no item exists for
// index). If this function is called with a NULL "value" pointer (or a
// zero "length") then only the actual length of the item name is returned
// (not counting the terminating NULL). This can be used to determine the
// actual memory to be allocated beforehand.

int WavpackGetTagItemIndexed (WavpackContext *wpc, int index, char *item, int size)
{
    M_Tag *m_tag = &wpc->m_tag;

    if (item && size)
        *item = 0;

    if (m_tag->ape_tag_hdr.ID [0] == 'A')
        return get_ape_tag_item_indexed (m_tag, index, item, size);
    else if (m_tag->id3_tag.tag_id [0] == 'T')
        return get_id3_tag_item_indexed (m_tag, index, item, size);
    else
        return 0;
}

// Limited functionality to append APEv2 tags to WavPack files when they are
// created has been added for version 4.2. This function is used to append the
// specified field to the tag being created. If no tag has been started, then
// an empty one will be allocated first. When finished, use WavpackWriteTag()
// to write the completed tag to the file. Note that ID3 tags are not
// supported and that no editing of existing tags is allowed (there are several
// fine libraries available for this). A size parameter is included so that
// values containing multiple (NULL separated) strings can be written.

int WavpackAppendTagItem (WavpackContext *wpc, const char *item, const char *value, int vsize)
{
    M_Tag *m_tag = &wpc->m_tag;
    int isize = (int) strlen (item);

    while (WavpackDeleteTagItem (wpc, item));

    if (!m_tag->ape_tag_hdr.ID [0]) {
        strncpy (m_tag->ape_tag_hdr.ID, "APETAGEX", sizeof (m_tag->ape_tag_hdr.ID));
        m_tag->ape_tag_hdr.version = 2000;
        m_tag->ape_tag_hdr.length = sizeof (m_tag->ape_tag_hdr);
        m_tag->ape_tag_hdr.item_count = 0;
        m_tag->ape_tag_hdr.flags = 0x80000000;  // we will include header on tags we originate
    }

    if (m_tag->ape_tag_hdr.ID [0] == 'A') {
        int new_item_len = vsize + isize + 9, flags = 0;
        unsigned char *p;

        m_tag->ape_tag_hdr.item_count++;
        m_tag->ape_tag_hdr.length += new_item_len;
        p = m_tag->ape_tag_data = realloc (m_tag->ape_tag_data, m_tag->ape_tag_hdr.length);
        p += m_tag->ape_tag_hdr.length - sizeof (APE_Tag_Hdr) - new_item_len;

        *p++ = (unsigned char) vsize;
        *p++ = (unsigned char) (vsize >> 8);
        *p++ = (unsigned char) (vsize >> 16);
        *p++ = (unsigned char) (vsize >> 24);

        *p++ = (unsigned char) flags;
        *p++ = (unsigned char) (flags >> 8);
        *p++ = (unsigned char) (flags >> 16);
        *p++ = (unsigned char) (flags >> 24);

        strcpy (p, item);
        p += isize + 1;
        memcpy (p, value, vsize);

        return TRUE;
    }
    else
        return FALSE;
}

int WavpackDeleteTagItem (WavpackContext *wpc, const char *item)
{
    M_Tag *m_tag = &wpc->m_tag;

    if (m_tag->ape_tag_hdr.ID [0] == 'A') {
        unsigned char *p = m_tag->ape_tag_data;
        unsigned char *q = p + m_tag->ape_tag_hdr.length - sizeof (APE_Tag_Hdr);
        int i;

        for (i = 0; i < m_tag->ape_tag_hdr.item_count; ++i) {
            int vsize, flags, isize;

            vsize = p[0] + (p[1] << 8) + (p[2] << 16) + (p[3] << 24); p += 4;
            flags = p[0] + (p[1] << 8) + (p[2] << 16) + (p[3] << 24); p += 4;
            for (isize = 0; p[isize] && p + isize < q; ++isize);

            if (vsize < 0 || vsize > m_tag->ape_tag_hdr.length || p + isize + vsize + 1 > q)
                break;

            if (isize && vsize && !stricmp (item, p)) {
                unsigned char *d = p - 8;

                p += isize + vsize + 1;

                while (p < q)
                    *d++ = *p++;

                m_tag->ape_tag_hdr.length = (int32_t)(d - m_tag->ape_tag_data) + sizeof (APE_Tag_Hdr);
                m_tag->ape_tag_hdr.item_count--;
                return 1;
            }
            else
                p += isize + vsize + 1;
        }
    }

    return 0;
}

// Once a APEv2 tag has been created with WavpackAppendTag(), this function is
// used to write the completed tag to the end of the WavPack file. Note that
// this function uses the same "blockout" function that is used to write
// regular WavPack blocks, although that's where the similarity ends.

int WavpackWriteTag (WavpackContext *wpc)
{
    if (wpc->blockout)
        return write_tag_blockout (wpc);
    else
        return write_tag_reader (wpc);
}

//////// Utility functions provided to other modules (but not part of lib API) /////////

// This function attempts to load an ID3v1 or APEv2 tag from the specified
// file into the specified M_Tag structure. The ID3 tag fits in completely,
// but an APEv2 tag is variable length and so space must be allocated here
// to accomodate the data, and this will need to be freed later. A return
// value of TRUE indicates a valid tag was found and loaded. Note that the
// file pointer is undefined when this function exits.

int load_tag (WavpackContext *wpc)
{
    int ape_tag_length, ape_tag_items;
    M_Tag *m_tag = &wpc->m_tag;

    CLEAR (*m_tag);

    while (1) {

        // attempt to find an APEv2 tag either at end-of-file or before a ID3v1 tag we found

        if (m_tag->id3_tag.tag_id [0] == 'T')
            wpc->reader->set_pos_rel (wpc->wv_in, -(int32_t)(sizeof (APE_Tag_Hdr) + sizeof (ID3_Tag)), SEEK_END);
        else
            wpc->reader->set_pos_rel (wpc->wv_in, -(int32_t)sizeof (APE_Tag_Hdr), SEEK_END);

        if (wpc->reader->read_bytes (wpc->wv_in, &m_tag->ape_tag_hdr, sizeof (APE_Tag_Hdr)) == sizeof (APE_Tag_Hdr) &&
            !strncmp (m_tag->ape_tag_hdr.ID, "APETAGEX", 8)) {

                little_endian_to_native (&m_tag->ape_tag_hdr, APE_Tag_Hdr_Format);

                if (m_tag->ape_tag_hdr.version == 2000 && m_tag->ape_tag_hdr.item_count &&
                    m_tag->ape_tag_hdr.length > sizeof (m_tag->ape_tag_hdr) &&
                    m_tag->ape_tag_hdr.length < (1024 * 1024) &&
                    (m_tag->ape_tag_data = malloc (m_tag->ape_tag_hdr.length)) != NULL) {

                        ape_tag_items = m_tag->ape_tag_hdr.item_count;
                        ape_tag_length = m_tag->ape_tag_hdr.length;

                        if (m_tag->id3_tag.tag_id [0] == 'T')
                            m_tag->tag_file_pos = -(int32_t)sizeof (ID3_Tag);
                        else
                            m_tag->tag_file_pos = 0;

                        m_tag->tag_file_pos -= ape_tag_length;

                        // if the footer claims there is a header present also, we will read that and use it
                        // instead of the footer (after verifying it, of course) for enhanced robustness

                        if (m_tag->ape_tag_hdr.flags & 0x80000000)
                            m_tag->tag_file_pos -= sizeof (APE_Tag_Hdr);

                        wpc->reader->set_pos_rel (wpc->wv_in, m_tag->tag_file_pos, SEEK_END);
                        memset (m_tag->ape_tag_data, 0, ape_tag_length);

                        if (m_tag->ape_tag_hdr.flags & 0x80000000) {
                            if (wpc->reader->read_bytes (wpc->wv_in, &m_tag->ape_tag_hdr, sizeof (APE_Tag_Hdr)) !=
                                sizeof (APE_Tag_Hdr) || strncmp (m_tag->ape_tag_hdr.ID, "APETAGEX", 8)) {
                                    free (m_tag->ape_tag_data);
                                    CLEAR (*m_tag);
                                    return FALSE;       // something's wrong...
                            }

                            little_endian_to_native (&m_tag->ape_tag_hdr, APE_Tag_Hdr_Format);

                            if (m_tag->ape_tag_hdr.version != 2000 || m_tag->ape_tag_hdr.item_count != ape_tag_items ||
                                m_tag->ape_tag_hdr.length != ape_tag_length) {
                                    free (m_tag->ape_tag_data);
                                    CLEAR (*m_tag);
                                    return FALSE;       // something's wrong...
                            }
                        }

                        if (wpc->reader->read_bytes (wpc->wv_in, m_tag->ape_tag_data,
                            ape_tag_length - sizeof (APE_Tag_Hdr)) != ape_tag_length - sizeof (APE_Tag_Hdr)) {
                                free (m_tag->ape_tag_data);
                                CLEAR (*m_tag);
                                return FALSE;       // something's wrong...
                        }
                        else {
                            CLEAR (m_tag->id3_tag); // ignore ID3v1 tag if we found APEv2 tag
                            return TRUE;
                        }
                }
        }

        if (m_tag->id3_tag.tag_id [0] == 'T') {     // settle for the ID3v1 tag that we found
            CLEAR (m_tag->ape_tag_hdr);
            return TRUE;
        }

        // look for ID3v1 tag if APEv2 tag not found during first pass

        m_tag->tag_file_pos = -(int32_t)sizeof (ID3_Tag);
        wpc->reader->set_pos_rel (wpc->wv_in, m_tag->tag_file_pos, SEEK_END);

        if (wpc->reader->read_bytes (wpc->wv_in, &m_tag->id3_tag, sizeof (ID3_Tag)) != sizeof (ID3_Tag) ||
            strncmp (m_tag->id3_tag.tag_id, "TAG", 3)) {
                CLEAR (*m_tag);
                return FALSE;       // neither type of tag found
        }
    }
}

// Return TRUE is a valid ID3v1 or APEv2 tag has been loaded.

int valid_tag (M_Tag *m_tag)
{
    if (m_tag->ape_tag_hdr.ID [0] == 'A')
        return 'A';
    else if (m_tag->id3_tag.tag_id [0] == 'T')
        return 'T';
    else
        return 0;
}

// Free the data for any APEv2 tag that was allocated.

void free_tag (M_Tag *m_tag)
{
    if (m_tag->ape_tag_data) {
        free (m_tag->ape_tag_data);
        m_tag->ape_tag_data = NULL;
    }
}

////////////////////////// local static functions /////////////////////////////

static int get_ape_tag_item (M_Tag *m_tag, const char *item, char *value, int size)
{
    unsigned char *p = m_tag->ape_tag_data;
    unsigned char *q = p + m_tag->ape_tag_hdr.length - sizeof (APE_Tag_Hdr);
    int i;

    for (i = 0; i < m_tag->ape_tag_hdr.item_count && q - p > 8; ++i) {
        int vsize, flags, isize;

        vsize = p[0] + (p[1] << 8) + (p[2] << 16) + (p[3] << 24); p += 4;
        flags = p[0] + (p[1] << 8) + (p[2] << 16) + (p[3] << 24); p += 4;
        for (isize = 0; p[isize] && p + isize < q; ++isize);

        if (vsize < 0 || vsize > m_tag->ape_tag_hdr.length || p + isize + vsize + 1 > q)
            break;

        if (isize && vsize && !stricmp (item, p) && !(flags & 6)) {

            if (!value || !size)
                return vsize;

            if (vsize < size) {
                memcpy (value, p + isize + 1, vsize);
                value [vsize] = 0;
                return vsize;
            }
            else if (size >= 4) {
                memcpy (value, p + isize + 1, size - 1);
                value [size - 4] = value [size - 3] = value [size - 2] = '.';
                value [size - 1] = 0;
                return size - 1;
            }
            else
                return 0;
        }
        else
            p += isize + vsize + 1;
    }

    return 0;
}

static int get_id3_tag_item (M_Tag *m_tag, const char *item, char *value, int size)
{
    char lvalue [64];
    int len;

    lvalue [0] = 0;

    if (!stricmp (item, "title"))
        tagcpy (lvalue, m_tag->id3_tag.title, sizeof (m_tag->id3_tag.title));
    else if (!stricmp (item, "artist"))
        tagcpy (lvalue, m_tag->id3_tag.artist, sizeof (m_tag->id3_tag.artist));
    else if (!stricmp (item, "album"))
        tagcpy (lvalue, m_tag->id3_tag.album, sizeof (m_tag->id3_tag.album));
    else if (!stricmp (item, "year"))
        tagcpy (lvalue, m_tag->id3_tag.year, sizeof (m_tag->id3_tag.year));
    else if (!stricmp (item, "comment"))
        tagcpy (lvalue, m_tag->id3_tag.comment, sizeof (m_tag->id3_tag.comment));
    else if (!stricmp (item, "track") && m_tag->id3_tag.comment [29] && !m_tag->id3_tag.comment [28])
        sprintf (lvalue, "%d", m_tag->id3_tag.comment [29]);
    else
        return 0;

    len = (int) strlen (lvalue);

    if (!value || !size)
        return len;

    if (len < size) {
        strcpy (value, lvalue);
        return len;
    }
    else if (size >= 4) {
        strncpy (value, lvalue, size - 1);
        value [size - 4] = value [size - 3] = value [size - 2] = '.';
        value [size - 1] = 0;
        return size - 1;
    }
    else
        return 0;
}

static int get_ape_tag_item_indexed (M_Tag *m_tag, int index, char *item, int size)
{
    unsigned char *p = m_tag->ape_tag_data;
    unsigned char *q = p + m_tag->ape_tag_hdr.length - sizeof (APE_Tag_Hdr);
    int i;

    for (i = 0; i < m_tag->ape_tag_hdr.item_count && index >= 0 && q - p > 8; ++i) {
        int vsize, flags, isize;

        vsize = p[0] + (p[1] << 8) + (p[2] << 16) + (p[3] << 24); p += 4;
        flags = p[0] + (p[1] << 8) + (p[2] << 16) + (p[3] << 24); p += 4;
        for (isize = 0; p[isize] && p + isize < q; ++isize);

        if (vsize < 0 || vsize > m_tag->ape_tag_hdr.length || p + isize + vsize + 1 > q)
            break;

        if (isize && vsize && !(flags & 6) && !index--) {

            if (!item || !size)
                return isize;

            if (isize < size) {
                memcpy (item, p, isize);
                item [isize] = 0;
                return isize;
            }
            else if (size >= 4) {
                memcpy (item, p, size - 1);
                item [size - 4] = item [size - 3] = item [size - 2] = '.';
                item [size - 1] = 0;
                return size - 1;
            }
            else
                return 0;
        }
        else
            p += isize + vsize + 1;
    }

    return 0;
}

static int get_id3_tag_item_indexed (M_Tag *m_tag, int index, char *item, int size)
{
    char lvalue [16];
    int len;

    lvalue [0] = 0;

    if (tagdata (m_tag->id3_tag.title, sizeof (m_tag->id3_tag.title)) && !index--)
        strcpy (lvalue, "Title");
    else if (tagdata (m_tag->id3_tag.artist, sizeof (m_tag->id3_tag.artist)) && !index--)
        strcpy (lvalue, "Artist");
    else if (tagdata (m_tag->id3_tag.album, sizeof (m_tag->id3_tag.album)) && !index--)
        strcpy (lvalue, "Album");
    else if (tagdata (m_tag->id3_tag.year, sizeof (m_tag->id3_tag.year)) && !index--)
        strcpy (lvalue, "Year");
    else if (tagdata (m_tag->id3_tag.comment, sizeof (m_tag->id3_tag.comment)) && !index--)
        strcpy (lvalue, "Comment");
    else if (m_tag->id3_tag.comment [29] && !m_tag->id3_tag.comment [28] && !index--)
        strcpy (lvalue, "Track");
    else
        return 0;

    len = (int) strlen (lvalue);

    if (!item || !size)
        return len;

    if (len < size) {
        strcpy (item, lvalue);
        return len;
    }
    else if (size >= 4) {
        strncpy (item, lvalue, size - 1);
        item [size - 4] = item [size - 3] = item [size - 2] = '.';
        item [size - 1] = 0;
        return size - 1;
    }
    else
        return 0;
}

static int write_tag_blockout (WavpackContext *wpc)
{
    M_Tag *m_tag = &wpc->m_tag;
    int result = TRUE;

    if (m_tag->ape_tag_hdr.ID [0] == 'A' && m_tag->ape_tag_hdr.item_count) {

        // only write header if it's specified in the flags

        if (m_tag->ape_tag_hdr.flags & 0x80000000) {
            m_tag->ape_tag_hdr.flags |= 0x20000000;
            native_to_little_endian (&m_tag->ape_tag_hdr, APE_Tag_Hdr_Format);
            result = wpc->blockout (wpc->wv_out, &m_tag->ape_tag_hdr, sizeof (m_tag->ape_tag_hdr));
            little_endian_to_native (&m_tag->ape_tag_hdr, APE_Tag_Hdr_Format);
        }

        if (m_tag->ape_tag_hdr.length > sizeof (m_tag->ape_tag_hdr))
            result = wpc->blockout (wpc->wv_out, m_tag->ape_tag_data, m_tag->ape_tag_hdr.length - sizeof (m_tag->ape_tag_hdr));

        m_tag->ape_tag_hdr.flags &= ~0x20000000;
        native_to_little_endian (&m_tag->ape_tag_hdr, APE_Tag_Hdr_Format);
        result = wpc->blockout (wpc->wv_out, &m_tag->ape_tag_hdr, sizeof (m_tag->ape_tag_hdr));
        little_endian_to_native (&m_tag->ape_tag_hdr, APE_Tag_Hdr_Format);
    }

    if (!result)
        strcpy (wpc->error_message, "can't write WavPack data, disk probably full!");

    return result;
}

static int write_tag_reader (WavpackContext *wpc)
{
    M_Tag *m_tag = &wpc->m_tag;
    int32_t tag_size = 0;
    int result = TRUE;

    if (m_tag->ape_tag_hdr.ID [0] == 'A' && m_tag->ape_tag_hdr.item_count &&
        m_tag->ape_tag_hdr.length > sizeof (m_tag->ape_tag_hdr))
            tag_size = m_tag->ape_tag_hdr.length;

    // only write header if it's specified in the flags

    if (m_tag->ape_tag_hdr.flags & 0x80000000)
        tag_size += sizeof (m_tag->ape_tag_hdr);

    result = (wpc->open_flags & OPEN_EDIT_TAGS) && wpc->reader->can_seek (wpc->wv_in) &&
        !wpc->reader->set_pos_rel (wpc->wv_in, m_tag->tag_file_pos, SEEK_END);

    if (result && tag_size < -m_tag->tag_file_pos) {
        int nullcnt = -m_tag->tag_file_pos - tag_size;
        char zero [1] = { 0 };

        while (nullcnt--)
            wpc->reader->write_bytes (wpc->wv_in, &zero, 1);
    }

    if (result && tag_size) {
        if (m_tag->ape_tag_hdr.flags & 0x80000000) {
            m_tag->ape_tag_hdr.flags |= 0x20000000;
            native_to_little_endian (&m_tag->ape_tag_hdr, APE_Tag_Hdr_Format);
            result = (wpc->reader->write_bytes (wpc->wv_in, &m_tag->ape_tag_hdr, sizeof (m_tag->ape_tag_hdr)) == sizeof (m_tag->ape_tag_hdr));
            little_endian_to_native (&m_tag->ape_tag_hdr, APE_Tag_Hdr_Format);
        }

        result = (wpc->reader->write_bytes (wpc->wv_in, m_tag->ape_tag_data, m_tag->ape_tag_hdr.length - sizeof (m_tag->ape_tag_hdr)) == sizeof (m_tag->ape_tag_hdr));
        m_tag->ape_tag_hdr.flags &= ~0x20000000;
        native_to_little_endian (&m_tag->ape_tag_hdr, APE_Tag_Hdr_Format);
        result = (wpc->reader->write_bytes (wpc->wv_in, &m_tag->ape_tag_hdr, sizeof (m_tag->ape_tag_hdr)) == sizeof (m_tag->ape_tag_hdr));
        little_endian_to_native (&m_tag->ape_tag_hdr, APE_Tag_Hdr_Format);
    }

    if (!result)
        strcpy (wpc->error_message, "can't write WavPack data, disk probably full!");

    return result;
}

// Copy the specified ID3v1 tag value (with specified field size) from the
// source pointer to the destination, eliminating leading spaces and trailing
// spaces and nulls.

static void tagcpy (char *dest, char *src, int tag_size)
{
    char *s1 = src, *s2 = src + tag_size - 1;

    if (*s2 && !s2 [-1])
        s2--;

    while (s1 <= s2)
        if (*s1 == ' ')
            ++s1;
        else if (!*s2 || *s2 == ' ')
            --s2;
        else
            break;

    while (*s1 && s1 <= s2)
        *dest++ = *s1++;

    *dest = 0;
}

static int tagdata (char *src, int tag_size)
{
    char *s1 = src, *s2 = src + tag_size - 1;

    if (*s2 && !s2 [-1])
        s2--;

    while (s1 <= s2)
        if (*s1 == ' ')
            ++s1;
        else if (!*s2 || *s2 == ' ')
            --s2;
        else
            break;

    return (*s1 && s1 <= s2);
}

#endif
