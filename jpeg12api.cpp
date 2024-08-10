#include <cstdint>
#include <csetjmp>
#include <emscripten.h>
#include <cstring>

#include "json.hpp"
#define PACKER
#include "BitMask2D.h"
#include "Packer_RLE.h"

extern "C"
{
#include "jpeg12-6b/jpeglib.h"
#include "jpeg12-6b/jerror.h"
}

// The exported functions
extern "C"
{
    // Returns a json string containing information about the jpeg12 data
    EMSCRIPTEN_KEEPALIVE
    char *getinfo(uint8_t *, uint32_t);

    // Returns a json string containing information about the jpeg12 data
    // On failure, the json.message contains the error message
    EMSCRIPTEN_KEEPALIVE
    char *decode(uint8_t *, size_t, uint16_t *, size_t);
}

using json = nlohmann::json;

struct jpeginfo
{
    int width;
    int height;
    int num_components;
    int data_precision;
    char error[JMSG_LENGTH_MAX];
};

// returns true if it works, works for either 8 or 12 bit
static bool isjpeg(uint8_t *data, size_t size, jpeginfo *info)
{
    if (size < 10)
    {
        strcpy(info->error, "Not enough input data");
        return false;
    }
    if (data[0] != 0xff || data[1] != 0xd8)
    {
        strcpy(info->error, "Not a JPEG file");
        return false;
    }

    auto *last = data + size;
    data += 2;

    while (data < last)
    {
        if (*data++ != 0xff)
            continue; // Skip non-markers
        // Make sure the marker can be read
        if (data >= last)
        {
            strcpy(info->error, "Not enough data for marker");
            return false;
        }

        // chunks with no size, RST, EOI, TEM or raw 0xff
        // Also check that we can read two more bytes
        auto marker = *data++;
        if (marker == 0 || (marker >= 0xd0 && marker <= 0xd9) || data > last - 2)
            continue;

        switch (marker)
        {
        // Chunks with no size
        case 0:    // Raw 0xff
        case 0xd0: // RST0
        case 0xd1:
        case 0xd2:
        case 0xd3:
        case 0xd4:
        case 0xd5:
        case 0xd6:
        case 0xd7: // RST7
        case 0xd8: // Start of image
            continue;

        case 0xd9: // EOI, should not be here
            strcpy(info->error, "Found end of image too early");
            return false;

        case 0xda: // Start of scan, we got too far
            strcpy(info->error, "Found start of scan too early");
            return false;

        case 0xc0: // SOF0
        case 0xc1: // SOF1, also baseline
        {
            // Make sure we can read the size
            if (data > last - 2)
            {
                strcpy(info->error, "Not enough data for segment size");
                return false;
            }
            auto sz = (data[0] << 8) + data[1];
            if (data + sz > last)
            {
                strcpy(info->error, "Not enough data for header segment");
                return false;
            }
            // Size is 8 bytes + 3 * num_components
            info->data_precision = data[2];
            info->height = (data[3] << 8) + data[4];
            info->width = (data[5] << 8) + data[6];
            info->num_components = data[7];
            // Check the size
            if (sz != 8 + 3 * info->num_components)
            {
                strcpy(info->error, "Invalid header segment size");
                return false;
            }
            return true;
        };

        default: // normal segments, skip
            // Skip the size, 2 bytes
            if (data > last - 2)
            {
                strcpy(info->error, "Not enough data for segment size");
                return false;
            }
            data += (data[0] << 8) + data[1];
        }
    }

    // not reached
    strcpy(info->error, "No start of frame found");
    return false;
}

char *getinfo(uint8_t *data, uint32_t size)
{
    jpeginfo info;
    if (!isjpeg(data, size, &info))
    {
        json j = {{"error", info.error}};
        return strdup(j.dump().c_str());
    }

    json j = {
        {"width", info.width},
        {"height", info.height},
        {"numComponents", info.num_components},
        {"dataPrecision", info.data_precision},
    };

    return strdup(j.dump().c_str());
}

struct JPG12Handle
{
    jmp_buf setjmp_buffer;
    char *message;
    struct
    {
        JOCTET *buffer;
        size_t size;
    } zenChunk;
};

static void emitMessage(j_common_ptr cinfo, int msg_level)
{
    if (msg_level > 0)
        return; // No trace
    // There can be may warnings, so only store the first one
    if (cinfo->err->num_warnings++ > 1)
        return;

    JPG12Handle *handle = (JPG12Handle *)cinfo->client_data;
    cinfo->err->format_message(cinfo, handle->message);
}

