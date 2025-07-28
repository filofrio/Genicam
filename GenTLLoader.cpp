#include "GenTLLoader.h"
#include <algorithm>
#include <filesystem>
#include <iostream>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace GenICamWrapper {

    // === Costruttore e Distruttore ===

    GenTLLoader::GenTLLoader() 
        : m_hModule(nullptr)
        , m_isInitialized(false) {
        ResetFunctionPointers();
    }

    GenTLLoader::~GenTLLoader() {
        UnloadProducer();
    }

    // === Caricamento/Scaricamento Producer ===

    bool GenTLLoader::LoadProducer(const std::string& ctiPath) {
        // Se c'è già un producer caricato, scaricalo
        if (m_hModule) {
            UnloadProducer();
        }

        // Verifica che il file esista
        if (!fs::exists(ctiPath)) {
            m_lastError = "File non trovato: " + ctiPath;
            return false;
        }

        // Verifica l'estensione
        fs::path path(ctiPath);
        if (path.extension() != ".cti") {
            m_lastError = "Il file deve avere estensione .cti: " + ctiPath;
            return false;
        }

        // Windows: carica la DLL
        m_hModule = LoadLibraryA(ctiPath.c_str());
        if (!m_hModule) {
            std::string error = GetLastError();
            m_lastError = "Impossibile caricare la libreria: " + error;
            return false;
        }

        m_producerPath = ctiPath;

        // Inizializza i puntatori alle funzioni
        if (!InitializeFunctionPointers()) {
            UnloadProducer();
            return false;
        }

        m_isInitialized = true;
        return true;
    }

    void GenTLLoader::UnloadProducer() {
        if (m_hModule) {
            // Se GenTL è stato inizializzato, chiudilo prima di scaricare
            if (m_isInitialized && GCCloseLib) {
                GenTL::GC_ERROR err = GCCloseLib();
            }

#ifdef _WIN32
            FreeLibrary(m_hModule);
#else
            dlclose(m_hModule);
#endif
            m_hModule = nullptr;
        }

        ResetFunctionPointers();
        m_producerPath.clear();
        m_isInitialized = false;
    }

    // === Inizializzazione Puntatori ===

    bool GenTLLoader::InitializeFunctionPointers() {
        if (!m_hModule) {
            m_lastError = "Nessuna libreria caricata";
            return false;
        }

        // Funzioni obbligatorie - se mancano, il producer non è valido
        if (!AssignFunction(GCGetInfo, "GCGetInfo")) return false;
        if (!AssignFunction(GCGetLastError, "GCGetLastError")) return false;
        if (!AssignFunction(GCInitLib, "GCInitLib")) return false;
        if (!AssignFunction(GCCloseLib, "GCCloseLib")) return false;
        if (!AssignFunction(GCReadPort, "GCReadPort")) return false;
        if (!AssignFunction(GCWritePort, "GCWritePort")) return false;
        if (!AssignFunction(GCGetPortURL, "GCGetPortURL")) return false;
        if (!AssignFunction(GCGetPortInfo, "GCGetPortInfo")) return false;
        if (!AssignFunction(GCRegisterEvent, "GCRegisterEvent")) return false;
        if (!AssignFunction(GCUnregisterEvent, "GCUnregisterEvent")) return false;
        if (!AssignFunction(EventGetData, "EventGetData")) return false;
        if (!AssignFunction(EventGetDataInfo, "EventGetDataInfo")) return false;
        if (!AssignFunction(EventGetInfo, "EventGetInfo")) return false;
        if (!AssignFunction(EventFlush, "EventFlush")) return false;
        if (!AssignFunction(EventKill, "EventKill")) return false;
        if (!AssignFunction(TLOpen, "TLOpen")) return false;
        if (!AssignFunction(TLClose, "TLClose")) return false;
        if (!AssignFunction(TLGetInfo, "TLGetInfo")) return false;
        if (!AssignFunction(TLGetNumInterfaces, "TLGetNumInterfaces")) return false;
        if (!AssignFunction(TLGetInterfaceID, "TLGetInterfaceID")) return false;
        if (!AssignFunction(TLGetInterfaceInfo, "TLGetInterfaceInfo")) return false;
        if (!AssignFunction(TLOpenInterface, "TLOpenInterface")) return false;
        if (!AssignFunction(TLUpdateInterfaceList, "TLUpdateInterfaceList")) return false;
        if (!AssignFunction(IFClose, "IFClose")) return false;
        if (!AssignFunction(IFGetInfo, "IFGetInfo")) return false;
        if (!AssignFunction(IFGetNumDevices, "IFGetNumDevices")) return false;
        if (!AssignFunction(IFGetDeviceID, "IFGetDeviceID")) return false;
        if (!AssignFunction(IFUpdateDeviceList, "IFUpdateDeviceList")) return false;
        if (!AssignFunction(IFGetDeviceInfo, "IFGetDeviceInfo")) return false;
        if (!AssignFunction(IFOpenDevice, "IFOpenDevice")) return false;
        if (!AssignFunction(DevGetPort, "DevGetPort")) return false;
        if (!AssignFunction(DevGetNumDataStreams, "DevGetNumDataStreams")) return false;
        if (!AssignFunction(DevGetDataStreamID, "DevGetDataStreamID")) return false;
        if (!AssignFunction(DevOpenDataStream, "DevOpenDataStream")) return false;
        if (!AssignFunction(DevGetInfo, "DevGetInfo")) return false;
        if (!AssignFunction(DevClose, "DevClose")) return false;
        if (!AssignFunction(DSAnnounceBuffer, "DSAnnounceBuffer")) return false;
        if (!AssignFunction(DSAllocAndAnnounceBuffer, "DSAllocAndAnnounceBuffer")) return false;
        if (!AssignFunction(DSFlushQueue, "DSFlushQueue")) return false;
        if (!AssignFunction(DSStartAcquisition, "DSStartAcquisition")) return false;
        if (!AssignFunction(DSStopAcquisition, "DSStopAcquisition")) return false;
        if (!AssignFunction(DSGetInfo, "DSGetInfo")) return false;
        if (!AssignFunction(DSGetBufferID, "DSGetBufferID")) return false;
        if (!AssignFunction(DSClose, "DSClose")) return false;
        if (!AssignFunction(DSRevokeBuffer, "DSRevokeBuffer")) return false;
        if (!AssignFunction(DSQueueBuffer, "DSQueueBuffer")) return false;
        if (!AssignFunction(DSGetBufferInfo, "DSGetBufferInfo")) return false;

        // Funzioni opzionali GenTL v1.1+ (non causano errore se mancano)
        AssignFunction(GCGetNumPortURLs, "GCGetNumPortURLs", false);
        AssignFunction(GCGetPortURLInfo, "GCGetPortURLInfo", false);
        AssignFunction(GCReadPortStacked, "GCReadPortStacked", false);
        AssignFunction(GCWritePortStacked, "GCWritePortStacked", false);

        // Funzioni opzionali GenTL v1.3+
        AssignFunction(DSGetBufferChunkData, "DSGetBufferChunkData", false);

        // Funzioni opzionali GenTL v1.4+
        AssignFunction(IFGetParentTL, "IFGetParentTL", false);
        AssignFunction(DevGetParentIF, "DevGetParentIF", false);
        AssignFunction(DSGetParentDev, "DSGetParentDev", false);

        // Funzioni opzionali GenTL v1.5+
        AssignFunction(DSGetNumBufferParts, "DSGetNumBufferParts", false);
        AssignFunction(DSGetBufferPartInfo, "DSGetBufferPartInfo", false);

        // Funzioni opzionali GenTL v1.6+
        AssignFunction(DSAnnounceCompositeBuffer, "DSAnnounceCompositeBuffer", false);
        AssignFunction(DSGetBufferInfoStacked, "DSGetBufferInfoStacked", false);
        AssignFunction(DSGetBufferPartInfoStacked, "DSGetBufferPartInfoStacked", false);
        AssignFunction(DSGetNumFlows, "DSGetNumFlows", false);
        AssignFunction(DSGetFlowInfo, "DSGetFlowInfo", false);
        AssignFunction(DSGetNumBufferSegments, "DSGetNumBufferSegments", false);
        AssignFunction(DSGetBufferSegmentInfo, "DSGetBufferSegmentInfo", false);

        return true;
    }

    void GenTLLoader::ResetFunctionPointers() {
        // Resetta tutti i puntatori a nullptr
        GCGetInfo = nullptr;
        GCGetLastError = nullptr;
        GCInitLib = nullptr;
        GCCloseLib = nullptr;
        GCReadPort = nullptr;
        GCWritePort = nullptr;
        GCGetPortURL = nullptr;
        GCGetPortInfo = nullptr;
        GCRegisterEvent = nullptr;
        GCUnregisterEvent = nullptr;
        EventGetData = nullptr;
        EventGetDataInfo = nullptr;
        EventGetInfo = nullptr;
        EventFlush = nullptr;
        EventKill = nullptr;
        TLOpen = nullptr;
        TLClose = nullptr;
        TLGetInfo = nullptr;
        TLGetNumInterfaces = nullptr;
        TLGetInterfaceID = nullptr;
        TLGetInterfaceInfo = nullptr;
        TLOpenInterface = nullptr;
        TLUpdateInterfaceList = nullptr;
        IFClose = nullptr;
        IFGetInfo = nullptr;
        IFGetNumDevices = nullptr;
        IFGetDeviceID = nullptr;
        IFUpdateDeviceList = nullptr;
        IFGetDeviceInfo = nullptr;
        IFOpenDevice = nullptr;
        DevGetPort = nullptr;
        DevGetNumDataStreams = nullptr;
        DevGetDataStreamID = nullptr;
        DevOpenDataStream = nullptr;
        DevGetInfo = nullptr;
        DevClose = nullptr;
        DSAnnounceBuffer = nullptr;
        DSAllocAndAnnounceBuffer = nullptr;
        DSFlushQueue = nullptr;
        DSStartAcquisition = nullptr;
        DSStopAcquisition = nullptr;
        DSGetInfo = nullptr;
        DSGetBufferID = nullptr;
        DSClose = nullptr;
        DSRevokeBuffer = nullptr;
        DSQueueBuffer = nullptr;
        DSGetBufferInfo = nullptr;
        GCGetNumPortURLs = nullptr;
        GCGetPortURLInfo = nullptr;
        GCReadPortStacked = nullptr;
        GCWritePortStacked = nullptr;
        DSGetBufferChunkData = nullptr;
        IFGetParentTL = nullptr;
        DevGetParentIF = nullptr;
        DSGetParentDev = nullptr;
        DSGetNumBufferParts = nullptr;
        DSGetBufferPartInfo = nullptr;
        DSAnnounceCompositeBuffer = nullptr;
        DSGetBufferInfoStacked = nullptr;
        DSGetBufferPartInfoStacked = nullptr;
        DSGetNumFlows = nullptr;
        DSGetFlowInfo = nullptr;
        DSGetNumBufferSegments = nullptr;
        DSGetBufferSegmentInfo = nullptr;
    }

    // === Helper Functions ===

    void* GenTLLoader::GetFunctionAddress(const std::string& functionName) {
        if (!m_hModule) {
            return nullptr;
        }

#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(m_hModule, functionName.c_str()));
#else
        return dlsym(m_hModule, functionName.c_str());
#endif
    }

    // === Enumerazione Producer ===

    std::vector<std::string> GenTLLoader::EnumerateProducers(const std::string& directory) {
        std::vector<std::string> producers;

        if (!fs::exists(directory) || !fs::is_directory(directory)) {
            return producers;
        }

        try {
            for (const auto& entry : fs::directory_iterator(directory)) {
                if (fs::is_regular_file(entry.path()) &&
                    entry.path().extension() == ".cti") {
                    producers.push_back(entry.path().string());
                }
            }
        }
        catch (const std::exception& e) {
            std::cerr << "Errore durante l'enumerazione dei producer: " << e.what() << std::endl;
        }

        return producers;
    }

    std::vector<std::string> GenTLLoader::EnumerateProducersInStandardPaths() {
        std::vector<std::string> allProducers;
        std::vector<std::string> searchPaths;

        // Percorsi standard GenTL
        // Windows: variabile d'ambiente GENICAM_GENTL64_PATH o GENICAM_GENTL32_PATH
        #ifdef _WIN64
            const char* envVar = "GENICAM_GENTL64_PATH";
        #else
            const char* envVar = "GENICAM_GENTL32_PATH";
        #endif
        
        char* envPath = nullptr;
        size_t envSize = 0;
        _dupenv_s(&envPath, &envSize, envVar);
        
        if (envPath) {
            // La variabile può contenere più percorsi separati da ';'
            std::string pathStr(envPath);
            free(envPath);
            
            size_t start = 0;
            size_t end = pathStr.find(';');
            
            while (end != std::string::npos) {
                searchPaths.push_back(pathStr.substr(start, end - start));
                start = end + 1;
                end = pathStr.find(';', start);
            }
            
            if (start < pathStr.length()) {
                searchPaths.push_back(pathStr.substr(start));
            }
        }
        
        // Percorso di default su Windows
        searchPaths.push_back("C:\\Program Files\\Common Files\\GenTL Producer");

        // Aggiungi anche la directory corrente
        searchPaths.push_back(".");
        searchPaths.push_back("./gentl");

        // Enumera i producer in tutti i percorsi
        for (const auto& path : searchPaths) {
            auto producers = EnumerateProducers(path);
            allProducers.insert(allProducers.end(), producers.begin(), producers.end());
        }

        // Rimuovi duplicati
        std::sort(allProducers.begin(), allProducers.end());
        allProducers.erase(std::unique(allProducers.begin(), allProducers.end()), allProducers.end());

        return allProducers;
    }

} // namespace GenICamWrapper