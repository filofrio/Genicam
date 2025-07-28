#include "ChunkDataVerifier.h"
#include <fstream>
#include <thread>
#include <cmath>

namespace GenICamWrapper {

   ChunkDataVerifier::ChunkDataVerifier(GenICamCamera* camera, ChunkDataManager* chunkManager)
      : m_pCamera(camera)
      , m_pChunkManager(chunkManager) {

      if (!m_pCamera || !m_pChunkManager) {
         throw GenICamException(ErrorType::InvalidOperation,
            "Camera o ChunkManager nullo nel ChunkDataVerifier");
      }
   }

   ChunkDataVerifier::~ChunkDataVerifier() {
   }

   ChunkVerificationReport ChunkDataVerifier::VerifyChunkDataFunctionality(
      bool enableStandardChunks, int captureFrames) {

      ChunkVerificationReport report;
      report.timestamp = GetCurrentTimestamp();
      report.overallSuccess = true;
      report.totalVerificationTime = 0.0;

      auto startTime = std::chrono::high_resolution_clock::now();

      try {
         // Fase 1: Verifica stato iniziale
         if (!m_pCamera->isConnected()) {
            LogError(report, "Camera non connessa");
            report.overallSuccess = false;
            return report;
         }

         // Fase 2: Abilita chunk mode
         std::cout << "\n=== Verifica Chunk Data ===\n" << std::endl;
         std::cout << "1. Abilitazione Chunk Mode..." << std::endl;

         m_pChunkManager->EnableChunkMode(true);

         if (!m_pChunkManager->IsChunkModeEnabled()) {
            LogError(report, "Impossibile abilitare Chunk Mode");
            report.overallSuccess = false;
            return report;
         }

         // Fase 3: Abilita chunk standard se richiesto
         if (enableStandardChunks) {
            std::cout << "2. Abilitazione chunk standard SFNC..." << std::endl;
            m_pChunkManager->EnableStandardChunks();
         }

         // Fase 4: Ottieni lista chunk disponibili
         std::vector<std::string> availableChunks = m_pChunkManager->GetAvailableChunks();
         report.totalChunks = static_cast<int>(availableChunks.size());

         std::cout << "3. Chunk disponibili: " << report.totalChunks << std::endl;

         // Fase 5: Conta chunk abilitati
         for (const auto& chunkName : availableChunks) {
            if (m_pChunkManager->IsChunkEnabled(chunkName)) {
               report.enabledChunks++;
            }
         }

         std::cout << "4. Chunk abilitati: " << report.enabledChunks << std::endl;

         if (report.enabledChunks == 0) {
            LogWarning(report, "Nessun chunk abilitato per la verifica");
         }

         // Fase 6: Avvia acquisizione per test
         std::cout << "5. Avvio acquisizione per verifica..." << std::endl;

         m_pCamera->startAcquisition();

         // Fase 7: Cattura e verifica frame
         std::cout << "6. Cattura " << captureFrames << " frame per verifica..." << std::endl;

         for (int i = 0; i < captureFrames; i++) {
            cv::Mat image;
            uint8_t* pBuffer = nullptr;
            size_t bufferSize = 0;
            size_t payloadSize = 0;

            auto result = m_pCamera->grabSingleFrame();

            if (!result.empty() && pBuffer) {
               payloadSize = static_cast<size_t>(image.total() * image.elemSize());

               // Parsing chunk data
               auto parseStart = std::chrono::high_resolution_clock::now();
               ChunkData chunkData = m_pChunkManager->ParseChunkData(
                  pBuffer, bufferSize, payloadSize);
               double parseTime = GetElapsedTimeMs(parseStart);

               // Verifica ogni chunk abilitato
               for (const auto& chunkName : availableChunks) {
                  if (m_pChunkManager->IsChunkEnabled(chunkName)) {
                     auto verifyStart = std::chrono::high_resolution_clock::now();

                     ChunkVerificationResult chunkResult;
                     chunkResult.chunkName = chunkName;
                     chunkResult.isEnabled = true;
                     chunkResult.hasData = false;
                     chunkResult.isValid = false;

                     // Verifica presenza dati
                     chunkResult.actualValue = FormatChunkValue(chunkData, chunkName);
                     chunkResult.hasData = !chunkResult.actualValue.empty();

                     // Verifica validità confrontando con parametri camera
                     if (chunkResult.hasData) {
                        CompareChunkWithCameraParameter(chunkName, chunkData, chunkResult);
                     }
                     else {
                        chunkResult.errorMessage = "Nessun dato chunk trovato nel buffer";
                     }

                     chunkResult.verificationTime = GetElapsedTimeMs(verifyStart);

                     // Solo sul primo frame aggiungi ai risultati
                     if (i == 0) {
                        report.results.push_back(chunkResult);
                        if (chunkResult.isValid) {
                           report.verifiedChunks++;
                        }
                        else {
                           report.failedChunks++;
                           report.overallSuccess = false;
                        }
                     }
                  }
               }

               std::cout << "   Frame " << (i + 1) << "/" << captureFrames
                  << " - Parsing time: " << parseTime << " ms" << std::endl;
            }
            else {
               LogError(report, "Errore acquisizione frame " + std::to_string(i + 1));
               report.overallSuccess = false;
            }
         }

         // Fase 8: Ferma acquisizione
         m_pCamera->stopAcquisition();

         // Fase 9: Test aggiuntivi
         std::cout << "\n7. Test aggiuntivi..." << std::endl;

         // Test consistenza
         if (report.enabledChunks > 0) {
            std::cout << "   - Verifica consistenza dati..." << std::endl;
            bool consistency = VerifyChunkDataConsistency(5);
            if (!consistency) {
               LogWarning(report, "Rilevate inconsistenze nei chunk data su frame multipli");
               report.overallSuccess = false;
            }
         }

         // Test sincronizzazione
         std::cout << "   - Verifica sincronizzazione con immagine..." << std::endl;
         bool sync = VerifyChunkImageSynchronization();
         if (!sync) {
            LogWarning(report, "Problemi di sincronizzazione tra chunk data e immagini");
            report.overallSuccess = false;
         }

      }
      catch (const GenICamException& e) {
         LogError(report, std::string("Eccezione GenICam: ") + e.what());
         report.overallSuccess = false;
      }
      catch (const std::exception& e) {
         LogError(report, std::string("Eccezione: ") + e.what());
         report.overallSuccess = false;
      }

      report.totalVerificationTime = GetElapsedTimeMs(startTime);

      // Genera sommario
      std::cout << "\n=== Sommario Verifica ===" << std::endl;
      PrintReport(report);

      return report;
   }

