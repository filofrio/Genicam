#include "ChunkDataManager.h"
#include <iostream>
#include <sstream>
#include <iomanip>

using namespace GenApi;
using namespace GENICAM_NAMESPACE;

namespace GenICamWrapper {

   ChunkDataManager::ChunkDataManager()
      : m_pDeviceNodeMap(nullptr)
      , m_pStreamNodeMap(nullptr)
      , m_chunkModeEnabled(false)
      , m_pChunkModeActive(nullptr)
      , m_pChunkSelector(nullptr)
      , m_pChunkEnable(nullptr) {
   }

   ChunkDataManager::~ChunkDataManager() {
      try {
         if (m_chunkModeEnabled) {
            EnableChunkMode(false);
         }
      }
      catch (...) {
         // Ignora eccezioni nel distruttore
      }
   }

   void ChunkDataManager::Initialize(INodeMap* deviceNodeMap, INodeMap* streamNodeMap) {
      if (!deviceNodeMap) {
         throw GenICamException(ErrorType::ChunkDataError,
            "Device NodeMap nullo durante inizializzazione ChunkDataManager");
      }

      m_pDeviceNodeMap = deviceNodeMap;
      m_pStreamNodeMap = streamNodeMap;

      try {
         // Ottieni i nodi principali per la gestione chunk
         m_pChunkModeActive = m_pDeviceNodeMap->GetNode("ChunkModeActive");
         if (!m_pChunkModeActive.IsValid()) {
            throw GenICamException(ErrorType::ChunkDataError,
               "Nodo ChunkModeActive non trovato - dispositivo potrebbe non supportare chunk data");
         }

         m_pChunkSelector = m_pDeviceNodeMap->GetNode("ChunkSelector");
         m_pChunkEnable = m_pDeviceNodeMap->GetNode("ChunkEnable");

         if (!m_pChunkSelector.IsValid() || !m_pChunkEnable.IsValid()) {
            throw GenICamException(ErrorType::ChunkDataError,
               "Nodi ChunkSelector o ChunkEnable non trovati");
         }

         // Raccogli informazioni sui chunk disponibili
         CollectAvailableChunks();
      }
      catch (const GenericException& e) {
         throw GenICamException(ErrorType::ChunkDataError,
            std::string("Errore durante inizializzazione chunk manager: ") + e.what());
      }
   }

   void ChunkDataManager::EnableChunkMode(bool enable) {
      ValidateInitialization();

      try {
         if (enable) {
            m_pChunkModeActive->SetIntValue(1); // Attiva chunk mode
            std::cout << "Modalità Chunk Data attivata" << std::endl;
         }
         else {
            m_pChunkModeActive->SetIntValue(0); // Disattiva chunk mode
            std::cout << "Modalità Chunk Data disattivata" << std::endl;
         }
         m_chunkModeEnabled = enable;
      }
      catch (const GenericException& e) {
         throw GenICamException(ErrorType::ChunkDataError,
            std::string("Errore durante ") + (enable ? "attivazione" : "disattivazione") +
            " chunk mode: " + e.what());
      }
   }

   bool ChunkDataManager::IsChunkModeEnabled() const {
      if (!m_pChunkModeActive.IsValid()) {
         return false;
      }

      try {
         return m_pChunkModeActive->GetIntValue() == 1;
      }
      catch (...) {
         return false;
      }
   }

   void ChunkDataManager::EnableChunk(const std::string& chunkName, bool enable) {
      ValidateInitialization();

      try {
         // Seleziona il chunk
         CEnumEntryPtr pEntry = m_pChunkSelector->GetEntryByName(chunkName.c_str());
         if (!pEntry.IsValid()) {
            throw GenICamException(ErrorType::ChunkDataError,
               "Chunk '" + chunkName + "' non disponibile");
         }

         m_pChunkSelector->SetIntValue(pEntry->GetValue());

         // Abilita/disabilita il chunk
         m_pChunkEnable->SetValue(enable);

         // Aggiorna lo stato nel map
         if (m_chunkInfoMap.find(chunkName) != m_chunkInfoMap.end()) {
            m_chunkInfoMap[chunkName].isEnabled = enable;
         }

         std::cout << "Chunk '" << chunkName << "' "
            << (enable ? "abilitato" : "disabilitato") << std::endl;
      }
      catch (const GenericException& e) {
         throw GenICamException(ErrorType::ChunkDataError,
            "Errore durante abilitazione chunk '" + chunkName + "': " + e.what());
      }
   }

