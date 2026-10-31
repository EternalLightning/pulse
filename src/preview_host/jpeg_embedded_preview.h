// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

// jpeg_embedded_preview.h - the preview pictures cameras store inside their
// JPEGs. Some cameras append an MPF "large thumbnail" after the main picture
// or store a smaller JPEG in the Exif block. These can avoid decoding the
// full camera image for a grid thumbnail.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::preview {

struct JpegHeaderInfo {
    uint32_t width = 0, height = 0;   // the main picture (SOF)
    uint32_t orientation = 1;         // Exif orientation of the main picture
    bool colour_profile = false;      // ICC profile, or Exif colour space "uncalibrated"
    // Exif thumbnail, as offsets into the header bytes; 0 = none.
    size_t exif_thumbnail_offset = 0, exif_thumbnail_size = 0;
    // MPF large thumbnails, as absolute file offsets.
    struct Entry { uint64_t offset = 0; uint32_t size = 0; };
    std::vector<Entry> mpf_previews;
};

// Parses the markers up to the first scan. False when the bytes do not hold
// the start of a JPEG with a frame header before the first scan.
bool ParseJpegHeader(const uint8_t* data, size_t size, JpegHeaderInfo& out);

// Width and height from a JPEG's frame header; false if none is found.
bool JpegFrameSize(const uint8_t* data, size_t size, uint32_t& width, uint32_t& height);

struct JpegEmbeddedPreview {
    std::vector<uint8_t> jpeg;        // the embedded picture, a complete JPEG
    uint32_t width = 0, height = 0;   // its size
    uint32_t source_width = 0, source_height = 0;  // the main picture (stored orientation)
    uint32_t orientation = 1;         // to apply to the embedded picture
};

// An embedded picture whose longest side reaches min_edge, with
// the main picture's proportions (a letterboxed 160 x 120 thumbnail of a 3:2
// photo is refused). False when there is none: decode the main picture.
// Attributes are checked again here before opening; placeholders never hydrate.
bool ReadJpegEmbeddedPreview(const std::wstring& path, uint32_t attrs, uint32_t min_edge,
                             JpegEmbeddedPreview& out);

} // namespace pulse::preview