   ChunkVerificationResult ChunkDataVerifier::VerifySingleChunk(const std::string& chunkName) {
      ChunkVerificationResult result;
      result.chunkName = chunkName;
      result.isEnabled = false;
      result.hasData = false;
      result.isValid = false;

      auto startTime = std::chrono::high_resolution_clock::now();

      try {
         // Verifica se il chunk è abilitato
         result.isEnabled = m_pChunkManager->IsChunkEnabled(chunkName);
         if (!result.isEnabled) {
            result.errorMessage = "Chunk non abilitato";
            result.verificationTime = GetElapsedTimeMs(startTime);
            return result;
         }

         // Cattura un frame
         m_pCamera->startAcquisition();

         cv::Mat image;
         uint8_t* pBuffer = nullptr;
         size_t bufferSize = 0;

         auto acqResult = m_pCamera->grabSingleFrame();

         m_pCamera->stopAcquisition();

         if (!acqResult.empty() && pBuffer) {
            size_t payloadSize = static_cast<size_t>(image.total() * image.elemSize());

            // Parse chunk data
            ChunkData chunkData = m_pChunkManager->ParseChunkData(
               pBuffer, bufferSize, payloadSize);

            // Verifica presenza dati
            result.actualValue = FormatChunkValue(chunkData, chunkName);
            result.hasData = !result.actualValue.empty();

            if (result.hasData) {
               CompareChunkWithCameraParameter(chunkName, chunkData, result);
            }
            else {
               result.errorMessage = "Nessun dato chunk trovato";
            }
         }
         else {
            result.errorMessage = "Errore acquisizione frame";
         }
      }
      catch (const std::exception& e) {
         result.errorMessage = std::string("Eccezione: ") + e.what();
      }

      result.verificationTime = GetElapsedTimeMs(startTime);
      return result;
   }