   bool ChunkDataManager::IsChunkEnabled(const std::string& chunkName) const {
      auto it = m_chunkInfoMap.find(chunkName);
      if (it != m_chunkInfoMap.end()) {
         return it->second.isEnabled;
      }
      return false;
   }

   std::vector<std::string> ChunkDataManager::GetAvailableChunks() const {
      std::vector<std::string> chunks;
      for (const auto& pair : m_chunkInfoMap) {
         chunks.push_back(pair.first);
      }
      return chunks;
   }

   void ChunkDataManager::EnableStandardChunks() {
      // Abilita i chunk standard più comuni secondo SFNC
      EnableTimestampChunk(true);
      EnableFrameIDChunk(true);
      EnableExposureTimeChunk(true);
      EnableGainChunk(true);
   }

   void ChunkDataManager::EnableTimestampChunk(bool enable) {
      try {
         EnableChunk("Timestamp", enable);
      }
      catch (...) {
         // Se non disponibile, prova con nome alternativo
         try {
            EnableChunk("ChunkTimestamp", enable);
         }
         catch (const GenICamException&) {
            std::cout << "Chunk Timestamp non disponibile su questo dispositivo" << std::endl;
         }
      }
   }

   void ChunkDataManager::EnableFrameIDChunk(bool enable) {
      try {
         EnableChunk("FrameID", enable);
      }
      catch (...) {
         try {
            EnableChunk("ChunkFrameID", enable);
         }
         catch (const GenICamException&) {
            std::cout << "Chunk FrameID non disponibile su questo dispositivo" << std::endl;
         }
      }
   }

   void ChunkDataManager::EnableExposureTimeChunk(bool enable) {
      try {
         EnableChunk("ExposureTime", enable);
      }
      catch (...) {
         try {
            EnableChunk("ChunkExposureTime", enable);
         }
         catch (const GenICamException&) {
            std::cout << "Chunk ExposureTime non disponibile su questo dispositivo" << std::endl;
         }
      }
   }

   void ChunkDataManager::EnableGainChunk(bool enable) {
      try {
         EnableChunk("Gain", enable);
      }
      catch (...) {
         try {
            EnableChunk("ChunkGain", enable);
         }
         catch (const GenICamException&) {
            std::cout << "Chunk Gain non disponibile su questo dispositivo" << std::endl;
         }
      }
   }

   void ChunkDataManager::EnableLineStatusAllChunk(bool enable) {
      try {
         EnableChunk("LineStatusAll", enable);
      }
      catch (const GenICamException&) {
         std::cout << "Chunk LineStatusAll non disponibile su questo dispositivo" << std::endl;
      }
   }

   void ChunkDataManager::EnableCounterValueChunk(bool enable) {
      try {
         EnableChunk("CounterValue", enable);
      }
      catch (const GenICamException&) {
         std::cout << "Chunk CounterValue non disponibile su questo dispositivo" << std::endl;
      }
   }

   void ChunkDataManager::EnableSequencerSetActiveChunk(bool enable) {
      try {
         EnableChunk("SequencerSetActive", enable);
      }
      catch (const GenICamException&) {
         std::cout << "Chunk SequencerSetActive non disponibile su questo dispositivo" << std::endl;
      }
   }

