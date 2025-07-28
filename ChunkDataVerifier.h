#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <sstream>
#include <iomanip>
#include "ChunkDataManager.h"
#include "GenICamCamera.h"
#include "opencv2/opencv.hpp"

namespace GenICamWrapper {

   /**
    * @brief Risultato della verifica di un singolo chunk
    */
   struct ChunkVerificationResult {
      std::string chunkName;
      bool isEnabled;
      bool hasData;
      bool isValid;
      std::string actualValue;
      std::string expectedValue;
      std::string errorMessage;
      double verificationTime; // ms
   };

   /**
    * @brief Report completo della verifica chunk data
    */
   struct ChunkVerificationReport {
      bool overallSuccess;
      int totalChunks;
      int enabledChunks;
      int verifiedChunks;
      int failedChunks;
      double totalVerificationTime; // ms
      std::vector<ChunkVerificationResult> results;
      std::vector<std::string> warnings;
      std::vector<std::string> errors;
      std::string timestamp;
   };

   /**
    * @brief Classe helper per verificare il funzionamento dei chunk data
    */
   class ChunkDataVerifier {
   public:
      ChunkDataVerifier(GenICamCamera* camera, ChunkDataManager* chunkManager);
      ~ChunkDataVerifier();

      /**
       * @brief Esegue una verifica completa dei chunk data
       * @param enableStandardChunks Se true, abilita automaticamente i chunk standard SFNC
       * @param captureFrames Numero di frame da catturare per la verifica
       * @return Report dettagliato della verifica
       */
      ChunkVerificationReport VerifyChunkDataFunctionality(bool enableStandardChunks = true,
         int captureFrames = 5);

      /**
       * @brief Verifica un singolo chunk
       * @param chunkName Nome del chunk da verificare
       * @return Risultato della verifica
       */
      ChunkVerificationResult VerifySingleChunk(const std::string& chunkName);

      /**
       * @brief Verifica la consistenza dei chunk data su più frame
       * @param frameCount Numero di frame da verificare
       * @return true se i dati sono consistenti
       */
      bool VerifyChunkDataConsistency(int frameCount = 10);

      /**
       * @brief Verifica le performance del parsing dei chunk data
       * @param iterations Numero di iterazioni per il test
       * @return Tempo medio di parsing in millisecondi
       */
      double BenchmarkChunkParsing(int iterations = 100);

      /**
       * @brief Salva il report di verifica su file
       * @param report Report da salvare
       * @param filename Nome del file (default: chunk_verification_report_[timestamp].txt)
       */
      void SaveReportToFile(const ChunkVerificationReport& report,
         const std::string& filename = "");

      /**
       * @brief Stampa il report di verifica sulla console
       * @param report Report da stampare
       */
      void PrintReport(const ChunkVerificationReport& report);

      /**
       * @brief Verifica la sincronizzazione tra chunk data e metadati immagine
       * @return true se sincronizzati correttamente
       */
      bool VerifyChunkImageSynchronization();

      /**
       * @brief Test stress per chunk data con acquisizione continua
       * @param durationSeconds Durata del test in secondi
       * @return Numero di errori rilevati
       */
      int StressTestChunkData(int durationSeconds = 30);

      /**
       * @brief Verifica compatibilità chunk data con trigger hardware
       * @return true se compatibile
       */
      bool VerifyChunkDataWithHardwareTrigger();

   private:
      GenICamCamera* m_pCamera;
      ChunkDataManager* m_pChunkManager;

      // Metodi di supporto interni
      bool CompareChunkWithCameraParameter(const std::string& chunkName,
         const ChunkData& data,
         ChunkVerificationResult& result);

      std::string GetCurrentTimestamp() const;

      bool VerifyTimestampChunk(const ChunkData& data, ChunkVerificationResult& result);
      bool VerifyFrameIDChunk(const ChunkData& data, ChunkVerificationResult& result);
      bool VerifyExposureTimeChunk(const ChunkData& data, ChunkVerificationResult& result);
      bool VerifyGainChunk(const ChunkData& data, ChunkVerificationResult& result);

      double GetElapsedTimeMs(const std::chrono::high_resolution_clock::time_point& start) const;

      std::string FormatChunkValue(const ChunkData& data, const std::string& chunkName) const;

      void LogError(ChunkVerificationReport& report, const std::string& error);
      void LogWarning(ChunkVerificationReport& report, const std::string& warning);
   };

} // namespace GenICamWrapper
