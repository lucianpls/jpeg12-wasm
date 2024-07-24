#include <cstdint>
#include <csetjmp>
#include <emscripten.h>
#include <cstring>

#include "json.hpp"

extern "C" {
#include "jpeg12-6b/jpeglib.h"
#include "jpeg12-6b/jerror.h"
}

extern "C" {
    // Returns a json string containing information about the jpeg12 data
    EMSCRIPTEN_KEEPALIVE
    char *getinfo(uint8_t *, uint32_t);

    // Returns a json string containing information about the jpeg12 data
    // On failure, the json.message contains the error message
    EMSCRIPTEN_KEEPALIVE
    char *decode(uint8_t *, size_t, uint16_t *, size_t);
}

using json = nlohmann::json;

struct jpeginfo {
    int width;
    int height;
    int num_components;
    int data_precision;
    char error[JMSG_LENGTH_MAX];
};

// returns true if it works, works for either 8 or 12 bit
static bool isjpeg(uint8_t *data, size_t size, jpeginfo *info) {
    if (size < 10) {
        strcpy(info->error, "Not enough input data");
        return false;
    }
    if (data[0] != 0xff || data[1] != 0xd8) {
        strcpy(info->error, "Not a JPEG file");
        return false;
    }

    auto *last = data + size;
    data += 2;

    while (data < last) {
        if (*data++ != 0xff)
            continue; // Skip non-markers
        // Make sure the marker can be read
        if (data >= last) {
            strcpy(info->error, "Not enough data for marker");
            return false;
        }
        
        // chunks with no size, RST, EOI, TEM or raw 0xff
        // Also check that we can read two more bytes
        auto marker = *data++;
        if (marker == 0 || (marker >= 0xd0 && marker <= 0xd9) || data > last - 2)
            continue;
        
        switch(marker) {
            // Chunks with no size
            case 0: // Raw 0xff
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
                if (data > last - 2) {
                    strcpy(info->error, "Not enough data for segment size");
                    return false;
                }
                auto sz = (data[0] << 8) + data[1];
                if (data + sz > last) {
                    strcpy(info->error, "Not enough data for header segment");
                    return false;
                }
                // Size is 8 bytes + 3 * num_components
                info->data_precision = data[2];
                info->height = (data[3] << 8) + data[4];
                info->width = (data[5] << 8) + data[6];
                info->num_components = data[7];
                // Check the size
                if (sz != 8 + 3 * info->num_components) {
                    strcpy(info->error, "Invalid header segment size");
                    return false;
                }
                return true;
            };

            default: // normal segments, skip
                // Skip the size, 2 bytes
                if (data > last - 2) {
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

char *getinfo(uint8_t *data, uint32_t size) {
    jpeginfo info;
    if (!isjpeg(data, size, &info)) {
        json j = {{"error", info.error}};
        return strdup(j.dump().c_str());
    }

    json j = {
        {"width", info.width},
        {"height", info.height},
        {"num_components", info.num_components},
        {"data_precision", info.data_precision},
    };

    return strdup(j.dump().c_str());

}

struct JPG12Handle {
    jmp_buf setjmp_buffer;
    char *message;
};

static void emitMessage(j_common_ptr cinfo, int msg_level) {
    if (msg_level > 0) return; // No trace
    // There can be may warnings, so only store the first one
    if (cinfo->err->num_warnings++ > 1) return;

    JPG12Handle *handle = (JPG12Handle *) cinfo->client_data;
    cinfo->err->format_message(cinfo, handle->message);
}

static void errorExit(j_common_ptr cinfo) {
    JPG12Handle *handle = (JPG12Handle *) cinfo->client_data;
    cinfo->err->format_message(cinfo, handle->message);
    longjmp(handle->setjmp_buffer, 1);
}

// Do nothing stub, called
static void stub_source_dec(j_decompress_ptr cinfo) {
    // Do nothing
}

// Called for unknown chunks, needs to skip the data
static void skip_input_data_dec(j_decompress_ptr cinfo, long num_bytes) {
    struct jpeg_source_mgr *src = cinfo->src;
    if (num_bytes > 0) {
        src->next_input_byte += num_bytes;
        src->bytes_in_buffer -= num_bytes;
    }
}

static boolean fill_input_buffer_dec(j_decompress_ptr cinfo) {
    return FALSE;
}


// double macros, so they need to be redefined to use the 12bit version
#undef jpeg_create_compress
#undef jpeg_create_decompress

#define jpeg_create_compress(cinfo) \
    jpeg_CreateCompress_12((cinfo), JPEG_LIB_VERSION, \
			(size_t) sizeof(struct jpeg_compress_struct))
#define jpeg_create_decompress(cinfo) \
    jpeg_CreateDecompress_12((cinfo), JPEG_LIB_VERSION, \
			  (size_t) sizeof(struct jpeg_decompress_struct))

// Returns a json string containing either the error message or the info about the decoded image
char *decode(uint8_t *jpeg12, size_t size, uint16_t *output, size_t outsize) {
    // Get the info before we start
    jpeginfo info = {};
    if (!isjpeg(jpeg12, size, &info)) {
        json j = {{"error", info.error}};
        return strdup(j.dump().c_str());
    };

    // Check that the size matches expectations
    size_t expected_size = info.width * info.height * info.num_components * 2;
    if (info.data_precision != 12 || expected_size != outsize) {
        json j = {
            {"error", "Output buffer too small"},
            {"width", info.width},
            {"height", info.height},
            {"num_components", info.num_components},
            {"data_precision", info.data_precision},
        };
        return strdup(j.dump().c_str());
    }

    struct jpeg_decompress_struct cinfo;
    JPG12Handle handle;
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

    if (setjmp(handle.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        return nullptr;
        json j = {{"error", info.error}};
        return strdup(j.dump().c_str());
    }

    jpeg_create_decompress(&cinfo);
    cinfo.src = &s;
    jpeg_read_header(&cinfo, TRUE);

    // Use the message as an error message
    if (jpeg_has_multiple_scans(&cinfo) || cinfo.arith_code)
        strcpy(info.error, "JPEG type not supported");

    if (cinfo.data_precision != 12)
        strcpy(info.error, "JPEG data precision not 12 bits");
    
    if (0 != strlen(info.error)) {
        jpeg_destroy_decompress(&cinfo);
        json j = {{"error", info.error}};
        return strdup(j.dump().c_str());
    }

    // It worked, get and return the info
    jpeg_start_decompress(&cinfo);
    JSAMPLE *rp[2]; // Two lines is what JPEG does
    while (cinfo.output_scanline < cinfo.output_height) {
        rp[0] = (JSAMPROW)(output + cinfo.output_scanline * info.width * info.num_components);
        rp[1] = (JSAMPROW)(output + (cinfo.output_scanline + 1) * info.width * info.num_components);
        jpeg_read_scanlines(&cinfo, rp, 2);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    // Done, return the info, no error
    json j = {
        {"width", info.width},
        {"height", info.height},
        {"num_components", info.num_components},
        {"data_precision", info.data_precision},
    };
    return strdup(j.dump().c_str());
}