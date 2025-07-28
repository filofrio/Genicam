#pragma once

#include <cstdint>
#include <memory>
#include <chrono>
#include <opencv2/core.hpp>

namespace GenICamWrapper {

    /**
     * @brief Enumerazione dei formati pixel supportati
     */
   enum class PixelFormat {
      // Formati monocromatici
      Mono8,              // 8-bit grayscale
      Mono10,             // 10-bit grayscale
      Mono12,             // 12-bit grayscale
      Mono14,             // 14-bit grayscale
      Mono16,             // 16-bit grayscale

      // Formati monocromatici packed
      Mono10Packed,       // 10-bit packed
      Mono12Packed,       // 12-bit packed

      // Formati RGB/BGR 8 bit
      RGB8,               // 8-bit RGB
      BGR8,               // 8-bit BGR (OpenCV native)
      RGBa8,              // 8-bit RGBA
      BGRa8,              // 8-bit BGRA

      // Formati RGB/BGR 10/12/16 bit
      RGB10,              // 10-bit RGB
      BGR10,              // 10-bit BGR
      RGB12,              // 12-bit RGB
      BGR12,              // 12-bit BGR
      RGB16,              // 16-bit RGB
      BGR16,              // 16-bit BGR

      // Formati Bayer 8 bit
      BayerGR8,           // Bayer pattern GR 8-bit
      BayerRG8,           // Bayer pattern RG 8-bit
      BayerGB8,           // Bayer pattern GB 8-bit
      BayerBG8,           // Bayer pattern BG 8-bit

      // Formati Bayer 10 bit
      BayerGR10,          // Bayer pattern GR 10-bit
      BayerRG10,          // Bayer pattern RG 10-bit
      BayerGB10,          // Bayer pattern GB 10-bit
      BayerBG10,          // Bayer pattern BG 10-bit

      // Formati Bayer 12 bit
      BayerGR12,          // Bayer pattern GR 12-bit
      BayerRG12,          // Bayer pattern RG 12-bit
      BayerGB12,          // Bayer pattern GB 12-bit
      BayerBG12,          // Bayer pattern BG 12-bit

      // Formati Bayer 16 bit
      BayerGR16,          // Bayer pattern GR 16-bit
      BayerRG16,          // Bayer pattern RG 16-bit
      BayerGB16,          // Bayer pattern GB 16-bit
      BayerBG16,          // Bayer pattern BG 16-bit

      // Formati Bayer packed
      BayerGR10Packed,    // Bayer pattern GR 10-bit packed
      BayerRG10Packed,    // Bayer pattern RG 10-bit packed
      BayerGB10Packed,    // Bayer pattern GB 10-bit packed
      BayerBG10Packed,    // Bayer pattern BG 10-bit packed
      BayerGR12Packed,    // Bayer pattern GR 12-bit packed
      BayerRG12Packed,    // Bayer pattern RG 12-bit packed
      BayerGB12Packed,    // Bayer pattern GB 12-bit packed
      BayerBG12Packed,    // Bayer pattern BG 12-bit packed

      // Formati YUV
      YUV422_8,           // YUV 4:2:2 generico
      YUV422_8_UYVY,      // YUV 4:2:2 UYVY
      YUV422_8_YUYV,      // YUV 4:2:2 YUYV
      YUV444_8,           // YUV 4:4:4

      // Formati 3D
      Coord3D_ABC32f,     // 3D coordinates float
      Coord3D_ABC16,      // 3D coordinates 16-bit
      Confidence8,        // Confidence map 8-bit
      Confidence16,       // Confidence map 16-bit

      // Formato non definito
      Undefined
   };

    /**
     * @brief Struttura per i parametri ROI (Region of Interest)
     */
    struct ROI {
        uint32_t x;         // Offset X
        uint32_t y;         // Offset Y
        uint32_t width;     // Larghezza
        uint32_t height;    // Altezza

        ROI() : x(0), y(0), width(0), height(0) {}
        ROI(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
            : x(x), y(y), width(w), height(h) {}
    };

    /**
     * @brief Struttura contenente i dati dell'immagine e metadati
     */
    class ImageData {
    public:
        // Dati raw dell'immagine
        std::shared_ptr<uint8_t> buffer;
        size_t bufferSize;

        // Informazioni sull'immagine
        uint32_t width;
        uint32_t height;
        PixelFormat pixelFormat;
        size_t stride;          // Bytes per riga

        // Metadati temporali
        uint64_t frameID;       // ID univoco del frame
        std::chrono::steady_clock::time_point timestamp;

        // Informazioni di acquisizione
        double exposureTime;    // in microsecondi
        double gain;            // gain analogico/digitale

