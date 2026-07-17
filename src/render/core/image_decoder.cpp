#include "render/core/image_decoder.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <memory>
#include <utility>
#include <vector>

#include "include/codec/SkCodec.h"
#include "include/codec/SkEncodedImageFormat.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"

namespace {

  std::unique_ptr<SkCodec> makeCodec(const std::uint8_t* data, std::size_t size) {
    return SkCodec::MakeFromData(SkData::MakeWithoutCopy(data, size));
  }

  std::expected<DecodedRasterImage, std::string> decodeWithSkia(const std::uint8_t* data, std::size_t size) {
    auto codec = makeCodec(data, size);
    if (!codec) {
      return std::unexpected("Skia could not recognize the image format");
    }

    const SkISize dimensions = codec->dimensions();
    if (dimensions.isEmpty()) {
      return std::unexpected("decoded image has invalid dimensions");
    }

    DecodedRasterImage decoded;
    decoded.width = dimensions.width();
    decoded.height = dimensions.height();
    const std::size_t rowBytes = static_cast<std::size_t>(decoded.width) * 4;
    decoded.pixels.assign(rowBytes * static_cast<std::size_t>(decoded.height), 0);

    const SkImageInfo info =
        SkImageInfo::Make(decoded.width, decoded.height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    SkCodec::Options options;
    options.fZeroInitialized = SkCodec::kYes_ZeroInitialized;
    const SkCodec::Result result = codec->getPixels(info, decoded.pixels.data(), rowBytes, &options);
    if (result != SkCodec::kSuccess) {
      return std::unexpected(std::string("Skia image decode failed: ") + SkCodec::ResultToString(result));
    }
    return decoded;
  }

  bool isIco(const std::uint8_t* data, std::size_t size) {
    return size >= 6 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01 && data[3] == 0x00;
  }

  bool isPng(const std::uint8_t* data, std::size_t size) {
    return size >= 8
        && data[0] == 0x89
        && data[1] == 'P'
        && data[2] == 'N'
        && data[3] == 'G'
        && data[4] == 0x0D
        && data[5] == 0x0A
        && data[6] == 0x1A
        && data[7] == 0x0A;
  }

  std::uint16_t readU16LE(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

  std::uint32_t readU32LE(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0])
        | (static_cast<std::uint32_t>(p[1]) << 8)
        | (static_cast<std::uint32_t>(p[2]) << 16)
        | (static_cast<std::uint32_t>(p[3]) << 24);
  }

  // ICO files contain a directory of sub-images (PNG or BMP DIB). Pick the
  // largest one and decode it through the normal raster pipeline. For BMP
  // sub-images we prepend a synthetic BITMAPFILEHEADER so Skia sees a
  // complete BMP.
  std::expected<DecodedRasterImage, std::string> decodeIco(const std::uint8_t* data, std::size_t size) {
    const std::uint16_t count = readU16LE(data + 4);
    if (count == 0) {
      return std::unexpected("ICO file has no images");
    }

    const std::size_t dirEnd = 6 + static_cast<std::size_t>(count) * 16;
    if (dirEnd > size) {
      return std::unexpected("ICO directory extends past end of file");
    }

    int bestIdx = -1;
    int bestArea = 0;
    int bestBpp = 0;
    for (int i = 0; i < count; ++i) {
      const std::uint8_t* entry = data + 6 + i * 16;
      int w = entry[0] == 0 ? 256 : entry[0];
      int h = entry[1] == 0 ? 256 : entry[1];
      int bpp = readU16LE(entry + 6);
      int area = w * h;
      if (area > bestArea || (area == bestArea && bpp > bestBpp)) {
        bestArea = area;
        bestBpp = bpp;
        bestIdx = i;
      }
    }

    const std::uint8_t* entry = data + 6 + bestIdx * 16;
    const std::uint32_t imgSize = readU32LE(entry + 8);
    const std::uint32_t imgOffset = readU32LE(entry + 12);

    if (static_cast<std::size_t>(imgOffset) + imgSize > size || imgSize == 0) {
      return std::unexpected("ICO entry points outside file");
    }

    const std::uint8_t* imgData = data + imgOffset;

    if (isPng(imgData, imgSize)) {
      return decodeRasterImage(imgData, imgSize);
    }

    // Skia's BMP codec treats 32bpp as BGRX
    // (alpha forced to 0xFF), but ICO uses that byte as real alpha. Decode
    // 32bpp manually; for other depths fall back to Skia + the AND mask.
    if (imgSize < 40) {
      return std::unexpected("ICO BMP sub-image too small for BITMAPINFOHEADER");
    }

    const std::uint32_t dibHeaderSize = readU32LE(imgData);
    const auto dibWidth = static_cast<std::int32_t>(readU32LE(imgData + 4));
    const auto dibHeight = static_cast<std::int32_t>(readU32LE(imgData + 8));
    const std::uint16_t bpp = readU16LE(imgData + 14);
    const int width = dibWidth > 0 ? dibWidth : -dibWidth;
    const int height = (dibHeight > 0 ? dibHeight : -dibHeight) / 2;

    if (width <= 0 || height <= 0 || width > 1024 || height > 1024) {
      return std::unexpected("ICO BMP sub-image has invalid dimensions");
    }

    const std::size_t rowStride = static_cast<std::size_t>(((width * bpp + 31) / 32)) * 4;
    const std::size_t pixelDataSize = rowStride * static_cast<std::size_t>(height);
    const std::size_t pixelDataOffset = dibHeaderSize;

    if (pixelDataOffset + pixelDataSize > imgSize) {
      return std::unexpected("ICO BMP pixel data extends past sub-image");
    }

    if (bpp == 32) {
      DecodedRasterImage decoded;
      decoded.width = width;
      decoded.height = height;
      decoded.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);

      const std::uint8_t* pixels = imgData + pixelDataOffset;
      const bool bottomUp = dibHeight > 0;
      for (int y = 0; y < height; ++y) {
        const int srcRow = bottomUp ? (height - 1 - y) : y;
        const std::uint8_t* srcLine = pixels + static_cast<std::size_t>(srcRow) * rowStride;
        std::uint8_t* dstLine =
            decoded.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
        for (int x = 0; x < width; ++x) {
          dstLine[x * 4 + 0] = srcLine[x * 4 + 2]; // R
          dstLine[x * 4 + 1] = srcLine[x * 4 + 1]; // G
          dstLine[x * 4 + 2] = srcLine[x * 4 + 0]; // B
          dstLine[x * 4 + 3] = srcLine[x * 4 + 3]; // A
        }
      }
      return decoded;
    }