static void errorExit(j_common_ptr cinfo)
{
    JPG12Handle *handle = (JPG12Handle *)cinfo->client_data;
    cinfo->err->format_message(cinfo, handle->message);
    longjmp(handle->setjmp_buffer, 1);
}

// Do nothing stub, called
static void stub_source_dec(j_decompress_ptr cinfo)
{
    // Do nothing
}

// Called for unknown chunks, needs to skip the data
static void skip_input_data_dec(j_decompress_ptr cinfo, long num_bytes)
{
    struct jpeg_source_mgr *src = cinfo->src;
    if (num_bytes > 0)
    {
        src->next_input_byte += num_bytes;
        src->bytes_in_buffer -= num_bytes;
    }
}

// This might need to return TRUE, not needed when the
// whole jpeg is in the buffer
static boolean fill_input_buffer_dec(j_decompress_ptr cinfo)
{
    return FALSE;
}

//
// JPEG marker processor, for the Zen app3 marker
// Can't return error, only works if the Zen chunk is fully in buffer
// Since this decoder has the whole JPEG in memory, we can just store a pointer
//
#define CHUNK_NAME "Zen"
#define CHUNK_NAME_SIZE 4

// Save the chunk pointer, otherwise it's a skip_input_data,
static boolean zenChunkHandler(j_decompress_ptr cinfo)
{
    struct jpeg_source_mgr *src = cinfo->src;
    if (src->bytes_in_buffer < 2)
        ERREXIT(cinfo, JERR_CANT_SUSPEND);

    // 16 bit, big endian chunk length
    int len = (*src->next_input_byte++) << 8;
    len += *src->next_input_byte++;
    // The length includes the two bytes we just read
    src->bytes_in_buffer -= 2;
    len -= 2;
    // Check that it is safe to read the rest
    if (src->bytes_in_buffer < static_cast<size_t>(len))
        ERREXIT(cinfo, JERR_CANT_SUSPEND);

    // filter out chunks that have the wrong signature, just skip them
    if (strcmp(reinterpret_cast<const char *>(src->next_input_byte), CHUNK_NAME))
    {
        src->bytes_in_buffer -= len;
        src->next_input_byte += len;
        return true;
    }

    // Skip the signature and keep a direct chunk pointer
    src->bytes_in_buffer -= CHUNK_NAME_SIZE;
    src->next_input_byte += CHUNK_NAME_SIZE;
    len -= static_cast<int>(CHUNK_NAME_SIZE);

    auto jh = reinterpret_cast<JPG12Handle *>(cinfo->client_data);
    // Store a pointer to the Zen chunk in the handler
    jh->zenChunk.buffer = const_cast<JOCTET *>(src->next_input_byte);
    jh->zenChunk.size = len;

    src->bytes_in_buffer -= len;
    src->next_input_byte += len;
    return true;
}

// Apply the mask to the buffer, in place
// Needs to know the number of channels since the mask is per pixel
template <typename T>
static void apply_mask(BitMap2D<uint64_t> &mask, T *s, int nc)
{
    int w = mask.getWidth();
    int h = mask.getHeight();

    if (nc == 1)
    {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                if (mask.isSet(x, y))
                { // Non zero pixel
                    if (*s == 0)
                        *s = 1;
                }
                else
                { // Zero pixel
                    if (*s != 0)
                        *s = 0;
                }
                s++;
            }
        return;
    }

    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            if (mask.isSet(x, y))
            { // Non zero pixel
                for (int c = 0; c < nc; c++, s++)
                {
                    if (*s == 0)
                        *s = 1;
                }
            }
            else
            { // Zero pixel
                for (int c = 0; c < nc; c++)
                    *s++ = 0;
            }
        }
}

// Unpacks and applies the Zen chunk in place
static void applyZenChunk(const jpeginfo &info, const JPG12Handle &handle, uint16_t *output)
{
    BitMap2D<uint64_t> bm(info.width, info.height);
    RLEC3Packer packer;
    bm.set_packer(&packer);
    storage_manager src = {
        reinterpret_cast<char *>(handle.zenChunk.buffer),
        handle.zenChunk.size};

    // load and apply the mask
    if (bm.load(&src))
        apply_mask(bm, output, info.num_components);
}

// These would be double macros, they need to be redefined to use the 12bit version
#undef jpeg_create_compress
#undef jpeg_create_decompress

