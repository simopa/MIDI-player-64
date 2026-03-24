/* midiplayer64 -- MIDI file player for Commodore 64 via SID chip.
 *
 * Reads a standard MIDI file (type 0 or 1) from a 1541 disk drive,
 * converts tick-based timing to PAL video frames, and plays note
 * events through the three SID voices with voice stealing.
 *
 * The C64 has no seek on sequential I/O, so we buffer reads in 256-byte
 * chunks and parse the entire file in a single forward pass. All note
 * events are stored in RAM, sorted, then converted from MIDI ticks to
 * frame numbers before playback begins. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <conio.h>
#include <c64/kernalio.h>
#include <c64/sid.h>
#include <c64/vic.h>

#ifndef DISK_SCAN_MODE
#define DISK_SCAN_MODE 0
#endif

/* =========================== Configuration ================================ */

/* KERNAL logical file numbers -- avoid 0 and 1 which are keyboard/screen. */

#define MIDI_FNUM 2
#define DIR_FNUM 3

/* All event storage is pre-allocated statically because the C64 has no
 * usable malloc. 2800 events * 7 bytes each is ~19 KB, leaving enough
 * room for code, stack, and the SID frequency table. */

#define MAX_EVENTS 3150
#define MAX_TEMPO_EVENTS 128

/* Type 1 MIDI files split instruments across tracks. Only tracks with
 * a reasonable number of note-on events are kept -- too few means it's
 * probably metadata, too many would flood the 3-voice SID. */

#define TYPE1_NOTE_TRACK_LIMIT 4
#define TYPE1_MIN_NOTE_ON 24
#define TYPE1_MAX_NOTE_ON 650

#define MAX_SCAN_CANDIDATES 8
#define MAX_SCAN_SPEC_LEN 20

/* The SID can't reproduce very low or very high notes usefully, so
 * the playable range is clamped to 6 octaves that sound decent. */

#define MIDI_NOTE_MIN 24
#define MIDI_NOTE_MAX 96

/* PAL runs at 50 fps = 20000 us per frame. This is the playback time
 * quantum since vic_waitFrame() gives exactly this resolution. */

#define FRAME_US 20000UL
#define DEFAULT_TEMPO_US 500000UL

/* Channel is packed into the command byte so each note event fits in
 * just two bytes (cmd + note) instead of three. */

#define CMD_NOTE_OFF(ch) ((uint8_t)(0x80 | ((ch) & 0x0f)))
#define CMD_NOTE_ON(ch) ((uint8_t)(0x90 | ((ch) & 0x0f)))

/* ============================= Data types ================================= */

typedef enum load_result {
    LOAD_OK = 0,
    LOAD_OPEN_FAILED,
    LOAD_BAD_HEADER,
    LOAD_UNSUPPORTED_FORMAT,
    LOAD_UNSUPPORTED_DIVISION,
    LOAD_TRUNCATED,
    LOAD_BAD_MIDI_EVENT,
    LOAD_TOO_MANY_EVENTS,
    LOAD_TOO_MANY_TEMPO_EVENTS,
    LOAD_NO_TRACK,
    LOAD_NO_NOTE_EVENTS
} load_result;

typedef struct voice_state {
    bool        active;
    uint8_t     note;
    uint8_t     channel;
    uint16_t    age;      /* monotonic counter for voice stealing */
} voice_state;

/* ============================= Global state =============================== */

/* Events are stored in parallel arrays rather than an array of structs
 * because oscar64 generates tighter code for indexed access this way,
 * and on the 6502 every cycle counts. */

static uint32_t event_time[MAX_EVENTS];
static uint8_t  event_cmd[MAX_EVENTS];
static uint8_t  event_note[MAX_EVENTS];
__zeropage static uint16_t event_count;

static uint32_t tempo_tick[MAX_TEMPO_EVENTS];
static uint32_t tempo_us_value[MAX_TEMPO_EVENTS];
__zeropage static uint8_t tempo_count;

static uint32_t song_length_frames;
static uint32_t frame_denom;  /* division * FRAME_US, denominator for tick->frame math */
static uint16_t division;
static bool     opened_as_prg;  /* PRG files have a 2-byte load address we must skip */
static bool     event_overflow;
static char     open_spec[32];

/* Buffered I/O for MIDI file -- the KERNAL read is slow per call,
 * so we read 256 bytes at a time and serve from the buffer. */

static uint8_t  io_buf[256];
__zeropage static uint16_t io_pos;
__zeropage static uint16_t io_len;

/* Separate smaller buffer for directory listing, which we may read
 * while a MIDI file channel is not yet open. */

static uint8_t  dir_io_buf[64];
static uint8_t  dir_io_pos;
static uint8_t  dir_io_len;

static voice_state   voices[3];
static uint8_t      voice_wave[3];  /* waveform control byte per voice */
static uint16_t     voice_age;
static char         scan_candidates[MAX_SCAN_CANDIDATES][MAX_SCAN_SPEC_LEN];
__zeropage static uint8_t scan_candidate_count;

/* Filenames to try when looking for MIDI data on disk. Both bare names
 * and .PRG suffixed variants are tried because some copy utilities
 * append extensions. */

static const char * midi_candidates[] = {
    "0:MIDI",
    "0:MIDI.PRG",
    "0:MJ",
    "0:MJ.PRG",
    "0:SONG",
    "0:SONG.PRG"
};

/* Pre-computed SID frequency register values for each MIDI note.
 * Notes above 95 are clamped to 65535 (max SID frequency) since the
 * chip can't go higher and we'd rather play something than nothing. */

