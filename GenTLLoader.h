#pragma once

#include <string>
#include <vector>
#include <memory>
#include <stdexcept>
#include <GenICam.h>
#include <GenTL/GenTL.h>

#ifdef _WIN32
    #include <windows.h>
    typedef HMODULE LibraryHandle;
#else
    #include <dlfcn.h>
    typedef void* LibraryHandle;
#endif

namespace GenICamWrapper {

    /**
     * @brief Classe per il caricamento dinamico delle librerie GenTL (.cti)
     * 
     * Questa classe gestisce il caricamento dei GenTL Producer (.cti files)
     * e l'inizializzazione di tutti i puntatori alle funzioni GenTL.
     */
    class GenTLLoader {
    public:
        /**
         * @brief Costruttore
         */
        GenTLLoader();

        /**
         * @brief Distruttore - scarica automaticamente la libreria
         */
        ~GenTLLoader();

        /**
         * @brief Carica un GenTL Producer da file .cti
         * @param ctiPath Percorso completo al file .cti
         * @return true se il caricamento ha successo
         */
        bool LoadProducer(const std::string& ctiPath);

        /**
         * @brief Scarica il producer correntemente caricato
         */
        void UnloadProducer();

        /**
         * @brief Verifica se un producer è caricato
         * @return true se un producer è caricato
         */
        bool IsLoaded() const { return m_hModule != nullptr; }

        /**
         * @brief Ottiene il percorso del producer caricato
         * @return Percorso del file .cti
         */
        const std::string& GetProducerPath() const { return m_producerPath; }

        /**
         * @brief Enumera tutti i file .cti disponibili in una directory
         * @param directory Directory da scansionare
         * @return Vector con i percorsi completi dei file .cti trovati
         */
        static std::vector<std::string> EnumerateProducers(const std::string& directory);

        /**
         * @brief Enumera i producer nelle directory standard GenTL
         * @return Vector con i percorsi completi dei file .cti trovati
         */
        static std::vector<std::string> EnumerateProducersInStandardPaths();

        // === Puntatori alle funzioni GenTL ===
        // Tutti i puntatori alle funzioni sono pubblici per accesso diretto
        
        // Funzioni generali
        GenTL::PGCGetInfo               GCGetInfo;
        GenTL::PGCGetLastError          GCGetLastError;
        GenTL::PGCInitLib               GCInitLib;
        GenTL::PGCCloseLib              GCCloseLib;
        
        // Funzioni porta
        GenTL::PGCReadPort              GCReadPort;
        GenTL::PGCWritePort             GCWritePort;
        GenTL::PGCGetPortURL            GCGetPortURL;
        GenTL::PGCGetPortInfo           GCGetPortInfo;
        
        // Funzioni eventi
        GenTL::PGCRegisterEvent         GCRegisterEvent;
        GenTL::PGCUnregisterEvent       GCUnregisterEvent;
        GenTL::PEventGetData            EventGetData;
        GenTL::PEventGetDataInfo        EventGetDataInfo;
        GenTL::PEventGetInfo            EventGetInfo;
        GenTL::PEventFlush              EventFlush;
        GenTL::PEventKill               EventKill;
        
        // Funzioni Transport Layer
        GenTL::PTLOpen                  TLOpen;
        GenTL::PTLClose                 TLClose;
        GenTL::PTLGetInfo               TLGetInfo;
        GenTL::PTLGetNumInterfaces      TLGetNumInterfaces;
        GenTL::PTLGetInterfaceID        TLGetInterfaceID;
        GenTL::PTLGetInterfaceInfo      TLGetInterfaceInfo;
        GenTL::PTLOpenInterface         TLOpenInterface;
        GenTL::PTLUpdateInterfaceList   TLUpdateInterfaceList;
        
        // Funzioni Interface
        GenTL::PIFClose                 IFClose;
        GenTL::PIFGetInfo               IFGetInfo;
        GenTL::PIFGetNumDevices         IFGetNumDevices;
        GenTL::PIFGetDeviceID           IFGetDeviceID;
        GenTL::PIFUpdateDeviceList      IFUpdateDeviceList;
        GenTL::PIFGetDeviceInfo         IFGetDeviceInfo;
        GenTL::PIFOpenDevice            IFOpenDevice;
        
        // Funzioni Device
        GenTL::PDevGetPort              DevGetPort;
        GenTL::PDevGetNumDataStreams    DevGetNumDataStreams;
        GenTL::PDevGetDataStreamID      DevGetDataStreamID;
        GenTL::PDevOpenDataStream       DevOpenDataStream;
        GenTL::PDevGetInfo              DevGetInfo;
        GenTL::PDevClose                DevClose;
        
