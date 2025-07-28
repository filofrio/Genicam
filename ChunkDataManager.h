#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include "GenICamCamera.h"
#include "GenICamException.h"

namespace GenICamWrapper {

   // Struttura per rappresentare un singolo chunk
   struct ChunkInfo {
      std::string name;
      std::string displayName;
      GenApi::EInterfaceType type;
      bool isEnabled;
      size_t offset;
      size_t size;
   };

   // Struttura per i dati estratti dai chunk
   struct ChunkData {
      std::map<std::string, int64_t> integerValues;
      std::map<std::string, double> floatValues;
      std::map<std::string, std::string> stringValues;
      std::map<std::string, bool> booleanValues;
      uint64_t timestamp;
      uint64_t frameID;
   };

   class ChunkDataManager {
   public:
      ChunkDataManager();
      ~ChunkDataManager();

      // Inizializza il manager con il nodemap del dispositivo
      void Initialize(GenApi::INodeMap* deviceNodeMap, GenApi::INodeMap* streamNodeMap);

      // Abilita/disabilita la modalità chunk
      void EnableChunkMode(bool enable);
      bool IsChunkModeEnabled() const;

      // Gestione dei singoli chunk
      void EnableChunk(const std::string& chunkName, bool enable);
      bool IsChunkEnabled(const std::string& chunkName) const;
      std::vector<std::string> GetAvailableChunks() const;

      // Abilita chunk standard SFNC
      void EnableStandardChunks();
      void EnableTimestampChunk(bool enable);
      void EnableFrameIDChunk(bool enable);
      void EnableExposureTimeChunk(bool enable);
      void EnableGainChunk(bool enable);
      void EnableLineStatusAllChunk(bool enable);
      void EnableCounterValueChunk(bool enable);
      void EnableSequencerSetActiveChunk(bool enable);

      // Parsing dei chunk data dal buffer
      ChunkData ParseChunkData(const void* buffer, size_t bufferSize, size_t payloadSize);

      // Estrae informazioni specifiche
      bool GetChunkTimestamp(const ChunkData& data, uint64_t& timestamp) const;
      bool GetChunkFrameID(const ChunkData& data, uint64_t& frameID) const;
      bool GetChunkExposureTime(const ChunkData& data, double& exposureTime) const;
      bool GetChunkGain(const ChunkData& data, double& gain) const;

      // Utility
      void RefreshChunkInfo();
      void PrintChunkInfo() const;

   private:
      GenApi::INodeMap* m_pDeviceNodeMap;
      GenApi::INodeMap* m_pStreamNodeMap;
      std::map<std::string, ChunkInfo> m_chunkInfoMap;
      bool m_chunkModeEnabled;

      // Nodi principali per la gestione chunk
      GenApi::CEnumerationPtr m_pChunkModeActive;
      GenApi::CEnumerationPtr m_pChunkSelector;
      GenApi::CBooleanPtr m_pChunkEnable;

      // Metodi interni
      void ValidateInitialization() const;
      void CollectAvailableChunks();
      GenApi::INode* GetChunkNode(const std::string& chunkName) const;
      void ExtractChunkValue(GenApi::INode* pNode, ChunkData& data) const;
   };

} // namespace GenICamWrapper