static const uint16_t note_to_sid[128] = {
    139, 147, 156, 166, 175, 186, 197, 209,
    221, 234, 248, 263, 278, 295, 313, 331,
    351, 372, 394, 417, 442, 468, 496, 526,
    557, 590, 625, 662, 702, 743, 788, 834,
    884, 937, 992, 1051, 1114, 1180, 1250, 1325,
    1403, 1487, 1575, 1669, 1768, 1873, 1985, 2103,
    2228, 2360, 2500, 2649, 2807, 2973, 3150, 3338,
    3536, 3746, 3969, 4205, 4455, 4720, 5001, 5298,
    5613, 5947, 6300, 6675, 7072, 7493, 7938, 8410,
    8910, 9440, 10001, 10596, 11226, 11894, 12601, 13350,
    14144, 14985, 15876, 16820, 17820, 18880, 20003, 21192,
    22452, 23787, 25202, 26700, 28288, 29970, 31752, 33640,
    35641, 37760, 40005, 42384, 44904, 47574, 50403, 53401,
    56576, 59940, 63504, 65535, 65535, 65535, 65535, 65535,
    65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535,
    65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535
};

/* ============================ Error messages ============================== */

static const char * load_result_text(load_result r) {
    switch (r) {
        case LOAD_OK:                   return "OK";
        case LOAD_OPEN_FAILED:          return "CANNOT OPEN MIDI FILE";
        case LOAD_BAD_HEADER:           return "INVALID MIDI HEADER/CHUNK";
        case LOAD_UNSUPPORTED_FORMAT:   return "ONLY MIDI TYPE 0/1 IS SUPPORTED";
        case LOAD_UNSUPPORTED_DIVISION: return "SMPTE TIMING IS NOT SUPPORTED";
        case LOAD_TRUNCATED:            return "TRUNCATED FILE";
        case LOAD_BAD_MIDI_EVENT:       return "INVALID MIDI EVENT STREAM";
        case LOAD_TOO_MANY_EVENTS:      return "TOO MANY NOTE EVENTS FOR RAM";
        case LOAD_TOO_MANY_TEMPO_EVENTS:return "TOO MANY TEMPO CHANGES FOR RAM";
        case LOAD_NO_TRACK:             return "NO TRACK FOUND";
        case LOAD_NO_NOTE_EVENTS:       return "NO NOTE ON/OFF EVENTS FOUND";
        default:                        return "UNKNOWN ERROR";
    }
}

/* ========================= Event storage ================================== */

static void reset_conversion_state(void) {
    event_count = 0;
    tempo_count = 0;
    song_length_frames = 0;
    frame_denom = 0;
    division = 0;
    opened_as_prg = false;
    event_overflow = false;
    open_spec[0] = 0;
    io_pos = 0;
    io_len = 0;
}

static bool push_note_event(uint32_t tick, uint8_t cmd, uint8_t note) {
    if (event_count >= MAX_EVENTS) {
        event_overflow = true;
        return false;
    }

    event_time[event_count] = tick;
    event_cmd[event_count] = cmd;
    event_note[event_count] = note;
    event_count++;
    return true;
}

/* Channel 9 is General MIDI percussion -- the SID has no sample playback
 * so drums would just be random pitched beeps. Notes outside our playable
 * range are also dropped to avoid SID frequency register wraparound. */

static bool event_note_is_usable(uint8_t channel, uint8_t note) {
    if (channel == 9)
        return false;
    if (note < MIDI_NOTE_MIN || note > MIDI_NOTE_MAX)
        return false;
    return true;
}

static bool push_tempo_event(uint32_t tick, uint32_t tempo_us) {
    if (tempo_count >= MAX_TEMPO_EVENTS)
        return false;

    /* Some broken MIDI files have zero-tempo markers. */
    if (tempo_us == 0)
        tempo_us = DEFAULT_TEMPO_US;

    tempo_tick[tempo_count] = tick;
    tempo_us_value[tempo_count] = tempo_us;
    tempo_count++;
    return true;
}

/* ========================= KERNAL file I/O ================================ */

/* Try opening a file with a specific CBM file type (P=program, S=sequential).
 * The KERNAL needs the type appended to the filename as ",P,R" or ",S,R".
 * PRG is tried first because that's how most MIDI files end up on C64
 * disks (copied via file managers that default to PRG). */

static bool midi_open_with_type(const char * filename, char type_char) {
    char spec[32];
    uint8_t i = 0;

    while (filename[i] && i < sizeof(spec) - 5) {
        spec[i] = filename[i];
        i++;
    }
    if (filename[i])
        return false;

    spec[i++] = ',';
    spec[i++] = type_char;
    spec[i++] = ',';
    spec[i++] = 'R';
    spec[i] = 0;

    krnio_setnam(spec);
    if (!krnio_open(MIDI_FNUM, 8, MIDI_FNUM))
        return false;

    memcpy(open_spec, spec, sizeof(spec));
    opened_as_prg = (type_char == 'P');
    return true;
}

static bool midi_open_file(const char * filename) {
    io_pos = 0;
    io_len = 0;

    if (midi_open_with_type(filename, 'P'))
        return true;
    if (midi_open_with_type(filename, 'S'))
        return true;
    return false;
}

static void midi_close_file(void) {
    krnio_close(MIDI_FNUM);
}

/* ========================= Character conversion =========================== */

/* The C64 uses PETSCII, not ASCII. Uppercase letters live at $C1-$DA in
 * PETSCII, so both ranges must be handled when comparing filenames
 * case-insensitively. $A0 is the PETSCII non-breaking space. */

static uint8_t petscii_to_ascii_upper(uint8_t c) {
    if (c >= 'a' && c <= 'z')
        return (uint8_t)(c - ('a' - 'A'));
    if (c >= 0xc1 && c <= 0xda)
        return (uint8_t)(c - 0x80);
    if (c == 0xa0)
        return ' ';
    return c;
}

static bool name_ends_with_ci(const char * name, const char * suffix) {
    uint8_t nlen = (uint8_t)strlen(name);
    uint8_t slen = (uint8_t)strlen(suffix);

    if (nlen < slen)
        return false;

    for (uint8_t i = 0; i < slen; ++i) {
        uint8_t a = petscii_to_ascii_upper((uint8_t)name[nlen - slen + i]);
        uint8_t b = petscii_to_ascii_upper((uint8_t)suffix[i]);
        if (a != b)
            return false;
    }
    return true;
}