   ChunkData ChunkDataManager::ParseChunkData(const void* buffer, size_t bufferSize, size_t payloadSize) {
      ChunkData data;

      if (!buffer || bufferSize == 0) {
         throw GenICamException(ErrorType::ChunkDataError, "Buffer chunk data non valido");
      }

      if (!m_pStreamNodeMap) {
         throw GenICamException(ErrorType::ChunkDataError,
            "Stream NodeMap non disponibile per parsing chunk data");
      }

      try {
         // Allega il buffer al parser chunk dello stream
         CPortPtr pPort = m_pStreamNodeMap->GetNode("ChunkPort");
         if (pPort.IsValid()) {
            // Il chunk parser utilizza il buffer dopo i dati immagine
            const uint8_t* chunkStart = static_cast<const uint8_t*>(buffer) + payloadSize;
            size_t chunkSize = bufferSize - payloadSize;

            // Qui dovremmo impostare il buffer per il parsing
            // ma l'implementazione esatta dipende dal GenTL producer specifico
         }

         // Estrai i valori dai chunk abilitati
         for (const auto& chunkPair : m_chunkInfoMap) {
            if (chunkPair.second.isEnabled) {
               INode* pNode = GetChunkNode(chunkPair.first);
               if (pNode && IsReadable(pNode)) {
                  ExtractChunkValue(pNode, data);
               }
            }
         }

         // Estrai timestamp e frame ID se disponibili
         CIntegerPtr pTimestamp = m_pStreamNodeMap->GetNode("ChunkTimestamp");
         if (pTimestamp.IsValid() && IsReadable(pTimestamp)) {
            data.timestamp = pTimestamp->GetValue();
         }

         CIntegerPtr pFrameID = m_pStreamNodeMap->GetNode("ChunkFrameID");
         if (pFrameID.IsValid() && IsReadable(pFrameID)) {
            data.frameID = pFrameID->GetValue();
         }
      }
      catch (const GenericException& e) {
         throw GenICamException(ErrorType::ChunkDataError,
            std::string("Errore durante parsing chunk data: ") + e.what());
      }

      return data;
   }

   bool ChunkDataManager::GetChunkTimestamp(const ChunkData& data, uint64_t& timestamp) const {
      if (data.timestamp != 0) {
         timestamp = data.timestamp;
         return true;
      }

      auto it = data.integerValues.find("Timestamp");
      if (it != data.integerValues.end()) {
         timestamp = static_cast<uint64_t>(it->second);
         return true;
      }

      it = data.integerValues.find("ChunkTimestamp");
      if (it != data.integerValues.end()) {
         timestamp = static_cast<uint64_t>(it->second);
         return true;
      }

      return false;
   }

   bool ChunkDataManager::GetChunkFrameID(const ChunkData& data, uint64_t& frameID) const {
      if (data.frameID != 0) {
         frameID = data.frameID;
         return true;
      }

      auto it = data.integerValues.find("FrameID");
      if (it != data.integerValues.end()) {
         frameID = static_cast<uint64_t>(it->second);
         return true;
      }

      it = data.integerValues.find("ChunkFrameID");
      if (it != data.integerValues.end()) {
         frameID = static_cast<uint64_t>(it->second);
         return true;
      }

      return false;
   }

   bool ChunkDataManager::GetChunkExposureTime(const ChunkData& data, double& exposureTime) const {
      auto it = data.floatValues.find("ExposureTime");
      if (it != data.floatValues.end()) {
         exposureTime = it->second;
         return true;
      }

      it = data.floatValues.find("ChunkExposureTime");
      if (it != data.floatValues.end()) {
         exposureTime = it->second;
         return true;
      }

      return false;
   }

   bool ChunkDataManager::GetChunkGain(const ChunkData& data, double& gain) const {
      auto it = data.floatValues.find("Gain");
      if (it != data.floatValues.end()) {
         gain = it->second;
         return true;
      }

      it = data.floatValues.find("ChunkGain");
      if (it != data.floatValues.end()) {
         gain = it->second;
         return true;
      }

      return false;
   }

   void ChunkDataManager::RefreshChunkInfo() {
      m_chunkInfoMap.clear();
      CollectAvailableChunks();
   }

   void ChunkDataManager::PrintChunkInfo() const {
      std::cout << "\n=== Informazioni Chunk Data ===" << std::endl;
      std::cout << "Modalità Chunk: " << (m_chunkModeEnabled ? "Attiva" : "Inattiva") << std::endl;
      std::cout << "Chunk disponibili: " << m_chunkInfoMap.size() << std::endl;

      for (const auto& pair : m_chunkInfoMap) {
         const ChunkInfo& info = pair.second;
         std::cout << "\n- " << info.name << " (" << info.displayName << ")" << std::endl;
         std::cout << "  Tipo: ";
         switch (info.type) {
         case intfIInteger: std::cout << "Integer"; break;
         case intfIFloat: std::cout << "Float"; break;
         case intfIString: std::cout << "String"; break;
         case intfIBoolean: std::cout << "Boolean"; break;
         default: std::cout << "Altro"; break;
         }
         std::cout << std::endl;
         std::cout << "  Abilitato: " << (info.isEnabled ? "Sì" : "No") << std::endl;
      }
   }