#define jpeg_create_compress(cinfo)                   \
    jpeg_CreateCompress_12((cinfo), JPEG_LIB_VERSION, \
                           (size_t)sizeof(struct jpeg_compress_struct))
#define jpeg_create_decompress(cinfo)                   \
    jpeg_CreateDecompress_12((cinfo), JPEG_LIB_VERSION, \
                             (size_t)sizeof(struct jpeg_decompress_struct))

//
// Decodes the JPEG12 data into a buffer
// Returns a json string containing either the error message or the info about the decoded image
//
char *decode(uint8_t *jpeg12, size_t size, uint16_t *output, size_t outsize)
{
    // Get the info before we start
    jpeginfo info = {};
    if (!isjpeg(jpeg12, size, &info))
    {
        json j = {{"error", info.error}};
        return strdup(j.dump().c_str());
    };

    // Check that the size matches expectations
    size_t expected_size = info.width * info.height * info.num_components * 2;
    if (info.data_precision != 12 || expected_size != outsize)
    {
        json j = {
            {"error", "Output buffer too small"},
            {"width", info.width},
            {"height", info.height},
            {"numComponents", info.num_components},
            {"dataPrecision", info.data_precision},
        };
        return strdup(j.dump().c_str());
    }

    struct jpeg_decompress_struct cinfo;
    JPG12Handle handle;
    handle.zenChunk.buffer = nullptr;
    memset(&handle, 0, sizeof(handle));
    jpeg_error_mgr jerr;
    memset(&jerr, 0, sizeof(jerr));
    handle.message = info.error; // reuse the info for the error message

    struct jpeg_source_mgr s = {(JOCTET *)jpeg12, size};
    cinfo.err = jpeg_std_error(&jerr);
    jerr.error_exit = errorExit;
    jerr.emit_message = emitMessage;

    s.term_source = s.init_source = stub_source_dec;
    s.skip_input_data = skip_input_data_dec;
    s.fill_input_buffer = fill_input_buffer_dec;
    s.resync_to_restart = jpeg_resync_to_restart;
    cinfo.client_data = &handle;

    if (setjmp(handle.setjmp_buffer))
    {
        jpeg_destroy_decompress(&cinfo);
        return nullptr;
        json j = {{"error", info.error}};
        return strdup(j.dump().c_str());
    }

    jpeg_create_decompress(&cinfo);
    cinfo.src = &s;
    // This is the only marker we are interested in, saves the pointer and size
    // If present, it does get called in the read_header, before the data is decoded
    jpeg_set_marker_processor(&cinfo, JPEG_APP0 + 3, zenChunkHandler);
    jpeg_read_header(&cinfo, TRUE);

    // Use the message as an error message
    if (jpeg_has_multiple_scans(&cinfo) || cinfo.arith_code)
        strcpy(info.error, "JPEG type not supported");

    if (cinfo.data_precision != 12)
        strcpy(info.error, "JPEG data precision not 12 bits");

    if (0 != strlen(info.error))
    {
        jpeg_destroy_decompress(&cinfo);
        json j = {{"error", info.error}};
        return strdup(j.dump().c_str());
    }

    // Use DCT_FLOAT, just in case it's not the default
    // It is faster than JDCT_ISLOW and almost as fast as JDCT_IFAST
    cinfo.dct_method = JDCT_FLOAT;

    // Decode and return the info
    jpeg_start_decompress(&cinfo);
    auto linesize = info.width * info.num_components;
    while (cinfo.output_scanline < cinfo.output_height)
    {
        auto rp = (JSAMPROW)(output + cinfo.output_scanline * linesize);
        jpeg_read_scanlines(&cinfo, &rp, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    // Done, return the info, no error
    json j = {
        {"width", info.width},
        {"height", info.height},
        {"numComponents", info.num_components},
        {"dataPrecision", info.data_precision}};

#ifndef IGNORE_ZEN_CHUNK
    // Flag the caller that the zen chunk was detected and processed
    if (handle.zenChunk.buffer)
        j["zenChunkSize"] = handle.zenChunk.size;
    // If the zen chunk is present
    if (handle.zenChunk.buffer)
    {
        if (handle.zenChunk.size > 0) // Not empty
            applyZenChunk(info, handle, output);
        else
        { // present but empty zen chunk, force all values non-zero
            for (size_t i = 0; i < outsize / 2; i++)
                if (output[i] == 0)
                    output[i] = 1;
        }
    }
#endif

    return strdup(j.dump().c_str());
}