/* Heuristic: if the filename contains "MID" anywhere or ends with a
 * known MIDI extension, it's likely a MIDI file. Used to prioritize
 * candidates when scanning the disk directory. */

static bool name_has_midi_hint(const char * spec) {
    const char * name = spec;

    if (name[0] == '0' && name[1] == ':')
        name += 2;

    if (name_ends_with_ci(name, ".MID") || name_ends_with_ci(name, ".MIDI") || name_ends_with_ci(name, ".SEQ"))
        return true;

    for (uint8_t i = 0; name[i + 2]; ++i) {
        if (petscii_to_ascii_upper((uint8_t)name[i]) == 'M' &&
            petscii_to_ascii_upper((uint8_t)name[i + 1]) == 'I' &&
            petscii_to_ascii_upper((uint8_t)name[i + 2]) == 'D')
            return true;
    }
    return false;
}

/* ========================= Disk directory scan ============================ */

/* The 1541 directory is read as a BASIC program listing over the serial
 * bus. We open it with secondary address 0 to get the raw listing stream
 * rather than loading it into BASIC memory. */

static bool dir_open_listing(void) {
    dir_io_pos = 0;
    dir_io_len = 0;

    /* Try with drive number prefix first, fall back without. */
    krnio_setnam("0:$");
    if (krnio_open(DIR_FNUM, 8, 0))
        return true;

    krnio_setnam("$");
    if (krnio_open(DIR_FNUM, 8, 0))
        return true;

    return false;
}

static void dir_close_listing(void) {
    krnio_close(DIR_FNUM);
}

static bool dir_read_u8(uint8_t * out) {
    int n;

    if (dir_io_pos >= dir_io_len) {
        n = krnio_read(DIR_FNUM, (char *)dir_io_buf, sizeof(dir_io_buf));
        if (n <= 0)
            return false;
        dir_io_len = (uint8_t)n;
        dir_io_pos = 0;
    }

    *out = dir_io_buf[dir_io_pos++];
    return true;
}

/* Deduplicate candidates -- the same file might appear if we scan twice. */

static void scan_add_candidate(const char * name) {
    char * dst;

    if (!name[0] || scan_candidate_count >= MAX_SCAN_CANDIDATES)
        return;

    for (uint8_t i = 0; i < scan_candidate_count; ++i) {
        if (!strcmp(scan_candidates[i] + 2, name))
            return;
    }

    /* Prefix with "0:" for KERNAL file open. */
    dst = scan_candidates[scan_candidate_count];
    dst[0] = '0';
    dst[1] = ':';
    uint8_t j;
    for (j = 0; name[j] && j < MAX_SCAN_SPEC_LEN - 3; ++j)
        dst[j + 2] = name[j];
    dst[j + 2] = 0;

    scan_candidate_count++;
}

/* Parse one line of the BASIC-encoded directory listing. Each line has
 * the format: <block count> "<FILENAME>" <TYPE>
 * The quotes delimit the filename, and the type is a 3-letter code
 * like PRG, SEQ, DEL, REL. Only PRG and SEQ matter here since those
 * are the only types that can contain MIDI data. */

static void scan_parse_listing_line(const uint8_t * line, uint8_t len) {
    int8_t q1 = -1;
    int8_t q2 = -1;
    uint8_t i, j, k;
    char name[17];
    char type[4];

    for (i = 0; i < len; ++i) {
        if (line[i] == '\"') {
            if (q1 < 0)
                q1 = (int8_t)i;
            else {
                q2 = (int8_t)i;
                break;
            }
        }
    }

    if (q1 < 0 || q2 <= q1 + 1)
        return;

    j = 0;
    for (i = (uint8_t)(q1 + 1); i < (uint8_t)q2 && j < sizeof(name) - 1; ++i) {
        uint8_t c = petscii_to_ascii_upper(line[i]);
        name[j++] = (char)c;
    }
    while (j > 0 && name[j - 1] == ' ')
        j--;
    name[j] = 0;

    if (!name[0])
        return;

    /* Skip decorators that some DOS versions add after the closing quote. */
    i = (uint8_t)(q2 + 1);
    while (i < len && (line[i] == ' ' || line[i] == 0xa0 || line[i] == '*' || line[i] == '<' || line[i] == '>'))
        i++;

    k = 0;
    while (i < len && k < 3) {
        uint8_t c = petscii_to_ascii_upper(line[i]);
        if (c >= 'A' && c <= 'Z')
            type[k++] = (char)c;
        else if (k > 0)
            break;
        i++;
    }
    if (k != 3)
        return;
    type[3] = 0;

    if (strcmp(type, "PRG") && strcmp(type, "SEQ"))
        return;

    scan_add_candidate(name);
}

static uint8_t scan_disk_for_candidates(void) {
    uint8_t link_lo, link_hi;
    uint8_t line_lo, line_hi;
    uint8_t b;

    scan_candidate_count = 0;

    if (!dir_open_listing())
        return 0;

    if (!krnio_chkin(DIR_FNUM)) {
        dir_close_listing();
        return 0;
    }

    /* The directory stream starts with a 2-byte BASIC load address
     * that we don't need -- just consume it. */
    krnio_chrin();
    krnio_chrin();

    while (scan_candidate_count < MAX_SCAN_CANDIDATES) {
        uint8_t line[40];
        uint8_t len = 0;
        unsigned line_blocks;

        if (krnio_status())
            break;
        link_lo = (uint8_t)krnio_chrin();
        if (krnio_status())
            break;
        link_hi = (uint8_t)krnio_chrin();
        if (krnio_status())
            break;

        if (link_lo == 0 && link_hi == 0)
            break;

        line_lo = (uint8_t)krnio_chrin();
        if (krnio_status())
            break;
        line_hi = (uint8_t)krnio_chrin();
        if (krnio_status())
            break;
        line_blocks = (unsigned)line_lo + ((unsigned)line_hi << 8);

        while (1) {
            b = (uint8_t)krnio_chrin();
            if (krnio_status())
                break;
            if (b == 0)
                break;
            if (len < sizeof(line) - 1)
                line[len++] = b;
        }
        line[len] = 0;

        /* Lines with block count 0 are the disk header or trailer. */
        if (line_blocks > 0)
            scan_parse_listing_line(line, len);
    }

    krnio_clrchn();
    dir_close_listing();
    return scan_candidate_count;
}

