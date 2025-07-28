#pragma once

#include <exception>
#include <string>
#include <sstream>
#include "GenICamCamera.h"

namespace GenICamWrapper {

   enum class ErrorType {
      GenApiError,        // Errori relativi a GenApi
      GenTLError,         // Errori relativi a GenTL
      ConnectionError,    // Errori di connessione
      AcquisitionError,   // Errori durante l'acquisizione
      ParameterError,     // Errori nei parametri
      TimeoutError,       // Errori di timeout
      BufferError,        // Errori relativi ai buffer
      InvalidOperation,   // Operazione non valida nel contesto corrente
		ChunkDataError,   // Errori relativi ai chunk data
      Unknown             // Errore sconosciuto
   };

   class GenICamException : public std::exception {
   private:
      ErrorType m_type;
      std::string m_message;
      GenTL::GC_ERROR m_errorCode;
      std::string m_fullMessage;

   public:
      /**
       * @brief Costruttore con tipo errore e messaggio
       */
      GenICamException(ErrorType type, const std::string& message, GenTL::GC_ERROR errorCode = GenTL::GC_ERR_SUCCESS)
         : m_type(type), m_message(message), m_errorCode(errorCode) {

         std::stringstream ss;
         ss << "[" << errorTypeToString(type) << "] " << message;

         if (errorCode != GenTL::GC_ERR_SUCCESS) {
            ss << " (GenTL Error: " << errorCode << " - "
               << getGenTLErrorString(errorCode) << ")";
         }

         m_fullMessage = ss.str();
      }

      /**
       * @brief Ritorna il messaggio di errore completo
       */
      const char* what() const noexcept override {
         return m_fullMessage.c_str();
      }

      /**
       * @brief Ritorna il tipo di errore
       */
      ErrorType getType() const { return m_type; }

      /**
       * @brief Ritorna il codice di errore GenTL
       */
      GenTL::GC_ERROR getErrorCode() const { return m_errorCode; }

      /**
       * @brief Converte il codice di errore GenTL in stringa descrittiva
       */
      static std::string getGenTLErrorString(GenTL::GC_ERROR error) {
         switch (error) {
         case GenTL::GC_ERR_SUCCESS: return "Success";
         case GenTL::GC_ERR_ERROR: return "Generic error";
         case GenTL::GC_ERR_NOT_INITIALIZED: return "Not initialized";
         case GenTL::GC_ERR_NOT_IMPLEMENTED: return "Not implemented";
         case GenTL::GC_ERR_RESOURCE_IN_USE: return "Resource in use";
         case GenTL::GC_ERR_ACCESS_DENIED: return "Access denied";
         case GenTL::GC_ERR_INVALID_HANDLE: return "Invalid handle";
         case GenTL::GC_ERR_INVALID_ID: return "Invalid ID";
         case GenTL::GC_ERR_NO_DATA: return "No data available";
         case GenTL::GC_ERR_INVALID_PARAMETER: return "Invalid parameter";
         case GenTL::GC_ERR_IO: return "I/O error";
         case GenTL::GC_ERR_TIMEOUT: return "Timeout";
         case GenTL::GC_ERR_ABORT: return "Operation aborted";
         case GenTL::GC_ERR_INVALID_BUFFER: return "Invalid buffer";
         case GenTL::GC_ERR_NOT_AVAILABLE: return "Not available";
         case GenTL::GC_ERR_INVALID_ADDRESS: return "Invalid address";
         case GenTL::GC_ERR_BUFFER_TOO_SMALL: return "Buffer too small";
         case GenTL::GC_ERR_INVALID_INDEX: return "Invalid index";
         case GenTL::GC_ERR_PARSING_CHUNK_DATA: return "Error parsing chunk data";
         case GenTL::GC_ERR_INVALID_VALUE: return "Invalid value";
         case GenTL::GC_ERR_RESOURCE_EXHAUSTED: return "Resource exhausted";
         case GenTL::GC_ERR_OUT_OF_MEMORY: return "Out of memory";
         case GenTL::GC_ERR_BUSY: return "Resource busy";
         case GenTL::GC_ERR_AMBIGUOUS: return "Ambiguous";
         default: return "Unknown error";
         }
      }

   private:
      /**
       * @brief Converte ErrorType in stringa descrittiva
       */
      static std::string errorTypeToString(ErrorType type) {
         switch (type) {
         case ErrorType::GenApiError: return "GenApi Error";
         case ErrorType::GenTLError: return "GenTL Error";
         case ErrorType::ConnectionError: return "Connection Error";
         case ErrorType::AcquisitionError: return "Acquisition Error";
         case ErrorType::ParameterError: return "Parameter Error";
         case ErrorType::TimeoutError: return "Timeout Error";
         case ErrorType::BufferError: return "Buffer Error";
         case ErrorType::InvalidOperation: return "Invalid Operation";
         case ErrorType::ChunkDataError: return "Chunk Data Error";
         case ErrorType::Unknown: return "Unknown Error";
         default: return "Unknown Error";
         }
      }
   };

    // Macro helper per lanciare eccezioni
#define THROW_GENICAM_ERROR(type, msg) \
        throw GenICamException(type, msg)

#define THROW_GENICAM_ERROR_CODE(type, msg, code) \
        throw GenICamException(type, msg, code)

} // namespace GenICamWrapper