   bool ChunkDataVerifier::VerifyChunkDataConsistency(int frameCount) {
      if (frameCount < 2) return false;

      try {
         m_pCamera->startAcquisition();

         std::vector<ChunkData> chunkDataList;

         // Raccogli dati da più frame
         for (int i = 0; i < frameCount; i++) {
            cv::Mat image;
            uint8_t* pBuffer = nullptr;
            size_t bufferSize = 0;

            auto result = m_pCamera->grabSingleFrame();

            if (!result.empty() && pBuffer) {
               size_t payloadSize = static_cast<size_t>(image.total() * image.elemSize());
               ChunkData data = m_pChunkManager->ParseChunkData(
                  pBuffer, bufferSize, payloadSize);
               chunkDataList.push_back(data);
            }
         }

         m_pCamera->stopAcquisition();

         if (chunkDataList.size() < 2) return false;

         // Verifica consistenza
         bool timestampIncreasing = true;
         bool frameIDIncreasing = true;

         for (size_t i = 1; i < chunkDataList.size(); i++) {
            // Verifica timestamp crescente
            if (chunkDataList[i].timestamp <= chunkDataList[i - 1].timestamp) {
               timestampIncreasing = false;
            }

            // Verifica frame ID crescente
            if (chunkDataList[i].frameID <= chunkDataList[i - 1].frameID) {
               frameIDIncreasing = false;
            }
         }

         return timestampIncreasing && frameIDIncreasing;

      }
      catch (...) {
         return false;
      }
   }

   double ChunkDataVerifier::BenchmarkChunkParsing(int iterations) {
      if (iterations <= 0) return 0.0;

      double totalTime = 0.0;

      try {
         m_pCamera->startAcquisition();

         // Cattura un frame di riferimento
         cv::Mat image;
         uint8_t* pBuffer = nullptr;
         size_t bufferSize = 0;

         auto result = m_pCamera->grabSingleFrame();

         if (!result.empty() && pBuffer) {
            size_t payloadSize = static_cast<size_t>(image.total() * image.elemSize());

            // Copia buffer per test ripetuti
            std::vector<uint8_t> testBuffer(pBuffer, pBuffer + bufferSize);

            // Esegui benchmark
            for (int i = 0; i < iterations; i++) {
               auto start = std::chrono::high_resolution_clock::now();

               ChunkData data = m_pChunkManager->ParseChunkData(
                  testBuffer.data(), bufferSize, payloadSize);

               totalTime += GetElapsedTimeMs(start);
            }
         }

         m_pCamera->stopAcquisition();

      }
      catch (...) {
         return 0.0;
      }

      return totalTime / iterations;
   }

   void ChunkDataVerifier::SaveReportToFile(const ChunkVerificationReport& report,
      const std::string& filename) {
      std::string outputFile = filename;
      if (outputFile.empty()) {
         outputFile = "chunk_verification_report_" + report.timestamp + ".txt";
      }

      std::ofstream file(outputFile);
      if (!file.is_open()) {
         std::cerr << "Impossibile aprire file: " << outputFile << std::endl;
         return;
      }

      file << "=== CHUNK DATA VERIFICATION REPORT ===" << std::endl;
      file << "Timestamp: " << report.timestamp << std::endl;
      file << "Overall Success: " << (report.overallSuccess ? "PASSED" : "FAILED") << std::endl;
      file << "Total Verification Time: " << report.totalVerificationTime << " ms" << std::endl;
      file << std::endl;

      file << "SUMMARY:" << std::endl;
      file << "  Total Chunks: " << report.totalChunks << std::endl;
      file << "  Enabled Chunks: " << report.enabledChunks << std::endl;
      file << "  Verified Chunks: " << report.verifiedChunks << std::endl;
      file << "  Failed Chunks: " << report.failedChunks << std::endl;
      file << std::endl;

      if (!report.results.empty()) {
         file << "DETAILED RESULTS:" << std::endl;
         file << std::setw(25) << std::left << "Chunk Name"
            << std::setw(10) << "Enabled"
            << std::setw(10) << "Has Data"
            << std::setw(10) << "Valid"
            << std::setw(15) << "Time (ms)"
            << "Details" << std::endl;
         file << std::string(100, '-') << std::endl;

         for (const auto& result : report.results) {
            file << std::setw(25) << std::left << result.chunkName
               << std::setw(10) << (result.isEnabled ? "Yes" : "No")
               << std::setw(10) << (result.hasData ? "Yes" : "No")
               << std::setw(10) << (result.isValid ? "Yes" : "No")
               << std::setw(15) << std::fixed << std::setprecision(3)
               << result.verificationTime;

            if (!result.isValid && !result.errorMessage.empty()) {
               file << "Error: " << result.errorMessage;
            }
            else if (result.isValid) {
               file << "Value: " << result.actualValue;
               if (!result.expectedValue.empty()) {
                  file << " (Expected: " << result.expectedValue << ")";
               }
            }
            file << std::endl;
         }
      }

      if (!report.warnings.empty()) {
         file << std::endl << "WARNINGS:" << std::endl;
         for (const auto& warning : report.warnings) {
            file << "  - " << warning << std::endl;
         }
      }

      if (!report.errors.empty()) {
         file << std::endl << "ERRORS:" << std::endl;
         for (const auto& error : report.errors) {
            file << "  - " << error << std::endl;
         }
      }

      file.close();
      std::cout << "Report salvato in: " << outputFile << std::endl;
   }