/* ========================= Buffered MIDI read ============================= */

/* The fp parameter is for respect :) -- actual I/O goes through KERNAL, not
 * stdio, but keeping it gives the parser functions a familiar shape. */

static bool file_read_u8(FILE * fp, uint8_t * out) {
    int n;

    (void)fp;

    if (io_pos >= io_len) {
        n = krnio_read(MIDI_FNUM, (char *)io_buf, sizeof(io_buf));
        if (n <= 0)
            return false;
        io_len = (uint16_t)n;
        io_pos = 0;
    }

    *out = io_buf[io_pos++];
    return true;
}

static bool file_read_be16(FILE * fp, uint16_t * out) {
    uint8_t hi, lo;
    if (!file_read_u8(fp, &hi))
        return false;
    if (!file_read_u8(fp, &lo))
        return false;
    *out = ((uint16_t)hi << 8) | lo;
    return true;
}

static bool file_read_be32(FILE * fp, uint32_t * out) {
    uint8_t b0, b1, b2, b3;
    if (!file_read_u8(fp, &b0))
        return false;
    if (!file_read_u8(fp, &b1))
        return false;
    if (!file_read_u8(fp, &b2))
        return false;
    if (!file_read_u8(fp, &b3))
        return false;
    *out = ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) | ((uint32_t)b2 << 8) | b3;
    return true;
}

static bool file_read_chunk_id(FILE * fp, char id[4]) {
    uint8_t c0, c1, c2, c3;
    if (!file_read_u8(fp, &c0))
        return false;
    if (!file_read_u8(fp, &c1))
        return false;
    if (!file_read_u8(fp, &c2))
        return false;
    if (!file_read_u8(fp, &c3))
        return false;
    id[0] = (char)c0;
    id[1] = (char)c1;
    id[2] = (char)c2;
    id[3] = (char)c3;
    return true;
}

/* No seek on C64 serial I/O, so skipping means reading and discarding. */

static bool file_skip_bytes(FILE * fp, uint32_t count) {
    while (count > 0) {
        uint8_t dummy;
        if (!file_read_u8(fp, &dummy))
            return false;
        count--;
    }
    return true;
}

/* ========================= MIDI track parsing ============================= */

/* Track-level reads decrement a remaining byte counter so we know when
 * we've consumed the entire chunk without relying on EOF detection,
 * which is unreliable over the C64 serial bus. */

static bool track_read_u8(FILE * fp, uint32_t * remaining, uint8_t * out) {
    if (*remaining == 0)
        return false;

    if (!file_read_u8(fp, out))
        return false;

    (*remaining)--;
    return true;
}

static bool track_skip_bytes(FILE * fp, uint32_t * remaining, uint32_t count) {
    while (count > 0) {
        uint8_t dummy;
        if (!track_read_u8(fp, remaining, &dummy))
            return false;
        count--;
    }
    return true;
}

/* MIDI variable-length quantity: 7 bits per byte, high bit means "more".
 * Max 4 bytes = 28 bits, which is enough for any real-world delta time. */

static bool track_read_varlen(FILE * fp, uint32_t * remaining, uint32_t * out) {
    uint32_t v = 0;
    uint8_t b;
    uint8_t i = 0;

    do {
        if (i >= 4)
            return false;

        if (!track_read_u8(fp, remaining, &b))
            return false;

        v = (v << 7) | (b & 0x7f);
        i++;
    } while (b & 0x80);

    *out = v;
    return true;
}

/* Walk through a track chunk, extracting note on/off and tempo events.
 * Everything else (control change, pitch bend, etc...) is skipped
 * because the SID has no way to use that information. */