    // Non-32bpp: delegate to Skia, then apply the AND mask for transparency.
    constexpr std::size_t kBmpHeaderSize = 14;
    std::vector<std::uint8_t> bmp(kBmpHeaderSize + imgSize);
    std::memcpy(bmp.data() + kBmpHeaderSize, imgData, imgSize);

    // Fix double-height → real height in the DIB header.
    {
      const std::int32_t realHeight = dibHeight > 0 ? dibHeight / 2 : dibHeight;
      const auto rh = static_cast<std::uint32_t>(realHeight);
      bmp[kBmpHeaderSize + 8] = static_cast<std::uint8_t>(rh & 0xFF);
      bmp[kBmpHeaderSize + 9] = static_cast<std::uint8_t>((rh >> 8) & 0xFF);
      bmp[kBmpHeaderSize + 10] = static_cast<std::uint8_t>((rh >> 16) & 0xFF);
      bmp[kBmpHeaderSize + 11] = static_cast<std::uint8_t>((rh >> 24) & 0xFF);
    }

    const auto pixelOffset = static_cast<std::uint32_t>(kBmpHeaderSize + dibHeaderSize);
    const auto totalSize = static_cast<std::uint32_t>(bmp.size());

    bmp[0] = 'B';
    bmp[1] = 'M';
    bmp[2] = static_cast<std::uint8_t>(totalSize & 0xFF);
    bmp[3] = static_cast<std::uint8_t>((totalSize >> 8) & 0xFF);
    bmp[4] = static_cast<std::uint8_t>((totalSize >> 16) & 0xFF);
    bmp[5] = static_cast<std::uint8_t>((totalSize >> 24) & 0xFF);
    bmp[6] = 0;
    bmp[7] = 0;
    bmp[8] = 0;
    bmp[9] = 0;
    bmp[10] = static_cast<std::uint8_t>(pixelOffset & 0xFF);
    bmp[11] = static_cast<std::uint8_t>((pixelOffset >> 8) & 0xFF);
    bmp[12] = static_cast<std::uint8_t>((pixelOffset >> 16) & 0xFF);
    bmp[13] = static_cast<std::uint8_t>((pixelOffset >> 24) & 0xFF);

    auto decoded = decodeRasterImage(bmp.data(), bmp.size());
    if (!decoded)
      return decoded;