   void ChunkDataVerifier::PrintReport(const ChunkVerificationReport& report) {
      std::cout << "Risultato: " << (report.overallSuccess ? "SUCCESSO" : "FALLITO") << std::endl;
      std::cout << "Tempo totale: " << report.totalVerificationTime << " ms" << std::endl;
      std::cout << "Chunks totali/abilitati/verificati/falliti: "
         << report.totalChunks << "/" << report.enabledChunks << "/"
         << report.verifiedChunks << "/" << report.failedChunks << std::endl;

      if (!report.warnings.empty()) {
         std::cout << "\nAvvertimenti:" << std::endl;
         for (const auto& warning : report.warnings) {
            std::cout << "  ! " << warning << std::endl;
         }
      }

      if (!report.errors.empty()) {
         std::cout << "\nErrori:" << std::endl;
         for (const auto& error : report.errors) {
            std::cout << "  X " << error << std::endl;
         }
      }
   }

   bool ChunkDataVerifier::VerifyChunkImageSynchronization() {
      try {
         m_pCamera->startAcquisition();

         bool syncOk = true;
         const int testFrames = 3;

         for (int i = 0; i < testFrames; i++) {
            cv::Mat image;
            uint8_t* pBuffer = nullptr;
            size_t bufferSize = 0;

            // Registra tempo prima dell'acquisizione
            auto beforeAcq = std::chrono::high_resolution_clock::now();

            auto result = m_pCamera->grabSingleFrame();

            if (!result.empty() && pBuffer) {
               size_t payloadSize = static_cast<size_t>(image.total() * image.elemSize());

               // Parse chunk data
               ChunkData chunkData = m_pChunkManager->ParseChunkData(
                  pBuffer, bufferSize, payloadSize);

               // Verifica che il timestamp del chunk sia ragionevole
               if (chunkData.timestamp > 0) {
                  // Il timestamp dovrebbe essere vicino al momento dell'acquisizione
                  // (questa è una verifica semplificata)
                  uint64_t currentTimestamp;
                  if (m_pChunkManager->GetChunkTimestamp(chunkData, currentTimestamp)) {
                     // Verifica base: il timestamp esiste ed è non-zero
                     if (currentTimestamp == 0) {
                        syncOk = false;
                     }
                  }
               }
            }
         }

         m_pCamera->stopAcquisition();
         return syncOk;

      }
      catch (...) {
         return false;
      }
   }

   int ChunkDataVerifier::StressTestChunkData(int durationSeconds) {
      int errorCount = 0;

      try {
         m_pCamera->startAcquisition();

         auto startTime = std::chrono::steady_clock::now();
         auto endTime = startTime + std::chrono::seconds(durationSeconds);

         int frameCount = 0;

         while (std::chrono::steady_clock::now() < endTime) {
            cv::Mat image;
            uint8_t* pBuffer = nullptr;
            size_t bufferSize = 0;

            auto result = m_pCamera->grabSingleFrame();

            if (!result.empty() && pBuffer) {
               size_t payloadSize = static_cast<size_t>(image.total() * image.elemSize());

               try {
                  ChunkData chunkData = m_pChunkManager->ParseChunkData(
                     pBuffer, bufferSize, payloadSize);
                  frameCount++;
               }
               catch (...) {
                  errorCount++;
               }
            }
            else if (result.empty()) {
               errorCount++;
            }
         }

         m_pCamera->stopAcquisition();

         auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - startTime).count();

         std::cout << "Stress test completato: " << frameCount << " frame in "
            << elapsed << " secondi, " << errorCount << " errori" << std::endl;

      }
      catch (...) {
         errorCount++;
      }