static load_result parse_track_events(FILE * fp, uint32_t track_len, uint16_t * note_on_count) {
    uint32_t remaining = track_len;
    uint8_t running_status = 0;
    uint32_t track_tick = 0;

    *note_on_count = 0;

    while (remaining > 0) {
        uint32_t delta;
        uint8_t first;
        uint8_t status;
        uint8_t data1 = 0;
        uint8_t data2 = 0;
        bool have_data1 = false;

        if (!track_read_varlen(fp, &remaining, &delta))
            return LOAD_TRUNCATED;
        track_tick += delta;

        if (!track_read_u8(fp, &remaining, &first))
            return LOAD_TRUNCATED;

        /* MIDI running status: if the high bit is clear, reuse the
         * previous status byte. This is how MIDI saves bandwidth. */
        if (first & 0x80) {
            status = first;
        } else {
            if (!running_status)
                return LOAD_BAD_MIDI_EVENT;
            status = running_status;
            data1 = first;
            have_data1 = true;
        }

        /* Meta events (0xFF): only end-of-track (0x2F) and tempo
         * change (0x51) matter. Everything else is skipped. */
        if (status == 0xff) {
            uint8_t meta_type;
            uint32_t meta_len;

            running_status = 0;

            if (!track_read_u8(fp, &remaining, &meta_type))
                return LOAD_TRUNCATED;
            if (!track_read_varlen(fp, &remaining, &meta_len))
                return LOAD_TRUNCATED;

            if (meta_type == 0x2f) { /* end of track */
                if (!track_skip_bytes(fp, &remaining, meta_len))
                    return LOAD_TRUNCATED;
                break;
            } else if (meta_type == 0x51 && meta_len == 3) { /* tempo */
                uint8_t t0, t1, t2;
                uint32_t tempo_us;

                if (!track_read_u8(fp, &remaining, &t0))
                    return LOAD_TRUNCATED;
                if (!track_read_u8(fp, &remaining, &t1))
                    return LOAD_TRUNCATED;
                if (!track_read_u8(fp, &remaining, &t2))
                    return LOAD_TRUNCATED;

                tempo_us = ((uint32_t)t0 << 16) | ((uint32_t)t1 << 8) | t2;
                if (!push_tempo_event(track_tick, tempo_us))
                    return LOAD_TOO_MANY_TEMPO_EVENTS;
            } else {
                if (!track_skip_bytes(fp, &remaining, meta_len))
                    return LOAD_TRUNCATED;
            }

            continue;
        }

        /* SysEx: skip entirely, the SID can't do anything with it. */
        if (status == 0xf0 || status == 0xf7) {
            uint32_t syx_len;
            running_status = 0;
            if (!track_read_varlen(fp, &remaining, &syx_len))
                return LOAD_TRUNCATED;
            if (!track_skip_bytes(fp, &remaining, syx_len))
                return LOAD_TRUNCATED;
            continue;
        }

        if (status >= 0xf0)
            return LOAD_BAD_MIDI_EVENT;

        /* Channel voice messages. */
        {
            uint8_t msg = status & 0xf0;
            uint8_t ch = status & 0x0f;
            /* Program change and channel pressure are 1-byte,
             * everything else is 2-byte. */
            bool need_two = !(msg == 0xc0 || msg == 0xd0);

            if (!have_data1) {
                if (!track_read_u8(fp, &remaining, &data1))
                    return LOAD_TRUNCATED;
            }

            if (need_two) {
                if (!track_read_u8(fp, &remaining, &data2))
                    return LOAD_TRUNCATED;
            }

            running_status = status;

            /* Note-on with velocity 0 is actually note-off per MIDI spec.
             * Many sequencers use this instead of real note-off to save
             * bytes via running status. */
            if (msg == 0x90) {
                if (data2 == 0) {
                    if (event_note_is_usable(ch, data1))
                        push_note_event(track_tick, CMD_NOTE_OFF(ch), data1);
                } else {
                    if (event_note_is_usable(ch, data1)) {
                        (*note_on_count)++;
                        push_note_event(track_tick, CMD_NOTE_ON(ch), data1);
                    }
                }
            } else if (msg == 0x80) {
                if (event_note_is_usable(ch, data1))
                    push_note_event(track_tick, CMD_NOTE_OFF(ch), data1);
            }
        }
    }

    if (remaining > 0) {
        if (!track_skip_bytes(fp, &remaining, remaining))
            return LOAD_TRUNCATED;
    }

    return LOAD_OK;
}

/* ========================= Event sorting ================================== */

/* When two events share the same tick, note-off must come before note-on.
 * Otherwise we'd release a voice and immediately re-trigger it on the
 * same frame, causing audible clicks. */

static bool note_event_before(uint32_t ta, uint8_t ca, uint32_t tb, uint8_t cb) {
    uint8_t ma, mb;

    if (ta != tb)
        return ta < tb;

    ma = ca & 0xf0;
    mb = cb & 0xf0;
    if (ma != mb) {
        if (ma == 0x80)
            return true;
        if (mb == 0x80)
            return false;
    }
    return ca < cb;
}

/* Shell sort: quicksort's stack depth is too risky on the 6502, and
 * bubble sort would be too slow for 2-3000 events. Shell sort is a good
 * middle ground -- O(n^1.5) with constant extra memory. */

static void sort_note_events(void) {
    uint16_t gap, i, j;

    for (gap = event_count >> 1; gap > 0; gap >>= 1) {
        for (i = gap; i < event_count; ++i) {
            uint32_t tt = event_time[i];
            uint8_t tc = event_cmd[i];
            uint8_t tn = event_note[i];

            j = i;
            while (j >= gap && note_event_before(tt, tc, event_time[j - gap], event_cmd[j - gap])) {
                event_time[j] = event_time[j - gap];
                event_cmd[j] = event_cmd[j - gap];
                event_note[j] = event_note[j - gap];
                j -= gap;
            }

            event_time[j] = tt;
            event_cmd[j] = tc;
            event_note[j] = tn;
        }
    }
}

static void sort_tempo_events(void) {
    uint8_t gap, i, j;

    for (gap = tempo_count >> 1; gap > 0; gap >>= 1) {
        for (i = gap; i < tempo_count; ++i) {
            uint32_t tt = tempo_tick[i];
            uint32_t tv = tempo_us_value[i];

            j = i;
            while (j >= gap && tt < tempo_tick[j - gap]) {
                tempo_tick[j] = tempo_tick[j - gap];
                tempo_us_value[j] = tempo_us_value[j - gap];
                j -= gap;
            }

            tempo_tick[j] = tt;
            tempo_us_value[j] = tv;
        }
    }
}

/* ========================= Tick-to-frame conversion ======================= */

/* Convert MIDI ticks to PAL frame numbers. A fractional frame counter
 * (frameNumer / frameDenom) avoids losing precision from integer
 * division. Each multiplication step is capped at 255 ticks to prevent
 * 32-bit overflow on the 6502. */

static void advance_frames_by_ticks(uint32_t delta_ticks, uint32_t tempo_us, uint32_t * cur_frame, uint32_t * frame_numer) {
    while (delta_ticks > 0) {
        uint16_t step;

        if (delta_ticks > 255UL)
            step = 255;
        else
            step = (uint16_t)delta_ticks;

        delta_ticks -= step;
        *frame_numer += (uint32_t)step * tempo_us;

        while (*frame_numer >= frame_denom) {
            *frame_numer -= frame_denom;
            (*cur_frame)++;
        }
    }
}

/* Replace tick timestamps with frame numbers in-place, walking through
 * tempo changes as we go. After this, events are ready for the frame-
 * based playback loop. */