        // Funzioni Data Stream
        GenTL::PDSAnnounceBuffer        DSAnnounceBuffer;
        GenTL::PDSAllocAndAnnounceBuffer DSAllocAndAnnounceBuffer;
        GenTL::PDSFlushQueue            DSFlushQueue;
        GenTL::PDSStartAcquisition      DSStartAcquisition;
        GenTL::PDSStopAcquisition       DSStopAcquisition;
        GenTL::PDSGetInfo               DSGetInfo;
        GenTL::PDSGetBufferID           DSGetBufferID;
        GenTL::PDSClose                 DSClose;
        GenTL::PDSRevokeBuffer          DSRevokeBuffer;
        GenTL::PDSQueueBuffer           DSQueueBuffer;
        GenTL::PDSGetBufferInfo         DSGetBufferInfo;
        
        // Funzioni GenTL v1.1
        GenTL::PGCGetNumPortURLs        GCGetNumPortURLs;
        GenTL::PGCGetPortURLInfo        GCGetPortURLInfo;
        GenTL::PGCReadPortStacked       GCReadPortStacked;
        GenTL::PGCWritePortStacked      GCWritePortStacked;
        
        // Funzioni GenTL v1.3
        GenTL::PDSGetBufferChunkData    DSGetBufferChunkData;
        
        // Funzioni GenTL v1.4
        GenTL::PIFGetParentTL           IFGetParentTL;
        GenTL::PDevGetParentIF          DevGetParentIF;
        GenTL::PDSGetParentDev          DSGetParentDev;
        
        // Funzioni GenTL v1.5
        GenTL::PDSGetNumBufferParts     DSGetNumBufferParts;
        GenTL::PDSGetBufferPartInfo     DSGetBufferPartInfo;
        
        // Funzioni GenTL v1.6
        GenTL::PDSAnnounceCompositeBuffer DSAnnounceCompositeBuffer;
        GenTL::PDSGetBufferInfoStacked  DSGetBufferInfoStacked;
        GenTL::PDSGetBufferPartInfoStacked DSGetBufferPartInfoStacked;
        GenTL::PDSGetNumFlows           DSGetNumFlows;
        GenTL::PDSGetFlowInfo           DSGetFlowInfo;
        GenTL::PDSGetNumBufferSegments  DSGetNumBufferSegments;
        GenTL::PDSGetBufferSegmentInfo  DSGetBufferSegmentInfo;

    private:
        LibraryHandle m_hModule;
        std::string m_producerPath;
        bool m_isInitialized;

        /**
         * @brief Inizializza tutti i puntatori alle funzioni
         * @return true se tutte le funzioni obbligatorie sono state trovate
         */
        bool InitializeFunctionPointers();

        /**
         * @brief Resetta tutti i puntatori a nullptr
         */
        void ResetFunctionPointers();

        /**
         * @brief Helper per ottenere l'indirizzo di una funzione
         * @param functionName Nome della funzione da cercare
         * @return Puntatore alla funzione o nullptr se non trovata
         */
        void* GetFunctionAddress(const std::string& functionName);

        /**
         * @brief Helper template per assegnare puntatori a funzione
         */
        template<typename T>
        bool AssignFunction(T& funcPtr, const std::string& funcName, bool mandatory = true) {
            funcPtr = reinterpret_cast<T>(GetFunctionAddress(funcName));
            if (!funcPtr && mandatory) {
                m_lastError = "Funzione obbligatoria non trovata: " + funcName;
                return false;
            }
            return true;
        }

        std::string m_lastError;

    public:
        /**
         * @brief Ottiene l'ultimo messaggio di errore
         * @return Stringa con la descrizione dell'errore
         */
        const std::string& GetLastError() const { return m_lastError; }
    };

    /**
     * @brief Singleton per gestire un loader GenTL globale
     * 
     * Utile per applicazioni che usano un solo producer alla volta
     */
    class GenTLLoaderSingleton {
    public:
        static GenTLLoader& GetInstance() {
            static GenTLLoader instance;
            return instance;
        }

    private:
        GenTLLoaderSingleton() = default;
        ~GenTLLoaderSingleton() = default;
        GenTLLoaderSingleton(const GenTLLoaderSingleton&) = delete;
        GenTLLoaderSingleton& operator=(const GenTLLoaderSingleton&) = delete;
    };
    
    /**
     * @brief Macro helper per accesso rapido alle funzioni GenTL
     * 
     * Esempio: GENTL_CALL(TLOpen)(&handle) invece di loader.TLOpen(&handle)
     */
    #define GENTL_LOADER GenICamWrapper::GenTLLoaderSingleton::GetInstance()
    #define GENTL_CALL(func) GENTL_LOADER.func

} // namespace GenICamWrapper