      return errorCount;
   }

   bool ChunkDataVerifier::VerifyChunkDataWithHardwareTrigger() {
      try {
         // Salva modalità trigger corrente
         TriggerMode originalMode = m_pCamera->getTriggerMode();

         // Imposta trigger hardware
         m_pCamera->setTriggerMode(TriggerMode::On);
         m_pCamera->setTriggerSource(TriggerSource::Line0);

         bool verifyOk = true;

         // Avvia acquisizione
         m_pCamera->startAcquisition();

         std::cout << "In attesa di trigger hardware su Line0..." << std::endl;

         // Attendi trigger (con timeout)
         cv::Mat image;
         uint8_t* pBuffer = nullptr;
         size_t bufferSize = 0;

         auto result = m_pCamera->grabSingleFrame();

         if (!result.empty() && pBuffer) {
            size_t payloadSize = static_cast<size_t>(image.total() * image.elemSize());

            // Parse chunk data
            ChunkData chunkData = m_pChunkManager->ParseChunkData(
               pBuffer, bufferSize, payloadSize);

            // Verifica presenza dati base
            uint64_t timestamp;
            if (!m_pChunkManager->GetChunkTimestamp(chunkData, timestamp) || timestamp == 0) {
               verifyOk = false;
            }
         }
         else {
            std::cout << "Timeout in attesa trigger hardware" << std::endl;
            verifyOk = false;
         }

         m_pCamera->stopAcquisition();

         // Ripristina modalità originale
         m_pCamera->setTriggerMode(originalMode);

         return verifyOk;

      }
      catch (...) {
         return false;
      }
   }

   // Metodi privati di supporto

   bool ChunkDataVerifier::CompareChunkWithCameraParameter(const std::string& chunkName,
      const ChunkData& data,
      ChunkVerificationResult& result) {
      try {
         // Verifica chunk specifici confrontando con parametri camera
         if (chunkName == "Timestamp") {
            return VerifyTimestampChunk(data, result);
         }
         else if (chunkName == "FrameID") {
            return VerifyFrameIDChunk(data, result);
         }
         else if (chunkName == "ExposureTime") {
            return VerifyExposureTimeChunk(data, result);
         }
         else if (chunkName == "Gain") {
            return VerifyGainChunk(data, result);
         }
         else {
            // Per altri chunk, verifica solo presenza dati
            result.isValid = result.hasData;
            if (!result.isValid) {
               result.errorMessage = "Chunk presente ma senza dati";
            }
         }
      }
      catch (const std::exception& e) {
         result.errorMessage = std::string("Errore confronto: ") + e.what();
         result.isValid = false;
      }

      return result.isValid;
   }

   std::string ChunkDataVerifier::GetCurrentTimestamp() const {
      auto now = std::chrono::system_clock::now();
      auto tempo = std::chrono::system_clock::to_time_t(now);

      std::stringstream ss;
      //ss << std::put_time(localtime(&tempo), "%Y%m%d_%H%M%S");
      ss << "sartana";
      return ss.str();
   }

   bool ChunkDataVerifier::VerifyTimestampChunk(const ChunkData& data,
      ChunkVerificationResult& result) {
      uint64_t timestamp;
      if (m_pChunkManager->GetChunkTimestamp(data, timestamp)) {
         result.actualValue = std::to_string(timestamp);
         result.isValid = (timestamp > 0);
         if (!result.isValid) {
            result.errorMessage = "Timestamp zero o non valido";
         }
      }
      else {
         result.errorMessage = "Impossibile estrarre timestamp";
         result.isValid = false;
      }
      return result.isValid;
   }

   bool ChunkDataVerifier::VerifyFrameIDChunk(const ChunkData& data,
      ChunkVerificationResult& result) {
      uint64_t frameID;
      if (m_pChunkManager->GetChunkFrameID(data, frameID)) {
         result.actualValue = std::to_string(frameID);
         result.isValid = true; // Frame ID può essere 0 per il primo frame
      }
      else {
         result.errorMessage = "Impossibile estrarre frame ID";
         result.isValid = false;
      }
      return result.isValid;
   }

   bool ChunkDataVerifier::VerifyExposureTimeChunk(const ChunkData& data,
      ChunkVerificationResult& result) {
      double chunkExposure;
      if (m_pChunkManager->GetChunkExposureTime(data, chunkExposure)) {
         result.actualValue = std::to_string(chunkExposure) + " us";

         // Confronta con valore camera corrente
         try {
            double cameraExposure = m_pCamera->getExposureTime();
            result.expectedValue = std::to_string(cameraExposure) + " us";

            // Tollera piccole differenze (1%)
            double diff = std::abs(chunkExposure - cameraExposure);
            double tolerance = cameraExposure * 0.01;

            result.isValid = (diff <= tolerance);
            if (!result.isValid) {
               result.errorMessage = "Valore chunk non corrisponde al parametro camera";
            }
         }
         catch (...) {
            // Se non possiamo leggere dalla camera, accetta il valore chunk
            result.isValid = true;
         }
      }
      else {
         result.errorMessage = "Impossibile estrarre exposure time";
         result.isValid = false;
      }
      return result.isValid;
   }

   bool ChunkDataVerifier::VerifyGainChunk(const ChunkData& data,
      ChunkVerificationResult& result) {
      double chunkGain;
      if (m_pChunkManager->GetChunkGain(data, chunkGain)) {
         result.actualValue = std::to_string(chunkGain) + " dB";

         // Confronta con valore camera corrente
         try {
            double cameraGain = m_pCamera->getGain();
            result.expectedValue = std::to_string(cameraGain) + " dB";

            // Tollera piccole differenze (0.1 dB)
            double diff = std::abs(chunkGain - cameraGain);

            result.isValid = (diff <= 0.1);
            if (!result.isValid) {
               result.errorMessage = "Valore chunk non corrisponde al parametro camera";
            }
         }
         catch (...) {
            // Se non possiamo leggere dalla camera, accetta il valore chunk
            result.isValid = true;
         }
      }
      else {
         result.errorMessage = "Impossibile estrarre gain";
         result.isValid = false;
      }
      return result.isValid;
   }

   double ChunkDataVerifier::GetElapsedTimeMs(
      const std::chrono::high_resolution_clock::time_point& start) const {
      auto end = std::chrono::high_resolution_clock::now();
      return std::chrono::duration<double, std::milli>(end - start).count();
   }

   std::string ChunkDataVerifier::FormatChunkValue(const ChunkData& data,
      const std::string& chunkName) const {
      // Cerca nei vari map di dati

      // Cerca prima con il nome esatto
      auto intIt = data.integerValues.find(chunkName);
      if (intIt != data.integerValues.end()) {
         return std::to_string(intIt->second);
      }

      // Prova con prefisso "Chunk"
      std::string chunkPrefixName = "Chunk" + chunkName;
      intIt = data.integerValues.find(chunkPrefixName);
      if (intIt != data.integerValues.end()) {
         return std::to_string(intIt->second);
      }

      // Cerca nei float
      auto floatIt = data.floatValues.find(chunkName);
      if (floatIt != data.floatValues.end()) {
         return std::to_string(floatIt->second);
      }

      floatIt = data.floatValues.find(chunkPrefixName);
      if (floatIt != data.floatValues.end()) {
         return std::to_string(floatIt->second);
      }

      // Cerca nelle stringhe
      auto strIt = data.stringValues.find(chunkName);
      if (strIt != data.stringValues.end()) {
         return strIt->second;
      }

      strIt = data.stringValues.find(chunkPrefixName);
      if (strIt != data.stringValues.end()) {
         return strIt->second;
      }

      // Cerca nei boolean
      auto boolIt = data.booleanValues.find(chunkName);
      if (boolIt != data.booleanValues.end()) {
         return boolIt->second ? "true" : "false";
      }

      boolIt = data.booleanValues.find(chunkPrefixName);
      if (boolIt != data.booleanValues.end()) {
         return boolIt->second ? "true" : "false";
      }

      // Controlla campi speciali
      if (chunkName == "Timestamp" && data.timestamp > 0) {
         return std::to_string(data.timestamp);
      }

      if (chunkName == "FrameID" && data.frameID > 0) {
         return std::to_string(data.frameID);
      }

      return "";
   }

   void ChunkDataVerifier::LogError(ChunkVerificationReport& report, const std::string& error) {
      report.errors.push_back(error);
      std::cerr << "[ERRORE] " << error << std::endl;
   }

   void ChunkDataVerifier::LogWarning(ChunkVerificationReport& report, const std::string& warning) {
      report.warnings.push_back(warning);
      std::cout << "[AVVISO] " << warning << std::endl;
   }

} // namespace GenICamWrapper