static void convert_ticks_to_frames(void) {
    uint32_t cur_tick = 0;
    uint32_t cur_frame = 0;
    uint32_t frame_numer = 0;
    uint32_t tempo_us = DEFAULT_TEMPO_US;
    uint8_t ti = 0;

    /* Consume any tempo events at tick 0 to set the initial BPM. */
    while (ti < tempo_count && tempo_tick[ti] == 0) {
        tempo_us = tempo_us_value[ti];
        ti++;
    }

    for (uint16_t i = 0; i < event_count; ++i) {
        uint32_t target_tick = event_time[i];

        while (ti < tempo_count && tempo_tick[ti] <= target_tick) {
            uint32_t tempo_tick_now = tempo_tick[ti];
            advance_frames_by_ticks(tempo_tick_now - cur_tick, tempo_us, &cur_frame, &frame_numer);
            cur_tick = tempo_tick_now;
            tempo_us = tempo_us_value[ti];
            ti++;
        }

        advance_frames_by_ticks(target_tick - cur_tick, tempo_us, &cur_frame, &frame_numer);
        cur_tick = target_tick;
        event_time[i] = cur_frame;
    }

    song_length_frames = cur_frame;

    /* Many MIDI files have silence at the beginning (count-in, empty
     * first measure). Strip it so playback starts immediately. */
    if (event_count > 0 && event_time[0] > 0) {
        uint32_t base = event_time[0];
        for (uint16_t i = 0; i < event_count; ++i)
            event_time[i] -= base;
        if (song_length_frames > base)
            song_length_frames -= base;
        else
            song_length_frames = 0;
    }
}

/* ========================= MIDI file loading ============================== */

static load_result load_and_convert_midi(const char * filename) {
    FILE * fp = nullptr;
    char chunk_id[4];
    uint32_t chunk_len;
    uint16_t format, ntracks, file_division;
    uint16_t i;
    uint16_t parsed_tracks = 0;
    uint8_t type1_note_tracks = 0;
    uint16_t note_on_count = 0;
    load_result tr = LOAD_OK;
    uint8_t h0, h1, h2, h3, h4, h5;

    reset_conversion_state();

    if (!midi_open_file(filename))
        return LOAD_OPEN_FAILED;

    if (!file_read_u8(fp, &h0) || !file_read_u8(fp, &h1) || !file_read_u8(fp, &h2) || !file_read_u8(fp, &h3)) {
        midi_close_file();
        return LOAD_BAD_HEADER;
    }

    /* PRG files have a 2-byte load address before the actual data.
     * If we opened as PRG and the first 4 bytes aren't "MThd", check
     * if bytes 2-5 are "MThd" instead -- that means bytes 0-1 were
     * the load address and we just need to consume 2 more. */
    if (h0 == 'M' && h1 == 'T' && h2 == 'h' && h3 == 'd') {
    } else if (h2 == 'M' && h3 == 'T') {
        if (!file_read_u8(fp, &h4) || !file_read_u8(fp, &h5)) {
            midi_close_file();
            return LOAD_TRUNCATED;
        }
        if (!(h4 == 'h' && h5 == 'd')) {
            midi_close_file();
            return LOAD_BAD_HEADER;
        }
    } else {
        midi_close_file();
        return LOAD_BAD_HEADER;
    }

    if (!file_read_be32(fp, &chunk_len)) {
        midi_close_file();
        return LOAD_TRUNCATED;
    }
    if (!file_read_be16(fp, &format)) {
        midi_close_file();
        return LOAD_TRUNCATED;
    }
    if (!file_read_be16(fp, &ntracks)) {
        midi_close_file();
        return LOAD_TRUNCATED;
    }
    if (!file_read_be16(fp, &file_division)) {
        midi_close_file();
        return LOAD_TRUNCATED;
    }

    if (chunk_len > 6) {
        if (!file_skip_bytes(fp, chunk_len - 6)) {
            midi_close_file();
            return LOAD_TRUNCATED;
        }
    }

    if (format > 1) {
        midi_close_file();
        return LOAD_UNSUPPORTED_FORMAT;
    }

    /* SMPTE timing (bit 15 set) uses absolute time codes instead of
     * ticks-per-quarter-note. Almost no MIDI files use it. */
    if ((file_division & 0x8000) != 0 || file_division == 0) {
        midi_close_file();
        return LOAD_UNSUPPORTED_DIVISION;
    }

    division = file_division;
    frame_denom = (uint32_t)division * FRAME_US;

    for (i = 0; i < ntracks; ++i) {
        if (!file_read_chunk_id(fp, chunk_id)) {
            midi_close_file();
            return LOAD_TRUNCATED;
        }
        if (!file_read_be32(fp, &chunk_len)) {
            midi_close_file();
            return LOAD_TRUNCATED;
        }

        if (memcmp(chunk_id, "MTrk", 4) != 0) {
            if (!file_skip_bytes(fp, chunk_len)) {
                midi_close_file();
                return LOAD_TRUNCATED;
            }
            continue;
        }

        /* For type 1 files, stop after enough melodic tracks. More
         * tracks would just pile notes onto 3 voices anyway. */
        if (format == 1 && parsed_tracks > 0 && type1_note_tracks >= TYPE1_NOTE_TRACK_LIMIT)
            break;

        /* Parse the track speculatively. If a type 1 track turns out
         * to have too few or too many notes, roll back -- it's likely
         * a conductor track or an overly dense percussion part that
         * would make the 3-voice mix worse. */
        {
            uint16_t old_events = event_count;
            bool old_overflow = event_overflow;

            tr = parse_track_events(fp, chunk_len, &note_on_count);
            if (tr != LOAD_OK) {
                midi_close_file();
                return tr;
            }
            parsed_tracks++;

            if (format == 1 && parsed_tracks > 1) {
                if (note_on_count >= TYPE1_MIN_NOTE_ON && note_on_count <= TYPE1_MAX_NOTE_ON)
                    type1_note_tracks++;
                else {
                    event_count = old_events;
                    event_overflow = old_overflow;
                }
            }
        }
    }

    midi_close_file();

    if (!parsed_tracks)
        return LOAD_NO_TRACK;
    if (!event_count)
        return LOAD_NO_NOTE_EVENTS;

    sort_note_events();
    sort_tempo_events();
    convert_ticks_to_frames();

    return LOAD_OK;
}