    // Apply the 1bpp AND mask that follows the pixel data.
    const std::size_t andRowStride = static_cast<std::size_t>(((width + 31) / 32)) * 4;
    const std::size_t andOffset = pixelDataOffset + pixelDataSize;
    if (andOffset + andRowStride * static_cast<std::size_t>(height) <= imgSize) {
      const std::uint8_t* andMask = imgData + andOffset;
      const bool bottomUp = dibHeight > 0;
      for (int y = 0; y < height; ++y) {
        const int maskRow = bottomUp ? (height - 1 - y) : y;
        const std::uint8_t* maskLine = andMask + static_cast<std::size_t>(maskRow) * andRowStride;
        std::uint8_t* dstLine =
            decoded->pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
        for (int x = 0; x < width; ++x) {
          if (maskLine[x / 8] & (0x80 >> (x % 8)))
            dstLine[x * 4 + 3] = 0;
        }
      }
    }

    return decoded;
  }

} // namespace

std::expected<DecodedRasterImage, std::string> decodeRasterImage(const std::uint8_t* data, std::size_t size) {
  if ((data == nullptr) || (size == 0)) {
    return std::unexpected("empty image buffer");
  }

  if (isIco(data, size))
    return decodeIco(data, size);

  return decodeWithSkia(data, size);
}

namespace {

  bool isGif(const std::uint8_t* data, std::size_t size) {
    return size >= 6
        && data[0] == 'G'
        && data[1] == 'I'
        && data[2] == 'F'
        && data[3] == '8'
        && (data[4] == '7' || data[4] == '9')
        && data[5] == 'a';
  }

  std::uint32_t clampGifDurationMs(int durationMs) {
    if (durationMs < 10) {
      return 100; // browsers' rule for "0 / ASAP" GIFs
    }
    return static_cast<std::uint32_t>(durationMs);
  }

} // namespace

std::expected<DecodedRasterAnimation, std::string>
decodeAnimatedGif(const std::uint8_t* data, std::size_t size, int maxFrames, std::size_t maxRgbaBytes) {
  auto fail = [](const char* msg) -> std::expected<DecodedRasterAnimation, std::string> {
    return std::unexpected(msg);
  };

  if (data == nullptr || size == 0) {
    return fail("empty image buffer");
  }
  if (!isGif(data, size)) {
    return fail("not a GIF");
  }

  auto codec = makeCodec(data, size);
  if (!codec || codec->getEncodedFormat() != SkEncodedImageFormat::kGIF) {
    return fail("Skia failed to create GIF decoder");
  }

  const SkISize dimensions = codec->dimensions();
  const int width = dimensions.width();
  const int height = dimensions.height();
  if (width <= 0 || height <= 0) {
    return fail("GIF has zero dimensions");
  }
  const std::uint64_t canvasBytes64 =
      static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 4;
  if (canvasBytes64 > maxRgbaBytes) {
    return fail("GIF canvas exceeds size cap");
  }
  const auto canvasBytes = static_cast<std::size_t>(canvasBytes64);

  DecodedRasterAnimation result;
  result.width = width;
  result.height = height;

  const int frameCount = codec->getFrameCount();
  if (frameCount <= 0) {
    return fail("GIF produced no frames");
  }
  const std::vector<SkCodec::FrameInfo> frameInfo = codec->getFrameInfo();
  const SkImageInfo outputInfo = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
  const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
  std::size_t cumulativeBytes = 0;

  for (int index = 0; index < frameCount; ++index) {
    DecodedRasterFrame frame;
    frame.rgba.assign(canvasBytes, 0);

    SkCodec::Options options;
    options.fFrameIndex = index;
    options.fPriorFrame = SkCodec::kNoFrame;
    options.fZeroInitialized = SkCodec::kYes_ZeroInitialized;
    options.fMaxDecodeMemory = maxRgbaBytes;
    const SkCodec::Result decodeResult =
        codec->getPixels(outputInfo, frame.rgba.data(), rowBytes, &options);
    if (decodeResult != SkCodec::kSuccess) {
      if (result.frames.empty()) {
        return std::unexpected(
            std::string("Skia GIF frame decode failed: ") + SkCodec::ResultToString(decodeResult)
        );
      }
      // Preserve the previous behavior of returning valid leading frames when
      // malformed trailing data prevents a later frame from decoding.
      break;
    }

    frame.durationMs =
        clampGifDurationMs(
            index < static_cast<int>(frameInfo.size()) ? frameInfo[static_cast<std::size_t>(index)].fDuration : 0
        );
    cumulativeBytes += canvasBytes;
    result.frames.push_back(std::move(frame));

    if (static_cast<int>(result.frames.size()) >= maxFrames || cumulativeBytes >= maxRgbaBytes) {
      result.truncated = index + 1 < frameCount;
      break;
    }
  }

  if (result.frames.empty()) {
    return fail("GIF produced no frames");
  }
  return result;
}