   void ChunkDataManager::ValidateInitialization() const {
      if (!m_pDeviceNodeMap) {
         throw GenICamException(ErrorType::ChunkDataError,
            "ChunkDataManager non inizializzato");
      }
   }

   void ChunkDataManager::CollectAvailableChunks() {
      if (!m_pChunkSelector.IsValid()) {
         return;
      }

      try {
         // Salva il valore corrente del selector
         int64_t currentValue = m_pChunkSelector->GetIntValue();

         // Itera attraverso tutti i chunk disponibili
         StringList_t entries;
         m_pChunkSelector->GetSymbolics(entries);

         for (size_t i = 0; i < entries.size(); ++i) {
            try {
               // Seleziona il chunk
               CEnumEntryPtr pEntry = m_pChunkSelector->GetEntryByName(entries[i].c_str());
               if (!pEntry.IsValid() || !IsAvailable(pEntry)) {
                  continue;
               }

               m_pChunkSelector->SetIntValue(pEntry->GetValue());

               // Crea info chunk
               ChunkInfo info;
               info.name = entries[i];
               info.displayName = pEntry->GetSymbolic(); 
               info.isEnabled = m_pChunkEnable.IsValid() ? m_pChunkEnable->GetValue() : false;

               // Determina il tipo del chunk data
               std::string chunkValueName = "Chunk" + info.name;
               INode* pChunkValue = m_pDeviceNodeMap->GetNode(chunkValueName.c_str());
               if (pChunkValue) {
                  info.type = pChunkValue->GetPrincipalInterfaceType();
               }

               m_chunkInfoMap[info.name] = info;
            }
            catch (...) {
               // Ignora chunk non accessibili
            }
         }

         // Ripristina il valore originale
         m_pChunkSelector->SetIntValue(currentValue);
      }
      catch (const GenericException& e) {
         std::cerr << "Errore durante raccolta informazioni chunk: " << e.what() << std::endl;
      }
   }

   INode* ChunkDataManager::GetChunkNode(const std::string& chunkName) const {
      if (!m_pDeviceNodeMap) {
         return nullptr;
      }

      // Prova prima con il nome diretto
      INode* pNode = m_pDeviceNodeMap->GetNode(chunkName.c_str());
      if (pNode) {
         return pNode;
      }

      // Prova con prefisso "Chunk"
      std::string chunkValueName = "Chunk" + chunkName;
      return m_pDeviceNodeMap->GetNode(chunkValueName.c_str());
   }

   void ChunkDataManager::ExtractChunkValue(INode* pNode, ChunkData& data) const {
      if (!pNode || !IsReadable(pNode)) {
         return;
      }

      std::string nodeName = pNode->GetName().c_str();

      try {
         switch (pNode->GetPrincipalInterfaceType()) {
         case intfIInteger: {
            CIntegerPtr pInt = pNode;
            if (pInt.IsValid()) {
               data.integerValues[nodeName] = pInt->GetValue();
            }
            break;
         }
         case intfIFloat: {
            CFloatPtr pFloat = pNode;
            if (pFloat.IsValid()) {
               data.floatValues[nodeName] = pFloat->GetValue();
            }
            break;
         }
         case intfIString: {
            CStringPtr pString = pNode;
            if (pString.IsValid()) {
               data.stringValues[nodeName] = pString->GetValue().c_str();
            }
            break;
         }
         case intfIBoolean: {
            CBooleanPtr pBool = pNode;
            if (pBool.IsValid()) {
               data.booleanValues[nodeName] = pBool->GetValue();
            }
            break;
         }
         default:
            // Tipo non supportato
            break;
         }
      }
      catch (const GenericException& e) {
         std::cerr << "Errore estrazione valore chunk '" << nodeName << "': "
            << e.what() << std::endl;
      }
   }

} // namespace GenICamWrapper