        /**
         * @brief Costruttore di default
         */
        ImageData()
            : buffer(nullptr), bufferSize(0), width(0), height(0),
            pixelFormat(PixelFormat::Undefined), stride(0),
            frameID(0), exposureTime(0.0), gain(0.0) {
            timestamp = std::chrono::steady_clock::now();
        }

        /**
         * @brief Converte l'immagine in formato OpenCV Mat
         * @return cv::Mat con i dati dell'immagine
         */
        cv::Mat toCvMat() const {
           if (!buffer || bufferSize == 0) {
              return cv::Mat();
           }

           int cvType;
           cv::Mat resultMat;

           switch (pixelFormat) {
              // Formati monocromatici
           case PixelFormat::Mono8:
              cvType = CV_8UC1;
              resultMat = cv::Mat(height, width, cvType, buffer.get(), stride);
              break;

           case PixelFormat::Mono10:
           case PixelFormat::Mono12:
           case PixelFormat::Mono14:
           case PixelFormat::Mono16:
              cvType = CV_16UC1;
              resultMat = cv::Mat(height, width, cvType, buffer.get(), stride);
              break;

              // Formati RGB/BGR 8 bit
           case PixelFormat::RGB8:
           case PixelFormat::BGR8:
              cvType = CV_8UC3;
              resultMat = cv::Mat(height, width, cvType, buffer.get(), stride);
              break;

              // Formati RGBA/BGRA 8 bit
           case PixelFormat::RGBa8:
           case PixelFormat::BGRa8:
              cvType = CV_8UC4;
              resultMat = cv::Mat(height, width, cvType, buffer.get(), stride);
              break;

              // Formati Bayer 8 bit
           case PixelFormat::BayerGR8:
           case PixelFormat::BayerRG8:
           case PixelFormat::BayerGB8:
           case PixelFormat::BayerBG8:
              cvType = CV_8UC1;
              resultMat = cv::Mat(height, width, cvType, buffer.get(), stride);
              break;

              // Formati Bayer 10/12/16 bit
           case PixelFormat::BayerGR10:
           case PixelFormat::BayerRG10:
           case PixelFormat::BayerGB10:
           case PixelFormat::BayerBG10:
           case PixelFormat::BayerGR12:
           case PixelFormat::BayerRG12:
           case PixelFormat::BayerGB12:
           case PixelFormat::BayerBG12:
           case PixelFormat::BayerGR16:
           case PixelFormat::BayerRG16:
           case PixelFormat::BayerGB16:
           case PixelFormat::BayerBG16:
              cvType = CV_16UC1;
              resultMat = cv::Mat(height, width, cvType, buffer.get(), stride);
              break;

              // Formati YUV
           case PixelFormat::YUV422_8:
           case PixelFormat::YUV422_8_UYVY:
           case PixelFormat::YUV422_8_YUYV:
              // YUV422 ha 2 bytes per pixel in formato packed
              cvType = CV_8UC2;
              resultMat = cv::Mat(height, width, cvType, buffer.get(), stride);
              break;

           case PixelFormat::YUV444_8:
              cvType = CV_8UC3;
              resultMat = cv::Mat(height, width, cvType, buffer.get(), stride);
              break;

              // Formati packed (10/12 bit packed)
           case PixelFormat::Mono10Packed:
           case PixelFormat::BayerGR10Packed:
           case PixelFormat::BayerRG10Packed:
           case PixelFormat::BayerGB10Packed:
           case PixelFormat::BayerBG10Packed:
           {
              // Mono10Packed: 4 pixel in 5 byte
              size_t unpackedSize = width * height * sizeof(uint16_t);
              std::unique_ptr<uint16_t[]> unpackedBuffer(new uint16_t[width * height]);

              const uint8_t* srcPtr = buffer.get();
              uint16_t* dstPtr = unpackedBuffer.get();

              // Decompressione: ogni gruppo di 5 byte contiene 4 pixel da 10 bit
              for (size_t y = 0; y < height; ++y) {
                 const uint8_t* rowPtr = srcPtr + y * stride;
                 uint16_t* dstRowPtr = dstPtr + y * width;

                 size_t x = 0;
                 while (x < width) {
                    if (x + 3 < width) {
                       // Gruppo completo di 4 pixel
                       uint8_t b0 = rowPtr[0];
                       uint8_t b1 = rowPtr[1];
                       uint8_t b2 = rowPtr[2];
                       uint8_t b3 = rowPtr[3];
                       uint8_t b4 = rowPtr[4];

                       // Estrai 4 pixel da 10 bit
                       dstRowPtr[x] = (static_cast<uint16_t>(b0) << 2) | (b1 >> 6);
                       dstRowPtr[x + 1] = ((static_cast<uint16_t>(b1) & 0x3F) << 4) | (b2 >> 4);
                       dstRowPtr[x + 2] = ((static_cast<uint16_t>(b2) & 0x0F) << 6) | (b3 >> 2);
                       dstRowPtr[x + 3] = ((static_cast<uint16_t>(b3) & 0x03) << 8) | b4;

                       rowPtr += 5;
                       x += 4;
                    }
                    else {
                       // Gestione pixel rimanenti
                       while (x < width && rowPtr < srcPtr + (y + 1) * stride) {
                          // Pixel singolo - assumiamo padding
                          dstRowPtr[x] = static_cast<uint16_t>(*rowPtr) << 2;
                          rowPtr++;
                          x++;
                       }
                    }
                 }
              }

              cvType = CV_16UC1;
              resultMat = cv::Mat(height, width, cvType, unpackedBuffer.get()).clone();
              break;
           }

           case PixelFormat::Mono12Packed:
           case PixelFormat::BayerGR12Packed:
           case PixelFormat::BayerRG12Packed:
           case PixelFormat::BayerGB12Packed:
           case PixelFormat::BayerBG12Packed:
           {
              // Mono12Packed: 2 pixel in 3 byte
              size_t unpackedSize = width * height * sizeof(uint16_t);
              std::unique_ptr<uint16_t[]> unpackedBuffer(new uint16_t[width * height]);

              const uint8_t* srcPtr = buffer.get();
              uint16_t* dstPtr = unpackedBuffer.get();

              // Decompressione: ogni gruppo di 3 byte contiene 2 pixel da 12 bit
              for (size_t y = 0; y < height; ++y) {
                 const uint8_t* rowPtr = srcPtr + y * stride;
                 uint16_t* dstRowPtr = dstPtr + y * width;

                 size_t x = 0;
                 while (x < width) {
                    if (x + 1 < width) {
                       // Gruppo completo di 2 pixel
                       uint8_t b0 = rowPtr[0];
                       uint8_t b1 = rowPtr[1];
                       uint8_t b2 = rowPtr[2];

                       // Estrai 2 pixel da 12 bit
                       dstRowPtr[x] = (static_cast<uint16_t>(b0) << 4) | (b1 >> 4);
                       dstRowPtr[x + 1] = ((static_cast<uint16_t>(b1) & 0x0F) << 8) | b2;

                       rowPtr += 3;
                       x += 2;
                    }
                    else {
                       // Ultimo pixel dispari
                       if (x < width && rowPtr + 1 < srcPtr + (y + 1) * stride) {
                          uint8_t b0 = rowPtr[0];
                          uint8_t b1 = rowPtr[1];
                          dstRowPtr[x] = (static_cast<uint16_t>(b0) << 4) | (b1 >> 4);
                          x++;
                       }
                    }
                 }
              }

              cvType = CV_16UC1;
              resultMat = cv::Mat(height, width, cvType, unpackedBuffer.get()).clone();
              break;
           }

              // Formati RGB/BGR 10/12/16 bit
           case PixelFormat::RGB10:
           case PixelFormat::BGR10:
           case PixelFormat::RGB12:
           case PixelFormat::BGR12:
           case PixelFormat::RGB16:
           case PixelFormat::BGR16:
              cvType = CV_16UC3;
              resultMat = cv::Mat(height, width, cvType, buffer.get(), stride);
              break;

              // Altri formati
           case PixelFormat::Confidence8:
           case PixelFormat::Confidence16:
              cvType = (pixelFormat == PixelFormat::Confidence8) ? CV_8UC1 : CV_16UC1;
              resultMat = cv::Mat(height, width, cvType, buffer.get(), stride);
              break;

           case PixelFormat::Coord3D_ABC32f:
              cvType = CV_32FC3;
              resultMat = cv::Mat(height, width, cvType, buffer.get(), stride);
              break;

           case PixelFormat::Coord3D_ABC16:
              cvType = CV_16UC3;
              resultMat = cv::Mat(height, width, cvType, buffer.get(), stride);
              break;

           default:
              // Formato non supportato
              return cv::Mat();
           }

           return resultMat;
        }

        /**
         * @brief Crea una copia deep dell'immagine come cv::Mat
         * @return cv::Mat con copia dei dati
         */
        cv::Mat toCvMatCopy() const {
            cv::Mat mat = toCvMat();
            return mat.clone();
        }
    };
} // namespace GenICamWrapper#pragma once
