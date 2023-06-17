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
  EXRLoader(const std::string &binary) {
    bool verbose = false;

    const unsigned char *binary_ptr =
        reinterpret_cast<const unsigned char *>(binary.data());
    const float *ptr = reinterpret_cast<const float *>(binary.data());

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

      // Read HALF channel as FLOAT.
      for (int i = 0; i < num_exr_headers; i++) {
        for (int c = 0; c < exr_headers[i]->num_channels; c++) {
          if (exr_headers[i]->pixel_types[c] == TINYEXR_PIXELTYPE_HALF) {
            exr_headers[i]->requested_pixel_types[c] = TINYEXR_PIXELTYPE_FLOAT;
          }
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


      for (int i = 0; i < num_exr_headers; i++) {
        EXRHeader *selectedEXRHeader = exr_headers[i];
        EXRImage selectedEXRImage = exr_images[i];

        std::vector<std::string> layer_names;
        tinyexr::GetLayers(*selectedEXRHeader, layer_names);
        if (verbose) {
          for (size_t i = 0; i < layer_names.size(); i++) {
            printf("layer name = %s\n", layer_names[i].c_str());
          }
        }

        std::vector<tinyexr::LayerChannel> channels;
        tinyexr::ChannelsInLayer(*selectedEXRHeader, "", channels);
        if (verbose) {
          for (size_t i = 0; i < channels.size(); i++) {
            printf("channel name = %s\n", channels[i].name.c_str());
          }
        }

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
        const size_t nbytes = pixel_size * sizeof(float) * 4;
        std::vector<unsigned char> image(nbytes);
        float *float_image = reinterpret_cast<float *>(&image.at(0));
        for (size_t i = 0; i < pixel_size; i++) {
          float_image[4 * i + 0] =
              reinterpret_cast<float **>(selectedEXRImage.images)[idxR][i];
          float_image[4 * i + 1] =
              reinterpret_cast<float **>(selectedEXRImage.images)[idxG][i];
          float_image[4 * i + 2] =
              reinterpret_cast<float **>(selectedEXRImage.images)[idxB][i];
          if (idxA != -1) {
            float_image[4 * i + 3] =
                reinterpret_cast<float **>(selectedEXRImage.images)[idxA][i];
          } else {
            float_image[4 * i + 3] = 1.0;
          }
        }

        images_and_names_.emplace_back(selectedEXRHeader->name, std::move(image));
        sizes_and_names_.emplace_back(selectedEXRHeader->name,
                                      std::make_pair(selectedEXRImage.width, selectedEXRImage.height));
      }
      num_exr_headers_ = num_exr_headers;
      exr_headers_ = exr_headers;

      // free images
      for (size_t i = 0; i < exr_images.size(); i++) {
        FreeEXRImage(&exr_images[i]);
      }
      return;
    }


    // SINGLE PART
    num_exr_headers_ = 1;
    exr_headers_ = (EXRHeader **)malloc(num_exr_headers_ * sizeof(EXRHeader *));
    exr_headers_[0] = (EXRHeader *)malloc(sizeof(EXRHeader));
    result_ = ParseEXRHeaderFromMemory(exr_headers_[0], &exr_version,
                                       binary_ptr, binary.size(), &err);
    EXRHeader *selectedEXRHeader = exr_headers_[0];
    if (result_ != 0) {
      fprintf(stderr, "Parse EXR err: %s\n", err);
      error_ = std::string(err);
      FreeEXRErrorMessage(err);
      return;
    }

    int width, height;
    float *rgba = nullptr;
    result_ = LoadEXRFromMemory(
        &rgba, &width, &height,
        reinterpret_cast<const unsigned char *>(binary.data()), binary.size(),
        &err);

    if (TINYEXR_SUCCESS == result_) {
      std::vector<unsigned char> image;
      size_t nbytes = width * height * 4 * sizeof(float);
      image.resize(nbytes);
      memcpy(image.data(), rgba, nbytes);
      free(rgba);
      images_and_names_.emplace_back(selectedEXRHeader->name, std::move(image));
      sizes_and_names_.emplace_back(selectedEXRHeader->name,
                                    std::make_pair(width, height));
    } else {
      if (err) {
        error_ = std::string(err);
      }
    }
  }


  ~EXRLoader() {
    if (exr_headers_) {
      for (int i = 0; i < num_exr_headers_; i++) {
        FreeEXRHeader(exr_headers_[i]);
      }
      free(exr_headers_);
    }
  }

  // Return as memory views
  emscripten::val getBytes(const std::string &part_name) const {    
    for (size_t i = 0; i < images_and_names_.size(); i++) {
      if (images_and_names_[i].first == part_name) {
        const auto &image = images_and_names_[i].second;
        return emscripten::val(
            emscripten::typed_memory_view(image.size() / sizeof(float), (float*)image.data()));
      }
    }
    return emscripten::val::null();
  }

  bool ok() const { return (TINYEXR_SUCCESS == result_); }

  const std::string error() const { return error_; }

  int width(const std::string &part_name) const { 
    for (size_t i = 0; i < sizes_and_names_.size(); i++) {
      if (sizes_and_names_[i].first == part_name) {
        return sizes_and_names_[i].second.first;
      }
    }
    return -1;
  }

  int height(const std::string &part_name) const {
    for (size_t i = 0; i < sizes_and_names_.size(); i++) {
      if (sizes_and_names_[i].first == part_name) {
        return sizes_and_names_[i].second.second;
      }
    }
    return -1;
  }

  emscripten::val partNames() const {
    std::vector<std::string> names;
    for (size_t i = 0; i < num_exr_headers_; i++) {
      names.emplace_back(exr_headers_[i]->name);
    }
    return emscripten::val::array(names);
  }

 private:
  std::vector<std::pair<std::string, std::vector<unsigned char>>> images_and_names_;
  std::vector<std::pair<std::string, std::pair<int, int>>> sizes_and_names_;

  int result_;
  std::string error_;
  size_t num_exr_headers_;
  EXRHeader **exr_headers_;
};

// Register STL
EMSCRIPTEN_BINDINGS(stl_wrappters) {
  register_vector<float>("VectorFloat");
  // register_vector<std::string>("VectorString");
  value_array<std::vector<std::string> >("VectorString");
}

EMSCRIPTEN_BINDINGS(tinyexr_module) {
  class_<EXRLoader>("EXRLoader")
      .constructor<const std::string &>()
      .function("ok", &EXRLoader::ok)
      .function("error", &EXRLoader::error)
      .function("partNames", &EXRLoader::partNames)
      .function("width", &EXRLoader::width)
      .function("height", &EXRLoader::height)
      .function("getBytes", &EXRLoader::getBytes);
}
