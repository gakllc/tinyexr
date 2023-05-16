#include <emscripten/bind.h>

#include <vector>

#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"

using namespace emscripten;

///
/// Simple C++ wrapper class for Emscripten
///
class EXRLoader {
 public:
  ///
  /// `binary` is the buffer for EXR binary(e.g. buffer read by fs.readFileSync)
  /// std::string can be used as UInt8Array in JS layer.
  ///
  EXRLoader(const std::string &binary, const std::string &partName) {
    const unsigned char *binary_ptr =
        reinterpret_cast<const unsigned char *>(binary.data());
    const float *ptr = reinterpret_cast<const float *>(binary.data());

    float *rgba = nullptr;
    width_ = -1;
    height_ = -1;
    const char *err = nullptr;

    error_.clear();

    EXRVersion exr_version;
    result_ =
        ParseEXRVersionFromMemory(&exr_version, binary_ptr, binary.size());
    if (result_ != 0) {
      fprintf(stderr, "Invalid EXR file\n");
      return;
    }

    if (exr_version.multipart) {
      EXRHeader **exr_headers;  // list of EXRHeader pointers.
      int num_exr_headers;
      const char *err = nullptr;

      // Memory for EXRHeader is allocated inside of
      // ParseEXRMultipartHeaderFromFile,
      result_ = ParseEXRMultipartHeaderFromMemory(
          &exr_headers, &num_exr_headers, &exr_version, binary_ptr,
          binary.size(), &err);
      if (result_ != 0) {
        fprintf(stderr, "Parse EXR err: %s\n", err);
        error_ = std::string(err);
        FreeEXRErrorMessage(err);  // free's buffer for an error message
        return;
      }

      size_t partIdx = 0;
      if (partName != "") {
        int i = 0;
        for (; i < num_exr_headers; i++) {
          if (std::string(exr_headers[i]->name) == partName) {
            partIdx = i;
            break;
          }
        }
        if (i == num_exr_headers) {
          fprintf(stderr, "Cannot find part %s\n", partName.c_str());
          error_ = std::string("Cannot find part ") + partName;
          result_ = -1;
          return;
        }
      }
      EXRHeader selectedEXRHeader = *exr_headers[partIdx];

      std::vector<std::string> layer_names;
      tinyexr::GetLayers(selectedEXRHeader, layer_names);
      // for (size_t i = 0; i < layer_names.size(); i++) {
      //   printf("layer name = %s\n", layer_names[i].c_str());
      // }

      std::vector<tinyexr::LayerChannel> channels;
      tinyexr::ChannelsInLayer(selectedEXRHeader, "", channels);
      // for (size_t i = 0; i < channels.size(); i++) {
      //   printf("channel name = %s\n", channels[i].name.c_str());
      // }

      // Read HALF channel as FLOAT.
      for (int i = 0; i < selectedEXRHeader.num_channels; i++) {
        if (selectedEXRHeader.pixel_types[i] == TINYEXR_PIXELTYPE_HALF) {
          selectedEXRHeader.requested_pixel_types[i] = TINYEXR_PIXELTYPE_FLOAT;
        }
      }

      std::vector<EXRImage> exr_images(num_exr_headers);
      for (int i = 0; i < num_exr_headers; i++) {
        InitEXRImage(&exr_images[i]);
      }

      result_ = LoadEXRMultipartImageFromMemory(
          &exr_images.at(0), const_cast<const EXRHeader **>(exr_headers),
          num_exr_headers, binary_ptr, binary.size(), &err);
      if (result_ != 0) {
        fprintf(stderr, "Parse EXR err: %s\n", err);
        error_ = std::string(err);
        FreeEXRErrorMessage(err);  // free's buffer for an error message
        return;
      }

      EXRImage selectedEXRImage = exr_images[partIdx];

      int idxR = -1;
      int idxG = -1;
      int idxB = -1;
      int idxA = -1;
      size_t ch_count = channels.size() < 4 ? channels.size() : 4;
      for (size_t c = 0; c < ch_count; c++) {
        const tinyexr::LayerChannel &ch = channels[c];

        if (ch.name == "R") {
          idxR = int(ch.index);
        } else if (ch.name == "G") {
          idxG = int(ch.index);
        } else if (ch.name == "B") {
          idxB = int(ch.index);
        } else if (ch.name == "A") {
          idxA = int(ch.index);
        }
      }

      const size_t pixel_size = static_cast<size_t>(selectedEXRImage.width) *
                                static_cast<size_t>(selectedEXRImage.height);
      width_ = selectedEXRImage.width;
      height_ = selectedEXRImage.height;
      image_.resize(size_t(pixel_size * 4));
      for (size_t i = 0; i < pixel_size; i++) {
        image_[4 * i + 0] =
            reinterpret_cast<float **>(selectedEXRImage.images)[idxR][i];
        image_[4 * i + 1] =
            reinterpret_cast<float **>(selectedEXRImage.images)[idxG][i];
        image_[4 * i + 2] =
            reinterpret_cast<float **>(selectedEXRImage.images)[idxB][i];
        if (idxA != -1) {
          image_[4 * i + 3] =
              reinterpret_cast<float **>(selectedEXRImage.images)[idxA][i];
        } else {
          image_[4 * i + 3] = 1.0;
        }
      }

      for (int i = 0; i < num_exr_headers; i++) {
        FreeEXRHeader(exr_headers[i]);
      }
      free(exr_headers);

      // free images
      for (size_t i = 0; i < exr_images.size(); i++) {
        FreeEXRImage(&exr_images[i]);
      }
      return;
    }

    // SINGLE PART
    result_ = LoadEXRFromMemory(
        &rgba, &width_, &height_,
        reinterpret_cast<const unsigned char *>(binary.data()), binary.size(),
        &err);

    if (TINYEXR_SUCCESS == result_) {
      image_.resize(size_t(width_ * height_ * 4));
      memcpy(image_.data(), rgba, sizeof(float) * size_t(width_ * height_ * 4));
      free(rgba);
    } else {
      if (err) {
        error_ = std::string(err);
      }
    }
  }
  ~EXRLoader() {}

  // Return as memory views
  emscripten::val getBytes() const {
    return emscripten::val(
        emscripten::typed_memory_view(image_.size(), image_.data()));
  }

  bool ok() const { return (TINYEXR_SUCCESS == result_); }

  const std::string error() const { return error_; }

  int width() const { return width_; }

  int height() const { return height_; }

 private:
  std::vector<float> image_;  // RGBA
  int width_;
  int height_;
  int result_;
  std::string error_;
};

// Register STL
EMSCRIPTEN_BINDINGS(stl_wrappters) { register_vector<float>("VectorFloat"); }

EMSCRIPTEN_BINDINGS(tinyexr_module) {
  class_<EXRLoader>("EXRLoader")
      .constructor<const std::string &, const std::string &>()
      .function("getBytes", &EXRLoader::getBytes)
      .function("ok", &EXRLoader::ok)
      .function("error", &EXRLoader::error)
      .function("width", &EXRLoader::width)
      .function("height", &EXRLoader::height);
}