/* Try all scanned disk candidates, prioritizing files whose names
 * suggest MIDI content (contain "MID", end in ".MID", etc). */

static load_result try_load_scanned_candidates(char * selected_out, uint8_t selected_out_size) {
    load_result lr = LOAD_OPEN_FAILED;
    uint8_t n;

    n = scan_disk_for_candidates();
    printf("DISK SCAN: %u CANDIDATES\n", (unsigned)n);
    if (!n) {
        puts("SCAN FOUND NO PRG/SEQ FILES ON DEVICE 8.\n");
        return LOAD_OPEN_FAILED;
    }

    for (uint8_t i = 0; i < scan_candidate_count; ++i)
        printf(" - %s\n", scan_candidates[i]);
    puts("");

    /* Two passes: first try files with MIDI-like names, then the rest. */
    for (uint8_t pass = 0; pass < 2; ++pass) {
        for (uint8_t i = 0; i < scan_candidate_count; ++i) {
            bool hinted = name_has_midi_hint(scan_candidates[i]);

            if (pass == 0 && !hinted)
                continue;
            if (pass == 1 && hinted)
                continue;

            printf("TRYING %s ...\n", scan_candidates[i]);
            lr = load_and_convert_midi(scan_candidates[i]);
            if (lr == LOAD_OK) {
                uint8_t j;
                for (j = 0; scan_candidates[i][j] && j < selected_out_size - 1; ++j)
                    selected_out[j] = scan_candidates[i][j];
                selected_out[j] = 0;
                return LOAD_OK;
            }
            if (lr != LOAD_OPEN_FAILED && lr != LOAD_BAD_HEADER)
                return lr;
        }
    }

    return lr;
}

/* ========================= SID playback engine ============================ */

static void sid_all_notes_off(void) {
    for (uint8_t i = 0; i < 3; ++i) {
        sid.voices[i].ctrl = voice_wave[i]; /* clear gate bit */
        voices[i].active = false;
    }
}

static void sid_init_player(void) {
    sid.fmodevol = 0x0a;
    sid.ffreq = 0;
    sid.resfilt = 0;

    for (uint8_t i = 0; i < 3; ++i) {
        voices[i].active = false;
        voices[i].note = 0;
        voices[i].channel = 0;
        voices[i].age = 0;

        sid.voices[i].freq = 0;
        sid.voices[i].pwm = 0x0800;  /* 50% duty cycle */
        sid.voices[i].attdec = SID_ATK_8 | SID_DKY_72;
        sid.voices[i].susrel = 0xa8;
    }

    /* Different waveforms per voice so polyphonic passages don't
     * sound like a single instrument playing chords. The triangle
     * on voice 1 gives a softer contrast against the two rectangles. */
    voice_wave[0] = SID_CTRL_RECT;
    voice_wave[1] = SID_CTRL_TRI;
    voice_wave[2] = SID_CTRL_RECT;

    for (uint8_t i = 0; i < 3; ++i)
        sid.voices[i].ctrl = voice_wave[i];

    voice_age = 1;
}

/* ========================= Voice allocation =============================== */

/* Only 3 SID voices for potentially dozens of MIDI channels, so a voice
 * stealing strategy is needed. The priority order:
 *   1. Reuse the voice already playing this exact note+channel
 *   2. Reuse a voice on the same channel (instrument continuity)
 *   3. Use a free voice
 *   4. Steal the oldest active voice (least recently triggered)
 * This heuristic keeps melodies coherent while allowing accompaniment. */

static int8_t voice_find_exact(uint8_t note, uint8_t channel) {
    for (uint8_t i = 0; i < 3; ++i) {
        if (voices[i].active && voices[i].note == note && voices[i].channel == channel)
            return (int8_t)i;
    }
    return -1;
}

static int8_t voice_find_same_note(uint8_t note) {
    for (uint8_t i = 0; i < 3; ++i) {
        if (voices[i].active && voices[i].note == note)
            return (int8_t)i;
    }
    return -1;
}

static int8_t voice_find_same_channel(uint8_t channel) {
    for (uint8_t i = 0; i < 3; ++i) {
        if (voices[i].active && voices[i].channel == channel)
            return (int8_t)i;
    }
    return -1;
}

static int8_t voice_find_free(void) {
    for (uint8_t i = 0; i < 3; ++i) {
        if (!voices[i].active)
            return (int8_t)i;
    }
    return -1;
}

static uint8_t voice_pick_oldest(void) {
    uint8_t oldest = 0;
    uint16_t age = voices[0].age;

    for (uint8_t i = 1; i < 3; ++i) {
        if (voices[i].age < age) {
            age = voices[i].age;
            oldest = i;
        }
    }
    return oldest;
}

static void sid_note_on(uint8_t channel, uint8_t note) {
    int8_t v = voice_find_exact(note, channel);

    if (v < 0)
        v = voice_find_same_channel(channel);

    if (v < 0)
        v = voice_find_free();

    if (v < 0)
        v = (int8_t)voice_pick_oldest();

    voices[v].active = true;
    voices[v].note = note;
    voices[v].channel = channel;
    voices[v].age = voice_age++;
    if (voice_age == 0)
        voice_age = 1; /* avoid 0 so "oldest" comparison works */

    sid.voices[v].freq = note_to_sid[note];
    /* Momentarily clear gate to restart the ADSR envelope, then set it. */
    sid.voices[v].ctrl = voice_wave[v];
    sid.voices[v].ctrl = voice_wave[v] | SID_CTRL_GATE;
}

static void sid_note_off(uint8_t channel, uint8_t note) {
    int8_t v = voice_find_exact(note, channel);

    /* Fall back to any voice playing this note, even on a different
     * channel, to handle sloppy MIDI files that send note-off on
     * the wrong channel. */
    if (v < 0)
        v = voice_find_same_note(note);

    if (v >= 0) {
        sid.voices[v].ctrl = voice_wave[v]; /* clear gate = release */
        voices[v].active = false;
    }
}

/* Quick A440 beep so you know audio is working before the song starts. */

static void sid_test_beep(void) {
    sid.voices[0].freq = note_to_sid[69]; /* A4 = 440 Hz */
    sid.voices[0].ctrl = SID_CTRL_RECT | SID_CTRL_GATE;
    vic_waitFrames(12);
    sid.voices[0].ctrl = SID_CTRL_RECT;
}

/* ========================= Playback loop ================================== */

static void process_event(uint8_t cmd, uint8_t note) {
    uint8_t m = cmd & 0xf0;
    uint8_t ch = cmd & 0x0f;

    if (note >= 128)
        return;

    if (m == 0x90)
        sid_note_on(ch, note);
    else if (m == 0x80)
        sid_note_off(ch, note);
}

/* Play result tells the transport loop what happened so it can
 * show the right prompt afterwards. */
typedef enum play_result {
    PLAY_FINISHED,
    PLAY_STOPPED
} play_result;

/* Frame-locked playback: process all events due on this frame, then
 * wait for the next VBlank. This ties us to the PAL 50 Hz clock,
 * which is the most accurate timer available without CIA tricks. */
/* Check the STOP key directly via the CIA keyboard matrix. Much faster
 * than the KERNAL getchx() which does a full keyboard scan -- that
 * overhead on every frame would slow down playback noticeably. */

static bool stop_key_pressed(void) {
    /* CIA1 port A row select, port B column read.
     * STOP key is at row 7, column 7. */
    *(volatile uint8_t *)0xdc00 = 0x7f;  /* select row 7 */
    return (*(volatile uint8_t *)0xdc01 & 0x80) == 0;
}

static play_result play_events(void) {
    uint16_t idx = 0;
    uint32_t frame = 0;

    if (!event_count)
        return PLAY_FINISHED;

    while (idx < event_count) {
        while (idx < event_count && event_time[idx] <= frame) {
            process_event(event_cmd[idx], event_note[idx]);
            idx++;
        }

        if (idx >= event_count)
            break;

        if (stop_key_pressed()) {
            sid_all_notes_off();
            return PLAY_STOPPED;
        }

        vic_waitFrame();
        frame++;
    }

    sid_all_notes_off();
    return PLAY_FINISHED;
}


/* ================================ Main ==================================== */

int main(void) {
    load_result lr;
    const char * selected_name = 0;
    char selected_from_scan[32];

    iocharmap(IOCHM_PETSCII_1);
    bordercolor(COLOR_BLUE);
    bgcolor(COLOR_BLUE);
    textcolor(COLOR_LT_BLUE);
    clrscr();

    puts("         **   **  **  ***    **\n");
    puts("         **   **  **  ** **  **\n");
    puts("         *** ***  **  **  ** **\n");
    puts("         ** * **  **  **  ** **\n");
    puts("         ** * **  **  **  ** **\n");
    puts("         **   **  **  ** **  **\n");
    puts("         **   **  **  ***    **\n");
    puts("\n");
    puts("               PLAYER 64\n\n");
    puts("MIDI TYPE 0/1 -> SID.\n");
    puts("3 SID VOICES AUTO-MAP.\n");
    puts("\n");
    selected_from_scan[0] = 0;
    lr = LOAD_OPEN_FAILED;
    for (uint8_t i = 0; i < sizeof(midi_candidates) / sizeof(midi_candidates[0]); ++i) {
        selected_name = midi_candidates[i];
        printf("TRYING %s ...\n", selected_name);
        lr = load_and_convert_midi(selected_name);
        if (lr == LOAD_OK)
            break;
        if (lr != LOAD_OPEN_FAILED && lr != LOAD_BAD_HEADER)
            break;
    }

    /* If fixed fallback names are not present, try scanning the
     * whole directory for PRG/SEQ candidates and probe them. */
    if (lr != LOAD_OK && (lr == LOAD_OPEN_FAILED || lr == LOAD_BAD_HEADER)) {
        puts("NO FIXED NAME MATCH, TRY DISK SCAN.\n");
        lr = try_load_scanned_candidates(selected_from_scan, sizeof(selected_from_scan));
        if (lr == LOAD_OK)
            selected_name = selected_from_scan;
    }

    if (lr != LOAD_OK) {
        printf("ERROR: %s\n", load_result_text(lr));
        puts("\nMIDI IS READ FROM DEVICE 8, DRIVE 0.\n");
        puts("TRY NAMES: MIDI/MJ/SONG OR ATTACH A D64\n");
        puts("CREATED WITH C64BUILD/C64DISK.\n");
        return 1;
    }

    printf("MIDI FILE: %s\n", selected_name);
    printf("EVENTS: %u\n", (unsigned)event_count);
    {
        unsigned long sec = song_length_frames / 50UL;
        printf("LENGTH: %lu:%02lu\n", sec / 60UL, sec % 60UL);
    }
    if (event_overflow)
        puts("WARNING: EVENT BUFFER FULL, SONG WAS CLIPPED.\n");

    sid_init_player();

    /* Simple play loop: any key starts, STOP key aborts,
     * then any key replays. No overhead during playback. */
    while (1) {
        puts("\nPRESS ANY KEY TO PLAY.\n");
        /* Drain any leftover keys in the keyboard buffer. */
        while (kbhit()) getch();
        getch();
        sid_test_beep();
        if (play_events() == PLAY_FINISHED)
            puts("\n>> END OF SONG.\n");
        else
            puts("\n>> STOPPED.\n");
    }

    return 0;
}
