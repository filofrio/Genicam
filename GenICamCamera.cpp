#include "GenICamCamera.h"
#include "GenICamException.h"
#include "GenTLLoader.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <thread>
#include <shared_mutex>
#include <filesystem> // Include necessario per std::filesystem

namespace GenICamWrapper {

    // === Costruttore e Distruttore ===

    GenICamCamera::GenICamCamera(std::string fileProducer)
        : m_tlHandle(nullptr)
        , m_ifHandle(nullptr)
        , m_devHandle(nullptr)
        , m_dsHandle(nullptr)
        , m_portHandle(nullptr)
        , m_eventHandle(nullptr)
        , m_bufferSize(0)
        , m_state(CameraState::Disconnected)
        , m_isAcquiring(false)
        , m_stopAcquisition(false)
        , m_eventListener(nullptr),
        m_featureEventHandle(nullptr) {

        try {
            initializeGenTL(fileProducer);
        }
        catch (const GenICamException& e) {
           e.getErrorCode();
            throw;
        }
    }

    GenICamCamera::~GenICamCamera() {
        try {
            if (m_isAcquiring) {
                stopAcquisition();
            }
            if (isConnected()) {
                disconnect();
            }
            cleanupGenTL();
        }
        catch (...) {
            // Evita eccezioni nel distruttore
        }
    }

    // === Inizializzazione GenTL ===

    void GenICamCamera::initializeGenTL(std::string fileProducer) {
        GenTLLoader& loader = GenTLLoaderSingleton::GetInstance();

        if (!loader.IsLoaded()) {
            auto producers = GenTLLoader::EnumerateProducersInStandardPaths();

            if (producers.empty()) {
                THROW_GENICAM_ERROR(ErrorType::GenTLError,
                    "Nessun producer GenTL (.cti) trovato nel sistema");
            }

            bool loaded = false;
            for (const auto& producerPath : producers) {
                std::cout << "Tentativo di caricamento producer: " << producerPath << std::endl;

                size_t lastSlash = producerPath.find_last_of("\\/");
                std::string curProducer;
                if (lastSlash != std::string::npos)
                   curProducer = producerPath.substr(lastSlash + 1);
                else
                   curProducer = producerPath;

                if (curProducer == fileProducer) {
                   if (loader.LoadProducer(producerPath)) {
                      std::cout << "Producer caricato con successo: " << producerPath << std::endl;
                      loaded = true;
                      break;
                   }
                }
            }

            if (!loaded) {
                THROW_GENICAM_ERROR(ErrorType::GenTLError, "Impossibile caricare " + fileProducer);
            }
        }

        GenTL::GC_ERROR err = GENTL_CALL(GCInitLib)();
        if (err != GenTL::GC_ERR_SUCCESS) {
            THROW_GENICAM_ERROR_CODE(ErrorType::GenTLError, "Impossibile inizializzare la libreria GenTL", err);
        }

        err = GENTL_CALL(TLOpen)(&m_tlHandle);
        if (err != GenTL::GC_ERR_SUCCESS) {
            GENTL_CALL(GCCloseLib)();
            THROW_GENICAM_ERROR_CODE(ErrorType::GenTLError,
                "Impossibile aprire il Transport Layer", err);
        }
    }

    void GenICamCamera::cleanupGenTL() {
        if (m_tlHandle) {
            GENTL_CALL(TLClose)(m_tlHandle);
            m_tlHandle = nullptr;
        }
        GENTL_CALL(GCCloseLib)();
    }

    // === Enumerazione Telecamere ===

    std::vector<infoCamere> GenICamCamera::enumerateCameras() {
        std::shared_lock<std::shared_mutex> lock(m_connectionMutex);

        if (!m_tlHandle) {
            THROW_GENICAM_ERROR(ErrorType::GenTLError, "Transport Layer non inizializzato");
        }

        bool8_t changed = 0;
        GenTL::GC_ERROR err = GENTL_CALL(TLUpdateInterfaceList)(m_tlHandle, &changed, 1000);
        if (err != GenTL::GC_ERR_SUCCESS) {
            THROW_GENICAM_ERROR_CODE(ErrorType::GenTLError, "Impossibile aggiornare la lista delle interfacce", err);
        }

        uint32_t numInterfaces = 0;
        err = GENTL_CALL(TLGetNumInterfaces)(m_tlHandle, &numInterfaces);
        if (err != GenTL::GC_ERR_SUCCESS) {
            THROW_GENICAM_ERROR_CODE(ErrorType::GenTLError, "Impossibile ottenere il numero di interfacce", err);
        }
        m_infoTelecamere.clear();
        for (uint32_t i = 0; i < numInterfaces; i++) {
            char interfaceID[256] = { 0 };
            size_t size = sizeof(interfaceID);

            err = GENTL_CALL(TLGetInterfaceID)(m_tlHandle, i, interfaceID, &size);
            if (err != GenTL::GC_ERR_SUCCESS) continue;

            GenTL::IF_HANDLE tempIF = nullptr;
            err = GENTL_CALL(TLOpenInterface)(m_tlHandle, interfaceID, &tempIF);
            if (err != GenTL::GC_ERR_SUCCESS) continue;

            err = GENTL_CALL(IFUpdateDeviceList)(tempIF, &changed, 1000);
            if (err == GenTL::GC_ERR_SUCCESS) {
                uint32_t numDevices = 0;
                err = GENTL_CALL(IFGetNumDevices)(tempIF, &numDevices);

                for (uint32_t j = 0; j < numDevices; j++) {
                    char deviceID[256] = { 0 };
                    size = sizeof(deviceID);

                    err = GENTL_CALL(IFGetDeviceID)(tempIF, j, deviceID, &size);
                    if (err == GenTL::GC_ERR_SUCCESS) {
                        char model[256] = { 0 };
                        size_t modelSize = sizeof(model);
                        infoCamere cameraInfo;
                        char userID[256] = { 0 };
                        size_t userIDSize = sizeof(userID);
                        GenTL::INFO_DATATYPE dataType;

                        GENTL_CALL(IFGetDeviceInfo)(tempIF, deviceID, GenTL::DEVICE_INFO_MODEL, &dataType, model, &modelSize);
                        GENTL_CALL(IFGetDeviceInfo)(tempIF, deviceID, GenTL::DEVICE_INFO_USER_DEFINED_NAME, &dataType, userID, &userIDSize);

                        cameraInfo.nomeConModello = deviceID;
                        cameraInfo.userID = userID;
                        if (strlen(model) > 0) {
                            cameraInfo.nomeConModello += " (";
                            cameraInfo.nomeConModello += model;
                            cameraInfo.nomeConModello += ")";
                            cameraInfo.nomeConModello += "    DeviceUserID: ";
                            cameraInfo.nomeConModello += userID;
                        }
                        m_infoTelecamere.push_back(cameraInfo);
                    }
                }
            }
            GENTL_CALL(IFClose)(tempIF);
        }

        return m_infoTelecamere;
    }

    // === Connessione/Disconnessione ===

    void GenICamCamera::connect(const std::string& cameraID) {
        std::unique_lock<std::shared_mutex> lock(m_connectionMutex);

        if (m_state != CameraState::Disconnected) {
            THROW_GENICAM_ERROR(ErrorType::ConnectionError,
                "Già connesso a una telecamera");
        }

        GenTL::GC_ERROR err;
        bool8_t changed = 0;

        try {
            std::string cleanID = cameraID;
            size_t pos = cleanID.find(" (");
            if (pos != std::string::npos) {
                cleanID = cleanID.substr(0, pos);
            }

            err = GENTL_CALL(TLUpdateInterfaceList)(m_tlHandle, &changed, 1000);
            if (err != GenTL::GC_ERR_SUCCESS) {
                THROW_GENICAM_ERROR_CODE(ErrorType::GenTLError,
                    "Impossibile aggiornare la lista delle interfacce", err);
            }

            uint32_t numInterfaces = 0;
            err = GENTL_CALL(TLGetNumInterfaces)(m_tlHandle, &numInterfaces);

            bool deviceFound = false;

            for (uint32_t i = 0; i < numInterfaces && !deviceFound; i++) {
                char interfaceID[256] = { 0 };
                size_t size = sizeof(interfaceID);

                err = GENTL_CALL(TLGetInterfaceID)(m_tlHandle, i, interfaceID, &size);
                if (err != GenTL::GC_ERR_SUCCESS) continue;

                GenTL::IF_HANDLE tempIF = nullptr;
                err = GENTL_CALL(TLOpenInterface)(m_tlHandle, interfaceID, &tempIF);
                if (err != GenTL::GC_ERR_SUCCESS) continue;

                err = GENTL_CALL(IFUpdateDeviceList)(tempIF, &changed, 1000);
                if (err == GenTL::GC_ERR_SUCCESS) {
                    uint32_t numDevices = 0;
                    err = GENTL_CALL(IFGetNumDevices)(tempIF, &numDevices);

                    for (uint32_t j = 0; j < numDevices; j++) {
                        char deviceID[256] = { 0 };
                        size = sizeof(deviceID);

                        err = GENTL_CALL(IFGetDeviceID)(tempIF, j, deviceID, &size);

                        if (err == GenTL::GC_ERR_SUCCESS/* && cleanID == deviceID*/) {
                            m_ifHandle = tempIF;

                            err = GENTL_CALL(IFOpenDevice)(m_ifHandle, deviceID, GenTL::DEVICE_ACCESS_EXCLUSIVE, &m_devHandle);

                            if (err != GenTL::GC_ERR_SUCCESS) {
                               GENTL_CALL(IFClose)(m_ifHandle);
                                m_ifHandle = nullptr;
                                THROW_GENICAM_ERROR_CODE(ErrorType::ConnectionError, "Impossibile aprire il dispositivo", err);
                            }

                            // controlla se la telecamera ha lo user Id desiderato
                            char camUserID[256] = { 0 };
                            size_t size = sizeof(camUserID);
                            GenTL::INFO_DATATYPE dataType;

                            GENTL_CALL(DevGetInfo)(m_devHandle, GenTL::DEVICE_INFO_USER_DEFINED_NAME, &dataType, camUserID, &size);

                            if (camUserID == cameraID) {
                                m_cameraID = deviceID;
                                deviceFound = true;
                                break;
                            }
                            else {
                                err = GENTL_CALL(DevClose)(m_devHandle);
                                m_devHandle = static_cast<GenTL::DEV_HANDLE>(nullptr);
                            }
                        }
                    }
                }

                if (!deviceFound && tempIF) {
                    GENTL_CALL(IFClose)(tempIF);
                }
            }

            if (!deviceFound) {
                THROW_GENICAM_ERROR(ErrorType::ConnectionError,
                    "Dispositivo non trovato: " + cameraID);
            }

            err = GENTL_CALL(DevGetPort)(m_devHandle, &m_portHandle);
            if (err != GenTL::GC_ERR_SUCCESS) {
                THROW_GENICAM_ERROR_CODE(ErrorType::GenTLError,
                    "Impossibile ottenere la porta del dispositivo", err);
            }

            try {
                m_pCameraPort = std::make_unique<CameraPort>(m_portHandle);

                char xmlURL[1024] = { 0 };
                size_t urlSize = sizeof(xmlURL);
                err = GENTL_CALL(GCGetPortURL)(m_portHandle, xmlURL, &urlSize);

                if (err == GenTL::GC_ERR_SUCCESS && strlen(xmlURL) > 0) {
                    std::string urlString(xmlURL);

                    // Gestisce diversi formati di URL
                    if ((urlString.find("local:///") == 0 || urlString.find("Local:") == 0) &&
                        urlString.find(".zip") != std::string::npos) {
                        // Formato con ZIP embedded
                        parseAndLoadXMLFromURL(urlString);
                    }
                    else if (urlString.find("file://") == 0) {
                        // File locale
                        std::string filePath = urlString.substr(7);
#ifdef _WIN32
                        if (filePath.length() > 0 && filePath[0] == '/') {
                            filePath = filePath.substr(1);
                        }
                        std::replace(filePath.begin(), filePath.end(), '/', '\\');
#endif

                        m_pNodeMap = std::make_unique<GenApi::CNodeMapRef>();
                        m_pNodeMap->_LoadXMLFromFile(filePath.c_str());
                        m_pNodeMap->_Connect(m_pCameraPort.get(), "Device");
                    }
                    else if (urlString.find("http://") == 0) {
                        // HTTP URL - richiede implementazione download
                        THROW_GENICAM_ERROR(ErrorType::GenApiError,
                            "URL HTTP non ancora supportato");
                    }
                    else {
                        // Prova come percorso file diretto
                        m_pNodeMap = std::make_unique<GenApi::CNodeMapRef>();
                        m_pNodeMap->_LoadXMLFromFile(xmlURL);
                        m_pNodeMap->_Connect(m_pCameraPort.get(), "Device");
                    }
                }
                else {
                    // Nessun URL disponibile - prova metodo alternativo
                    std::cout << "GCGetPortURL fallito, provo metodo alternativo..." << std::endl;
                    loadXMLFromDevice();
                }

                // se va avanti fin qui allora la m_nodeMap e' stata connessa in remote alla telecamera
                // Valida il NodeMap
                try {
                    // Verifica che il NodeMap sia valido testando l'accesso a un nodo base
                    GenApi::CNodePtr pRoot = m_pNodeMap->_GetNode("Root");
                    if (!pRoot.IsValid()) {
                        pRoot = m_pNodeMap->_GetNode("Device");
                    }

                    if (!pRoot.IsValid()) {
                        THROW_GENICAM_ERROR(ErrorType::GenApiError,
                            "NodeMap non valido: impossibile accedere al nodo root");
                    }

                    // Test lettura di un parametro standard per validare l'accesso
                    GenApi::CStringPtr pVendor = m_pNodeMap->_GetNode("DeviceVendorName");
                    if (pVendor.IsValid() && GenApi::IsReadable(pVendor)) {
                        std::string vendor = pVendor->GetValue().c_str();
                        std::cout << "NodeMap validato - Vendor: " << vendor << std::endl;
                    }

                    m_nodeMapValid = true;
                    m_lastNodeMapRefresh = std::chrono::steady_clock::now();

                }
                catch (const GENICAM_NAMESPACE::GenericException& e) {
                    m_nodeMapValid = false;
                    THROW_GENICAM_ERROR(ErrorType::GenApiError,
                        std::string("NodeMap non valido: ") + e.GetDescription());
                }

                // Registra eventi di invalidazione
                registerFeatureInvalidationEvents();

                std::cout << "Inizializzazione GenApi completata" << std::endl;

            }
            catch (const GENICAM_NAMESPACE::GenericException& e) {
                THROW_GENICAM_ERROR(ErrorType::GenApiError,
                    std::string("Errore inizializzazione GenApi: ") + e.GetDescription());
            }

            m_state = CameraState::Connected;

        }
        catch (const GenICamException&) {
            if (m_devHandle) {
                GENTL_CALL(DevClose)(m_devHandle);
                m_devHandle = nullptr;
            }
            if (m_ifHandle) {
                GENTL_CALL(IFClose)(m_ifHandle);
                m_ifHandle = nullptr;
            }
            m_state = CameraState::Disconnected;
            throw;
        }
    }

    void GenICamCamera::validateNodeMap() const {
        if (!m_nodeMapValid || !m_pNodeMap) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                "NodeMap non inizializzato o non valido");
        }

        // Verifica se il NodeMap necessita refresh
        if (isNodeMapStale()) {
            const_cast<GenICamCamera*>(this)->refreshNodeMap();
        }
    }

    bool GenICamCamera::isNodeMapStale() const {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastNodeMapRefresh);
        return elapsed > NODEMAP_REFRESH_INTERVAL;
    }

    void GenICamCamera::refreshNodeMap() {
        std::lock_guard<std::mutex> lock(m_nodeMapRefreshMutex);

        if (!m_pNodeMap || !m_pCameraPort) {
            return;
        }

        try {
            std::cout << "Refreshing NodeMap..." << std::endl;

            // Invalida la cache dei parametri
            m_parameterCache.clear();

            // Per alcune implementazioni, potrebbe essere necessario 
            // ricaricare completamente il NodeMap. Per ora facciamo un poll
            // dei nodi invalidati
            GenApi::CNodePtr pRoot = m_pNodeMap->_GetNode("Root");
            if (!pRoot.IsValid()) {
                pRoot = m_pNodeMap->_GetNode("Device");
            }

            if (pRoot.IsValid()) {
                // Il polling forza l'aggiornamento dei nodi invalidati
                pRoot->InvalidateNode();
            }

            m_lastNodeMapRefresh = std::chrono::steady_clock::now();
            m_nodeMapValid = true;

            std::cout << "NodeMap refreshed successfully" << std::endl;

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            std::cerr << "Error refreshing NodeMap: " << e.GetDescription() << std::endl;
            m_nodeMapValid = false;
        }
    }

    void GenICamCamera::handleFeatureInvalidation(const std::string& featureName) {
        std::cout << "Feature invalidated: " << featureName << std::endl;

        // Rimuovi dalla cache
        {
            std::unique_lock<std::shared_mutex> lock(m_parameterMutex);
            m_parameterCache.erase(featureName);
        }

        // Notifica il listener se presente
        {
            std::lock_guard<std::mutex> lock(m_callbackMutex);
            if (m_eventListener) {
                m_eventListener->OnParameterChanged(featureName, "INVALIDATED");
            }
        }

        // Se troppe invalidazioni, considera un refresh completo
        static std::atomic<int> invalidationCount{ 0 };
        invalidationCount++;

        if (invalidationCount > 10) {
            invalidationCount = 0;
            refreshNodeMap();
        }
    }

    void GenICamCamera::registerFeatureInvalidationEvents() {
        if (!m_devHandle) return;

        try {
            // Registra evento FEATURE_INVALIDATE sul device
            GenTL::GC_ERROR err = GENTL_CALL(GCRegisterEvent)(
                m_devHandle,
                GenTL::EVENT_FEATURE_INVALIDATE,
                &m_eventHandle);

            if (err == GenTL::GC_ERR_SUCCESS) {
                std::cout << "Feature invalidation events registered" << std::endl;
            }

            // Registra anche EVENT_FEATURE_CHANGE se supportato
            GenTL::EVENT_HANDLE changeEventHandle = nullptr;
            err = GENTL_CALL(GCRegisterEvent)(
                m_devHandle,
                GenTL::EVENT_FEATURE_CHANGE,
                &changeEventHandle);

            if (err == GenTL::GC_ERR_SUCCESS) {
                std::cout << "Feature change events registered" << std::endl;
            }

        }
        catch (...) {
            // Eventi non supportati, continua senza
            std::cout << "Feature invalidation events not supported" << std::endl;
        }
    }

    void GenICamCamera::unregisterFeatureInvalidationEvents() {
        if (!m_devHandle) return;

        try {
            GENTL_CALL(GCUnregisterEvent)(m_devHandle, GenTL::EVENT_FEATURE_INVALIDATE);
            GENTL_CALL(GCUnregisterEvent)(m_devHandle, GenTL::EVENT_FEATURE_CHANGE);
        }
        catch (...) {
            // Ignora errori durante cleanup
        }
    }
    void GenICamCamera::connectFirst(const std::string& cameraUsedID) {
        auto cameras = enumerateCameras();
        if (cameras.empty()) {
            THROW_GENICAM_ERROR(ErrorType::ConnectionError, "Nessuna telecamera disponibile");
        }
        connect(cameraUsedID);
    }

    void GenICamCamera::disconnect() {
       notifyParameterChanged("Disconnected", "");
       
       std::unique_lock<std::shared_mutex> lock(m_connectionMutex);

       if (m_state == CameraState::Disconnected) {
          return;
       }
       try {
          if (m_isAcquiring) {
             lock.unlock();  // Evita deadlock
             stopAcquisition();
             lock.lock();
          }

          unregisterFeatureInvalidationEvents();
          m_nodeMapValid = false;
          if (m_pNodeMap) {
             m_pNodeMap.reset();
          }

          if (m_pCameraPort) {
             m_pCameraPort.reset();
          }

          if (m_devHandle) {
             GENTL_CALL(DevClose)(m_devHandle);
             m_devHandle = nullptr;
          }

          if (m_ifHandle) {
             GENTL_CALL(IFClose)(m_ifHandle);
             m_ifHandle = nullptr;
          }

          m_portHandle = nullptr;
          m_state = CameraState::Disconnected;
          m_cameraID.clear();
          m_parameterCache.clear();
       }
       catch (...) {
          m_state = CameraState::Error;
          throw;
       }

       // Notifica listener DOPO averlo ripristinato
       lock.unlock();
    }
    bool GenICamCamera::isConnected() const {
        std::shared_lock<std::shared_mutex> lock(m_connectionMutex);
        return m_state == CameraState::Connected || m_state == CameraState::Acquiring;
    }

    CameraState GenICamCamera::getState() const {
        return m_state;
    }

    void GenICamCamera::parseAndLoadXMLFromURL(const std::string& urlString) {
        std::cout << "Parsing URL: " << urlString << std::endl;

        std::string zipName;
        uint64_t xmlAddress = 0;
        uint64_t xmlSize = 0;

        // Pattern 1: local:///filename.zip;address;size
        if (urlString.find("local:///") == 0) {
            // Formato standard GenICam
            std::string data = urlString.substr(9); // Rimuove "local:///"

            size_t pos1 = data.find(';');
            size_t pos2 = data.find(';', pos1 + 1);
            size_t pos3 = data.find('?');

            if (pos1 != std::string::npos && pos2 != std::string::npos) {
                zipName = data.substr(0, pos1);
                std::string addressStr = data.substr(pos1 + 1, pos2 - pos1 - 1);
                std::string sizeStr;

                if (pos3 != std::string::npos) {
                    sizeStr = data.substr(pos2 + 1, pos3 - pos2 - 1);
                }
                else {
                    sizeStr = data.substr(pos2 + 1);
                }

                // Converti hex string in numeri
                xmlAddress = std::stoull(addressStr, nullptr, 16);
                xmlSize = std::stoull(sizeStr, nullptr, 16);
            }
        }
        // Pattern 2: Local:filename.zip;address;size  
        else if (urlString.find("Local:") == 0) {
            // Formato Hikrobot
            std::string data = urlString.substr(6); // Rimuove "Local:"

            size_t pos1 = data.find(';');
            size_t pos2 = data.find(';', pos1 + 1);
            size_t pos3 = data.find('?');

            if (pos1 != std::string::npos && pos2 != std::string::npos) {
                zipName = data.substr(0, pos1);
                std::string addressStr = data.substr(pos1 + 1, pos2 - pos1 - 1);
                std::string sizeStr;

                if (pos3 != std::string::npos) {
                    sizeStr = data.substr(pos2 + 1, pos3 - pos2 - 1);
                }
                else {
                    sizeStr = data.substr(pos2 + 1);
                }

                xmlAddress = std::stoull(addressStr, nullptr, 16);
                xmlSize = std::stoull(sizeStr, nullptr, 16);
            }
        }
        else {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                "Formato URL non riconosciuto: " + urlString);
        }

        // Log dei parametri estratti
        std::cout << "ZIP Name: " << zipName << std::endl;
        std::cout << "Address: 0x" << std::hex << xmlAddress << std::dec
            << " (" << xmlAddress << ")" << std::endl;
        std::cout << "Size: 0x" << std::hex << xmlSize << std::dec
            << " (" << xmlSize << " bytes)" << std::endl;

        // Verifica parametri
        if (xmlAddress == 0 || xmlSize == 0) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                "Parametri URL non validi");
        }

        // Leggi il file ZIP dal dispositivo
        std::vector<uint8_t> zipData(xmlSize);
        size_t readSize = xmlSize;

        GenTL::GC_ERROR err = GENTL_CALL(GCReadPort)(m_portHandle, xmlAddress, zipData.data(), &readSize);

        if (err != GenTL::GC_ERR_SUCCESS) {
            THROW_GENICAM_ERROR_CODE(ErrorType::GenTLError,
                "Impossibile leggere XML dal dispositivo", err);
        }

        std::cout << "Letti " << readSize << " bytes dal dispositivo" << std::endl;

        // Verifica se è un file ZIP
        bool isZip = (zipData.size() >= 4 &&
            zipData[0] == 'P' && zipData[1] == 'K' &&
            zipData[2] == 0x03 && zipData[3] == 0x04);

        // Crea NodeMap
        m_pNodeMap = std::make_unique<GenApi::CNodeMapRef>();

        try {
            if (isZip) {
                std::cout << "Caricamento XML da ZIP..." << std::endl;
                m_pNodeMap->_LoadXMLFromZIPData(zipData.data(), zipData.size());
            }
            else {
                // Potrebbe essere XML non compresso
                std::cout << "Caricamento XML non compresso..." << std::endl;

                // Verifica se inizia con <?xml
                std::string xmlStart(reinterpret_cast<const char*>(zipData.data()),
                    (std::min)(size_t(10), zipData.size()));

                if (xmlStart.find("<?xml") != std::string::npos) {
                    std::string xmlString(reinterpret_cast<const char*>(zipData.data()),
                        zipData.size());
                    m_pNodeMap->_LoadXMLFromString(xmlString.c_str());
                }
                else {
                    THROW_GENICAM_ERROR(ErrorType::GenApiError,
                        "Dati non riconosciuti come ZIP o XML");
                }
            }

            // Connetti la porta
            m_pNodeMap->_Connect(m_pCameraPort.get(), "Device");
            std::cout << " XML caricato e connesso con successo" << std::endl;

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore caricamento XML: ") + e.GetDescription());
        }
    }

    // === Implementazione CameraPort ===

    void GenICamCamera::CameraPort::Read(void* pBuffer, int64_t Address, int64_t Length) {
        std::lock_guard<std::mutex> lock(m_portMutex);
        size_t size = static_cast<size_t>(Length);
        GenTL::GC_ERROR err = GENTL_CALL(GCReadPort)(m_portHandle, Address, pBuffer, &size);
        if (err != GenTL::GC_ERR_SUCCESS) {
            throw GENICAM_NAMESPACE::GenericException("Port read failed", __FILE__, __LINE__);
        }
    }

    void GenICamCamera::CameraPort::Write(const void* pBuffer, int64_t Address, int64_t Length) {
        std::lock_guard<std::mutex> lock(m_portMutex);
        size_t size = static_cast<size_t>(Length);
        GenTL::GC_ERROR err = GENTL_CALL(GCWritePort)(m_portHandle, Address, pBuffer, &size);
        if (err != GenTL::GC_ERR_SUCCESS) {
            throw GENICAM_NAMESPACE::GenericException("Port write failed", __FILE__, __LINE__);
        }
    }


    // Continuazione di GenICamCamera.cpp

    // === Controllo Acquisizione ===

    void GenICamCamera::startAcquisition(size_t bufferCount) {
        std::lock_guard<std::mutex> lock(m_acquisitionMutex);

        if (!isConnected()) {
            THROW_GENICAM_ERROR(ErrorType::ConnectionError,
                "Camera non connessa");
        }

        if (m_isAcquiring) {
            THROW_GENICAM_ERROR(ErrorType::AcquisitionError,
                "Acquisizione già in corso");
        }

        GenTL::GC_ERROR err;

        try {
            // 1. Prepara Transport Layer (SFNC compliant)
            prepareTransportLayerForAcquisition();

            // 2. Blocca parametri TL se necessario (vendor-specific ma gestito in modo portabile)
            // bisogna bloccare i parametri Transport Layer prima di proseguire dello streaming!!!!...e sbloccarli appena dopo le deallocazione dello streaming medesimo
            setTransportLayerLock(true);

            // 3. Apri data stream
            uint32_t numStreams = 0;
            err = GENTL_CALL(DevGetNumDataStreams)(m_devHandle, &numStreams);
            if (err != GenTL::GC_ERR_SUCCESS || numStreams == 0) {
                THROW_GENICAM_ERROR_CODE(ErrorType::GenTLError, "Nessun data stream disponibile", err);
            }

            char streamID[256] = { 0 };
            size_t streamIDSize = sizeof(streamID);
            err = GENTL_CALL(DevGetDataStreamID)(m_devHandle, 0, streamID, &streamIDSize);
            if (err != GenTL::GC_ERR_SUCCESS) {
                THROW_GENICAM_ERROR_CODE(ErrorType::GenTLError, "Impossibile ottenere l'ID dello stream", err);
            }

            err = GENTL_CALL(DevOpenDataStream)(m_devHandle, streamID, &m_dsHandle);
            if (err != GenTL::GC_ERR_SUCCESS) {
                THROW_GENICAM_ERROR_CODE(ErrorType::GenTLError, "Impossibile aprire il data stream", err);
            }

            // 4. Ottieni dimensione buffer
            GenTL::INFO_DATATYPE dataType;
            bool8_t definesPayloadSize = 0;
            size_t infoSize = sizeof(definesPayloadSize);

            err = GENTL_CALL(DSGetInfo)(m_dsHandle, GenTL::STREAM_INFO_DEFINES_PAYLOADSIZE, &dataType, &definesPayloadSize, &infoSize);

            if (definesPayloadSize) {
               infoSize = sizeof(m_bufferSize);
               err = GENTL_CALL(DSGetInfo)(m_dsHandle, GenTL::STREAM_INFO_PAYLOAD_SIZE, &dataType, &m_bufferSize, &infoSize);
               if (err != GenTL::GC_ERR_SUCCESS) {
                  THROW_GENICAM_ERROR_CODE(ErrorType::BufferError, "Impossibile determinare la dimensione del buffer", err);
               }
            }
            else {
               GenApi::IInteger* pp = dynamic_cast<GenApi::IInteger*>(m_pNodeMap->_GetNode("PayloadSize"));
               if (GenApi::IsReadable(pp))
                  m_bufferSize = static_cast<size_t>(pp->GetValue());
               else {
                  // Calcola manualmente per telecamere che non definiscono PayloadSize
                  ROI roi = getROI();
                  PixelFormat pf = getPixelFormat();

                  int bytesPerPixel = 1;
                  switch (pf) {
                  case PixelFormat::Mono8:
                  case PixelFormat::BayerRG8:
                  case PixelFormat::BayerGB8:
                  case PixelFormat::BayerGR8:
                  case PixelFormat::BayerBG8:
                     bytesPerPixel = 1;
                     break;
                  case PixelFormat::Mono10:
                  case PixelFormat::Mono12:
                  case PixelFormat::Mono16:
                     bytesPerPixel = 2;
                     break;
                  case PixelFormat::RGB8:
                  case PixelFormat::BGR8:
                     bytesPerPixel = 3;
                     break;
                  default:
                     bytesPerPixel = 1;
                  }

                  m_bufferSize = roi.width * roi.height * bytesPerPixel;
               }
            }

            allocateBuffers(bufferCount);

            for (auto& hBuffer : m_bufferHandles) {
                err = GENTL_CALL(DSQueueBuffer)(m_dsHandle, hBuffer);
                if (err != GenTL::GC_ERR_SUCCESS) {
                    THROW_GENICAM_ERROR_CODE(ErrorType::BufferError, "Impossibile accodare il buffer", err);
                }
            }

            err = GENTL_CALL(GCRegisterEvent)(m_dsHandle, GenTL::EVENT_NEW_BUFFER, &m_eventHandle);
            if (err != GenTL::GC_ERR_SUCCESS) {
                THROW_GENICAM_ERROR_CODE(ErrorType::GenTLError, "Impossibile registrare l'evento NEW_BUFFER", err);
            }

            err = GENTL_CALL(DSStartAcquisition)(m_dsHandle, GenTL::ACQ_START_FLAGS_DEFAULT, GENTL_INFINITE);
            if (err != GenTL::GC_ERR_SUCCESS) {
                THROW_GENICAM_ERROR_CODE(ErrorType::AcquisitionError, "Impossibile avviare l'acquisizione sul data stream", err);
            }

            // Usa GenApi per avviare l'acquisizione...
            try {
                GenApi::CCommandPtr pAcqStart = getCommandNode("AcquisitionStart");
                if (pAcqStart.IsValid() && GenApi::IsWritable(pAcqStart)) {
                    pAcqStart->Execute();
                }
            }
            catch (const GENICAM_NAMESPACE::GenericException& e) {
                THROW_GENICAM_ERROR(ErrorType::GenApiError,
                    std::string("Errore comando AcquisitionStart: ") + e.GetDescription());
            }

            m_isAcquiring = true;
            m_stopAcquisition = false;
            m_state = CameraState::Acquiring;

            // Notifica listener
            {
                std::lock_guard<std::mutex> cbLock(m_callbackMutex);
                if (m_eventListener) {
                    m_eventListener->OnAcquisitionStarted();
                }
            }

            m_acquisitionThread = std::thread(&GenICamCamera::acquisitionThreadFunction, this);

        }
        catch (...) {
            // Cleanup in caso di errore
            setTransportLayerLock(false);

            if (m_eventHandle) {
                GENTL_CALL(GCUnregisterEvent)(m_dsHandle, GenTL::EVENT_NEW_BUFFER);
                m_eventHandle = nullptr;
            }
            if (m_dsHandle) {
                GENTL_CALL(DSClose)(m_dsHandle);
                m_dsHandle = nullptr;
            }
            freeBuffers();
            throw;
        }
    }

    void GenICamCamera::stopAcquisition() {

        // Prima imposta il flag di stop FUORI dal mutex per evitare deadlock
        m_stopAcquisition = true;
        std::lock_guard<std::mutex> lock(m_acquisitionMutex);
        if (!m_isAcquiring) {
            return;
        }

        try {
            // 1. Stop acquisizione su camera (SFNC)
            // Usa GenApi per fermare l'acquisizione
            try {
                GenApi::CCommandPtr pAcqStop = getCommandNode("AcquisitionStop");
                if (pAcqStop.IsValid() && GenApi::IsWritable(pAcqStop)) {
                    pAcqStop->Execute();

                    // Attendi completamento con timeout
                    auto startTime = std::chrono::steady_clock::now();
                    while (!pAcqStop->IsDone()) {
                        auto elapsed = std::chrono::steady_clock::now() - startTime;
                        if (elapsed > std::chrono::milliseconds(1000)) {
                            break; // Timeout, procedi comunque
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }
            }
            catch (...) {
                // Non critico, continua con cleanup
            }

            // 2. Stop data stream
            if (m_dsHandle) {
                GENTL_CALL(DSStopAcquisition)(m_dsHandle, GenTL::ACQ_STOP_FLAGS_KILL); // vedere questo. ******************   ACQ_STOP_FLAGS_KILL
                GENTL_CALL(DSFlushQueue)(m_dsHandle, GenTL::ACQ_QUEUE_ALL_DISCARD);
            }
            
            // 3. Attendi thread di acquisizione
            if (m_acquisitionThread.joinable()) {
                // Implementa timeout per evitare blocco infinito
                auto startTime = std::chrono::steady_clock::now();
                const auto maxWaitTime = std::chrono::seconds(5);

                while (m_acquisitionThread.joinable()) {
                    // Controlla se il thread è terminato
                    if (m_acquisitionThread.joinable()) {
                        // Usa native_handle per verificare se il thread è ancora attivo
                        auto elapsed = std::chrono::steady_clock::now() - startTime;
                        if (elapsed > maxWaitTime) {
                            std::cerr << "WARNING: Thread acquisizione non terminato dopo "
                                << "5 secondi, forzando detach" << std::endl;
                            m_acquisitionThread.detach();
                            break;
                        }

                        // Prova join con attesa breve
                        if (m_acquisitionThread.joinable()) {
                            m_acquisitionThread.join();
                            break;
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }

            // 4. Cleanup eventi
            if (m_eventHandle) {
                GENTL_CALL(GCUnregisterEvent)(m_dsHandle, GenTL::EVENT_NEW_BUFFER);
                m_eventHandle = nullptr;
            }

            // 5. Chiudi data stream
            if (m_dsHandle) {
                GENTL_CALL(DSClose)(m_dsHandle);
                m_dsHandle = nullptr;
            }

            // 6. IMPORTANTE: Sblocca TL parameters DOPO aver chiuso lo stream
            setTransportLayerLock(false);

            // 7. Libera buffer
            freeBuffers();

            m_isAcquiring = false;
            m_state = CameraState::Connected;

            // 8. Notifica listener
            {
                std::lock_guard<std::mutex> cbLock(m_callbackMutex);
                if (m_eventListener) {
                    m_eventListener->OnAcquisitionStopped();
                }
            }
        }
        catch (const std::exception& e) {
            // Tenta cleanup anche in caso di errore
            setTransportLayerLock(false);
            m_state = CameraState::Error;
            THROW_GENICAM_ERROR(ErrorType::AcquisitionError,
                std::string("Errore durante stop acquisizione: ") + e.what());
        }
    }

    void GenICamCamera::acquisitionThreadFunction() {

        // Timeout breve per permettere controllo periodico di m_stopAcquisition
        const size_t BUFFER_WAIT_TIMEOUT_MS = 100;  // 100ms invece di GENTL_INFINITE

        // Variabili per gestione eventi feature
        bool hasFeatureEvents = false;
        try {
            // Verifica che l'handle sia valido tentando di ottenere informazioni
            if (m_featureEventHandle != nullptr) {
                GenTL::INFO_DATATYPE dataType;
                size_t infoSize = sizeof(int32_t);
                int32_t eventType;

                GenTL::GC_ERROR err = GENTL_CALL(EventGetInfo)(
                    m_featureEventHandle,
                    GenTL::EVENT_EVENT_TYPE,
                    &dataType,
                    &eventType,
                    &infoSize);

                if (err == GenTL::GC_ERR_SUCCESS &&
                    eventType == GenTL::EVENT_FEATURE_INVALIDATE) {
                    hasFeatureEvents = true;
                    std::cout << "Feature invalidation monitoring active in acquisition thread" << std::endl;
                }
            }
        }
        catch (...) {
            // Eventi non disponibili
            hasFeatureEvents = false;
        }

        while (!m_stopAcquisition) {
            // Check per eventi di invalidazione feature (non bloccante)
            if (hasFeatureEvents && m_featureEventHandle) {
                size_t dataSize = 256;
                char eventData[256] = { 0 };

                // Usa timeout 0 per non bloccare
                GenTL::GC_ERROR err = GENTL_CALL(EventGetData)(m_featureEventHandle, eventData, &dataSize, 0);

                if (err == GenTL::GC_ERR_SUCCESS) {
                    // EventGetDataInfo per ottenere il nome del parametro
                    GenTL::INFO_DATATYPE dataType;
                    char featureName[256] = { 0 };
                    size_t nameSize = sizeof(featureName);

                    err = GENTL_CALL(EventGetDataInfo)(
                        m_featureEventHandle,
                        eventData,
                        dataSize,
                        GenTL::EVENT_DATA_ID,  // ID del parametro invalidato
                        &dataType,
                        featureName,
                        &nameSize);

                    if (err == GenTL::GC_ERR_SUCCESS && strlen(featureName) > 0) {
                        // Processa evento di invalidazione
                        handleFeatureInvalidation(std::string(featureName));
                    }
                    else {
                        // Se non riusciamo a ottenere il nome, invalida tutto
                        std::cout << "Feature invalidation event received (unknown feature)" << std::endl;
                        refreshNodeMap();
                    }
                }
            }

            GenTL::EVENT_NEW_BUFFER_DATA bufferData;
            size_t bufferDataSize = sizeof(bufferData);

            GenTL::GC_ERROR err = GENTL_CALL(EventGetData)(m_eventHandle, &bufferData, &bufferDataSize, 100);

            if (err == GenTL::GC_ERR_TIMEOUT) {
                // Timeout normale, controlla se dobbiamo fermarci
                continue;
            }

            if (err == GenTL::GC_ERR_SUCCESS) {
                GenTL::BUFFER_HANDLE hBuffer = bufferData.BufferHandle;

                if (hBuffer) {
                    try {
                        GenTL::INFO_DATATYPE dataType;
                        void* pBuffer = nullptr;
                        size_t infoSize = sizeof(pBuffer);

                        err = GENTL_CALL(DSGetBufferInfo)(m_dsHandle, hBuffer, GenTL::BUFFER_INFO_BASE, &dataType, &pBuffer, &infoSize);

                        if (err == GenTL::GC_ERR_SUCCESS && pBuffer) {
                            uint32_t width = 0, height = 0;
                            uint64_t pixelFormat = 0;
                            size_t tempSize = sizeof(uint32_t);

                            GENTL_CALL(DSGetBufferInfo)(m_dsHandle, hBuffer, GenTL::BUFFER_INFO_WIDTH, &dataType, &width, &tempSize);
                            GENTL_CALL(DSGetBufferInfo)(m_dsHandle, hBuffer, GenTL::BUFFER_INFO_HEIGHT, &dataType, &height, &tempSize);
                            tempSize = sizeof(uint64_t);
                            GENTL_CALL(DSGetBufferInfo)(m_dsHandle, hBuffer, GenTL::BUFFER_INFO_PIXELFORMAT, &dataType, &pixelFormat, &tempSize);

                            cv::Mat image = convertBufferToMat(pBuffer, m_bufferSize, width, height, convertFromGenICamPixelFormat(pixelFormat));

                            if (!image.empty()) {
                                auto imageData = std::make_unique<ImageData>();
                                imageData->buffer = std::shared_ptr<uint8_t>(new uint8_t[image.total() * image.elemSize()], std::default_delete<uint8_t[]>());

                                std::memcpy(imageData->buffer.get(), image.data, image.total() * image.elemSize());

                                imageData->bufferSize = image.total() * image.elemSize();
                                imageData->width = width;
                                imageData->height = height;
                                imageData->pixelFormat = convertFromGenICamPixelFormat(pixelFormat);
                                imageData->stride = image.step;

                                tempSize = sizeof(uint64_t);
                                GENTL_CALL(DSGetBufferInfo)(m_dsHandle, hBuffer, GenTL::BUFFER_INFO_FRAMEID, &dataType, &imageData->frameID, &tempSize);

                                imageData->timestamp = std::chrono::steady_clock::now();

                                // Ottieni parametri correnti in modo thread-safe
                                try {
                                    imageData->exposureTime = getExposureTime();
                                    imageData->gain = getGain();
                                }
                                catch (...) {
                                    imageData->exposureTime = 0.0;
                                    imageData->gain = 0.0;
                                }

                                // Notifica callback
                                {
                                    std::lock_guard<std::mutex> lock(m_callbackMutex);
                                    if (m_eventListener) {
                                        m_eventListener->OnFrameReady(imageData.get(), image);  // in prima implementazione si e' cercato di inviare imageData al posto di image
                                    }
                                }
                            }
                        }
                    }
                    catch (const std::exception& e) {
                        std::lock_guard<std::mutex> lock(m_callbackMutex);
                        if (m_eventListener) {
                            m_eventListener->OnError(-1, std::string("Errore processamento buffer: ") + e.what());
                        }
                    }

                    GENTL_CALL(DSQueueBuffer)(m_dsHandle, hBuffer);
                }
            }
            else if (err != GenTL::GC_ERR_TIMEOUT) {
                std::lock_guard<std::mutex> lock(m_callbackMutex);
                if (m_eventListener) {
                    m_eventListener->OnError(err, "Errore durante l'acquisizione: " + getGenTLErrorString(err));
                }
            }
        }
    }

    // === Parametri Camera - Implementazione Uniforme GenApi ===
    GenApi::INodeMap* GenICamCamera::getNodeMap() const {
       std::shared_lock<std::shared_mutex> lock(m_connectionMutex);

       if (!isConnected()) {
          THROW_GENICAM_ERROR(ErrorType::ConnectionError,
             "Camera non connessa - NodeMap non disponibile");
       }

       if (!m_nodeMapValid || !m_pNodeMap) {
          THROW_GENICAM_ERROR(ErrorType::GenApiError,
             "NodeMap non valido o non inizializzato");
       }

       // Se CNodeMapRef ha un operatore di conversione o un metodo specifico
      // Controlla la documentazione GenApi per il metodo corretto

      // Possibilità 1: Operatore di conversione implicito
       //GenApi::INodeMap* pNodeMap = m_pNodeMap.get();

       // Possibilità 2: Metodo esplicito (verifica nella documentazione)
       //GenApi::INodeMap* pNodeMap = m_pNodeMap->GetInterface();

       // Possibilità 3: Cast C-style (meno sicuro, usa solo se necessario)
       GenApi::INodeMap* pNodeMap = (GenApi::INodeMap*)m_pNodeMap.get();

       return pNodeMap;
    }

    GenApi::INodeMap* GenICamCamera::getStreamNodeMap() const {
       std::shared_lock<std::shared_mutex> lock(m_connectionMutex);

       if (!isConnected()) {
          return nullptr;
       }

       // Per ora restituiamo nullptr poiché lo stream NodeMap 
       // non è ancora implementato in questa versione.
       // In futuro, se necessario, si può aggiungere:
       // - Un membro m_pStreamNodeMap
       // - Inizializzazione durante la connessione al DataStream
       // - Gestione del ciclo di vita dello stream NodeMap

       return nullptr;

       // Implementazione futura potrebbe essere:
       // return m_pStreamNodeMap ? m_pStreamNodeMap.get() : nullptr;
    }

    // Helper per ottenere nodi GenApi con gestione errori
    GenApi::CNodePtr GenICamCamera::getNode(const std::string& nodeName) const {
        std::shared_lock<std::shared_mutex> lock(m_parameterMutex);

        // Valida il NodeMap prima dell'uso
        validateNodeMap();

        if (!m_pNodeMap) {
            THROW_GENICAM_ERROR(ErrorType::ConnectionError,
                "NodeMap non disponibile");
        }

        GenApi::CNodePtr node = m_pNodeMap->_GetNode(nodeName.c_str());
        if (!node.IsValid()) {
            THROW_GENICAM_ERROR(ErrorType::ParameterError,
                "Nodo non trovato: " + nodeName);
        }

        return node;
    }

    GenApi::CFloatPtr GenICamCamera::getFloatNode(const std::string& nodeName) const {

        // Valida il NodeMap prima dell'uso
        validateNodeMap();

        GenApi::CNodePtr node = getNode(nodeName);
        GenApi::CFloatPtr floatNode(node);

        if (!floatNode.IsValid()) {
            THROW_GENICAM_ERROR(ErrorType::ParameterError,
                "Il nodo " + nodeName + " non è di tipo Float");
        }

        return floatNode;
    }

    GenApi::CIntegerPtr GenICamCamera::getIntegerNode(const std::string& nodeName) const {

        // Valida il NodeMap prima dell'uso
        validateNodeMap();

        GenApi::CNodePtr node = getNode(nodeName);
        GenApi::CIntegerPtr intNode(node);

        if (!intNode.IsValid()) {
            THROW_GENICAM_ERROR(ErrorType::ParameterError,
                "Il nodo " + nodeName + " non è di tipo Integer");
        }

        return intNode;
    }

    GenApi::CEnumerationPtr GenICamCamera::getEnumerationNode(const std::string& nodeName) const {

        // Valida il NodeMap prima dell'uso
        validateNodeMap();

        GenApi::CNodePtr node = getNode(nodeName);
        GenApi::CEnumerationPtr enumNode(node);

        if (!enumNode.IsValid()) {
            THROW_GENICAM_ERROR(ErrorType::ParameterError,
                "Il nodo " + nodeName + " non è di tipo Enumeration");
        }

        return enumNode;
    }

    GenApi::CCommandPtr GenICamCamera::getCommandNode(const std::string& nodeName) const {

        // Valida il NodeMap prima dell'uso
        validateNodeMap();

        GenApi::CNodePtr node = getNode(nodeName);
        GenApi::CCommandPtr cmdNode(node);

        if (!cmdNode.IsValid()) {
            THROW_GENICAM_ERROR(ErrorType::ParameterError,
                "Il nodo " + nodeName + " non è di tipo Command");
        }

        return cmdNode;
    }

// === Controllo dell'esposizione (SFNC 2.7 compliant) ===

    void GenICamCamera::setExposureMode(ExposureMode mode) {
        try {
            GenApi::CEnumerationPtr pExposureMode = getEnumerationNode("ExposureMode");

            if (!GenApi::IsWritable(pExposureMode)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "ExposureMode non scrivibile");
            }

            std::string modeString;
            switch (mode) {
            case ExposureMode::Off:
                modeString = "Off";
                break;
            case ExposureMode::Timed:
                modeString = "Timed";
                break;
            case ExposureMode::TriggerWidth:
                modeString = "TriggerWidth";
                break;
            case ExposureMode::TriggerControlled:
                modeString = "TriggerControlled";
                break;
            default:
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "ExposureMode non valido");
            }

            *pExposureMode = modeString.c_str();
            notifyParameterChanged("ExposureMode", modeString);

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione ExposureMode: ") + e.GetDescription());
        }
    }

    ExposureMode GenICamCamera::getExposureMode() const {
        try {
            // Verifica se ExposureMode esiste
            if (!isParameterAvailable("ExposureMode")) {
                // Se non esiste, assumiamo Timed mode (default SFNC)
                return ExposureMode::Timed;
            }

            GenApi::CEnumerationPtr pExposureMode = getEnumerationNode("ExposureMode");
            std::string mode = pExposureMode->ToString().c_str();

            if (mode == "Off") return ExposureMode::Off;
            if (mode == "Timed") return ExposureMode::Timed;
            if (mode == "TriggerWidth") return ExposureMode::TriggerWidth;
            if (mode == "TriggerControlled") return ExposureMode::TriggerControlled;

            return ExposureMode::Timed; // Default

        }
        catch (...) {
            return ExposureMode::Timed;
        }
    }

    void GenICamCamera::setExposureTime(double microseconds) {
        try {
            // Verifica prima ExposureMode se disponibile
            if (isParameterAvailable("ExposureMode")) {
                ExposureMode mode = getExposureMode();
                if (mode != ExposureMode::Timed) {
                    // Imposta automaticamente Timed mode se necessario
                    setExposureMode(ExposureMode::Timed);
                }
            }

            // Verifica se c'è un selector attivo
            if (isParameterAvailable("ExposureTimeSelector")) {
                // Il selector è già impostato, usa il valore corrente
            }

            // Prova i nomi SFNC in ordine di priorità
            GenApi::CFloatPtr pExposure;
            const std::vector<std::string> exposureNames = {
                "ExposureTime",       // SFNC 2.x standard
                "ExposureTimeAbs",    // Legacy SFNC 1.x
                "ExposureTimeRaw"     // Alcune implementazioni usano Raw
            };

            bool found = false;
            for (const auto& name : exposureNames) {
                try {
                    pExposure = getFloatNode(name);
                    if (pExposure.IsValid() && GenApi::IsWritable(pExposure)) {
                        found = true;
                        break;
                    }
                }
                catch (...) {
                    continue;
                }
            }

            if (!found) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "Nessun parametro ExposureTime scrivibile trovato");
            }

            // Verifica range
            double min = pExposure->GetMin();
            double max = pExposure->GetMax();

            if (microseconds < min || microseconds > max) {
                std::stringstream ss;
                ss << "Valore esposizione fuori range [" << min << ", " << max << "] µs";
                THROW_GENICAM_ERROR(ErrorType::ParameterError, ss.str());
            }

            // Verifica incremento se disponibile
            if (pExposure->GetIncMode() != GenApi::noIncrement) {
                double inc = pExposure->GetInc();
                // Arrotonda al valore di incremento più vicino
                microseconds = round(microseconds / inc) * inc;
            }

            pExposure->SetValue(microseconds);

            // Invalida cache
            m_parameterCache.erase("ExposureTime");

            notifyParameterChanged("ExposureTime", std::to_string(microseconds));

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione ExposureTime: ") + e.GetDescription());
        }
    }

    double GenICamCamera::getExposureTime() const {
        try {
            // Check cache first
            auto it = m_parameterCache.find("ExposureTime");
            if (it != m_parameterCache.end()) {
                auto now = std::chrono::steady_clock::now();
                if (now - it->second.second < CACHE_TIMEOUT) {
                    return std::stod(it->second.first);
                }
            }

            // Prova i nomi SFNC in ordine
            const std::vector<std::string> exposureNames = {
                "ExposureTime",       // SFNC 2.x
                "ExposureTimeAbs",    // SFNC 1.x
                "ExposureTimeRaw"     // Raw value
            };

            for (const auto& name : exposureNames) {
                try {
                    GenApi::CFloatPtr pExposure = getFloatNode(name);
                    if (pExposure.IsValid() && GenApi::IsReadable(pExposure)) {
                        double value = pExposure->GetValue();

                        // Se è ExposureTimeRaw, potrebbe servire conversione
                        if (name == "ExposureTimeRaw") {
                            // Cerca un fattore di conversione
                            try {
                                GenApi::CFloatPtr pConvFactor = getFloatNode("ExposureTimeBaseAbs");
                                if (pConvFactor.IsValid()) {
                                    value *= pConvFactor->GetValue();
                                }
                            }
                            catch (...) {}
                        }

                        // Update cache
                        m_parameterCache["ExposureTime"] = {
                            std::to_string(value),
                            std::chrono::steady_clock::now()
                        };

                        return value;
                    }
                }
                catch (...) {
                    continue;
                }
            }

            THROW_GENICAM_ERROR(ErrorType::ParameterError,
                "ExposureTime non disponibile");

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore lettura ExposureTime: ") + e.GetDescription());
        }
    }

    void GenICamCamera::setExposureAuto(ExposureAuto mode) {
        try {
            GenApi::CEnumerationPtr pExposureAuto = getEnumerationNode("ExposureAuto");

            if (!GenApi::IsWritable(pExposureAuto)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "ExposureAuto non scrivibile");
            }

            std::string modeString;
            switch (mode) {
            case ExposureAuto::Off:
                modeString = "Off";
                break;
            case ExposureAuto::Once:
                modeString = "Once";
                break;
            case ExposureAuto::Continuous:
                modeString = "Continuous";
                break;
            }

            *pExposureAuto = modeString.c_str();
            notifyParameterChanged("ExposureAuto", modeString);

            // Se impostato su Once, attendi che completi
            if (mode == ExposureAuto::Once) {
                // Attendi che torni a Off
                int maxRetries = 100;
                while (maxRetries-- > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    if (getExposureAuto() == ExposureAuto::Off) {
                        break;
                    }
                }
            }

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione ExposureAuto: ") + e.GetDescription());
        }
    }

    ExposureAuto GenICamCamera::getExposureAuto() const {
        try {
            GenApi::CEnumerationPtr pExposureAuto = getEnumerationNode("ExposureAuto");
            std::string mode = pExposureAuto->ToString().c_str();

            if (mode == "Off") return ExposureAuto::Off;
            if (mode == "Once") return ExposureAuto::Once;
            if (mode == "Continuous") return ExposureAuto::Continuous;

            return ExposureAuto::Off;

        }
        catch (...) {
            return ExposureAuto::Off;
        }
    }

    bool GenICamCamera::isExposureAutoAvailable() const {
        try {
            GenApi::CNodePtr node = m_pNodeMap->_GetNode("ExposureAuto");
            return node.IsValid() && GenApi::IsImplemented(node);
        }
        catch (...) {
            return false;
        }
    }

    // Helper per debug della configurazione esposizione
    std::string GenICamCamera::getExposureConfiguration() const {
        std::stringstream config;

        config << "=== Exposure Configuration (SFNC) ===" << std::endl;

        // ExposureMode
        if (isParameterAvailable("ExposureMode")) {
            config << "ExposureMode: ";
            switch (getExposureMode()) {
            case ExposureMode::Off: config << "Off"; break;
            case ExposureMode::Timed: config << "Timed"; break;
            case ExposureMode::TriggerWidth: config << "TriggerWidth"; break;
            case ExposureMode::TriggerControlled: config << "TriggerControlled"; break;
            }
            config << std::endl;
        }

        // ExposureTime
        if (isExposureTimeAvailable()) {
            double min, max;
            getExposureTimeRange(min, max);
            config << "ExposureTime: " << getExposureTime() << " µs"
                << " (Range: " << min << " - " << max << " µs)" << std::endl;
        }

        // ExposureAuto
        if (isExposureAutoAvailable()) {
            config << "ExposureAuto: ";
            switch (getExposureAuto()) {
            case ExposureAuto::Off: config << "Off"; break;
            case ExposureAuto::Once: config << "Once"; break;
            case ExposureAuto::Continuous: config << "Continuous"; break;
            }
            config << std::endl;
        }

        return config.str();
    }

    void GenICamCamera::getExposureTimeRange(double& min, double& max) const {
        try {
            // Lista dei possibili nomi per ExposureTime secondo SFNC
            const std::vector<std::string> exposureNames = {
                "ExposureTime",       // SFNC 2.x standard
                "ExposureTimeAbs",    // SFNC 1.x legacy
                "ExposureTimeRaw"     // Raw value (needs conversion)
            };

            bool found = false;

            for (const auto& name : exposureNames) {
                try {
                    GenApi::CFloatPtr pExposure = getFloatNode(name);
                    if (pExposure.IsValid() && GenApi::IsReadable(pExposure)) {
                        min = pExposure->GetMin();
                        max = pExposure->GetMax();

                        // Se è ExposureTimeRaw, applica conversione
                        if (name == "ExposureTimeRaw") {
                            try {
                                // Cerca il fattore di conversione per convertire da raw a microsecondi
                                GenApi::CFloatPtr pConvFactor = getFloatNode("ExposureTimeBaseAbs");
                                if (pConvFactor.IsValid() && GenApi::IsReadable(pConvFactor)) {
                                    double factor = pConvFactor->GetValue();
                                    min *= factor;
                                    max *= factor;
                                }
                            }
                            catch (...) {
                                // Se non c'è fattore di conversione, prova con ExposureTimeBase
                                try {
                                    GenApi::CIntegerPtr pTimeBase = getIntegerNode("ExposureTimeBase");
                                    if (pTimeBase.IsValid() && GenApi::IsReadable(pTimeBase)) {
                                        double factor = static_cast<double>(pTimeBase->GetValue());
                                        min *= factor;
                                        max *= factor;
                                    }
                                }
                                catch (...) {
                                    // Nessun fattore di conversione trovato, usa valori raw
                                }
                            }
                        }

                        found = true;
                        break;
                    }
                }
                catch (...) {
                    // Prova il prossimo nome
                    continue;
                }
            }

            // Se non trovato, prova con nodi Integer
            if (!found) {
                for (const auto& name : exposureNames) {
                    try {
                        GenApi::CIntegerPtr pExposure = getIntegerNode(name);
                        if (pExposure.IsValid() && GenApi::IsReadable(pExposure)) {
                            min = static_cast<double>(pExposure->GetMin());
                            max = static_cast<double>(pExposure->GetMax());

                            // Applica conversioni se necessario
                            if (name == "ExposureTimeRaw") {
                                try {
                                    GenApi::CFloatPtr pConvFactor = getFloatNode("ExposureTimeBaseAbs");
                                    if (pConvFactor.IsValid()) {
                                        double factor = pConvFactor->GetValue();
                                        min *= factor;
                                        max *= factor;
                                    }
                                }
                                catch (...) {}
                            }

                            found = true;
                            break;
                        }
                    }
                    catch (...) {
                        continue;
                    }
                }
            }

            // Se ancora non trovato, verifica se c'è ExposureMode
            if (!found && isParameterAvailable("ExposureMode")) {
                // Alcune camere potrebbero avere range diversi per modalità diverse
                ExposureMode mode = getExposureMode();

                // Prova a leggere range specifici per modalità
                std::string modeSuffix;
                switch (mode) {
                case ExposureMode::Timed:
                    modeSuffix = "Timed";
                    break;
                case ExposureMode::TriggerWidth:
                    modeSuffix = "TriggerWidth";
                    break;
                default:
                    break;
                }

                if (!modeSuffix.empty()) {
                    try {
                        std::string paramName = "ExposureTime" + modeSuffix + "Min";
                        min = std::stod(getParameter(paramName));
                        paramName = "ExposureTime" + modeSuffix + "Max";
                        max = std::stod(getParameter(paramName));
                        found = true;
                    }
                    catch (...) {}
                }
            }

            // Se non trovato nulla, usa valori di default ragionevoli
            if (!found) {
                // Valori tipici in microsecondi
                min = 10.0;      // 10 µs
                max = 10000000.0; // 10 secondi

                // Log warning per debug
                std::cerr << "Warning: ExposureTime range non disponibile, usando valori di default" << std::endl;
            }

            // Verifica sanità dei valori
            if (min > max) {
                std::swap(min, max);
            }

            // Assicura che non siano zero o negativi
            if (min <= 0) {
                min = 1.0; // Minimo 1 microsecondo
            }

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            // In caso di errore GenApi, usa valori di default
            min = 10.0;
            max = 10000000.0;

            // Log per debug
            std::cerr << "Error getting exposure range: " << e.GetDescription() << std::endl;
        }
        catch (...) {
            // Fallback finale
            min = 10.0;
            max = 10000000.0;
        }
    }

    bool GenICamCamera::isExposureTimeAvailable() const {
        try {
            // Prima verifica se siamo connessi
            if (!isConnected() || !m_pNodeMap) {
                return false;
            }

            // Lista dei possibili nomi ExposureTime secondo SFNC
            const std::vector<std::string> exposureNames = {
                "ExposureTime",       // SFNC 2.x standard
                "ExposureTimeAbs",    // SFNC 1.x legacy
                "ExposureTimeRaw"     // Raw value
            };

            // Verifica ogni possibile nome
            for (const auto& name : exposureNames) {
                try {
                    GenApi::CNodePtr node = m_pNodeMap->_GetNode(name.c_str());

                    if (node.IsValid() &&
                        GenApi::IsImplemented(node) &&
                        GenApi::IsAvailable(node)) {

                        // Verifica che sia almeno leggibile
                        if (GenApi::IsReadable(node)) {
                            // Verifica il tipo di nodo
                            GenApi::EInterfaceType nodeType = node->GetPrincipalInterfaceType();

                            // ExposureTime può essere Float o Integer
                            if (nodeType == GenApi::intfIFloat ||
                                nodeType == GenApi::intfIInteger) {
                                return true;
                            }
                        }
                    }
                }
                catch (...) {
                    // Continua con il prossimo nome
                    continue;
                }
            }

            // Se arriviamo qui, verifica anche se c'è ExposureMode
            // Alcune camere potrebbero richiedere ExposureMode = Timed
            if (isParameterAvailable("ExposureMode")) {
                try {
                    ExposureMode mode = getExposureMode();

                    // Se ExposureMode esiste ma è Off, ExposureTime potrebbe non essere disponibile
                    if (mode == ExposureMode::Off) {
                        return false;
                    }

                    // Se ExposureMode != Off, riprova la ricerca con modalità specifica
                    if (mode == ExposureMode::Timed) {
                        // Alcune camere usano nomi specifici per modalità
                        try {
                            GenApi::CNodePtr node = m_pNodeMap->_GetNode("ExposureTimeTimed");
                            if (node.IsValid() &&
                                GenApi::IsImplemented(node) &&
                                GenApi::IsAvailable(node) &&
                                GenApi::IsReadable(node)) {
                                return true;
                            }
                        }
                        catch (...) {}
                    }
                }
                catch (...) {}
            }

            // Verifica finale: prova a leggere il valore
            // Questo è il test definitivo
            try {
                double testValue = getExposureTime();
                // Se non lancia eccezione, è disponibile
                return true;
            }
            catch (...) {
                // Se getExposureTime() fallisce, non è disponibile
            }

            return false;

        }
        catch (...) {
            return false;
        }
    }

    // === Metodo helper aggiuntivo per debug ===
    std::string GenICamCamera::getExposureInfo() const {
        std::stringstream info;

        info << "=== Exposure Information (SFNC) ===" << std::endl;

        // Disponibilità
        info << "ExposureTime Available: " << (isExposureTimeAvailable() ? "Yes" : "No") << std::endl;

        if (isExposureTimeAvailable()) {
            try {
                // Valore corrente
                double current = getExposureTime();
                info << "Current ExposureTime: " << current << " µs" << std::endl;

                // Range
                double min, max;
                getExposureTimeRange(min, max);
                info << "ExposureTime Range: [" << min << " - " << max << "] µs" << std::endl;

                // Incremento se disponibile
                try {
                    const std::vector<std::string> exposureNames = {
                        "ExposureTime", "ExposureTimeAbs", "ExposureTimeRaw"
                    };

                    for (const auto& name : exposureNames) {
                        try {
                            GenApi::CFloatPtr pExposure = getFloatNode(name);
                            if (pExposure.IsValid() && pExposure->GetIncMode() != GenApi::noIncrement) {
                                info << "ExposureTime Increment: " << pExposure->GetInc() << " µs" << std::endl;
                                break;
                            }
                        }
                        catch (...) {}
                    }
                }
                catch (...) {}

                // Access mode
                info << "Access Mode: ";
                bool readable = false, writable = false;

                const std::vector<std::string> exposureNames = {
                    "ExposureTime", "ExposureTimeAbs", "ExposureTimeRaw"
                };

                for (const auto& name : exposureNames) {
                    if (isParameterReadable(name)) readable = true;
                    if (isParameterWritable(name)) writable = true;
                    if (readable || writable) break;
                }

                if (readable) info << "R";
                if (writable) info << "W";
                info << std::endl;

            }
            catch (const std::exception& e) {
                info << "Error reading exposure info: " << e.what() << std::endl;
            }
        }

        // ExposureMode info
        if (isParameterAvailable("ExposureMode")) {
            info << "\nExposureMode: ";
            try {
                switch (getExposureMode()) {
                case ExposureMode::Off: info << "Off"; break;
                case ExposureMode::Timed: info << "Timed"; break;
                case ExposureMode::TriggerWidth: info << "TriggerWidth"; break;
                case ExposureMode::TriggerControlled: info << "TriggerControlled"; break;
                }
                info << std::endl;

                // Modalità disponibili
                auto modes = getAvailableExposureModes();
                if (!modes.empty()) {
                    info << "Available modes: ";
                    for (const auto& mode : modes) {
                        switch (mode) {
                        case ExposureMode::Off: info << "Off "; break;
                        case ExposureMode::Timed: info << "Timed "; break;
                        case ExposureMode::TriggerWidth: info << "TriggerWidth "; break;
                        case ExposureMode::TriggerControlled: info << "TriggerControlled "; break;
                        }
                    }
                    info << std::endl;
                }
            }
            catch (...) {}
        }

        // ExposureAuto info
        if (isExposureAutoAvailable()) {
            info << "\nExposureAuto: ";
            try {
                switch (getExposureAuto()) {
                case ExposureAuto::Off: info << "Off"; break;
                case ExposureAuto::Once: info << "Once"; break;
                case ExposureAuto::Continuous: info << "Continuous"; break;
                }
                info << std::endl;
            }
            catch (...) {}
        }

        return info.str();
    }


    std::vector<ExposureMode> GenICamCamera::getAvailableExposureModes() const {
        std::vector<ExposureMode> modes;

        try {
            // Prima verifica se siamo connessi
            if (!isConnected() || !m_pNodeMap) {
                return modes; // Ritorna vector vuoto
            }

            // Verifica se ExposureMode esiste
            if (!isParameterAvailable("ExposureMode")) {
                // Se ExposureMode non esiste, assumiamo che la camera supporti
                // solo la modalità Timed (default SFNC)
                modes.push_back(ExposureMode::Timed);
                return modes;
            }

            // Ottieni il nodo ExposureMode
            GenApi::CEnumerationPtr pExposureMode = getEnumerationNode("ExposureMode");

            if (!pExposureMode.IsValid()) {
                // Fallback a modalità default
                modes.push_back(ExposureMode::Timed);
                return modes;
            }

            // Ottieni tutte le entries disponibili
            GenApi::NodeList_t entries;
            pExposureMode->GetEntries(entries);

            // Itera su tutte le entries
            for (auto& entry : entries) {
                // Verifica che l'entry sia disponibile e implementata
                if (GenApi::IsAvailable(entry) && GenApi::IsImplemented(entry)) {
                    // Ottieni il nome simbolico dell'entry
                    GenApi::CEnumEntryPtr pEntry(entry);
                    if (pEntry.IsValid()) {
                        std::string entryName = pEntry->GetSymbolic().c_str();

                        // Converti il nome in enum ExposureMode
                        if (entryName == "Off") {
                            modes.push_back(ExposureMode::Off);
                        }
                        else if (entryName == "Timed") {
                            modes.push_back(ExposureMode::Timed);
                        }
                        else if (entryName == "TriggerWidth") {
                            modes.push_back(ExposureMode::TriggerWidth);
                        }
                        else if (entryName == "TriggerControlled") {
                            modes.push_back(ExposureMode::TriggerControlled);
                        }
                        // Else: modalità non standard, ignora
                    }
                }
            }

            // Se non sono state trovate modalità valide, aggiungi default
            if (modes.empty()) {
                // Questo può succedere se il nodo esiste ma le entries non sono standard
                modes.push_back(ExposureMode::Timed);

                // Log warning per debug
                std::cerr << "Warning: ExposureMode node exists but no standard modes found" << std::endl;
            }

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            // In caso di errore GenApi
            std::cerr << "Error getting available exposure modes: " << e.GetDescription() << std::endl;

            // Ritorna almeno la modalità di default
            if (modes.empty()) {
                modes.push_back(ExposureMode::Timed);
            }
        }
        catch (...) {
            // Gestione errori generici
            if (modes.empty()) {
                modes.push_back(ExposureMode::Timed);
            }
        }

        // Rimuovi eventuali duplicati (non dovrebbero essercene, ma per sicurezza)
        std::sort(modes.begin(), modes.end());
        modes.erase(std::unique(modes.begin(), modes.end()), modes.end());

        return modes;
    }

    // === Metodo helper aggiuntivo per verificare se ExposureMode è disponibile ===
    bool GenICamCamera::isExposureModeAvailable() const {
        try {
            if (!isConnected() || !m_pNodeMap) {
                return false;
            }

            GenApi::CNodePtr node = m_pNodeMap->_GetNode("ExposureMode");
            return node.IsValid() &&
                GenApi::IsImplemented(node) &&
                GenApi::IsAvailable(node);

        }
        catch (...) {
            return false;
        }
    }

    // === Metodo helper per convertire ExposureMode in stringa ===
    std::string GenICamCamera::exposureModeToString(ExposureMode mode) const {
        switch (mode) {
        case ExposureMode::Off:
            return "Off";
        case ExposureMode::Timed:
            return "Timed";
        case ExposureMode::TriggerWidth:
            return "TriggerWidth";
        case ExposureMode::TriggerControlled:
            return "TriggerControlled";
        default:
            return "Unknown";
        }
    }

    // === Metodo per ottenere la modalità corrente con gestione errori migliorata ===
    std::vector<ExposureMode> GenICamCamera::getAvailableExposureModesFiltered() const {
        std::vector<ExposureMode> allModes = getAvailableExposureModes();
        std::vector<ExposureMode> filteredModes;

        // Filtra solo le modalità effettivamente utilizzabili
        for (const auto& mode : allModes) {
            try {
                // Salva modalità corrente
                ExposureMode currentMode = getExposureMode();

                // Prova a impostare la modalità
                const_cast<GenICamCamera*>(this)->setExposureMode(mode);

                // Se non lancia eccezione, è utilizzabile
                filteredModes.push_back(mode);

                // Ripristina modalità originale
                const_cast<GenICamCamera*>(this)->setExposureMode(currentMode);

            }
            catch (...) {
                // Modalità non utilizzabile, ignora
            }
        }

        return filteredModes;
    }

    // Modifica i metodi getGain() e setGain() in GenICamCamera.cpp

    bool GenICamCamera::setupGainSelector() const {
        try {
            GenApi::CEnumerationPtr pGainSelector = getEnumerationNode("GainSelector");
            if (pGainSelector.IsValid() && GenApi::IsWritable(pGainSelector)) {
                // Prova in ordine di preferenza
                const std::vector<std::string> preferredSelectors = {
                    "All",          // Gain globale
                    "AnalogAll",    // Gain analogico globale
                    "DigitalAll",   // Gain digitale globale
                    "Tap1",         // Primo tap
                    "Red",          // Per camere a colori
                    "Sensor"        // Gain del sensore
                };

                GenApi::NodeList_t entries;
                pGainSelector->GetEntries(entries);

                // Prova i selettori preferiti
                for (const auto& selector : preferredSelectors) {
                    for (auto& entry : entries) {
                        if (GenApi::IsAvailable(entry) &&
                            entry->GetName().c_str() == selector) {
                            *pGainSelector = selector.c_str();
                            return true;
                        }
                    }
                }

                // Se nessuno dei preferiti è disponibile, usa il primo disponibile
                for (auto& entry : entries) {
                    if (GenApi::IsAvailable(entry)) {
                        *pGainSelector = entry->GetName().c_str();
                        return true;
                    }
                }
            }
        }
        catch (...) {
            // GainSelector non disponibile
        }
        return false;
    }

    // Metodo getGain aggiornato
    double GenICamCamera::getGain() const {
        try {
            // Check cache first
            auto it = m_parameterCache.find("Gain");
            if (it != m_parameterCache.end()) {
                auto now = std::chrono::steady_clock::now();
                if (now - it->second.second < CACHE_TIMEOUT) {
                    return std::stod(it->second.first);
                }
            }

            // Setup GainSelector se necessario
            setupGainSelector();

            // Lista di possibili nomi di gain in ordine di priorità
            const std::vector<std::string> gainNames = {
                "Gain",           // Nome standard SFNC
                "GainRaw",        // Valore raw (integer)
                "GainAbs",        // Valore assoluto in dB (vecchie camere)
                "AnalogGain",     // Gain analogico
                "DigitalGain",    // Gain digitale
                "Brightness"      // Alcune camere USB usano questo
            };

            for (const auto& gainName : gainNames) {
                try {
                    // Prima prova come float
                    GenApi::CFloatPtr pGain = getFloatNode(gainName);
                    if (pGain.IsValid() && GenApi::IsReadable(pGain)) {
                        double value = pGain->GetValue();

                        // Cache il valore
                        m_parameterCache["Gain"] = { std::to_string(value),
                                                   std::chrono::steady_clock::now() };
                        return value;
                    }
                }
                catch (...) {
                    // Se fallisce, prova come integer
                    try {
                        GenApi::CIntegerPtr pGainInt = getIntegerNode(gainName);
                        if (pGainInt.IsValid() && GenApi::IsReadable(pGainInt)) {
                            double value = static_cast<double>(pGainInt->GetValue());

                            // Per GainRaw, potrebbe servire conversione
                            if (gainName == "GainRaw") {
                                try {
                                    // Cerca un fattore di conversione
                                    GenApi::CFloatPtr pGainFactor = getFloatNode("GainFactor");
                                    if (pGainFactor.IsValid() && GenApi::IsReadable(pGainFactor)) {
                                        value *= pGainFactor->GetValue();
                                    }
                                }
                                catch (...) {
                                    // Prova conversione da raw a dB
                                    // Formula tipica: gain_dB = 20 * log10(raw_value / reference)
                                    // Ma senza reference, restituiamo il valore raw
                                }
                            }

                            m_parameterCache["Gain"] = { std::to_string(value),
                                                       std::chrono::steady_clock::now() };
                            return value;
                        }
                    }
                    catch (...) {
                        continue;
                    }
                }
            }

            // Se arriviamo qui, nessun gain trovato
            THROW_GENICAM_ERROR(ErrorType::ParameterError,
                "Parametro Gain non disponibile o non leggibile");

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore lettura gain: ") + e.GetDescription());
        }
    }

    // Metodo setGain aggiornato
    void GenICamCamera::setGain(double gain) {
        try {
            // Setup GainSelector se necessario
            setupGainSelector();

            // Lista di possibili nomi
            const std::vector<std::string> gainNames = {
                "Gain", "GainRaw", "GainAbs", "AnalogGain", "DigitalGain", "Brightness"
            };

            bool gainSet = false;
            std::string lastError;

            for (const auto& gainName : gainNames) {
                try {
                    // Prova come float
                    GenApi::CFloatPtr pGain = getFloatNode(gainName);
                    if (pGain.IsValid() && GenApi::IsWritable(pGain)) {
                        double min = pGain->GetMin();
                        double max = pGain->GetMax();

                        // Verifica se ha incremento
                        double inc = 0.0;
                        try {
                            if (pGain->GetIncMode() != GenApi::noIncrement) {
                                inc = pGain->GetInc();
                            }
                        }
                        catch (...) {
                            inc = 0.0;
                        }

                        // Clamp al range valido
                        if (gain < min) gain = min;
                        if (gain > max) gain = max;

                        // Arrotonda all'incremento se specificato
                        if (inc > 0) {
                            gain = round(gain / inc) * inc;
                        }

                        pGain->SetValue(gain);
                        gainSet = true;
                        notifyParameterChanged("Gain", std::to_string(gain));
                        break;
                    }
                }
                catch (const std::exception& e) {
                    lastError = e.what();

                    // Prova come integer
                    try {
                        GenApi::CIntegerPtr pGainInt = getIntegerNode(gainName);
                        if (pGainInt.IsValid() && GenApi::IsWritable(pGainInt)) {
                            int64_t min = pGainInt->GetMin();
                            int64_t max = pGainInt->GetMax();

                            // Verifica se ha incremento
                            int64_t inc = 1;
                            try {
                                if (pGainInt->GetIncMode() != GenApi::noIncrement) {
                                    inc = pGainInt->GetInc();
                                }
                            }
                            catch (...) {
                                inc = 1;
                            }

                            // Converti a integer
                            int64_t intGain = static_cast<int64_t>(gain);

                            // Per GainRaw, potrebbe servire conversione inversa
                            if (gainName == "GainRaw") {
                                try {
                                    GenApi::CFloatPtr pGainFactor = getFloatNode("GainFactor");
                                    if (pGainFactor.IsValid() && GenApi::IsReadable(pGainFactor)) {
                                        double factor = pGainFactor->GetValue();
                                        if (factor != 0) {
                                            intGain = static_cast<int64_t>(gain / factor);
                                        }
                                    }
                                }
                                catch (...) {}
                            }

                            // Clamp e arrotonda
                            if (intGain < min) intGain = min;
                            if (intGain > max) intGain = max;

                            // Arrotonda all'incremento
                            if (inc > 1) {
                                intGain = (intGain / inc) * inc;
                            }

                            pGainInt->SetValue(intGain);
                            gainSet = true;
                            notifyParameterChanged("Gain", std::to_string(intGain));
                            break;
                        }
                    }
                    catch (const std::exception& e2) {
                        lastError = e2.what();
                        continue;
                    }
                }
            }

            if (!gainSet) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "Impossibile impostare il Gain. Ultimo errore: " + lastError);
            }

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione gain: ") + e.GetDescription());
        }
    }

    void GenICamCamera::notifyParameterChanged(const std::string& parameterName, const std::string& value) {
        std::lock_guard<std::mutex> lock(m_callbackMutex);

        if (m_eventListener) {
           // Notifica asincrona per evitare deadlock
              try {
                 // *****************************************************************************************
                 // // DA RIMETTERE!!!!!!!!!!!!!!!!!!!!!!!
                 //m_eventListener->OnParameterChanged(parameterName, value);
              }
              catch (...) {
                 // Ignora eccezioni nel callback
              }
        }
    }

    // Continuazione di GenICamCamera.cpp

    // === ROI (Region of Interest) ===

    void GenICamCamera::setROI(const ROI& roi) {
        if (m_isAcquiring) {
            THROW_GENICAM_ERROR(ErrorType::AcquisitionError,
                "Impossibile cambiare ROI durante l'acquisizione");
        }

        try {
            // Ottieni limiti del sensore
            GenApi::CIntegerPtr pWidthMax = getIntegerNode("WidthMax");
            GenApi::CIntegerPtr pHeightMax = getIntegerNode("HeightMax");

            int64_t maxWidth = pWidthMax->GetValue();
            int64_t maxHeight = pHeightMax->GetValue();

            if (roi.x + roi.width > static_cast<uint32_t>(maxWidth) ||
                roi.y + roi.height > static_cast<uint32_t>(maxHeight)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "ROI fuori dai limiti del sensore");
            }

            // Ordine importante: prima resetta offset, poi imposta dimensioni
            GenApi::CIntegerPtr pOffsetX = getIntegerNode("OffsetX");
            GenApi::CIntegerPtr pOffsetY = getIntegerNode("OffsetY");
            GenApi::CIntegerPtr pWidth = getIntegerNode("Width");
            GenApi::CIntegerPtr pHeight = getIntegerNode("Height");

            // Reset offset
            if (GenApi::IsWritable(pOffsetX)) pOffsetX->SetValue(0);
            if (GenApi::IsWritable(pOffsetY)) pOffsetY->SetValue(0);

            // Imposta dimensioni
            if (GenApi::IsWritable(pWidth)) {
                int64_t inc = pWidth->GetInc();
                int64_t alignedWidth = (roi.width / inc) * inc;
                pWidth->SetValue(alignedWidth);
            }

            if (GenApi::IsWritable(pHeight)) {
                int64_t inc = pHeight->GetInc();
                int64_t alignedHeight = (roi.height / inc) * inc;
                pHeight->SetValue(alignedHeight);
            }

            // Imposta offset
            if (GenApi::IsWritable(pOffsetX)) {
                int64_t inc = pOffsetX->GetInc();
                int64_t alignedX = (roi.x / inc) * inc;
                pOffsetX->SetValue(alignedX);
            }

            if (GenApi::IsWritable(pOffsetY)) {
                int64_t inc = pOffsetY->GetInc();
                int64_t alignedY = (roi.y / inc) * inc;
                pOffsetY->SetValue(alignedY);
            }

            std::stringstream ss;
            ss << roi.width << "x" << roi.height << "@" << roi.x << "," << roi.y;
            notifyParameterChanged("ROI", ss.str());

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione ROI: ") + e.GetDescription());
        }
    }

    ROI GenICamCamera::getROI() const {
        ROI roi;

        try {
            GenApi::CIntegerPtr pOffsetX = getIntegerNode("OffsetX");
            GenApi::CIntegerPtr pOffsetY = getIntegerNode("OffsetY");
            GenApi::CIntegerPtr pWidth = getIntegerNode("Width");
            GenApi::CIntegerPtr pHeight = getIntegerNode("Height");

            roi.x = static_cast<uint32_t>(pOffsetX->GetValue());
            roi.y = static_cast<uint32_t>(pOffsetY->GetValue());
            roi.width = static_cast<uint32_t>(pWidth->GetValue());
            roi.height = static_cast<uint32_t>(pHeight->GetValue());

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore lettura ROI: ") + e.GetDescription());
        }

        return roi;
    }

    void GenICamCamera::getSensorSize(uint32_t& width, uint32_t& height) const {
        try {
            GenApi::CIntegerPtr pWidthMax = getIntegerNode("WidthMax");
            GenApi::CIntegerPtr pHeightMax = getIntegerNode("HeightMax");

            width = static_cast<uint32_t>(pWidthMax->GetValue());
            height = static_cast<uint32_t>(pHeightMax->GetValue());

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
           e.what();
            // Prova con SensorWidth/SensorHeight
            try {
                GenApi::CIntegerPtr pSensorWidth = getIntegerNode("SensorWidth");
                GenApi::CIntegerPtr pSensorHeight = getIntegerNode("SensorHeight");

                width = static_cast<uint32_t>(pSensorWidth->GetValue());
                height = static_cast<uint32_t>(pSensorHeight->GetValue());
            }
            catch (...) {
                THROW_GENICAM_ERROR(ErrorType::GenApiError,
                    "Impossibile determinare le dimensioni del sensore");
            }
        }
    }

    // === Trigger Mode ===

// === Gestione Trigger Standard SFNC ===

    void GenICamCamera::setTriggerMode(TriggerMode mode) {
        if (m_isAcquiring) {
            THROW_GENICAM_ERROR(ErrorType::AcquisitionError, "Impossibile cambiare trigger durante l'acquisizione");
        }

        try {
            GenApi::CEnumerationPtr pTriggerMode = getEnumerationNode("TriggerMode");

            if (!GenApi::IsWritable(pTriggerMode)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError, "TriggerMode non scrivibile");
            }

            *pTriggerMode = (mode == TriggerMode::On) ? "On" : "Off";
            notifyParameterChanged("TriggerMode", (mode == TriggerMode::On) ? "On" : "Off");

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError, std::string("Errore impostazione TriggerMode: ") + e.GetDescription());
        }
    }

    TriggerMode GenICamCamera::getTriggerMode() const {
        try {
            GenApi::CEnumerationPtr pTriggerMode = getEnumerationNode("TriggerMode");
            std::string value = pTriggerMode->ToString().c_str();

            return (value == "On") ? TriggerMode::On : TriggerMode::Off;

        }
        catch (...) {
            return TriggerMode::Off;
        }
    }


    bool GenICamCamera::isTriggerModeAvailable() const {
        try {
            GenApi::CNodePtr node = m_pNodeMap->_GetNode("TriggerMode");
            return node.IsValid() && GenApi::IsImplemented(node);
        }
        catch (...) {
            return false;
        }
    }

 /**
  * @brief Converte TriggerSource enum in stringa con verifica delle capacità
  *
  * Questa funzione verifica quali valori sono effettivamente supportati dalla
  * telecamera e usa mapping alternativi per massima compatibilità SFNC.
  */
    std::string GenICamCamera::triggerSourceToString(TriggerSource source) const {
       // Se abbiamo già la cache, usala
       if (m_triggerSourceMapCached) {
          auto it = m_triggerSourceMap.find(source);
          if (it != m_triggerSourceMap.end()) {
             return it->second;
          }
       }

       // Mapping standard SFNC con alternative per compatibilità
       static const std::map<TriggerSource, std::vector<std::string>> sourceMap = {
          // Software trigger - molto standard
          {TriggerSource::Software, {"Software", "SoftwareTrigger", "SW"}},

          // Linee hardware - nomi possono variare
          {TriggerSource::Line0, {"Line0", "Line_0", "Input0", "DI0", "TTL_IN0"}},
          {TriggerSource::Line1, {"Line1", "Line_1", "Input1", "DI1", "TTL_IN1"}},
          {TriggerSource::Line2, {"Line2", "Line_2", "Input2", "DI2", "TTL_IN2"}},
          {TriggerSource::Line3, {"Line3", "Line_3", "Input3", "DI3", "TTL_IN3"}},
          {TriggerSource::Line4, {"Line4", "Line_4", "Input4", "DI4"}},
          {TriggerSource::Line5, {"Line5", "Line_5", "Input5", "DI5"}},
          {TriggerSource::Line6, {"Line6", "Line_6", "Input6", "DI6"}},
          {TriggerSource::Line7, {"Line7", "Line_7", "Input7", "DI7"}},

          // Counter end signals
          {TriggerSource::Counter0End, {"Counter0End", "Counter0", "CounterEnd0"}},
          {TriggerSource::Counter1End, {"Counter1End", "Counter1", "CounterEnd1"}},
          {TriggerSource::Counter2End, {"Counter2End", "Counter2", "CounterEnd2"}},

          // Timer end signals
          {TriggerSource::Timer0End, {"Timer0End", "Timer0", "TimerEnd0"}},
          {TriggerSource::Timer1End, {"Timer1End", "Timer1", "TimerEnd1"}},
          {TriggerSource::Timer2End, {"Timer2End", "Timer2", "TimerEnd2"}},

          // User outputs (loopback)
          {TriggerSource::UserOutput0, {"UserOutput0", "Output0", "DO0"}},
          {TriggerSource::UserOutput1, {"UserOutput1", "Output1", "DO1"}},
          {TriggerSource::UserOutput2, {"UserOutput2", "Output2", "DO2"}},

          // Action commands
          {TriggerSource::Action0, {"Action0", "ActionCommand0"}},
          {TriggerSource::Action1, {"Action1", "ActionCommand1"}},

          // Encoder signals
          {TriggerSource::Encoder0, {"Encoder0", "EncoderA", "Encoder"}},
          {TriggerSource::Encoder1, {"Encoder1", "EncoderB"}},

          // Frame triggers
          {TriggerSource::FrameTriggerWait, {"FrameTriggerWait", "FrameTrigger", "ExternalTrigger"}},

          // Exposure triggers  
          {TriggerSource::ExposureActive, {"ExposureActive", "ExposureOut", "FVAL"}}
       };

       // Se TriggerSource non è disponibile, ritorna default
       if (!isParameterAvailable("TriggerSource")) {
          auto it = sourceMap.find(source);
          if (it != sourceMap.end() && !it->second.empty()) {
             return it->second[0]; // Ritorna il nome standard
          }
          return "Software"; // Default
       }

       // Verifica quale valore è effettivamente supportato
       try {
          GenApi::CEnumerationPtr pTriggerSource = getEnumerationNode("TriggerSource");
          GenApi::NodeList_t entries;
          pTriggerSource->GetEntries(entries);

          auto it = sourceMap.find(source);
          if (it != sourceMap.end()) {
             // Cerca il primo valore supportato dalla camera
             for (const auto& value : it->second) {
                for (auto& entry : entries) {
                   if (entry->GetName().c_str() == value && GenApi::IsAvailable(entry)) {
                      return value;
                   }
                }
             }
          }

          // Se non trovato nelle mappature predefinite, 
          // potrebbe essere un valore custom del produttore
          // Costruisci il nome standard e verifica se esiste
          std::string standardName;
          switch (source) {
          case TriggerSource::Software: standardName = "Software"; break;
          case TriggerSource::Line0: standardName = "Line0"; break;
          case TriggerSource::Line1: standardName = "Line1"; break;
          case TriggerSource::Line2: standardName = "Line2"; break;
          case TriggerSource::Line3: standardName = "Line3"; break;
             // ... altri casi standard ...
          default: standardName = "Software";
          }

          // Verifica se il nome standard esiste
          for (auto& entry : entries) {
             std::string entryName = entry->GetName().c_str();
             if (entryName == standardName && GenApi::IsAvailable(entry)) {
                return standardName;
             }
          }

       }
       catch (...) {
          // In caso di errore, usa il default
       }

       // Se tutto fallisce, ritorna il primo valore dalla mappa standard
       auto it = sourceMap.find(source);
       if (it != sourceMap.end() && !it->second.empty()) {
          return it->second[0];
       }

       return "Software"; // Ultimate fallback
    }

       /**
 * @brief Converte stringa in TriggerSource enum
 */
    TriggerSource GenICamCamera::stringToTriggerSource(const std::string& str) const {
       // Mappa inversa per conversione rapida
       static const std::map<std::string, TriggerSource> stringMap = {
          // Software
          {"Software", TriggerSource::Software},
          {"SoftwareTrigger", TriggerSource::Software},
          {"SW", TriggerSource::Software},

          // Linee hardware
          {"Line0", TriggerSource::Line0},
          {"Line_0", TriggerSource::Line0},
          {"Input0", TriggerSource::Line0},
          {"DI0", TriggerSource::Line0},
          {"TTL_IN0", TriggerSource::Line0},

          {"Line1", TriggerSource::Line1},
          {"Line_1", TriggerSource::Line1},
          {"Input1", TriggerSource::Line1},
          {"DI1", TriggerSource::Line1},
          {"TTL_IN1", TriggerSource::Line1},

          {"Line2", TriggerSource::Line2},
          {"Line_2", TriggerSource::Line2},
          {"Input2", TriggerSource::Line2},
          {"DI2", TriggerSource::Line2},

          {"Line3", TriggerSource::Line3},
          {"Line_3", TriggerSource::Line3},
          {"Input3", TriggerSource::Line3},
          {"DI3", TriggerSource::Line3},

          {"Line4", TriggerSource::Line4},
          {"Line_4", TriggerSource::Line4},

          {"Line5", TriggerSource::Line5},
          {"Line_5", TriggerSource::Line5},

          {"Line6", TriggerSource::Line6},
          {"Line_6", TriggerSource::Line6},

          {"Line7", TriggerSource::Line7},
          {"Line_7", TriggerSource::Line7},

          // Counters
          {"Counter0End", TriggerSource::Counter0End},
          {"Counter0", TriggerSource::Counter0End},
          {"Counter1End", TriggerSource::Counter1End},
          {"Counter1", TriggerSource::Counter1End},

          // Timers
          {"Timer0End", TriggerSource::Timer0End},
          {"Timer0", TriggerSource::Timer0End},
          {"Timer1End", TriggerSource::Timer1End},
          {"Timer1", TriggerSource::Timer1End},

          // User outputs
          {"UserOutput0", TriggerSource::UserOutput0},
          {"Output0", TriggerSource::UserOutput0},
          {"DO0", TriggerSource::UserOutput0},

          {"UserOutput1", TriggerSource::UserOutput1},
          {"Output1", TriggerSource::UserOutput1},
          {"DO1", TriggerSource::UserOutput1},

          // Actions
          {"Action0", TriggerSource::Action0},
          {"ActionCommand0", TriggerSource::Action0},
          {"Action1", TriggerSource::Action1},
          {"ActionCommand1", TriggerSource::Action1},

          // Encoder
          {"Encoder0", TriggerSource::Encoder0},
          {"EncoderA", TriggerSource::Encoder0},
          {"Encoder", TriggerSource::Encoder0},

          // Frame/Exposure
          {"FrameTriggerWait", TriggerSource::FrameTriggerWait},
          {"ExposureActive", TriggerSource::ExposureActive}
       };

       auto it = stringMap.find(str);
       if (it != stringMap.end()) {
          return it->second;
       }

       // Se non trovato, prova a dedurre dal pattern
       if (str.find("Software") != std::string::npos) {
          return TriggerSource::Software;
       }

       // Pattern matching per Line
       if (str.find("Line") != std::string::npos ||
          str.find("Input") != std::string::npos ||
          str.find("DI") != std::string::npos) {

          // Estrai il numero
          for (char c : str) {
             if (c >= '0' && c <= '7') {
                int lineNum = c - '0';
                switch (lineNum) {
                case 0: return TriggerSource::Line0;
                case 1: return TriggerSource::Line1;
                case 2: return TriggerSource::Line2;
                case 3: return TriggerSource::Line3;
                case 4: return TriggerSource::Line4;
                case 5: return TriggerSource::Line5;
                case 6: return TriggerSource::Line6;
                case 7: return TriggerSource::Line7;
                }
             }
          }
       }

       // Default
       return TriggerSource::Software;
    }

    /**
 * @brief Cache delle mappature TriggerSource supportate dalla camera
 */
    void GenICamCamera::cacheTriggerSourceMappings() const {
       if (m_triggerSourceMapCached) {
          return; // Già in cache
       }

       m_triggerSourceMap.clear();

       // Lista di tutte le sorgenti da verificare
       const std::vector<TriggerSource> allSources = {
           TriggerSource::Software,
           TriggerSource::Line0, TriggerSource::Line1, TriggerSource::Line2, TriggerSource::Line3,
           TriggerSource::Line4, TriggerSource::Line5, TriggerSource::Line6, TriggerSource::Line7,
           TriggerSource::Counter0End, TriggerSource::Counter1End,
           TriggerSource::Timer0End, TriggerSource::Timer1End,
           TriggerSource::UserOutput0, TriggerSource::UserOutput1, TriggerSource::UserOutput2,
           TriggerSource::Action0, TriggerSource::Action1,
           TriggerSource::Encoder0, TriggerSource::Encoder1,
           TriggerSource::FrameTriggerWait, TriggerSource::ExposureActive
       };

       // Per ogni sorgente, trova il nome supportato
       for (auto source : allSources) {
          std::string mappedName = triggerSourceToString(source);
          if (!mappedName.empty()) {
             m_triggerSourceMap[source] = mappedName;
          }
       }

       m_triggerSourceMapCached = true;
    }

    /**
     * @brief Ottiene la sorgente trigger corrente
     */
    TriggerSource GenICamCamera::getTriggerSource() const {
       try {
          GenApi::CEnumerationPtr pTriggerSource = getEnumerationNode("TriggerSource");
          std::string value = pTriggerSource->ToString().c_str();

          return stringToTriggerSource(value);

       }
       catch (...) {
          return TriggerSource::Software; // Default
       }
    }

    /**
     * @brief Imposta la sorgente trigger con verifica
     */
    void GenICamCamera::setTriggerSource(TriggerSource source) {
       try {
          GenApi::CEnumerationPtr pTriggerSource = getEnumerationNode("TriggerSource");

          if (!GenApi::IsWritable(pTriggerSource)) {
             THROW_GENICAM_ERROR(ErrorType::ParameterError,
                "TriggerSource non scrivibile");
          }

          // Ottieni il nome corretto per questa camera
          std::string sourceStr = triggerSourceToString(source);

          // Verifica che sia effettivamente disponibile
          GenApi::CEnumEntryPtr pEntry = pTriggerSource->GetEntryByName(sourceStr.c_str());
          if (!pEntry.IsValid() || !GenApi::IsAvailable(pEntry)) {
             THROW_GENICAM_ERROR(ErrorType::ParameterError,
                std::string("TriggerSource non supportato: ") + sourceStr);
          }

          *pTriggerSource = sourceStr.c_str();
          notifyParameterChanged("TriggerSource", sourceStr);

       }
       catch (const GenICamException& e) {
          e.what();
          throw; // Ri-solleva
       }
       catch (const GENICAM_NAMESPACE::GenericException& e) {
          THROW_GENICAM_ERROR(ErrorType::GenApiError,
             std::string("Errore impostazione TriggerSource: ") + e.GetDescription());
       }
    }
    /**
 * @brief Ottiene le sorgenti trigger disponibili
 */
    std::vector<TriggerSource> GenICamCamera::getAvailableTriggerSources() const {
       std::vector<TriggerSource> sources;

       try {
          if (!isParameterAvailable("TriggerSource")) {
             // Se non disponibile, assume almeno Software
             sources.push_back(TriggerSource::Software);
             return sources;
          }

          GenApi::CEnumerationPtr pTriggerSource = getEnumerationNode("TriggerSource");
          GenApi::NodeList_t entries;
          pTriggerSource->GetEntries(entries);

          for (auto& entry : entries) {
             if (GenApi::IsAvailable(entry)) {
                std::string name = entry->GetName().c_str();
                TriggerSource source = stringToTriggerSource(name);

                // Evita duplicati
                if (std::find(sources.begin(), sources.end(), source) == sources.end()) {
                   sources.push_back(source);
                }
             }
          }

          // Se nessuna sorgente trovata, aggiungi almeno Software
          if (sources.empty()) {
             sources.push_back(TriggerSource::Software);
          }

       }
       catch (...) {
          // In caso di errore, ritorna almeno Software
          sources.push_back(TriggerSource::Software);
       }

       return sources;
    }

    /**
     * @brief Esegue trigger software con verifica compatibilità
     */
    void GenICamCamera::executeTriggerSoftware() {
       if (!m_isAcquiring) {
          THROW_GENICAM_ERROR(ErrorType::AcquisitionError,
             "Acquisizione non attiva");
       }

       try {
          // Verifica che il trigger software sia abilitato
          TriggerMode mode = getTriggerMode();
          if (mode != TriggerMode::On) {
             THROW_GENICAM_ERROR(ErrorType::InvalidOperation,
                "Trigger non abilitato");
          }

          // Verifica che la sorgente sia Software
          TriggerSource source = getTriggerSource();
          if (source != TriggerSource::Software) {
             THROW_GENICAM_ERROR(ErrorType::InvalidOperation,
                "TriggerSource non impostato a Software");
          }

          // Cerca il comando TriggerSoftware
          std::vector<std::string> commandNames = {
              "TriggerSoftware",      // SFNC standard
              "TriggerSoftwareExecute", // Alcune implementazioni
              "SoftwareTrigger",      // Variante
              "TriggerCmd"            // Legacy
          };

          bool executed = false;
          for (const auto& cmdName : commandNames) {
             try {
                GenApi::CCommandPtr pTriggerCmd = getCommandNode(cmdName);
                if (pTriggerCmd.IsValid() && GenApi::IsWritable(pTriggerCmd)) {
                   pTriggerCmd->Execute();
                   executed = true;
                   break;
                }
             }
             catch (...) {
                continue;
             }
          }

          if (!executed) {
             THROW_GENICAM_ERROR(ErrorType::ParameterError,
                "Comando TriggerSoftware non trovato o non eseguibile");
          }

       }
       catch (const GenICamException& e) {
          e.what();
          throw;
       }
       catch (const GENICAM_NAMESPACE::GenericException& e) {
          THROW_GENICAM_ERROR(ErrorType::GenApiError,
             std::string("Errore esecuzione trigger software: ") + e.GetDescription());
       }
    }
    void GenICamCamera::setTriggerActivation(TriggerActivation activation) {
        try {
            GenApi::CEnumerationPtr pActivation = getEnumerationNode("TriggerActivation");

            if (!GenApi::IsWritable(pActivation)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "TriggerActivation non scrivibile");
            }

            std::string activationStr;
            switch (activation) {
            case TriggerActivation::RisingEdge: activationStr = "RisingEdge"; break;
            case TriggerActivation::FallingEdge: activationStr = "FallingEdge"; break;
            case TriggerActivation::AnyEdge: activationStr = "AnyEdge"; break;
            case TriggerActivation::LevelHigh: activationStr = "LevelHigh"; break;
            case TriggerActivation::LevelLow: activationStr = "LevelLow"; break;
            }

            *pActivation = activationStr.c_str();
            notifyParameterChanged("TriggerActivation", activationStr);

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione TriggerActivation: ") + e.GetDescription());
        }
    }

    TriggerActivation GenICamCamera::getTriggerActivation() const {
        try {
            GenApi::CEnumerationPtr pActivation = getEnumerationNode("TriggerActivation");
            std::string value = pActivation->ToString().c_str();

            if (value == "RisingEdge") return TriggerActivation::RisingEdge;
            if (value == "FallingEdge") return TriggerActivation::FallingEdge;
            if (value == "AnyEdge") return TriggerActivation::AnyEdge;
            if (value == "LevelHigh") return TriggerActivation::LevelHigh;
            if (value == "LevelLow") return TriggerActivation::LevelLow;

            return TriggerActivation::RisingEdge; // Default

        }
        catch (...) {
            return TriggerActivation::RisingEdge;
        }
    }

    /**
     * @brief Abilita trigger software con compatibilità estesa
     */
    void GenICamCamera::enableSoftwareTrigger(bool enable) {
       if (m_isAcquiring) {
          THROW_GENICAM_ERROR(ErrorType::AcquisitionError,
             "Impossibile modificare trigger durante l'acquisizione");
       }

       try {
          // 1. Imposta TriggerSelector se disponibile
          setTriggerSelector(TriggerSelector::FrameStart);

          // 2. Imposta TriggerMode
          setTriggerMode(enable ? TriggerMode::On : TriggerMode::Off);

          if (enable) {
             // 3. Imposta TriggerSource a Software
             setTriggerSource(TriggerSource::Software);

             // 4. Configura parametri opzionali se disponibili
             configureTriggerOptions();
          }

       }
       catch (const GenICamException& e) {
          e.what();
          throw; // Ri-solleva eccezioni GenICam
       }
       catch (const GENICAM_NAMESPACE::GenericException& e) {
          THROW_GENICAM_ERROR(ErrorType::GenApiError,
             std::string("Errore configurazione trigger software: ") + e.GetDescription());
       }
    }

    /**
 * @brief Configura opzioni trigger opzionali
 */
    void GenICamCamera::configureTriggerOptions() {
       // TriggerActivation (opzionale secondo SFNC)
       if (isParameterAvailable("TriggerActivation")) {
          try {
             GenApi::CEnumerationPtr pActivation = getEnumerationNode("TriggerActivation");
             if (GenApi::IsWritable(pActivation)) {
                // Default a RisingEdge per software trigger
                *pActivation = "RisingEdge";
             }
          }
          catch (...) {
             // Non critico
          }
       }

       // TriggerDelay (opzionale)
       if (isParameterAvailable("TriggerDelay")) {
          try {
             GenApi::CFloatPtr pDelay = getFloatNode("TriggerDelay");
             if (GenApi::IsWritable(pDelay)) {
                pDelay->SetValue(0.0); // Nessun delay per default
             }
          }
          catch (...) {
             // Non critico
          }
       }

       // TriggerDivider (opzionale)
       if (isParameterAvailable("TriggerDivider")) {
          try {
             GenApi::CIntegerPtr pDivider = getIntegerNode("TriggerDivider");
             if (GenApi::IsWritable(pDivider)) {
                pDivider->SetValue(1); // Ogni trigger genera un frame
             }
          }
          catch (...) {
             // Non critico
          }
       }
    }

    void GenICamCamera::enableHardwareTrigger(TriggerSource line, TriggerActivation activation) {
        // Verifica che la sorgente sia una linea hardware
        if (line < TriggerSource::Line0 || line > TriggerSource::Line7) {
            THROW_GENICAM_ERROR(ErrorType::ParameterError,
                "La sorgente deve essere una linea hardware (Line0-Line7)");
        }

        setTriggerMode(TriggerMode::On);
        setTriggerSource(line);
        setTriggerActivation(activation);

        std::cout << "Hardware trigger abilitato su linea "
            << static_cast<int>(line) - static_cast<int>(TriggerSource::Line0)
            << std::endl;
    }

    bool GenICamCamera::isTriggerEnabled() const {
        return getTriggerMode() == TriggerMode::On;
    }

    /**
     * @brief Ottiene configurazione trigger come stringa per debug
     */
    std::string GenICamCamera::getTriggerConfiguration() const {
       std::ostringstream oss;

       oss << "=== Configurazione Trigger ===" << std::endl;

       try {
          // TriggerMode
          oss << "TriggerMode: ";
          if (isTriggerModeAvailable()) {
             TriggerMode mode = getTriggerMode();
             oss << (mode == TriggerMode::On ? "On" : "Off") << std::endl;
          }
          else {
             oss << "Non disponibile" << std::endl;
          }

          // TriggerSelector
          oss << "TriggerSelector: ";
          if (isParameterAvailable("TriggerSelector")) {
             try {
                TriggerSelector selector = getTriggerSelector();
                oss << triggerSelectorToString(selector) << std::endl;

                // Mostra tutti i selector disponibili
                auto selectors = getAvailableTriggerSelectors();
                oss << "  Disponibili: ";
                for (size_t i = 0; i < selectors.size(); ++i) {
                   if (i > 0) oss << ", ";
                   oss << triggerSelectorToString(selectors[i]);
                }
                oss << std::endl;
             }
             catch (...) {
                oss << "Errore lettura" << std::endl;
             }
          }
          else {
             oss << "Non disponibile (default: FrameStart)" << std::endl;
          }

          // TriggerSource
          oss << "TriggerSource: ";
          try {
             TriggerSource source = getTriggerSource();
             oss << triggerSourceToString(source) << std::endl;
          }
          catch (...) {
             oss << "Non disponibile" << std::endl;
          }

          // Altri parametri opzionali
          if (isParameterAvailable("TriggerActivation")) {
             oss << "TriggerActivation: " << getParameter("TriggerActivation") << std::endl;
          }

          if (isParameterAvailable("TriggerDelay")) {
             try {
                double delay = getTriggerDelay();
                oss << "TriggerDelay: " << delay << " µs" << std::endl;
             }
             catch (...) {}
          }

       }
       catch (...) {
          oss << "Errore lettura configurazione trigger" << std::endl;
       }

       return oss.str();
    }

    // === Pixel Format ===

    void GenICamCamera::setPixelFormat(PixelFormat format) {
        if (m_isAcquiring) {
            THROW_GENICAM_ERROR(ErrorType::AcquisitionError,
                "Impossibile cambiare formato pixel durante l'acquisizione");
        }

        try {
            GenApi::CEnumerationPtr pPixelFormat = getEnumerationNode("PixelFormat");

            if (!GenApi::IsWritable(pPixelFormat)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "PixelFormat non scrivibile");
            }

            std::string formatString;
            switch (format) {
            case PixelFormat::Mono8: formatString = "Mono8"; break;
            case PixelFormat::Mono10: formatString = "Mono10"; break;
            case PixelFormat::Mono12: formatString = "Mono12"; break;
            case PixelFormat::Mono16: formatString = "Mono16"; break;
            case PixelFormat::RGB8: formatString = "RGB8"; break;
            case PixelFormat::BGR8: formatString = "BGR8"; break;
            case PixelFormat::BayerRG8: formatString = "BayerRG8"; break;
            case PixelFormat::BayerGB8: formatString = "BayerGB8"; break;
            case PixelFormat::BayerGR8: formatString = "BayerGR8"; break;
            case PixelFormat::BayerBG8: formatString = "BayerBG8"; break;
            case PixelFormat::YUV422_8: formatString = "YUV422Packed"; break;
            default:
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "Formato pixel non supportato");
            }

            // Verifica se il formato è disponibile
            GenApi::NodeList_t entries;
            pPixelFormat->GetEntries(entries);
            bool found = false;

            // FIX: Conversione esplicita del nome a std::string
            for (GenApi::NodeList_t::iterator it = entries.begin();
                it != entries.end(); ++it) {
                GenApi::CEnumEntryPtr pEntry(*it);
                if (pEntry.IsValid()) {
                    // Conversione esplicita da gcstring a std::string
                    std::string entryName(pEntry->GetSymbolic().c_str());
                    if (entryName == formatString) {
                        found = true;
                        break;
                    }
                }
            }

            if (!found) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "Formato pixel non disponibile: " + formatString);
            }

            *pPixelFormat = formatString.c_str();
            notifyParameterChanged("PixelFormat", formatString);

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione formato pixel: ") + e.GetDescription());
        }
    }
    /*
    PixelFormat GenICamCamera::getPixelFormat() const {
        try {
            GenApi::CEnumerationPtr pPixelFormat = getEnumerationNode("PixelFormat");
            std::string format = pPixelFormat->ToString().c_str();

            if (format == "Mono8") return PixelFormat::Mono8;
            if (format == "Mono10") return PixelFormat::Mono10;
            if (format == "Mono12") return PixelFormat::Mono12;
            if (format == "Mono16") return PixelFormat::Mono16;
            if (format == "RGB8" || format == "RGB8Packed") return PixelFormat::RGB8;
            if (format == "BGR8" || format == "BGR8Packed") return PixelFormat::BGR8;
            if (format == "BayerRG8") return PixelFormat::BayerRG8;
            if (format == "BayerGB8") return PixelFormat::BayerGB8;
            if (format == "BayerGR8") return PixelFormat::BayerGR8;
            if (format == "BayerBG8") return PixelFormat::BayerBG8;
            if (format == "YUV422Packed" || format == "YUV422_8") return PixelFormat::YUV422_8;

            return PixelFormat::Undefined;

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            return PixelFormat::Undefined;
        }
    }*/

    PixelFormat GenICamCamera::getPixelFormat() const {
       try {
          // Ottieni il nodo PixelFormat secondo SFNC
          GenApi::CEnumerationPtr pixelFormatNode = getEnumerationNode("PixelFormat");
          std::string format = pixelFormatNode->ToString().c_str();
          //GenApi::CEnumerationPtr pixelFormatNode = nodeMap->GetNode("PixelFormat");
          if (!pixelFormatNode || !GenApi::IsReadable(pixelFormatNode)) {
             THROW_GENICAM_ERROR(ErrorType::GenApiError, "Nodo PixelFormat non accessibile");
          }

          // Ottieni il valore corrente dell'enumerazione
          GenApi::IEnumEntry* currentEntry = pixelFormatNode->GetCurrentEntry();
          if (!currentEntry) {
             THROW_GENICAM_ERROR(ErrorType::GenApiError, "Impossibile ottenere il formato pixel corrente");
          }

          // Ottieni il valore numerico del formato pixel (PFNC value)
          int64_t pfncValue = currentEntry->GetValue();

          // Log per debug
          std::string formatName = currentEntry->GetSymbolic().c_str();
          //logMessage("Formato pixel corrente: " + formatName + " (PFNC: 0x" + toHexString(pfncValue) + ")");

          // Converti dal valore PFNC al nostro enum PixelFormat
          PixelFormat pixFormat = convertFromGenICamPixelFormat(static_cast<uint64_t>(pfncValue));

          // Se il formato non è riconosciuto, proviamo a dedurlo dal nome simbolico
          if (pixFormat == GenICamWrapper::PixelFormat::Undefined) {
             pixFormat = getPixelFormatFromSymbolicName(formatName);

             if (pixFormat != PixelFormat::Undefined) {
                THROW_GENICAM_ERROR(ErrorType::GenApiError, "Formato dedotto dal nome simbolico: " + formatName);
             }
             else {
                THROW_GENICAM_ERROR(ErrorType::GenApiError, "ATTENZIONE: Formato pixel non riconosciuto: " + formatName + " (PFNC: 0x" + toHexString(pfncValue) + ")");
             }
          }

          return pixFormat;
       }
       catch (const GENICAM_NAMESPACE::GenericException& e) {
          THROW_GENICAM_ERROR(ErrorType::GenApiError, "Errore GenApi in getPixelFormat: " + std::string(e.what()));
       }
       catch (const std::exception& e) {
          THROW_GENICAM_ERROR(ErrorType::GenApiError, "Errore in getPixelFormat: " + std::string(e.what()));
       }
    }

    // Funzione helper per dedurre il formato dal nome simbolico
    PixelFormat GenICamCamera::getPixelFormatFromSymbolicName(const std::string& name) const {
       // Mappa dei nomi simbolici ai formati
       static const std::map<std::string, PixelFormat> symbolMap = {
          // Formati monocromatici
          {"Mono8", PixelFormat::Mono8},
          {"Mono10", PixelFormat::Mono10},
          {"Mono12", PixelFormat::Mono12},
          {"Mono14", PixelFormat::Mono14},
          {"Mono16", PixelFormat::Mono16},

          // Formati monocromatici packed
          {"Mono10Packed", PixelFormat::Mono10Packed},
          {"Mono12Packed", PixelFormat::Mono12Packed},
          {"Mono10p", PixelFormat::Mono10Packed},  // Alias comune
          {"Mono12p", PixelFormat::Mono12Packed},  // Alias comune

          // Formati RGB/BGR 8 bit
          {"RGB8", PixelFormat::RGB8},
          {"BGR8", PixelFormat::BGR8},
          {"RGBa8", PixelFormat::RGBa8},
          {"BGRa8", PixelFormat::BGRa8},
          {"RGB8Packed", PixelFormat::RGB8},  // Alias
          {"BGR8Packed", PixelFormat::BGR8},  // Alias

          // Formati RGB/BGR 10/12/16 bit
          {"RGB10", PixelFormat::RGB10},
          {"BGR10", PixelFormat::BGR10},
          {"RGB12", PixelFormat::RGB12},
          {"BGR12", PixelFormat::BGR12},
          {"RGB16", PixelFormat::RGB16},
          {"BGR16", PixelFormat::BGR16},

          // Formati Bayer 8 bit
          {"BayerGR8", PixelFormat::BayerGR8},
          {"BayerRG8", PixelFormat::BayerRG8},
          {"BayerGB8", PixelFormat::BayerGB8},
          {"BayerBG8", PixelFormat::BayerBG8},

          // Formati Bayer 10 bit
          {"BayerGR10", PixelFormat::BayerGR10},
          {"BayerRG10", PixelFormat::BayerRG10},
          {"BayerGB10", PixelFormat::BayerGB10},
          {"BayerBG10", PixelFormat::BayerBG10},

          // Formati Bayer 12 bit
          {"BayerGR12", PixelFormat::BayerGR12},
          {"BayerRG12", PixelFormat::BayerRG12},
          {"BayerGB12", PixelFormat::BayerGB12},
          {"BayerBG12", PixelFormat::BayerBG12},

          // Formati Bayer 16 bit
          {"BayerGR16", PixelFormat::BayerGR16},
          {"BayerRG16", PixelFormat::BayerRG16},
          {"BayerGB16", PixelFormat::BayerGB16},
          {"BayerBG16", PixelFormat::BayerBG16},

          // Formati Bayer packed
          {"BayerGR10Packed", PixelFormat::BayerGR10Packed},
          {"BayerRG10Packed", PixelFormat::BayerRG10Packed},
          {"BayerGB10Packed", PixelFormat::BayerGB10Packed},
          {"BayerBG10Packed", PixelFormat::BayerBG10Packed},
          {"BayerGR12Packed", PixelFormat::BayerGR12Packed},
          {"BayerRG12Packed", PixelFormat::BayerRG12Packed},
          {"BayerGB12Packed", PixelFormat::BayerGB12Packed},
          {"BayerBG12Packed", PixelFormat::BayerBG12Packed},

          // Alias comuni per formati Bayer packed
          {"BayerGR10p", PixelFormat::BayerGR10Packed},
          {"BayerRG10p", PixelFormat::BayerRG10Packed},
          {"BayerGB10p", PixelFormat::BayerGB10Packed},
          {"BayerBG10p", PixelFormat::BayerBG10Packed},
          {"BayerGR12p", PixelFormat::BayerGR12Packed},
          {"BayerRG12p", PixelFormat::BayerRG12Packed},
          {"BayerGB12p", PixelFormat::BayerGB12Packed},
          {"BayerBG12p", PixelFormat::BayerBG12Packed},

          // Formati YUV
          {"YUV422_8", PixelFormat::YUV422_8},
          {"YUV422_8_UYVY", PixelFormat::YUV422_8_UYVY},
          {"YUV422_8_YUYV", PixelFormat::YUV422_8_YUYV},
          {"YUV444_8", PixelFormat::YUV444_8},
          {"YCbCr422_8", PixelFormat::YUV422_8},  // Alias
          {"UYVY", PixelFormat::YUV422_8_UYVY},   // Alias
          {"YUYV", PixelFormat::YUV422_8_YUYV},   // Alias
          {"YUY2", PixelFormat::YUV422_8_YUYV},   // Alias comune

          // Formati 3D e speciali
          {"Coord3D_ABC32f", PixelFormat::Coord3D_ABC32f},
          {"Coord3D_ABC16", PixelFormat::Coord3D_ABC16},
          {"Confidence8", PixelFormat::Confidence8},
          {"Confidence16", PixelFormat::Confidence16}
       };

       auto it = symbolMap.find(name);
       if (it != symbolMap.end()) {
          return it->second;
       }

       return PixelFormat::Undefined;
    }

    // Funzione helper per ottenere informazioni dettagliate sul formato pixel
    PixelFormatInfo GenICamCamera::getPixelFormatInfo() const {
       PixelFormatInfo info;

       try {
          info.format = getPixelFormat();

          if (info.format == PixelFormat::Undefined) {
             info.isValid = false;
             return info;
          }

          // Ottieni informazioni aggiuntive dal nodo
          GenApi::CEnumerationPtr pixelFormatNode = getEnumerationNode("PixelFormat");
          if (pixelFormatNode && GenApi::IsReadable(pixelFormatNode)) {
             GenApi::IEnumEntry* currentEntry = pixelFormatNode->GetCurrentEntry();
             if (currentEntry) {
                info.name = currentEntry->GetSymbolic().c_str();
                info.pfncValue = static_cast<uint64_t>(currentEntry->GetValue());
                info.format = convertFromGenICamPixelFormat(info.pfncValue);

                info.isValid = true;
             }
          }

          // Calcola bytes per pixel
          switch (info.format) {
          case PixelFormat::Mono8:
          case PixelFormat::BayerGR8:
          case PixelFormat::BayerRG8:
          case PixelFormat::BayerGB8:
          case PixelFormat::BayerBG8:
          case PixelFormat::Confidence8:
             info.bytesPerPixel = 1.0;
             info.bitsPerPixel = 8;
             break;

          case PixelFormat::Mono10Packed:
          case PixelFormat::BayerGR10Packed:
          case PixelFormat::BayerRG10Packed:
          case PixelFormat::BayerGB10Packed:
          case PixelFormat::BayerBG10Packed:
             info.bytesPerPixel = 1.25;  // 10 bits = 1.25 bytes
             info.bitsPerPixel = 10;
             info.isPacked = true;
             break;

          case PixelFormat::Mono12Packed:
          case PixelFormat::BayerGR12Packed:
          case PixelFormat::BayerRG12Packed:
          case PixelFormat::BayerGB12Packed:
          case PixelFormat::BayerBG12Packed:
             info.bytesPerPixel = 1.5;   // 12 bits = 1.5 bytes
             info.bitsPerPixel = 12;
             info.isPacked = true;
             break;

          case PixelFormat::Mono10:
          case PixelFormat::Mono12:
          case PixelFormat::Mono14:
          case PixelFormat::Mono16:
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
          case PixelFormat::Confidence16:
             info.bytesPerPixel = 2.0;
             info.bitsPerPixel = 16;
             break;

          case PixelFormat::RGB8:
          case PixelFormat::BGR8:
          case PixelFormat::YUV444_8:
             info.bytesPerPixel = 3.0;
             info.bitsPerPixel = 24;
             break;

          case PixelFormat::RGBa8:
          case PixelFormat::BGRa8:
             info.bytesPerPixel = 4.0;
             info.bitsPerPixel = 32;
             break;

          case PixelFormat::YUV422_8:
          case PixelFormat::YUV422_8_UYVY:
          case PixelFormat::YUV422_8_YUYV:
             info.bytesPerPixel = 2.0;
             info.bitsPerPixel = 16;
             break;

          case PixelFormat::RGB10:
          case PixelFormat::BGR10:
          case PixelFormat::RGB12:
          case PixelFormat::BGR12:
          case PixelFormat::RGB16:
          case PixelFormat::BGR16:
          case PixelFormat::Coord3D_ABC16:
             info.bytesPerPixel = 6.0;
             info.bitsPerPixel = 48;
             break;

          case PixelFormat::Coord3D_ABC32f:
             info.bytesPerPixel = 12.0;
             info.bitsPerPixel = 96;
             break;

          default:
             info.bytesPerPixel = 0.0;
             info.bitsPerPixel = 0;
             break;
          }

          // Determina se è un formato Bayer
          info.isBayer = (info.format >= PixelFormat::BayerGR8 &&
             info.format <= PixelFormat::BayerBG12Packed);

          // Determina se è un formato colore
          info.isColor = info.isBayer ||
             (info.format >= PixelFormat::RGB8 && info.format <= PixelFormat::BGR16) ||
             (info.format >= PixelFormat::YUV422_8 && info.format <= PixelFormat::YUV444_8);

       }
       catch (const std::exception& e) {
          e.what();
          info.isValid = false;
          info.format = PixelFormat::Undefined;
       }

       return info;
    }

    // Funzione helper per convertire valore esadecimale in stringa
    std::string GenICamCamera::toHexString(uint64_t value) const {
       std::stringstream ss;
       ss << std::hex << std::uppercase << value;
       return ss.str();
    }

    std::vector<PixelFormat> GenICamCamera::getAvailablePixelFormats() const {
        std::vector<PixelFormat> formats;

        try {
            GenApi::CEnumerationPtr pPixelFormat = getEnumerationNode("PixelFormat");
            GenApi::NodeList_t entries;
            pPixelFormat->GetEntries(entries);

            for (auto& entry : entries) {
                if (GenApi::IsAvailable(entry)) {
                    std::string name = entry->GetName().c_str();

                    if (name == "Mono8") formats.push_back(PixelFormat::Mono8);
                    else if (name == "Mono10") formats.push_back(PixelFormat::Mono10);
                    else if (name == "Mono12") formats.push_back(PixelFormat::Mono12);
                    else if (name == "Mono16") formats.push_back(PixelFormat::Mono16);
                    else if (name == "RGB8" || name == "RGB8Packed")
                        formats.push_back(PixelFormat::RGB8);
                    else if (name == "BGR8" || name == "BGR8Packed")
                        formats.push_back(PixelFormat::BGR8);
                    else if (name == "BayerRG8") formats.push_back(PixelFormat::BayerRG8);
                    else if (name == "BayerGB8") formats.push_back(PixelFormat::BayerGB8);
                    else if (name == "BayerGR8") formats.push_back(PixelFormat::BayerGR8);
                    else if (name == "BayerBG8") formats.push_back(PixelFormat::BayerBG8);
                    else if (name == "YUV422Packed") formats.push_back(PixelFormat::YUV422_8);
                }
            }

        }
        catch (...) {
            // Return empty vector
        }

        return formats;
    }

    // === Frame Rate ===

    void GenICamCamera::setFrameRate(double fps) {
        try {
            GenApi::CFloatPtr pFrameRate;

            // Prova diversi nomi possibili
            const std::vector<std::string> frameRateNames = {
                "AcquisitionFrameRate", "FrameRate", "AcquisitionFrameRateAbs"
            };

            for (const auto& name : frameRateNames) {
                try {
                    pFrameRate = getFloatNode(name);
                    if (GenApi::IsWritable(pFrameRate)) {
                        break;
                    }
                }
                catch (...) {
                    continue;
                }
            }

            if (!pFrameRate.IsValid() || !GenApi::IsWritable(pFrameRate)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,"Frame rate non disponibile o non scrivibile");
            }

            double min = pFrameRate->GetMin();
            double max = pFrameRate->GetMax();

            if (fps < min || fps > max) {
                std::stringstream ss;
                ss << "Frame rate fuori range [" << min << ", " << max << "]";
                THROW_GENICAM_ERROR(ErrorType::ParameterError, ss.str());
            }

            pFrameRate->SetValue(fps);
            notifyParameterChanged("FrameRate", std::to_string(fps));

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione frame rate: ") + e.GetDescription());
        }
    }

    double GenICamCamera::getFrameRate() const {
        try {
            const std::vector<std::string> frameRateNames = {
                "AcquisitionFrameRate", "FrameRate", "AcquisitionFrameRateAbs"
            };

            for (const auto& name : frameRateNames) {
                try {
                    GenApi::CFloatPtr pFrameRate = getFloatNode(name);
                    if (GenApi::IsReadable(pFrameRate)) {
                        return pFrameRate->GetValue();
                    }
                }
                catch (...) {
                    continue;
                }
            }

            THROW_GENICAM_ERROR(ErrorType::ParameterError,
                "Frame rate non disponibile");

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore lettura frame rate: ") + e.GetDescription());
        }
    }

    bool GenICamCamera::isFrameRateAvailable() const {
        const std::vector<std::string> frameRateNames = {
            "AcquisitionFrameRate", "FrameRate", "AcquisitionFrameRateAbs"
        };

        for (const auto& name : frameRateNames) {
            try {
                GenApi::CNodePtr node = m_pNodeMap->_GetNode(name.c_str());
                if (node.IsValid() && GenApi::IsImplemented(node)) {
                    return true;
                }
            }
            catch (...) {
                continue;
            }
        }

        return false;
    }

    // === Acquisition Mode Implementation ===

    void GenICamCamera::setAcquisitionMode(AcquisitionMode mode) {
        if (m_isAcquiring) {
            THROW_GENICAM_ERROR(ErrorType::AcquisitionError,
                "Impossibile cambiare AcquisitionMode durante l'acquisizione");
        }

        try {
            GenApi::CEnumerationPtr pAcqMode = getEnumerationNode("AcquisitionMode");

            if (!GenApi::IsWritable(pAcqMode)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "AcquisitionMode non scrivibile");
            }

            std::string modeString;
            switch (mode) {
            case AcquisitionMode::SingleFrame:
                modeString = "SingleFrame";
                break;
            case AcquisitionMode::MultiFrame:
                modeString = "MultiFrame";
                break;
            case AcquisitionMode::Continuous:
                modeString = "Continuous";
                break;
            default:
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "Modalità di acquisizione non valida");
            }
            // Imposta il valore
            *pAcqMode = modeString.c_str();
            // Notifica il cambio
            notifyParameterChanged("AcquisitionMode", modeString);

            // Se stiamo passando a MultiFrame, potrebbe essere necessario 
            // impostare anche AcquisitionFrameCount
            if (mode == AcquisitionMode::MultiFrame) {
                try {
                    // Verifica se AcquisitionFrameCount esiste ed è configurabile
                    if (isParameterAvailable("AcquisitionFrameCount") &&
                        isParameterWritable("AcquisitionFrameCount")) {
                        // Se non è già impostato, usa un default
                        GenApi::CIntegerPtr pFrameCount = getIntegerNode("AcquisitionFrameCount");
                        if (pFrameCount->GetValue() == 0) {
                            pFrameCount->SetValue(10); // Default 10 frame
                        }
                    }
                }
                catch (...) {
                    // Non critico se AcquisitionFrameCount non è disponibile
                }
            }

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione AcquisitionMode: ") + e.GetDescription());
        }
    }

    AcquisitionMode GenICamCamera::getAcquisitionMode() const {
        try {
            // Check cache first
            auto it = m_parameterCache.find("AcquisitionMode");
            if (it != m_parameterCache.end()) {
                auto now = std::chrono::steady_clock::now();
                if (now - it->second.second < CACHE_TIMEOUT) {
                    std::string cachedValue = it->second.first;

                    if (cachedValue == "SingleFrame") return AcquisitionMode::SingleFrame;
                    if (cachedValue == "MultiFrame") return AcquisitionMode::MultiFrame;
                    if (cachedValue == "Continuous") return AcquisitionMode::Continuous;
                }
            }

            GenApi::CEnumerationPtr pAcqMode = getEnumerationNode("AcquisitionMode");

            if (!GenApi::IsReadable(pAcqMode)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "AcquisitionMode non leggibile");
            }

            std::string modeString = pAcqMode->ToString().c_str();

            // Update cache
            m_parameterCache["AcquisitionMode"] = { modeString,
                                                    std::chrono::steady_clock::now() };

            // Converti stringa in enum
            if (modeString == "SingleFrame") return AcquisitionMode::SingleFrame;
            if (modeString == "MultiFrame") return AcquisitionMode::MultiFrame;
            if (modeString == "Continuous") return AcquisitionMode::Continuous;

            // Se arriviamo qui, la camera ha un valore non standard
            // Default a Continuous
            return AcquisitionMode::Continuous;

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore lettura AcquisitionMode: ") + e.GetDescription());
        }
    }

    bool GenICamCamera::isAcquisitionModeAvailable() const {
        try {
            GenApi::CNodePtr node = m_pNodeMap->_GetNode("AcquisitionMode");
            return node.IsValid() && GenApi::IsImplemented(node);
        }
        catch (...) {
            return false;
        }
    }

    std::vector<AcquisitionMode> GenICamCamera::getAvailableAcquisitionModes() const {
        std::vector<AcquisitionMode> modes;

        try {
            GenApi::CEnumerationPtr pAcqMode = getEnumerationNode("AcquisitionMode");
            GenApi::NodeList_t entries;
            pAcqMode->GetEntries(entries);

            for (auto& entry : entries) {
                if (GenApi::IsAvailable(entry)) {
                    std::string name = entry->GetName().c_str();

                    if (name == "SingleFrame") {
                        modes.push_back(AcquisitionMode::SingleFrame);
                    }
                    else if (name == "MultiFrame") {
                        modes.push_back(AcquisitionMode::MultiFrame);
                    }
                    else if (name == "Continuous") {
                        modes.push_back(AcquisitionMode::Continuous);
                    }
                }
            }
        }
        catch (...) {
            // Se c'è un errore, ritorna vector vuoto
        }

        return modes;
    }

    // === Helper function per debug ===
    std::string GenICamCamera::getAcquisitionModeString(AcquisitionMode mode) {
        switch (mode) {
        case AcquisitionMode::SingleFrame: return "SingleFrame";
        case AcquisitionMode::MultiFrame: return "MultiFrame";
        case AcquisitionMode::Continuous: return "Continuous";
        default: return "Unknown";
        }
    }

    // === Utilities e Helper ===

    cv::Mat GenICamCamera::convertBufferToMat(void* buffer, size_t size, uint32_t width, uint32_t height, PixelFormat format) const {
       if (!buffer || size == 0 || width == 0 || height == 0) {
          return cv::Mat();
       }

       cv::Mat resultMat;

       switch (format) {
          // Formati monocromatici standard
       case PixelFormat::Mono8:
       {
          size_t expectedSize = width * height;
          if (size < expectedSize) return cv::Mat();
          resultMat = cv::Mat(height, width, CV_8UC1, buffer).clone();
          break;
       }

       case PixelFormat::Mono10:
       case PixelFormat::Mono12:
       case PixelFormat::Mono14:
       case PixelFormat::Mono16:
       {
          size_t expectedSize = width * height * 2;
          if (size < expectedSize) return cv::Mat();
          resultMat = cv::Mat(height, width, CV_16UC1, buffer).clone();
          break;
       }

       // Formati monocromatici packed
       case PixelFormat::Mono10Packed:
       {
          // 4 pixel in 5 byte
          size_t packedSize = ((width * height * 10 + 7) / 8);
          if (size < packedSize) return cv::Mat();

          cv::Mat unpackedMat(height, width, CV_16UC1);
          unpackMono10Packed(static_cast<uint8_t*>(buffer), reinterpret_cast<uint16_t*>(unpackedMat.data), width, height);
          resultMat = unpackedMat;
          break;
       }

       case PixelFormat::Mono12Packed:
       {
          // 2 pixel in 3 byte
          size_t packedSize = ((width * height * 12 + 7) / 8);
          if (size < packedSize) return cv::Mat();

          cv::Mat unpackedMat(height, width, CV_16UC1);
          unpackMono12Packed(static_cast<uint8_t*>(buffer), reinterpret_cast<uint16_t*>(unpackedMat.data), width, height);
          resultMat = unpackedMat;
          break;
       }

       // Formati RGB/BGR 8 bit
       case PixelFormat::RGB8:
       {
          size_t expectedSize = width * height * 3;
          if (size < expectedSize) return cv::Mat();
          cv::Mat rgbMat(height, width, CV_8UC3, buffer);
          cv::cvtColor(rgbMat, resultMat, cv::COLOR_RGB2BGR);
          break;
       }

       case PixelFormat::BGR8:
       {
          size_t expectedSize = width * height * 3;
          if (size < expectedSize) return cv::Mat();
          resultMat = cv::Mat(height, width, CV_8UC3, buffer).clone();
          break;
       }

       case PixelFormat::RGBa8:
       {
          size_t expectedSize = width * height * 4;
          if (size < expectedSize) return cv::Mat();
          cv::Mat rgbaMat(height, width, CV_8UC4, buffer);
          cv::cvtColor(rgbaMat, resultMat, cv::COLOR_RGBA2BGRA);
          break;
       }

       case PixelFormat::BGRa8:
       {
          size_t expectedSize = width * height * 4;
          if (size < expectedSize) return cv::Mat();
          resultMat = cv::Mat(height, width, CV_8UC4, buffer).clone();
          break;
       }

       // Formati RGB/BGR 10/12/16 bit
       case PixelFormat::RGB10:
       case PixelFormat::RGB12:
       case PixelFormat::RGB16:
       {
          size_t expectedSize = width * height * 3 * 2;
          if (size < expectedSize) return cv::Mat();
          cv::Mat rgbMat(height, width, CV_16UC3, buffer);
          cv::cvtColor(rgbMat, resultMat, cv::COLOR_RGB2BGR);
          break;
       }

       case PixelFormat::BGR10:
       case PixelFormat::BGR12:
       case PixelFormat::BGR16:
       {
          size_t expectedSize = width * height * 3 * 2;
          if (size < expectedSize) return cv::Mat();
          resultMat = cv::Mat(height, width, CV_16UC3, buffer).clone();
          break;
       }

       // Formati Bayer 8 bit
       case PixelFormat::BayerGR8:
       case PixelFormat::BayerRG8:
       case PixelFormat::BayerGB8:
       case PixelFormat::BayerBG8:
       {
          size_t expectedSize = width * height;
          if (size < expectedSize) return cv::Mat();
          cv::Mat bayerMat(height, width, CV_8UC1, buffer);

          // Converti Bayer a BGR usando il pattern appropriato
          int conversionCode;
          switch (format) {
          case PixelFormat::BayerGR8: conversionCode = cv::COLOR_BayerGR2BGR; break;
          case PixelFormat::BayerRG8: conversionCode = cv::COLOR_BayerRG2BGR; break;
          case PixelFormat::BayerGB8: conversionCode = cv::COLOR_BayerGB2BGR; break;
          case PixelFormat::BayerBG8: conversionCode = cv::COLOR_BayerBG2BGR; break;
          default: return cv::Mat();
          }
          cv::cvtColor(bayerMat, resultMat, conversionCode);
          break;
       }

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
       {
          size_t expectedSize = width * height * 2;
          if (size < expectedSize) return cv::Mat();
          cv::Mat bayerMat(height, width, CV_16UC1, buffer);

          // Determina il codice di conversione corretto
          int conversionCode;
          if (format == PixelFormat::BayerGR10 || format == PixelFormat::BayerGR12 || format == PixelFormat::BayerGR16) {
             conversionCode = cv::COLOR_BayerGR2BGR;
          }
          else if (format == PixelFormat::BayerRG10 || format == PixelFormat::BayerRG12 || format == PixelFormat::BayerRG16) {
             conversionCode = cv::COLOR_BayerRG2BGR;
          }
          else if (format == PixelFormat::BayerGB10 || format == PixelFormat::BayerGB12 || format == PixelFormat::BayerGB16) {
             conversionCode = cv::COLOR_BayerGB2BGR;
          }
          else {
             conversionCode = cv::COLOR_BayerBG2BGR;
          }

          // Converti a 8 bit per la demosaicizzazione
          cv::Mat bayer8bit;
          bayerMat.convertTo(bayer8bit, CV_8U, 255.0 / 65535.0);
          cv::cvtColor(bayer8bit, resultMat, conversionCode);
          break;
       }

       // Formati Bayer packed
       case PixelFormat::BayerGR10Packed:
       case PixelFormat::BayerRG10Packed:
       case PixelFormat::BayerGB10Packed:
       case PixelFormat::BayerBG10Packed:
       {
          size_t packedSize = ((width * height * 10 + 7) / 8);
          if (size < packedSize) return cv::Mat();

          cv::Mat unpackedMat(height, width, CV_16UC1);
          unpackMono10Packed(static_cast<uint8_t*>(buffer),
             reinterpret_cast<uint16_t*>(unpackedMat.data),
             width, height);

          // Converti Bayer a BGR
          int conversionCode;
          switch (format) {
          case PixelFormat::BayerGR10Packed: conversionCode = cv::COLOR_BayerGR2BGR; break;
          case PixelFormat::BayerRG10Packed: conversionCode = cv::COLOR_BayerRG2BGR; break;
          case PixelFormat::BayerGB10Packed: conversionCode = cv::COLOR_BayerGB2BGR; break;
          case PixelFormat::BayerBG10Packed: conversionCode = cv::COLOR_BayerBG2BGR; break;
          default: return cv::Mat();
          }

          cv::Mat bayer8bit;
          unpackedMat.convertTo(bayer8bit, CV_8U, 255.0 / 1023.0);
          cv::cvtColor(bayer8bit, resultMat, conversionCode);
          break;
       }

       case PixelFormat::BayerGR12Packed:
       case PixelFormat::BayerRG12Packed:
       case PixelFormat::BayerGB12Packed:
       case PixelFormat::BayerBG12Packed:
       {
          size_t packedSize = ((width * height * 12 + 7) / 8);
          if (size < packedSize) return cv::Mat();

          cv::Mat unpackedMat(height, width, CV_16UC1);
          unpackMono12Packed(static_cast<uint8_t*>(buffer),
             reinterpret_cast<uint16_t*>(unpackedMat.data),
             width, height);

          // Converti Bayer a BGR
          int conversionCode;
          switch (format) {
          case PixelFormat::BayerGR12Packed: conversionCode = cv::COLOR_BayerGR2BGR; break;
          case PixelFormat::BayerRG12Packed: conversionCode = cv::COLOR_BayerRG2BGR; break;
          case PixelFormat::BayerGB12Packed: conversionCode = cv::COLOR_BayerGB2BGR; break;
          case PixelFormat::BayerBG12Packed: conversionCode = cv::COLOR_BayerBG2BGR; break;
          default: return cv::Mat();
          }

          cv::Mat bayer8bit;
          unpackedMat.convertTo(bayer8bit, CV_8U, 255.0 / 4095.0);
          cv::cvtColor(bayer8bit, resultMat, conversionCode);
          break;
       }

       // Formati YUV
       case PixelFormat::YUV422_8:
       case PixelFormat::YUV422_8_UYVY:
       {
          size_t expectedSize = width * height * 2;
          if (size < expectedSize) return cv::Mat();
          cv::Mat yuvMat(height, width, CV_8UC2, buffer);
          cv::cvtColor(yuvMat, resultMat, cv::COLOR_YUV2BGR_UYVY);
          break;
       }

       case PixelFormat::YUV422_8_YUYV:
       {
          size_t expectedSize = width * height * 2;
          if (size < expectedSize) return cv::Mat();
          cv::Mat yuvMat(height, width, CV_8UC2, buffer);
          cv::cvtColor(yuvMat, resultMat, cv::COLOR_YUV2BGR_YUYV);
          break;
       }

       case PixelFormat::YUV444_8:
       {
          size_t expectedSize = width * height * 3;
          if (size < expectedSize) return cv::Mat();
          cv::Mat yuvMat(height, width, CV_8UC3, buffer);
          cv::cvtColor(yuvMat, resultMat, cv::COLOR_YUV2BGR);
          break;
       }

       // Formati 3D
       case PixelFormat::Coord3D_ABC32f:
       {
          size_t expectedSize = width * height * 3 * sizeof(float);
          if (size < expectedSize) return cv::Mat();
          resultMat = cv::Mat(height, width, CV_32FC3, buffer).clone();
          break;
       }

       case PixelFormat::Coord3D_ABC16:
       {
          size_t expectedSize = width * height * 3 * sizeof(uint16_t);
          if (size < expectedSize) return cv::Mat();
          resultMat = cv::Mat(height, width, CV_16UC3, buffer).clone();
          break;
       }

       case PixelFormat::Confidence8:
       {
          size_t expectedSize = width * height;
          if (size < expectedSize) return cv::Mat();
          resultMat = cv::Mat(height, width, CV_8UC1, buffer).clone();
          break;
       }

       case PixelFormat::Confidence16:
       {
          size_t expectedSize = width * height * 2;
          if (size < expectedSize) return cv::Mat();
          resultMat = cv::Mat(height, width, CV_16UC1, buffer).clone();
          break;
       }

       default:
          // Formato non supportato
          return cv::Mat();
       }

       return resultMat;
    }

    // Funzioni helper per la decompressione
    void GenICamCamera::unpackMono10Packed(const uint8_t* src, uint16_t* dst, uint32_t width, uint32_t height) const {
       for (uint32_t y = 0; y < height; ++y) {
          const uint8_t* srcRow = src + y * ((width * 10 + 7) / 8);
          uint16_t* dstRow = dst + y * width;

          for (uint32_t x = 0; x < width; x += 4) {
             if (x + 3 < width) {
                // Decompressione gruppo completo di 4 pixel
                dstRow[x] = (static_cast<uint16_t>(srcRow[0]) << 2) | (srcRow[1] >> 6);
                dstRow[x + 1] = ((static_cast<uint16_t>(srcRow[1]) & 0x3F) << 4) | (srcRow[2] >> 4);
                dstRow[x + 2] = ((static_cast<uint16_t>(srcRow[2]) & 0x0F) << 6) | (srcRow[3] >> 2);
                dstRow[x + 3] = ((static_cast<uint16_t>(srcRow[3]) & 0x03) << 8) | srcRow[4];
                srcRow += 5;
             }
             else {
                // Gestione pixel rimanenti
                for (uint32_t i = x; i < width; ++i) {
                   dstRow[i] = static_cast<uint16_t>(*srcRow++) << 2;
                }
             }
          }
       }
    }

    void GenICamCamera::unpackMono12Packed(const uint8_t* src, uint16_t* dst, uint32_t width, uint32_t height) const {
       for (uint32_t y = 0; y < height; ++y) {
          const uint8_t* srcRow = src + y * ((width * 12 + 7) / 8);
          uint16_t* dstRow = dst + y * width;

          for (uint32_t x = 0; x < width; x += 2) {
             if (x + 1 < width) {
                // Decompressione gruppo completo di 2 pixel
                dstRow[x] = (static_cast<uint16_t>(srcRow[0]) << 4) | (srcRow[1] >> 4);
                dstRow[x + 1] = ((static_cast<uint16_t>(srcRow[1]) & 0x0F) << 8) | srcRow[2];
                srcRow += 3;
             }
             else {
                // Ultimo pixel dispari
                dstRow[x] = (static_cast<uint16_t>(srcRow[0]) << 4) | (srcRow[1] >> 4);
             }
          }
       }
    }
    PixelFormat GenICamCamera::convertFromGenICamPixelFormat(uint64_t genICamFormat) const {
       // Mappatura basata su PFNC (Pixel Format Naming Convention) v2.5
       switch (genICamFormat) {
          // Formati monocromatici
       case 0x01080001: return PixelFormat::Mono8;
       case 0x01100003: return PixelFormat::Mono10;
       case 0x01100005: return PixelFormat::Mono12;
       case 0x01100009: return PixelFormat::Mono14;
       case 0x01100007: return PixelFormat::Mono16;

          // Formati monocromatici packed
       case 0x010C0004: return PixelFormat::Mono10Packed;
       case 0x010C0006: return PixelFormat::Mono12Packed;

          // Formati RGB 8 bit
       case 0x02180014: return PixelFormat::RGB8;
       case 0x02180015: return PixelFormat::BGR8;
       case 0x02200016: return PixelFormat::RGBa8;
       case 0x02200017: return PixelFormat::BGRa8;

          // Formati RGB 10/12/16 bit
       case 0x02300018: return PixelFormat::RGB10;
       case 0x02300019: return PixelFormat::BGR10;
       case 0x0230001C: return PixelFormat::RGB12;
       case 0x0230001D: return PixelFormat::BGR12;
       case 0x02300033: return PixelFormat::RGB16;
       case 0x0230004B: return PixelFormat::BGR16;

          // Formati Bayer 8 bit
       case 0x01080008: return PixelFormat::BayerGR8;
       case 0x01080009: return PixelFormat::BayerRG8;
       case 0x0108000A: return PixelFormat::BayerGB8;
       case 0x0108000B: return PixelFormat::BayerBG8;

          // Formati Bayer 10 bit
       case 0x0110000C: return PixelFormat::BayerGR10;
       case 0x0110000D: return PixelFormat::BayerRG10;
       case 0x0110000E: return PixelFormat::BayerGB10;
       case 0x0110000F: return PixelFormat::BayerBG10;

          // Formati Bayer 12 bit
       case 0x01100010: return PixelFormat::BayerGR12;
       case 0x01100011: return PixelFormat::BayerRG12;
       case 0x01100012: return PixelFormat::BayerGB12;
       case 0x01100013: return PixelFormat::BayerBG12;

          // Formati Bayer 16 bit
       case 0x0110002E: return PixelFormat::BayerGR16;
       case 0x0110002F: return PixelFormat::BayerRG16;
       case 0x01100030: return PixelFormat::BayerGB16;
       case 0x01100031: return PixelFormat::BayerBG16;

          // Formati Bayer packed
       case 0x010C0026: return PixelFormat::BayerGR10Packed;
       case 0x010C0027: return PixelFormat::BayerRG10Packed;
       case 0x010C0028: return PixelFormat::BayerGB10Packed;
       case 0x010C0029: return PixelFormat::BayerBG10Packed;
       case 0x010C002A: return PixelFormat::BayerGR12Packed;
       case 0x010C002B: return PixelFormat::BayerRG12Packed;
       case 0x010C002C: return PixelFormat::BayerGB12Packed;
       case 0x010C002D: return PixelFormat::BayerBG12Packed;

          // Formati YUV
       case 0x02100032: return PixelFormat::YUV422_8;      // YUV422_8 generico
       case 0x0210001F: return PixelFormat::YUV422_8_UYVY; // UYVY
       case 0x02100022: return PixelFormat::YUV422_8_YUYV; // YUYV  
       case 0x02180020: return PixelFormat::YUV444_8;

          // Formati 3D e speciali
       case 0x023000C0: return PixelFormat::Coord3D_ABC32f;
       case 0x023000C1: return PixelFormat::Coord3D_ABC16;
       case 0x010800C4: return PixelFormat::Confidence8;
       case 0x011000C5: return PixelFormat::Confidence16;

          // Formato non riconosciuto
       default: return PixelFormat::Undefined;
       }
    }

    uint64_t GenICamCamera::convertToGenICamPixelFormat(PixelFormat format) const {
       switch (format) {
          // Formati monocromatici
       case PixelFormat::Mono8:         return 0x01080001;
       case PixelFormat::Mono10:        return 0x01100003;
       case PixelFormat::Mono12:        return 0x01100005;
       case PixelFormat::Mono14:        return 0x01100009;
       case PixelFormat::Mono16:        return 0x01100007;

          // Formati monocromatici packed
       case PixelFormat::Mono10Packed:  return 0x010C0004;
       case PixelFormat::Mono12Packed:  return 0x010C0006;

          // Formati RGB 8 bit
       case PixelFormat::RGB8:          return 0x02180014;
       case PixelFormat::BGR8:          return 0x02180015;
       case PixelFormat::RGBa8:         return 0x02200016;
       case PixelFormat::BGRa8:         return 0x02200017;

          // Formati RGB 10/12/16 bit
       case PixelFormat::RGB10:         return 0x02300018;
       case PixelFormat::BGR10:         return 0x02300019;
       case PixelFormat::RGB12:         return 0x0230001C;
       case PixelFormat::BGR12:         return 0x0230001D;
       case PixelFormat::RGB16:         return 0x02300033;
       case PixelFormat::BGR16:         return 0x0230004B;

          // Formati Bayer 8 bit
       case PixelFormat::BayerGR8:      return 0x01080008;
       case PixelFormat::BayerRG8:      return 0x01080009;
       case PixelFormat::BayerGB8:      return 0x0108000A;
       case PixelFormat::BayerBG8:      return 0x0108000B;

          // Formati Bayer 10 bit
       case PixelFormat::BayerGR10:     return 0x0110000C;
       case PixelFormat::BayerRG10:     return 0x0110000D;
       case PixelFormat::BayerGB10:     return 0x0110000E;
       case PixelFormat::BayerBG10:     return 0x0110000F;

          // Formati Bayer 12 bit
       case PixelFormat::BayerGR12:     return 0x01100010;
       case PixelFormat::BayerRG12:     return 0x01100011;
       case PixelFormat::BayerGB12:     return 0x01100012;
       case PixelFormat::BayerBG12:     return 0x01100013;

          // Formati Bayer 16 bit
       case PixelFormat::BayerGR16:     return 0x0110002E;
       case PixelFormat::BayerRG16:     return 0x0110002F;
       case PixelFormat::BayerGB16:     return 0x01100030;
       case PixelFormat::BayerBG16:     return 0x01100031;

          // Formati Bayer packed
       case PixelFormat::BayerGR10Packed: return 0x010C0026;
       case PixelFormat::BayerRG10Packed: return 0x010C0027;
       case PixelFormat::BayerGB10Packed: return 0x010C0028;
       case PixelFormat::BayerBG10Packed: return 0x010C0029;
       case PixelFormat::BayerGR12Packed: return 0x010C002A;
       case PixelFormat::BayerRG12Packed: return 0x010C002B;
       case PixelFormat::BayerGB12Packed: return 0x010C002C;
       case PixelFormat::BayerBG12Packed: return 0x010C002D;

          // Formati YUV
       case PixelFormat::YUV422_8:      return 0x02100032;
       case PixelFormat::YUV422_8_UYVY: return 0x0210001F;
       case PixelFormat::YUV422_8_YUYV: return 0x02100022;
       case PixelFormat::YUV444_8:      return 0x02180020;

          // Formati 3D e speciali
       case PixelFormat::Coord3D_ABC32f: return 0x023000C0;
       case PixelFormat::Coord3D_ABC16:  return 0x023000C1;
       case PixelFormat::Confidence8:    return 0x010800C4;
       case PixelFormat::Confidence16:   return 0x011000C5;

          // Formato non definito
       case PixelFormat::Undefined:
       default:                         return 0x00000000;
       }
    }
    std::string GenICamCamera::getGenTLErrorString(GenTL::GC_ERROR error) const {
        return GenICamException::getGenTLErrorString(error);
    }

    void GenICamCamera::setEventListener(CameraEventListener* listener) {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        m_eventListener = listener;
    }

    // === Grab Single Frame ===
    cv::Mat GenICamCamera::grabSingleFrame(uint32_t timeoutMs) {
       if (!isConnected()) {
          THROW_GENICAM_ERROR(ErrorType::ConnectionError, "Camera non connessa");
       }

       if (m_isAcquiring) {
          THROW_GENICAM_ERROR(ErrorType::AcquisitionError, "Acquisizione continua in corso");
       }

       cv::Mat result;
       GenTL::GC_ERROR err;

       try {
          //configureHikrobotGigE(); // Configura la camera per SingleFrame
          debugAcquisitionStart();
          prepareTransportLayerForAcquisition();
          setTransportLayerLock(false);   // nel caso in cui fosse stato bloccato prima malamentemente

          //PRIMA configura la camera per SingleFrame (PRIMA di aprire il DataStream)
          std::cout << "1. Configurazione camera per acquisizione singola..." << std::endl;
          // Imposta AcquisitionMode PRIMA di tutto
          if (isAcquisitionModeAvailable()) {
             setAcquisitionMode(AcquisitionMode::Continuous);
          }

          // i parametri che pilotano l'acquisizione devono essere impostati prima del blocco dei parametri GenTL
          if (isTriggerModeAvailable()) {
             setTriggerMode(TriggerMode::On);
             if (getTriggerMode() == TriggerMode::On)
                setTriggerSource(TriggerSource::Software);
          }

          // bisogna bloccare i parametri Transport Layer prima di proseguire dello streaming!!!!...e sbloccarli appena dopo le deallocazione dello streaming
          setTransportLayerLock(true);

          // 2. ORA apri il DataStream
          std::cout << "2. Apertura DataStream..." << std::endl;

          uint32_t numStreams = 0;
          err = GENTL_CALL(DevGetNumDataStreams)(m_devHandle, &numStreams);
          if (err != GenTL::GC_ERR_SUCCESS || numStreams == 0) {
             THROW_GENICAM_ERROR_CODE(ErrorType::GenTLError,
                "Nessun data stream disponibile", err);
          }

          char streamID[256] = { 0 };
          size_t streamIDSize = sizeof(streamID);
          err = GENTL_CALL(DevGetDataStreamID)(m_devHandle, 0, streamID, &streamIDSize);
          if (err != GenTL::GC_ERR_SUCCESS) {
             THROW_GENICAM_ERROR_CODE(ErrorType::GenTLError,
                "Impossibile ottenere l'ID dello stream", err);
          }

          err = GENTL_CALL(DevOpenDataStream)(m_devHandle, streamID, &m_dsHandle);
          if (err != GenTL::GC_ERR_SUCCESS) {
             THROW_GENICAM_ERROR_CODE(ErrorType::GenTLError,
                "Impossibile aprire il data stream", err);
          }

          // 3. Ottieni dimensione buffer
          GenTL::INFO_DATATYPE dataType;
          bool8_t definesPayloadSize = 0;
          size_t infoSize = sizeof(definesPayloadSize);

          err = GENTL_CALL(DSGetInfo)(m_dsHandle, GenTL::STREAM_INFO_DEFINES_PAYLOADSIZE, &dataType, &definesPayloadSize, &infoSize);

          if (definesPayloadSize) {
             infoSize = sizeof(m_bufferSize);
             err = GENTL_CALL(DSGetInfo)(m_dsHandle, GenTL::STREAM_INFO_PAYLOAD_SIZE, &dataType, &m_bufferSize, &infoSize);
             if (err != GenTL::GC_ERR_SUCCESS) {
                THROW_GENICAM_ERROR_CODE(ErrorType::BufferError, "Impossibile determinare la dimensione del buffer", err);
             }
          }
          else {
             // meglio cosi' pero'
             GenApi::IInteger* pp = dynamic_cast<GenApi::IInteger*>(m_pNodeMap->_GetNode("PayloadSize"));
             if (GenApi::IsReadable(pp))
                m_bufferSize = static_cast<size_t>(pp->GetValue());
             else {
                // Calcola manualmente per telecamere che non definiscono PayloadSize
                ROI roi = getROI();
                PixelFormat pf = getPixelFormat();

                int bytesPerPixel = 1;
                switch (pf) {
                case PixelFormat::Mono8:
                case PixelFormat::BayerRG8:
                case PixelFormat::BayerGB8:
                case PixelFormat::BayerGR8:
                case PixelFormat::BayerBG8:
                   bytesPerPixel = 1;
                   break;
                case PixelFormat::Mono10:
                case PixelFormat::Mono12:
                case PixelFormat::Mono16:
                   bytesPerPixel = 2;
                   break;
                case PixelFormat::RGB8:
                case PixelFormat::BGR8:
                   bytesPerPixel = 3;
                   break;
                default:
                   bytesPerPixel = 1;
                }

                m_bufferSize = roi.width * roi.height * bytesPerPixel;
             }
          }

          std::cout << "3. Buffer size calcolato: " << m_bufferSize << " bytes" << std::endl;

          // 4. Alloca e registra buffer
          allocateBuffers(1);

          // 5. Metti il buffer in coda
          for (auto& hBuffer : m_bufferHandles) {
             err = GENTL_CALL(DSQueueBuffer)(m_dsHandle, hBuffer);
             if (err != GenTL::GC_ERR_SUCCESS) {
                THROW_GENICAM_ERROR_CODE(ErrorType::BufferError, "Impossibile accodare il buffer", err);
             }
          }

          // 8. IMPORTANTE: Per Hikrobot, potrebbe essere necessario un piccolo delay
          std::this_thread::sleep_for(std::chrono::milliseconds(100));

          // 6. Registra evento
          err = GENTL_CALL(GCRegisterEvent)(m_dsHandle, GenTL::EVENT_NEW_BUFFER, &m_eventHandle);
          if (err != GenTL::GC_ERR_SUCCESS) {
             THROW_GENICAM_ERROR_CODE(ErrorType::GenTLError, "Impossibile registrare l'evento NEW_BUFFER", err);
          }

          size_t numDelivered = 0, numQueued = 0, numStarted = 0, isGrabbing = 0, numAnnounced = 0, numAwaitDelivery = 0;

          GenTL::INFO_DATATYPE dt;
          size_t sz = sizeof(numQueued);
          GENTL_CALL(DSGetInfo)(m_dsHandle, GenTL::STREAM_INFO_NUM_QUEUED, &dt, &numQueued, &sz);
          std::cout << "   - Queued prima di DSAcquisition: " << numQueued << std::endl;

          // 7. Avvia il DataStream
          std::cout << "4. Avvio DataStream..." << std::endl;
          err = GENTL_CALL(DSStartAcquisition)(m_dsHandle, GenTL::ACQ_START_FLAGS_DEFAULT, GENTL_INFINITE);
          if (err != GenTL::GC_ERR_SUCCESS) {
             THROW_GENICAM_ERROR_CODE(ErrorType::AcquisitionError, "Impossibile avviare l'acquisizione sul data stream", err);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));

          // 10. Per SingleFrame, potrebbe essere necessario un trigger
/*            if (getAcquisitionMode() == AcquisitionMode::SingleFrame) {
                // Alcune telecamere Hikrobot richiedono un comando specifico
                try {
                    GenApi::CCommandPtr pFrameStart = getCommandNode("TriggerSoftware");
                    if (pFrameStart.IsValid() && GenApi::IsWritable(pFrameStart)) {
                        std::cout << "6. Esecuzione TriggerSoftware per SingleFrame..." << std::endl;
                        pFrameStart->Execute();
                    }
                }
                catch (...) {
                    // Non critico se non disponibile
                }
            }*/

            // 9. Avvia acquisizione sulla camera
          std::cout << "5. Avvio acquisizione camera..." << std::endl;

          try {
             //GenApi::CCommandPtr pAcqStart = getCommandNode("AcquisitionStart");
             GenApi::CCommandPtr pAcqStart = m_pNodeMap->_GetNode("AcquisitionStart");
             refreshNodeMap();
             
             if (pAcqStart.IsValid()) {
                pAcqStart->Execute();

                // Attendi che il comando sia completato
                int waitCount = 0;
                while (!pAcqStart->IsDone() && waitCount < 100) {
                   std::this_thread::sleep_for(std::chrono::milliseconds(10));
                   waitCount++;
                }
                m_isAcquiring = true;
                m_stopAcquisition = false;
                std::cout << "   AcquisitionStart eseguito con successo" << std::endl;
             }
             else {
                std::cout << "   WARNING: AcquisitionStart non disponibile" << std::endl;
             }
          }
          catch (const GENICAM_NAMESPACE::GenericException& e) {
             std::cout << "   WARNING: AcquisitionStart fallito: " << e.GetDescription() << std::endl;
             // Continua comunque, alcune camere non richiedono questo comando
          }

          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          if (getTriggerMode() == TriggerMode::On)
             executeTriggerSoftware();

          // 11. Attendi il frame
          GenTL::EVENT_NEW_BUFFER_DATA bufferData;
          size_t bufferDataSize = sizeof(bufferData);

          std::cout << "7. Attesa frame (timeout: " << timeoutMs << "ms)..." << std::endl;

          err = GENTL_CALL(EventGetData)(m_eventHandle, &bufferData, &bufferDataSize, timeoutMs);

          if (err != GenTL::GC_ERR_SUCCESS) {
             // Debug: verifica stato stream

             GenTL::INFO_DATATYPE dt;
             size_t sz = sizeof(numDelivered);

             GENTL_CALL(DSGetInfo)(m_dsHandle, GenTL::STREAM_INFO_NUM_DELIVERED, &dt, &numDelivered, &sz);
             GENTL_CALL(DSGetInfo)(m_dsHandle, GenTL::STREAM_INFO_NUM_QUEUED, &dt, &numQueued, &sz);
             GENTL_CALL(DSGetInfo)(m_dsHandle, GenTL::STREAM_INFO_NUM_STARTED, &dt, &numStarted, &sz);
             GENTL_CALL(DSGetInfo)(m_dsHandle, GenTL::STREAM_INFO_IS_GRABBING, &dt, &isGrabbing, &sz);
             GENTL_CALL(DSGetInfo)(m_dsHandle, GenTL::STREAM_INFO_NUM_ANNOUNCED, &dt, &numAnnounced, &sz);
             GENTL_CALL(DSGetInfo)(m_dsHandle, GenTL::STREAM_INFO_NUM_AWAIT_DELIVERY, &dt, &numAwaitDelivery, &sz);


             std::cout << "   Frame non ricevuto!" << std::endl;
             std::cout << "   - Delivered: " << numDelivered << std::endl;
             std::cout << "   - Queued: " << numQueued << std::endl;
             std::cout << "   - Started: " << numStarted << std::endl;
             std::cout << "   - Grabbing: " << isGrabbing << std::endl;
             std::cout << "   - Announced: " << numAnnounced << std::endl;
             std::cout << "   - Await delivery: " << numAwaitDelivery << std::endl;

             THROW_GENICAM_ERROR_CODE(ErrorType::TimeoutError, "Timeout acquisizione frame", err);
          }

          std::cout << "8. Frame ricevuto!" << std::endl;

          // 12. Processa il buffer
          GenTL::BUFFER_HANDLE hBuffer = bufferData.BufferHandle;
          if (hBuffer) {
             void* pBuffer = nullptr;
             size_t infoSize = sizeof(pBuffer);
             GenTL::INFO_DATATYPE dataType;

             err = GENTL_CALL(DSGetBufferInfo)(m_dsHandle, hBuffer, GenTL::BUFFER_INFO_BASE, &dataType, &pBuffer, &infoSize);

             if (err == GenTL::GC_ERR_SUCCESS && pBuffer) {
                uint32_t width = 0, height = 0;
                uint64_t pixelFormat = 0;
                size_t tempSize = sizeof(uint32_t);

                GENTL_CALL(DSGetBufferInfo)(m_dsHandle, hBuffer, GenTL::BUFFER_INFO_WIDTH, &dataType, &width, &tempSize);
                GENTL_CALL(DSGetBufferInfo)(m_dsHandle, hBuffer, GenTL::BUFFER_INFO_HEIGHT, &dataType, &height, &tempSize);

                tempSize = sizeof(uint64_t);
                GENTL_CALL(DSGetBufferInfo)(m_dsHandle, hBuffer, GenTL::BUFFER_INFO_PIXELFORMAT, &dataType, &pixelFormat, &tempSize);

                result = convertBufferToMat(pBuffer, m_bufferSize, width, height, convertFromGenICamPixelFormat(pixelFormat));
             }
          }

          // 13. Stop acquisizione camera
          try {
             GenApi::CCommandPtr pAcqStop = getCommandNode("AcquisitionStop");
             if (pAcqStop.IsValid() && GenApi::IsWritable(pAcqStop)) {
                pAcqStop->Execute();
             }
             m_isAcquiring = false;
             m_stopAcquisition = true;
          }
          catch (...) {}

          // Cleanup DataStream
          if (m_eventHandle) {
             m_isAcquiring = false;
             m_stopAcquisition = true;
             GENTL_CALL(GCUnregisterEvent)(m_dsHandle, GenTL::EVENT_NEW_BUFFER);
             m_eventHandle = nullptr;
          }

          if (m_dsHandle) {
             GENTL_CALL(DSStopAcquisition)(m_dsHandle, GenTL::ACQ_STOP_FLAGS_DEFAULT);
             GENTL_CALL(DSFlushQueue)(m_dsHandle, GenTL::ACQ_QUEUE_ALL_DISCARD);
             GENTL_CALL(DSClose)(m_dsHandle);
             m_dsHandle = nullptr;
          }

          setTransportLayerLock(false);
          setTriggerMode(TriggerMode::Off);

          freeBuffers(); // da inserirsi anche in qualche altro catch
       }
       catch (...) {
          // Cleanup
          if (m_eventHandle) {
             GENTL_CALL(GCUnregisterEvent)(m_dsHandle, GenTL::EVENT_NEW_BUFFER);
             m_eventHandle = nullptr;
          }
          if (m_dsHandle) {
             GENTL_CALL(DSStopAcquisition)(m_dsHandle, GenTL::ACQ_STOP_FLAGS_DEFAULT);
             GENTL_CALL(DSClose)(m_dsHandle);
             m_dsHandle = nullptr;
          }
          freeBuffers();
          throw;
       }

       return result;
    }

    void GenICamCamera::debugAcquisitionStart() {
        std::cout << "\n=== Debug AcquisitionStart ===" << std::endl;

        try {
            // 1. Verifica esistenza del nodo
            //GenApi::CCommandPtr pAcqStart = getCommandNode("AcquisitionStart");
            GenApi::CCommandPtr pAcqStart = m_pNodeMap->_GetNode("AcquisitionStart");
            std::cout << "AcquisitionStart exists: " << (pAcqStart.IsValid() ? "YES" : "NO") << std::endl;

            if (!pAcqStart.IsValid()) {
                std::cout << "ERRORE: Nodo AcquisitionStart non trovato!" << std::endl;
                return;
            }

            // 2. Verifica accessibilità
            std::cout << "IsImplemented: " << (GenApi::IsImplemented(pAcqStart) ? "YES" : "NO") << std::endl;
            std::cout << "IsAvailable: " << (GenApi::IsAvailable(pAcqStart) ? "YES" : "NO") << std::endl;
            std::cout << "IsReadable: " << (GenApi::IsReadable(pAcqStart) ? "YES" : "NO") << std::endl;
            std::cout << "IsWritable: " << (GenApi::IsWritable(pAcqStart) ? "YES" : "NO") << std::endl;

            // 3. Se non è writable, verifica perché
            if (!GenApi::IsWritable(pAcqStart)) {
                // Verifica visibility
                //GenApi::EVisibility visibility = pAcqStart->GetVisibility();
                std::cout << "Visibility: ";
                GenApi::EVisibility visibility = GenApi::Guru;
                switch (visibility) {
                case GenApi::Beginner: std::cout << "Beginner"; break;
                case GenApi::Expert: std::cout << "Expert"; break;
                case GenApi::Guru: std::cout << "Guru"; break;
                case GenApi::Invisible: std::cout << "Invisible"; break;
                default: std::cout << "Unknown";
                }
                std::cout << std::endl;

                // Verifica access mode
                GenApi::EAccessMode accessMode = pAcqStart->GetAccessMode();
                std::cout << "AccessMode: ";
                switch (accessMode) {
                case GenApi::NI: std::cout << "Not Implemented"; break;
                case GenApi::NA: std::cout << "Not Available"; break;
                case GenApi::WO: std::cout << "Write Only"; break;
                case GenApi::RO: std::cout << "Read Only"; break;
                case GenApi::RW: std::cout << "Read/Write"; break;
                default: std::cout << "Unknown";
                }
                std::cout << std::endl;

                // Verifica dipendenze
                std::cout << "\n--- Verifica Parametri Correlati ---" << std::endl;

                // AcquisitionMode
                if (isParameterAvailable("AcquisitionMode")) {
                    std::cout << "AcquisitionMode: " << getParameter("AcquisitionMode") << std::endl;
                }
                else {
                    std::cout << "AcquisitionMode: NON DISPONIBILE" << std::endl;
                }

                // TriggerMode
                if (isParameterAvailable("TriggerMode")) {
                    std::cout << "TriggerMode: " << getParameter("TriggerMode") << std::endl;
                }
                else {
                    std::cout << "TriggerMode: NON DISPONIBILE" << std::endl;
                }

                // AcquisitionStatus
                if (isParameterAvailable("AcquisitionStatus")) {
                    std::cout << "AcquisitionStatus: " << getParameter("AcquisitionStatus") << std::endl;
                }

                // Per GigE
                if (isParameterAvailable("GevCCP")) {
                    std::cout << "GevCCP (Control Privilege): " << getParameter("GevCCP") << std::endl;
                }

                // StreamGrabbing status
                if (isParameterAvailable("StreamIsGrabbing")) {
                    std::cout << "StreamIsGrabbing: " << getParameter("StreamIsGrabbing") << std::endl;
                }
            }

        }
        catch (const std::exception& e) {
            std::cout << "Errore durante debug: " << e.what() << std::endl;
        }
    }

    void GenICamCamera::configureHikrobotGigE() {
        try {
            // 1. Packet Size (importante per GigE)
            if (isParameterAvailable("GevSCPSPacketSize")) {
                setParameter("GevSCPSPacketSize", "1500"); // o "9000" per Jumbo frames
            }

            // 2. Packet Delay (per evitare perdita pacchetti)
            if (isParameterAvailable("GevSCPD")) {
                setParameter("GevSCPD", "1000"); // microsecondi
            }

            // 3. Disabilita features che potrebbero interferire
            if (isParameterAvailable("ChunkModeActive")) {
                setParameter("ChunkModeActive", "false");
            }

            // 4. Frame timeout
            if (isParameterAvailable("GevSCFTD")) {
                setParameter("GevSCFTD", "3000000"); // 3 secondi
            }

            // 5. Imposta modalità trasferimento
            if (isParameterAvailable("StreamBufferHandlingMode")) {
                setParameter("StreamBufferHandlingMode", "NewestOnly");
            }

            if (isParameterAvailable("GevCCP")) {
                setParameter("GevCCP", "ExclusiveAccess");
                std::cout << "Control Channel Privilege acquisito" << std::endl;
            }

            // Verifica il privilegio
            if (isParameterAvailable("GevCCP")) {
                std::string privilege = getParameter("GevCCP");
                std::cout << "Privilegio corrente: " << privilege << std::endl;
            }
        }
        catch (...) {
            // Non critico se alcuni parametri non sono disponibili
        }
    }
    // === Gestione Buffer ===

    void GenICamCamera::allocateBuffers(size_t count) {
        if (count == 0) {
            THROW_GENICAM_ERROR(ErrorType::BufferError,
                "Il numero di buffer deve essere > 0");
        }

        // Cleanup di eventuali buffer esistenti
        freeBuffers();

        // Clear dei vettori
        m_bufferHandles.clear();
        m_alignedBuffers.clear();

        // Riserva spazio per evitare riallocazioni
        m_bufferHandles.reserve(count);
        m_alignedBuffers.reserve(count);

        GenTL::GC_ERROR err;

        // Ottieni requisiti di allineamento dal data stream
        size_t alignment = 1;
        size_t alignInfoSize = sizeof(alignment);
        GenTL::INFO_DATATYPE dataType;

        err = GENTL_CALL(DSGetInfo)(m_dsHandle, GenTL::STREAM_INFO_BUF_ALIGNMENT, &dataType, &alignment, &alignInfoSize);

        if (err != GenTL::GC_ERR_SUCCESS || alignment == 0) {
            alignment = 64;  // Default sicuro per la maggior parte delle telecamere
            std::cout << "Warning: Using default alignment of " << alignment << " bytes" << std::endl;
        }
        else {
            std::cout << "Buffer alignment requirement: " << alignment << " bytes" << std::endl;
        }

        // Allinea la dimensione del buffer ai requisiti
        size_t alignedBufferSize = ((m_bufferSize + alignment - 1) / alignment) * alignment;

        if (alignedBufferSize != m_bufferSize) {
            std::cout << "Buffer size aligned from " << m_bufferSize
                << " to " << alignedBufferSize << " bytes" << std::endl;
        }

        // Flag per decidere il metodo di allocazione
        bool useProducerAllocation = true;

        // Prima tentativo: lascia che il producer allochi la memoria
        std::cout << "Trying producer-managed buffer allocation..." << std::endl;

        for (size_t i = 0; i < count; i++) {
            GenTL::BUFFER_HANDLE hBuffer = nullptr;

            err = GENTL_CALL(DSAllocAndAnnounceBuffer)(m_dsHandle, alignedBufferSize, nullptr, &hBuffer);

            if (err == GenTL::GC_ERR_SUCCESS) {
                m_bufferHandles.push_back(hBuffer);
            }
            else {
                std::cout << "Producer allocation failed: " << getGenTLErrorString(err)
                    << ", falling back to manual allocation" << std::endl;

                // Pulisci i buffer già allocati
                for (auto& handle : m_bufferHandles) {
                    GENTL_CALL(DSRevokeBuffer)(m_dsHandle, handle, nullptr, nullptr);
                }
                m_bufferHandles.clear();

                useProducerAllocation = false;
                break;
            }
        }

        // Se l'allocazione del producer ha avuto successo, siamo a posto
        if (useProducerAllocation) {
            std::cout << "Successfully allocated " << count
                << " buffers using producer allocation" << std::endl;
            return;
        }

        // Secondo tentativo: allocazione manuale con memoria allineata
        std::cout << "Using manual buffer allocation with "
            << alignment << "-byte alignment" << std::endl;

        for (size_t i = 0; i < count; i++) {
            // Alloca memoria allineata
            void* alignedPtr = nullptr;

#ifdef _WIN32
            alignedPtr = _aligned_malloc(alignedBufferSize, alignment);
#else
            // Assicurati che alignment sia una potenza di 2
            if (alignment & (alignment - 1)) {
                // Se non è potenza di 2, trova la prossima potenza di 2
                size_t powerOf2 = 1;
                while (powerOf2 < alignment) {
                    powerOf2 <<= 1;
                }
                alignment = powerOf2;
            }
            alignedPtr = aligned_alloc(alignment, alignedBufferSize);
#endif

            if (!alignedPtr) {
                freeBuffers();
                std::stringstream ss;
                ss << "Impossibile allocare " << alignedBufferSize
                    << " bytes con allineamento " << alignment;
                THROW_GENICAM_ERROR(ErrorType::BufferError, ss.str());
            }

            // Azzera la memoria (alcune telecamere lo richiedono)
            std::memset(alignedPtr, 0, alignedBufferSize);

            // Gestisci la memoria con unique_ptr e custom deleter
            m_alignedBuffers.push_back(std::unique_ptr<void, AlignedBufferDeleter>(alignedPtr));

            // Annuncia il buffer al data stream
            GenTL::BUFFER_HANDLE hBuffer = nullptr;
            err = GENTL_CALL(DSAnnounceBuffer)(m_dsHandle, alignedPtr, alignedBufferSize, nullptr, &hBuffer);

            if (err != GenTL::GC_ERR_SUCCESS) {
                // Log dettagliato dell'errore
                std::cerr << "DSAnnounceBuffer failed:" << std::endl;
                std::cerr << "  Error: " << getGenTLErrorString(err)
                    << " (0x" << std::hex << err << std::dec << ")" << std::endl;
                std::cerr << "  Buffer " << i << " of " << count << std::endl;
                std::cerr << "  Address: " << alignedPtr << std::endl;
                std::cerr << "  Size: " << alignedBufferSize << " bytes" << std::endl;
                std::cerr << "  Alignment: " << alignment << " bytes" << std::endl;

                freeBuffers();

                THROW_GENICAM_ERROR_CODE(ErrorType::BufferError,
                    "Impossibile annunciare il buffer", err);
            }

            m_bufferHandles.push_back(hBuffer);
        }

        std::cout << "Successfully allocated " << count
            << " buffers using manual allocation" << std::endl;
        std::cout << "Buffer size: " << alignedBufferSize
            << " bytes each" << std::endl;
    }

    void GenICamCamera::freeBuffers() {
        // Revoca tutti i buffer dal data stream
        for (auto& hBuffer : m_bufferHandles) {
            if (hBuffer && m_dsHandle) {
                GenTL::GC_ERROR err = GENTL_CALL(DSRevokeBuffer)(
                    m_dsHandle, hBuffer, nullptr, nullptr);

                if (err != GenTL::GC_ERR_SUCCESS) {
                    std::cerr << "Warning: DSRevokeBuffer failed: "
                        << getGenTLErrorString(err) << std::endl;
                }
            }
        }

        // Pulisci i vettori
        m_bufferHandles.clear();

        // La memoria allineata viene automaticamente liberata
        // grazie al custom deleter quando si fa clear()
        m_alignedBuffers.clear();

        // Se avevi anche il vecchio m_bufferMemory, puliscilo
        //m_bufferMemory.clear();
    }

    // === Informazioni Camera ===

    std::string GenICamCamera::getCameraInfo() const {
        std::shared_lock<std::shared_mutex> lock(m_connectionMutex);

        if (!isConnected()) {
            THROW_GENICAM_ERROR(ErrorType::ConnectionError,
                "Camera non connessa");
        }

        std::stringstream info;
        info << "Camera ID: " << m_cameraID << "\n";
        info << "Nome utente: " << getCameraUserID() << "\n";
        info << "Marca: " << getCameraVendor() << "\n";
        info << "Modello: " << getCameraModel() << "\n";
        info << "Serial Number: " << getCameraSerialNumber() << "\n";
        info << "Versione: " << getCameraVersion() << "\n";

        uint32_t width, height;
        getSensorSize(width, height);
        info << "Sensor Size: " << width << "x" << height << "\n";

        ROI roi = getROI();
        info << "Current ROI: " << roi.width << "x" << roi.height
            << " @ (" << roi.x << "," << roi.y << ")\n";

        info << "Pixel Format: ";
        switch (getPixelFormat()) {
        case PixelFormat::Mono8: info << "Mono8"; break;
        case PixelFormat::Mono10: info << "Mono10"; break;
        case PixelFormat::Mono12: info << "Mono12"; break;
        case PixelFormat::Mono16: info << "Mono16"; break;
        case PixelFormat::RGB8: info << "RGB8"; break;
        case PixelFormat::BGR8: info << "BGR8"; break;
        default: info << "Other"; break;
        }
        info << "\n";

        if (isExposureTimeAvailable()) {
            double min, max;
            getExposureTimeRange(min, max);
            info << "Exposure Time: " << getExposureTime() << " µs"
                << " (Range: " << min << " - " << max << ")\n";
        }

        if (isGainAvailable()) {
            double min, max;
            getGainRange(min, max);
            info << "Gain: " << getGain()
                << " (Range: " << min << " - " << max << ")\n";
        }

        if (isFrameRateAvailable()) {
            info << "Frame Rate: " << getFrameRate() << " fps\n";
        }

        info << "Trigger Mode: ";
        switch (getTriggerMode()) {
            case TriggerMode::Off:
                info << "Off";
                break;
            case TriggerMode::On:
                info << "On";
                break;
        }
        info << "\n";

        info << "Trigger Source: ";
        switch (getTriggerSource()) {
        case TriggerSource::Software: info << "Software"; break;
        case TriggerSource::Line0: info << "Line0"; break;
        case TriggerSource::Line1: info << "Line1"; break;
        case TriggerSource::Line2: info << "Line2"; break;
        case TriggerSource::Line3: info << "Line3"; break;
        case TriggerSource::Line4: info << "Line4"; break;
        case TriggerSource::Line5: info << "Line5"; break;
        case TriggerSource::Line6: info << "Line6"; break;
        case TriggerSource::Line7: info << "Line7"; break;
        case TriggerSource::Counter0End: info << "Counter0End"; break;
        case TriggerSource::Counter1End: info << "Counter1End"; break;
        case TriggerSource::Timer0End: info << "Timer0End"; break;
        case TriggerSource::Timer1End: info << "Timer1End"; break;
        case TriggerSource::UserOutput0: info << "UserOutput0"; break;
        case TriggerSource::UserOutput1: info << "UserOutput1"; break;
        case TriggerSource::UserOutput2: info << "UserOutput2"; break;
        case TriggerSource::UserOutput3: info << "UserOutput3"; break;
        case TriggerSource::Action0: info << "Action0"; break;
        case TriggerSource::Action1: info << "Action1"; break;
        }
        info << "\n";

        return info.str();
    }

    std::string GenICamCamera::getCameraModel() const {
        if (!isConnected()) {
            THROW_GENICAM_ERROR(ErrorType::ConnectionError,
                "Camera non connessa");
        }

        try {
            // Prima prova via GenApi
            GenApi::CStringPtr pModel = m_pNodeMap->_GetNode("DeviceModelName");
            if (pModel.IsValid() && GenApi::IsReadable(pModel)) {
                return std::string(pModel->GetValue());
            }
        }
        catch (...) {}

        // Fallback a GenTL
        char model[256] = { 0 };
        size_t size = sizeof(model);
        GenTL::INFO_DATATYPE dataType;

        GENTL_CALL(DevGetInfo)(m_devHandle, GenTL::DEVICE_INFO_MODEL,
            &dataType, model, &size);

        return std::string(model);
    }

    std::string GenICamCamera::getCameraSerialNumber() const {
        if (!isConnected()) {
            THROW_GENICAM_ERROR(ErrorType::ConnectionError,
                "Camera non connessa");
        }

        try {
            // Prima prova via GenApi
            GenApi::CStringPtr pSerial = m_pNodeMap->_GetNode("DeviceSerialNumber");
            if (pSerial.IsValid() && GenApi::IsReadable(pSerial)) {
                return std::string(pSerial->GetValue());
            }
        }
        catch (...) {}

        // Fallback a GenTL
        char serial[256] = { 0 };
        size_t size = sizeof(serial);
        GenTL::INFO_DATATYPE dataType;

        GENTL_CALL(DevGetInfo)(m_devHandle, GenTL::DEVICE_INFO_SERIAL_NUMBER, &dataType, serial, &size);

        return std::string(serial);
    }

    std::string GenICamCamera::getCameraVendor() const {
        if (!isConnected()) {
            THROW_GENICAM_ERROR(ErrorType::ConnectionError, "Camera non connessa");
        }

        try {
            GenApi::CStringPtr pVendor = m_pNodeMap->_GetNode("DeviceVendorName");
            if (pVendor.IsValid() && GenApi::IsReadable(pVendor)) {
                return std::string(pVendor->GetValue());
            }
        }
        catch (...) {}

        // Fallback a GenTL
        char vendor[256] = { 0 };
        size_t size = sizeof(vendor);
        GenTL::INFO_DATATYPE dataType;

        GENTL_CALL(DevGetInfo)(m_devHandle, GenTL::DEVICE_INFO_VENDOR, &dataType, vendor, &size);

        return std::string(vendor);
    }

    std::string GenICamCamera::getCameraUserID() const {
        if (!isConnected()) {
            THROW_GENICAM_ERROR(ErrorType::ConnectionError,
                "Camera non connessa");
        }

        try {
            GenApi::CStringPtr pDeviceUserID = m_pNodeMap->_GetNode("DeviceUserID");
            if (pDeviceUserID.IsValid() && GenApi::IsReadable(pDeviceUserID)) {
                return std::string(pDeviceUserID->GetValue());
            }
        }
        catch (...) {}

        // Fallback a GenTL
        char camUserID[256] = { 0 };
        size_t size = sizeof(camUserID);
        GenTL::INFO_DATATYPE dataType;

        GENTL_CALL(DevGetInfo)(m_devHandle, GenTL::DEVICE_INFO_USER_DEFINED_NAME, &dataType, camUserID, &size);

        return std::string(camUserID);
    }

    std::string GenICamCamera::getCameraVersion() const {
        if (!isConnected()) {
            THROW_GENICAM_ERROR(ErrorType::ConnectionError,
                "Camera non connessa");
        }

        try {
            GenApi::CStringPtr pVersion = m_pNodeMap->_GetNode("DeviceVersion");
            if (pVersion.IsValid() && GenApi::IsReadable(pVersion)) {
                return std::string(pVersion->GetValue());
            }
        }
        catch (...) {}

        // Fallback a GenTL
        char version[256] = { 0 };
        size_t size = sizeof(version);
        GenTL::INFO_DATATYPE dataType;

        GENTL_CALL(DevGetInfo)(m_devHandle, GenTL::DEVICE_INFO_VERSION,
            &dataType, version, &size);

        return std::string(version);
    }

    // === Parametri Generici ===

    std::vector<std::string> GenICamCamera::getAvailableParameters() const {
        std::shared_lock<std::shared_mutex> lock(m_parameterMutex);
        std::vector<std::string> parameters;

        if (!isConnected() || !m_pNodeMap) {
            return parameters;
        }

        try {
            GenApi::CNodePtr pRoot = m_pNodeMap->_GetNode("Root");
            if (!pRoot.IsValid()) {
                pRoot = m_pNodeMap->_GetNode("Device");
            }

            if (pRoot.IsValid()) {
                exploreNode(pRoot, parameters);
            }
            else {
                GenApi::NodeList_t nodes;
                m_pNodeMap->_GetNodes(nodes);

                for (auto& node : nodes) {
                    if (node->IsFeature() && GenApi::IsImplemented(node)) {
                        parameters.push_back(node->GetName().c_str());
                    }
                }
            }

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            std::cerr << "Errore esplorazione parametri: " << e.GetDescription() << std::endl;
        }

        // Rimuovi duplicati
        std::sort(parameters.begin(), parameters.end());
        parameters.erase(std::unique(parameters.begin(), parameters.end()),
            parameters.end());

        return parameters;
    }

    void GenICamCamera::exploreNode(GenApi::CNodePtr pNode,
        std::vector<std::string>& parameters) const {
        if (!pNode.IsValid()) return;

        GenApi::EInterfaceType type = pNode->GetPrincipalInterfaceType();
        if (type != GenApi::intfICategory && type != GenApi::intfIPort) {
            if (GenApi::IsImplemented(pNode)) {
                parameters.push_back(pNode->GetName().c_str());
            }
        }

        if (type == GenApi::intfICategory) {
            GenApi::CCategoryPtr pCategory(pNode);
            if (pCategory.IsValid()) {
                GenApi::FeatureList_t features;
                pCategory->GetFeatures(features);

                for (auto& feature : features) {
                    exploreNode(feature, parameters);
                }
            }
        }
    }

    std::string GenICamCamera::getParameter(const std::string& parameterName) const {
        try {
            GenApi::CNodePtr pNode = getNode(parameterName);

            if (!GenApi::IsReadable(pNode)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "Parametro non leggibile: " + parameterName);
            }

            switch (pNode->GetPrincipalInterfaceType()) {
            case GenApi::intfIInteger: {
                GenApi::CIntegerPtr pInt(pNode);
                if (pInt.IsValid()) {
                    return std::to_string(pInt->GetValue());
                }
                break;
            }
            case GenApi::intfIFloat: {
                GenApi::CFloatPtr pFloat(pNode);
                if (pFloat.IsValid()) {
                    return std::to_string(pFloat->GetValue());
                }
                break;
            }
            case GenApi::intfIString: {
                GenApi::CStringPtr pString(pNode);
                if (pString.IsValid()) {
                    return std::string(pString->GetValue());
                }
                break;
            }
            case GenApi::intfIEnumeration: {
                GenApi::CEnumerationPtr pEnum(pNode);
                if (pEnum.IsValid()) {
                    return std::string(pEnum->ToString());
                }
                break;
            }
            case GenApi::intfIBoolean: {
                GenApi::CBooleanPtr pBool(pNode);
                if (pBool.IsValid()) {
                    return pBool->GetValue() ? "true" : "false";
                }
                break;
            }
            default:
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "Tipo parametro non supportato: " + parameterName);
            }

            THROW_GENICAM_ERROR(ErrorType::ParameterError,
                "Errore conversione parametro: " + parameterName);

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore lettura parametro: ") + e.GetDescription());
        }
    }

    void GenICamCamera::setParameter(const std::string& parameterName,
        const std::string& value) {
        try {
            GenApi::CNodePtr pNode = getNode(parameterName);

            if (!GenApi::IsWritable(pNode)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "Parametro non scrivibile: " + parameterName);
            }

            switch (pNode->GetPrincipalInterfaceType()) {
            case GenApi::intfIInteger: {
                GenApi::CIntegerPtr pInt(pNode);
                if (pInt.IsValid()) {
                    int64_t intValue = std::stoll(value);
                    pInt->SetValue(intValue);
                    notifyParameterChanged(parameterName, value);
                    return;
                }
                break;
            }
            case GenApi::intfIFloat: {
                GenApi::CFloatPtr pFloat(pNode);
                if (pFloat.IsValid()) {
                    double floatValue = std::stod(value);
                    pFloat->SetValue(floatValue);
                    notifyParameterChanged(parameterName, value);
                    return;
                }
                break;
            }
            case GenApi::intfIString: {
                GenApi::CStringPtr pString(pNode);
                if (pString.IsValid()) {
                    pString->SetValue(value.c_str());
                    notifyParameterChanged(parameterName, value);
                    return;
                }
                break;
            }
            case GenApi::intfIEnumeration: {
                GenApi::CEnumerationPtr pEnum(pNode);
                if (pEnum.IsValid()) {
                    GenApi::CEnumEntryPtr pEntry = pEnum->GetEntryByName(value.c_str());
                    if (pEntry.IsValid()) {
                        pEnum->FromString(value.c_str());
                        notifyParameterChanged(parameterName, value);
                        return;
                    }
                    else {
                        try {
                            int64_t enumValue = std::stoll(value);
                            pEnum->SetIntValue(enumValue);
                            notifyParameterChanged(parameterName, value);
                            return;
                        }
                        catch (...) {
                            THROW_GENICAM_ERROR(ErrorType::ParameterError,
                                "Valore enumerazione non valido: " + value);
                        }
                    }
                }
                break;
            }
            case GenApi::intfIBoolean: {
                GenApi::CBooleanPtr pBool(pNode);
                if (pBool.IsValid()) {
                    bool boolValue = (value == "true" || value == "1" ||
                        value == "True" || value == "TRUE");
                    pBool->SetValue(boolValue);
                    notifyParameterChanged(parameterName, value);
                    return;
                }
                break;
            }
            case GenApi::intfICommand: {
                GenApi::CCommandPtr pCommand(pNode);
                if (pCommand.IsValid() && GenApi::IsWritable(pCommand)) {
                    pCommand->Execute();
                    while (!pCommand->IsDone()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    notifyParameterChanged(parameterName, "Executed");
                    return;
                }
                break;
            }
            default:
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "Tipo parametro non supportato per scrittura: " + parameterName);
            }

            THROW_GENICAM_ERROR(ErrorType::ParameterError,
                "Impossibile impostare il parametro: " + parameterName);

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione parametro: ") + e.GetDescription());
        }
        catch (const std::exception& e) {
            THROW_GENICAM_ERROR(ErrorType::ParameterError,
                std::string("Errore conversione valore: ") + e.what());
        }
    }

    bool GenICamCamera::isParameterAvailable(const std::string& parameterName) const {
        std::shared_lock<std::shared_mutex> lock(m_parameterMutex);

        if (!m_pNodeMap) return false;

        try {
            GenApi::CNodePtr node = m_pNodeMap->_GetNode(parameterName.c_str());
            return node.IsValid() && GenApi::IsImplemented(node);
        }
        catch (...) {
            return false;
        }
    }

    bool GenICamCamera::isParameterReadable(const std::string& parameterName) const {
        std::shared_lock<std::shared_mutex> lock(m_parameterMutex);

        if (!m_pNodeMap) return false;

        try {
            GenApi::CNodePtr node = m_pNodeMap->_GetNode(parameterName.c_str());
            return node.IsValid() && GenApi::IsReadable(node);
        }
        catch (...) {
            return false;
        }
    }

    bool GenICamCamera::isParameterWritable(const std::string& parameterName) const {
        std::shared_lock<std::shared_mutex> lock(m_parameterMutex);

        if (!m_pNodeMap) return false;

        try {
            GenApi::CNodePtr node = m_pNodeMap->_GetNode(parameterName.c_str());
            return node.IsValid() && GenApi::IsWritable(node);
        }
        catch (...) {
            return false;
        }
    }

    void GenICamCamera::debugAcquisitionParameters() const {
        std::cout << "\n=== Debug Parametri Acquisizione ===" << std::endl;

        // Verifica AcquisitionMode
        if (isParameterAvailable("AcquisitionMode")) {
            try {
                GenApi::CEnumerationPtr pAcqMode = getEnumerationNode("AcquisitionMode");
                GenApi::NodeList_t entries;
                pAcqMode->GetEntries(entries);

                std::cout << "AcquisitionMode supportati:" << std::endl;
                for (auto& entry : entries) {
                    if (GenApi::IsAvailable(entry)) {
                        std::cout << "  - " << entry->GetName().c_str() << std::endl;
                    }
                }
                std::cout << "Valore corrente: " << getParameter("AcquisitionMode") << std::endl;
            }
            catch (...) {}
        }
        else {
            std::cout << "AcquisitionMode: NON DISPONIBILE" << std::endl;
        }

        // Verifica altri parametri
        const std::vector<std::string> params = {
            "AcquisitionFrameRateEnable",
            "AcquisitionFrameRateEnableMode",
            "AcquisitionFrameRate",
            "TriggerMode",
            "TriggerSource",
            "TriggerSoftware",
            "AcquisitionStart",
            "AcquisitionStop"
        };

        for (const auto& param : params) {
            std::cout << param << ": ";
            if (isParameterAvailable(param)) {
                std::cout << "Disponibile";
                if (isParameterReadable(param)) std::cout << " [R]";
                if (isParameterWritable(param)) std::cout << " [W]";
                try {
                    std::cout << " = " << getParameter(param);
                }
                catch (...) {}
            }
            else {
                std::cout << "NON DISPONIBILE";
            }
            std::cout << std::endl;
        }
    }

    // === Load XML from Device ===

    void GenICamCamera::loadXMLFromDevice() {
        GenTL::INFO_DATATYPE dataType;
        uint64_t xmlAddress = 0;
        uint64_t xmlSize = 0;
        size_t infoSize = sizeof(uint64_t);

        GenTL::GC_ERROR err = GENTL_CALL(GCGetPortURLInfo)(m_portHandle, 0,
            GenTL::URL_INFO_FILE_REGISTER_ADDRESS, &dataType, &xmlAddress, &infoSize);

        if (err == GenTL::GC_ERR_SUCCESS) {
            err = GENTL_CALL(GCGetPortURLInfo)(m_portHandle, 0,
                GenTL::URL_INFO_FILE_SIZE, &dataType, &xmlSize, &infoSize);
        }

        if (err == GenTL::GC_ERR_SUCCESS && xmlSize > 0) {
            std::vector<uint8_t> xmlData(xmlSize);
            size_t readSize = xmlSize;
            err = GENTL_CALL(GCReadPort)(m_portHandle, xmlAddress, xmlData.data(), &readSize);

            if (err == GenTL::GC_ERR_SUCCESS) {
                m_pNodeMap = std::make_unique<GenApi::CNodeMapRef>();
                m_pNodeMap->_LoadXMLFromString(reinterpret_cast<const char*>(xmlData.data()));
                m_pNodeMap->_Connect(m_pCameraPort.get(), "Device");
            }
        }
    }

    // Funzioni helper mancanti per Gain e FrameRate range

    void GenICamCamera::getGainRange(double& min, double& max) const {
        try {
            // Setup GainSelector se necessario
            setupGainSelector();

            // Lista di possibili nomi
            const std::vector<std::string> gainNames = {
                "Gain", "GainRaw", "GainAbs", "AnalogGain", "DigitalGain", "Brightness"
            };

            for (const auto& gainName : gainNames) {
                try {
                    // Prova come float
                    GenApi::CFloatPtr pGain = getFloatNode(gainName);
                    if (pGain.IsValid() && GenApi::IsReadable(pGain)) {
                        min = pGain->GetMin();
                        max = pGain->GetMax();
                        return;
                    }
                }
                catch (...) {
                    // Prova come integer
                    try {
                        GenApi::CIntegerPtr pGainInt = getIntegerNode(gainName);
                        if (pGainInt.IsValid() && GenApi::IsReadable(pGainInt)) {
                            min = static_cast<double>(pGainInt->GetMin());
                            max = static_cast<double>(pGainInt->GetMax());

                            // Applica fattore di conversione se disponibile
                            if (gainName == "GainRaw") {
                                try {
                                    GenApi::CFloatPtr pGainFactor = getFloatNode("GainFactor");
                                    if (pGainFactor.IsValid()) {
                                        double factor = pGainFactor->GetValue();
                                        min *= factor;
                                        max *= factor;
                                    }
                                }
                                catch (...) {}
                            }

                            return;
                        }
                    }
                    catch (...) {
                        continue;
                    }
                }
            }

            // Valori di default se non trovato
            min = 0.0;
            max = 100.0;

        }
        catch (...) {
            // In caso di errore, ritorna valori di default
            min = 0.0;
            max = 100.0;
        }
    }

    // Metodo isGainAvailable aggiornato
    bool GenICamCamera::isGainAvailable() const {
        try {
            // Prima controlla se serve GainSelector
            bool hasGainSelector = false;
            try {
                GenApi::CNodePtr pGainSelector = m_pNodeMap->_GetNode("GainSelector");
                hasGainSelector = pGainSelector.IsValid() && GenApi::IsImplemented(pGainSelector);
            }
            catch (...) {}

            // Lista di possibili nomi di gain
            const std::vector<std::string> gainNames = {
                "Gain", "GainRaw", "GainAbs", "AnalogGain",
                "DigitalGain", "Brightness", "GainDB"
            };

            for (const auto& name : gainNames) {
                try {
                    GenApi::CNodePtr node = m_pNodeMap->_GetNode(name.c_str());

                    if (node.IsValid() && GenApi::IsImplemented(node)) {
                        // Verifica che sia effettivamente accessibile
                        bool isAccessible = GenApi::IsAvailable(node) &&
                            (GenApi::IsReadable(node) || GenApi::IsWritable(node));

                        if (isAccessible) {
                            // Se c'è GainSelector, verifica che almeno un'opzione sia disponibile
                            if (hasGainSelector) {
                                try {
                                    GenApi::CEnumerationPtr pGainSelector =
                                        getEnumerationNode("GainSelector");
                                    GenApi::NodeList_t entries;
                                    pGainSelector->GetEntries(entries);

                                    for (auto& entry : entries) {
                                        if (GenApi::IsAvailable(entry)) {
                                            return true;  // Almeno un selector è disponibile
                                        }
                                    }
                                    return false;  // Nessun selector disponibile
                                }
                                catch (...) {
                                    return true;  // Se errore nel check, assumiamo disponibile
                                }
                            }

                            return true;
                        }
                    }
                }
                catch (...) {
                    continue;
                }
            }

            return false;

        }
        catch (...) {
            return false;
        }
    }

    // Aggiungi anche questo metodo helper pubblico per debug
    std::string GenICamCamera::getGainInfo() const {
        std::stringstream info;

        info << "=== Gain Configuration ===" << std::endl;

        // Check GainSelector
        try {
            GenApi::CEnumerationPtr pGainSelector = getEnumerationNode("GainSelector");
            if (pGainSelector.IsValid()) {
                info << "GainSelector: " << pGainSelector->ToString() << std::endl;
                info << "  Available options: ";

                GenApi::NodeList_t entries;
                pGainSelector->GetEntries(entries);
                for (auto& entry : entries) {
                    if (GenApi::IsAvailable(entry)) {
                        info << entry->GetName().c_str() << " ";
                    }
                }
                info << std::endl;
            }
        }
        catch (...) {}

        // Check all gain nodes
        const std::vector<std::string> gainNames = {
            "Gain", "GainRaw", "GainAbs", "AnalogGain", "DigitalGain"
        };

        for (const auto& gainName : gainNames) {
            try {
                GenApi::CNodePtr pNode = getNode(gainName);
                if (pNode.IsValid() && GenApi::IsImplemented(pNode)) {
                    info << gainName << ": ";

                    // Float node
                    if (pNode->GetPrincipalInterfaceType() == GenApi::intfIFloat) {
                        GenApi::CFloatPtr pGain(pNode);
                        if (GenApi::IsReadable(pGain)) {
                            info << "Value=" << pGain->GetValue() << " ";
                            info << "Range=[" << pGain->GetMin() << ".." << pGain->GetMax() << "] ";

                            if (pGain->GetIncMode() != GenApi::noIncrement) {
                                info << "Inc=" << pGain->GetInc() << " ";
                            }

                            // Unit
                            try {
                                info << "Unit=" << pGain->GetUnit() << " ";
                            }
                            catch (...) {}
                        }
                    }
                    // Integer node
                    else if (pNode->GetPrincipalInterfaceType() == GenApi::intfIInteger) {
                        GenApi::CIntegerPtr pGain(pNode);
                        if (GenApi::IsReadable(pGain)) {
                            info << "Value=" << pGain->GetValue() << " ";
                            info << "Range=[" << pGain->GetMin() << ".." << pGain->GetMax() << "] ";

                            if (pGain->GetIncMode() != GenApi::noIncrement) {
                                info << "Inc=" << pGain->GetInc() << " ";
                            }
                        }
                    }

                    // Access mode
                    info << "Access=";
                    if (GenApi::IsReadable(pNode)) info << "R";
                    if (GenApi::IsWritable(pNode)) info << "W";

                    info << std::endl;
                }
            }
            catch (...) {}
        }

        // Current gain value
        try {
            double gain = getGain();
            info << "\nCurrent Gain: " << gain << std::endl;

            double min, max;
            getGainRange(min, max);
            info << "Gain Range: [" << min << " - " << max << "]" << std::endl;
        }
        catch (const std::exception& e) {
            info << "\nGain Error: " << e.what() << std::endl;
        }

        info << "Gain Available: " << (isGainAvailable() ? "Yes" : "No") << std::endl;

        return info.str();
    }
    void GenICamCamera::getFrameRateRange(double& min, double& max) const {
        try {
            const std::vector<std::string> frameRateNames = {
                "AcquisitionFrameRate", "FrameRate", "AcquisitionFrameRateAbs"
            };

            for (const auto& name : frameRateNames) {
                try {
                    GenApi::CFloatPtr pFrameRate = getFloatNode(name);
                    if (pFrameRate.IsValid()) {
                        min = pFrameRate->GetMin();
                        max = pFrameRate->GetMax();
                        return;
                    }
                }
                catch (...) {
                    continue;
                }
            }

        }
        catch (...) {}

        // Valori di default
        min = 1.0;
        max = 100.0;
    }

    // === Helper per conversione enum to string ===

    static const char* lineSelectorToString(LineSelector line) {
        switch (line) {
        case LineSelector::Line0: return "Line0";
        case LineSelector::Line1: return "Line1";
        case LineSelector::Line2: return "Line2";
        case LineSelector::Line3: return "Line3";
        case LineSelector::Line4: return "Line4";
        case LineSelector::Line5: return "Line5";
        case LineSelector::Line6: return "Line6";
        case LineSelector::Line7: return "Line7";
        case LineSelector::CC1: return "CC1";
        case LineSelector::CC2: return "CC2";
        case LineSelector::CC3: return "CC3";
        case LineSelector::CC4: return "CC4";
        default: return "Unknown";
        }
    }

    static const char* lineSourceToString(LineSource source) {
        switch (source) {
        case LineSource::Off: return "Off";
        case LineSource::ExposureActive: return "ExposureActive";
        case LineSource::FrameTriggerWait: return "FrameTriggerWait";
        case LineSource::FrameActive: return "FrameActive";
        case LineSource::FVAL: return "FVAL";
        case LineSource::LVAL: return "LVAL";
        case LineSource::UserOutput0: return "UserOutput0";
        case LineSource::UserOutput1: return "UserOutput1";
        case LineSource::UserOutput2: return "UserOutput2";
        case LineSource::UserOutput3: return "UserOutput3";
        case LineSource::Counter0Active: return "Counter0Active";
        case LineSource::Counter1Active: return "Counter1Active";
        case LineSource::Timer0Active: return "Timer0Active";
        case LineSource::Timer1Active: return "Timer1Active";
        case LineSource::Encoder0: return "Encoder0";
        case LineSource::Encoder1: return "Encoder1";
        case LineSource::SoftwareSignal0: return "SoftwareSignal0";
        case LineSource::SoftwareSignal1: return "SoftwareSignal1";
        case LineSource::Action0: return "Action0";
        case LineSource::Action1: return "Action1";
        default: return "Unknown";
        }
    }

    /**
 * @brief Converte TriggerSelector in stringa con fallback
 */
    std::string GenICamCamera::triggerSelectorToString(TriggerSelector selector) const {
       // Mappa standard SFNC
       static const std::map<TriggerSelector, std::vector<std::string>> selectorMap = {
           {TriggerSelector::FrameStart, {"FrameStart", "AcquisitionStart"}}, // Fallback per vecchie cam
           {TriggerSelector::FrameEnd, {"FrameEnd"}},
           {TriggerSelector::FrameBurstStart, {"FrameBurstStart", "FrameStart"}}, // Fallback
           {TriggerSelector::FrameBurstEnd, {"FrameBurstEnd", "FrameEnd"}},
           {TriggerSelector::LineStart, {"LineStart"}},
           {TriggerSelector::ExposureStart, {"ExposureStart"}},
           {TriggerSelector::ExposureEnd, {"ExposureEnd"}},
           {TriggerSelector::AcquisitionStart, {"AcquisitionStart"}},
           {TriggerSelector::AcquisitionEnd, {"AcquisitionEnd"}},
           {TriggerSelector::Action0, {"Action0"}},
           {TriggerSelector::Action1, {"Action1"}}
       };

       auto it = selectorMap.find(selector);
       if (it != selectorMap.end()) {
          // Verifica quale valore è supportato
          if (isParameterAvailable("TriggerSelector")) {
             try {
                GenApi::CEnumerationPtr pTriggerSelector = getEnumerationNode("TriggerSelector");
                GenApi::NodeList_t entries;
                pTriggerSelector->GetEntries(entries);

                // Cerca il primo valore supportato
                for (const auto& value : it->second) {
                   for (auto& entry : entries) {
                      if (entry->GetName().c_str() == value && GenApi::IsAvailable(entry)) {
                         return value;
                      }
                   }
                }
             }
             catch (...) {}
          }

          // Ritorna il primo valore come default
          return it->second[0];
       }

       return "FrameStart"; // Default SFNC
    }

    // === Trigger Avanzato ===

/**
 * @brief Imposta il TriggerSelector con verifica delle capacità
 */
    void GenICamCamera::setTriggerSelector(TriggerSelector selector) {
       try {
          // Verifica se TriggerSelector esiste
          if (!isParameterAvailable("TriggerSelector")) {
             // Alcune telecamere non hanno TriggerSelector e usano solo FrameStart
             // Non è un errore, semplicemente ignora
             return;
          }

          GenApi::CEnumerationPtr pTriggerSelector = getEnumerationNode("TriggerSelector");

          if (!GenApi::IsWritable(pTriggerSelector)) {
             // Se è readable ma non writable, potrebbe essere fisso su un valore
             // Non sollevare eccezione, solo log warning se necessario
             return;
          }

          // Ottieni il valore stringa appropriato con fallback
          std::string selectorStr = triggerSelectorToString(selector);

          // Verifica se il valore è effettivamente disponibile
          try {
             GenApi::CEnumEntryPtr pEntry = pTriggerSelector->GetEntryByName(selectorStr.c_str());
             if (!pEntry.IsValid() || !GenApi::IsAvailable(pEntry)) {
                // Prova con il default FrameStart
                selectorStr = "FrameStart";
                pEntry = pTriggerSelector->GetEntryByName(selectorStr.c_str());
                if (!pEntry.IsValid() || !GenApi::IsAvailable(pEntry)) {
                   // Nessun valore disponibile, mantieni quello corrente
                   return;
                }
             }
          }
          catch (...) {
             // Entry non trovata, usa il valore corrente
             return;
          }

          // Imposta il valore solo se diverso dal corrente
          std::string currentValue = pTriggerSelector->ToString().c_str();
          if (currentValue != selectorStr) {
             *pTriggerSelector = selectorStr.c_str();
             notifyParameterChanged("TriggerSelector", selectorStr);
          }

       }
       catch (const GENICAM_NAMESPACE::GenericException& e) {
          e.what();
          // Log warning invece di throw per maggiore compatibilità
          // Molte telecamere potrebbero non supportare tutti i selector
       }
    }

    /**
     * @brief Ottiene il TriggerSelector corrente con gestione errori
     */
    TriggerSelector GenICamCamera::getTriggerSelector() const {
       try {
          if (!isParameterAvailable("TriggerSelector")) {
             // Default a FrameStart se non disponibile
             return TriggerSelector::FrameStart;
          }

          GenApi::CEnumerationPtr pTriggerSelector = getEnumerationNode("TriggerSelector");
          std::string value = pTriggerSelector->ToString().c_str();

          // Mappa inversa per conversione
          static const std::map<std::string, TriggerSelector> stringToSelector = {
              {"FrameStart", TriggerSelector::FrameStart},
              {"FrameEnd", TriggerSelector::FrameEnd},
              {"FrameBurstStart", TriggerSelector::FrameBurstStart},
              {"FrameBurstEnd", TriggerSelector::FrameBurstEnd},
              {"LineStart", TriggerSelector::LineStart},
              {"ExposureStart", TriggerSelector::ExposureStart},
              {"ExposureEnd", TriggerSelector::ExposureEnd},
              {"AcquisitionStart", TriggerSelector::AcquisitionStart},
              {"AcquisitionEnd", TriggerSelector::AcquisitionEnd},
              {"Action0", TriggerSelector::Action0},
              {"Action1", TriggerSelector::Action1}
          };

          auto it = stringToSelector.find(value);
          if (it != stringToSelector.end()) {
             return it->second;
          }

          // Se AcquisitionStart è mappato a FrameStart (vecchie cam)
          if (value == "AcquisitionStart") {
             return TriggerSelector::FrameStart;
          }

          return TriggerSelector::FrameStart; // Default

       }
       catch (...) {
          return TriggerSelector::FrameStart;
       }
    }

    /**
     * @brief Ottiene i TriggerSelector disponibili con caching
     */
    std::vector<TriggerSelector> GenICamCamera::getAvailableTriggerSelectors() const {
       // Usa cache se disponibile
       if (m_triggerSelectorsCached) {
          return m_cachedTriggerSelectors;
       }

       std::vector<TriggerSelector> selectors;

       try {
          if (!isParameterAvailable("TriggerSelector")) {
             // Se non c'è TriggerSelector, assume solo FrameStart
             selectors.push_back(TriggerSelector::FrameStart);
          }
          else {
             GenApi::CEnumerationPtr pTriggerSelector = getEnumerationNode("TriggerSelector");
             GenApi::NodeList_t entries;
             pTriggerSelector->GetEntries(entries);

             for (auto& entry : entries) {
                if (GenApi::IsAvailable(entry)) {
                   std::string name = entry->GetName().c_str();

                   // Mappa a enum
                   if (name == "FrameStart") selectors.push_back(TriggerSelector::FrameStart);
                   else if (name == "FrameEnd") selectors.push_back(TriggerSelector::FrameEnd);
                   else if (name == "FrameBurstStart") selectors.push_back(TriggerSelector::FrameBurstStart);
                   else if (name == "FrameBurstEnd") selectors.push_back(TriggerSelector::FrameBurstEnd);
                   else if (name == "LineStart") selectors.push_back(TriggerSelector::LineStart);
                   else if (name == "ExposureStart") selectors.push_back(TriggerSelector::ExposureStart);
                   else if (name == "ExposureEnd") selectors.push_back(TriggerSelector::ExposureEnd);
                   else if (name == "AcquisitionStart") {
                      // Potrebbe essere mappato a FrameStart
                      if (std::find(selectors.begin(), selectors.end(),
                         TriggerSelector::FrameStart) == selectors.end()) {
                         selectors.push_back(TriggerSelector::AcquisitionStart);
                      }
                   }
                   else if (name == "AcquisitionEnd") selectors.push_back(TriggerSelector::AcquisitionEnd);
                   else if (name == "Action0") selectors.push_back(TriggerSelector::Action0);
                   else if (name == "Action1") selectors.push_back(TriggerSelector::Action1);
                }
             }
          }

          // Se nessun selector trovato, aggiungi default
          if (selectors.empty()) {
             selectors.push_back(TriggerSelector::FrameStart);
          }

       }
       catch (...) {
          // In caso di errore, ritorna almeno FrameStart
          selectors.push_back(TriggerSelector::FrameStart);
       }

       // Salva in cache
       m_cachedTriggerSelectors = selectors;
       m_triggerSelectorsCached = true;

       return selectors;
    }

    void GenICamCamera::setTriggerDelay(double delayUs) {
        try {
            GenApi::CFloatPtr pTriggerDelay = getFloatNode("TriggerDelay");

            if (!GenApi::IsWritable(pTriggerDelay)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "TriggerDelay non scrivibile");
            }

            double min = pTriggerDelay->GetMin();
            double max = pTriggerDelay->GetMax();

            if (delayUs < min || delayUs > max) {
                std::stringstream ss;
                ss << "Trigger delay fuori range [" << min << ", " << max << "]";
                THROW_GENICAM_ERROR(ErrorType::ParameterError, ss.str());
            }

            pTriggerDelay->SetValue(delayUs);
            notifyParameterChanged("TriggerDelay", std::to_string(delayUs));

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione TriggerDelay: ") + e.GetDescription());
        }
    }

    double GenICamCamera::getTriggerDelay() const {
        try {
            GenApi::CFloatPtr pTriggerDelay = getFloatNode("TriggerDelay");
            return pTriggerDelay->GetValue();
        }
        catch (...) {
            return 0.0;
        }
    }

    void GenICamCamera::getTriggerDelayRange(double& min, double& max) const {
        try {
            GenApi::CFloatPtr pTriggerDelay = getFloatNode("TriggerDelay");
            min = pTriggerDelay->GetMin();
            max = pTriggerDelay->GetMax();
        }
        catch (...) {
            min = 0.0;
            max = 1000000.0; // 1 secondo default
        }
    }

    void GenICamCamera::setTriggerDivider(uint32_t divider) {
        try {
            GenApi::CIntegerPtr pTriggerDivider = getIntegerNode("TriggerDivider");

            if (!GenApi::IsWritable(pTriggerDivider)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "TriggerDivider non scrivibile");
            }

            int64_t min = pTriggerDivider->GetMin();
            int64_t max = pTriggerDivider->GetMax();

            if (divider < min || divider > max) {
                std::stringstream ss;
                ss << "Trigger divider fuori range [" << min << ", " << max << "]";
                THROW_GENICAM_ERROR(ErrorType::ParameterError, ss.str());
            }

            pTriggerDivider->SetValue(divider);
            notifyParameterChanged("TriggerDivider", std::to_string(divider));

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione TriggerDivider: ") + e.GetDescription());
        }
    }

    uint32_t GenICamCamera::getTriggerDivider() const {
        try {
            GenApi::CIntegerPtr pTriggerDivider = getIntegerNode("TriggerDivider");
            return static_cast<uint32_t>(pTriggerDivider->GetValue());
        }
        catch (...) {
            return 1;
        }
    }

    // === Gestione Transport Layer Lock secondo SFNC ===

    /**
     * @brief Gestisce il blocco/sblocco dei parametri Transport Layer
     *
     * Secondo SFNC, alcuni vendor implementano meccanismi di lock per proteggere
     * i parametri critici durante lo streaming. Questo metodo gestisce le varie
     * implementazioni in modo portabile.
     *
     * @param lock true per bloccare, false per sbloccare
     * @return true se l'operazione ha successo o il parametro non esiste
     */
    bool GenICamCamera::setTransportLayerLock(bool lock) {
        // Lista dei possibili nomi del parametro secondo diversi vendor
        // Ordinati per frequenza d'uso
        const std::vector<std::pair<std::string, std::string>> lockParams = {
            std::make_pair("TLParamsLocked", "1"),              // Hikrobot, MVTec, Dalsa
            std::make_pair("StreamEnable", lock ? "true" : "false"),     // Basler
            std::make_pair("AcquisitionEnable", lock ? "true" : "false"), // Some GigE cameras
            std::make_pair("GevStreamChannelSelector", "0"),    // GigE Vision specific
            std::make_pair("StreamChannelEnable", lock ? "true" : "false") // Alternative naming
        };

        // Flag per tracciare se abbiamo trovato almeno un parametro
        bool paramFound = false;
        std::string usedParam;

        // Prova ogni possibile parametro
        for (const auto& lockParam : lockParams) {
            const std::string& paramName = lockParam.first;
            const std::string& lockValue = lockParam.second;

            try {
                if (!isParameterAvailable(paramName)) {
                    continue;
                }

                // Per GevStreamChannelSelector, è un selettore, non un lock
                if (paramName == "GevStreamChannelSelector") {
                    // Imposta il selettore prima di modificare StreamChannelEnable
                    setParameter(paramName, "0");
                    continue;
                }

                // Verifica se il parametro è scrivibile
                if (!isParameterWritable(paramName)) {
                    std::cout << "Info: " << paramName << " trovato ma non scrivibile"
                        << std::endl;
                    continue;
                }

                // Determina il valore corretto basato sul tipo di parametro
                std::string setValue;
                if (paramName == "TLParamsLocked") {
                    setValue = lock ? "1" : "0";
                }
                else {
                    setValue = lock ? lockValue : (lockValue == "true" ? "false" : "0");
                }

                // Imposta il valore
                setParameter(paramName, setValue);

                // Verifica che sia stato impostato correttamente
                if (isParameterReadable(paramName)) {
                    std::string readback = getParameter(paramName);
                    if (readback == setValue) {
                        paramFound = true;
                        usedParam = paramName;

                        // Log solo in modalità debug
#ifdef _DEBUG
                        std::cout << "Transport layer " << (lock ? "locked" : "unlocked")
                            << " usando: " << paramName << std::endl;
#endif

                        break; // Successo, esci dal loop
                    }
                }
            }
            catch (const std::exception& e) {
                // Log solo in modalità debug
#ifdef _DEBUG
                std::cerr << "Errore con parametro " << paramName << ": "
                    << e.what() << std::endl;
#endif
                continue;
            }
        }

        // Se non abbiamo trovato nessun parametro, non è necessariamente un errore
        // Alcune telecamere non richiedono questo lock
        if (!paramFound && lock) {
            // Solo in debug, informiamo che non abbiamo trovato parametri di lock
#ifdef _DEBUG
            std::cout << "Info: Nessun parametro di Transport Layer lock trovato. "
                << "Potrebbe non essere necessario per questa camera." << std::endl;
#endif
        }

        return true; // Ritorna sempre true per non bloccare l'acquisizione
    }
    /**
     * @brief Prepara i parametri Transport Layer per l'acquisizione
     *
     * Metodo SFNC-compliant che configura tutti i parametri necessari
     * prima di iniziare lo streaming.
     */
    void GenICamCamera::prepareTransportLayerForAcquisition() {
        // 1. Gestione Packet Size per GigE Vision (SFNC)
        if (isParameterAvailable("GevSCPSPacketSize")) {
            try {
                // Ottieni il valore ottimale di packet size
                int64_t optimalPacketSize = getOptimalPacketSize();

                GenApi::CIntegerPtr pPacketSize = getIntegerNode("GevSCPSPacketSize");
                if (GenApi::IsWritable(pPacketSize)) {
                    int64_t min = pPacketSize->GetMin();
                    int64_t max = pPacketSize->GetMax();
                    int64_t inc = pPacketSize->GetInc();

                    // Clamp al range valido
                    optimalPacketSize = (std::max)(min, (std::min)(optimalPacketSize, max));

                    // Allinea all'incremento
                    if (inc > 1) {
                        optimalPacketSize = (optimalPacketSize / inc) * inc;
                    }

                    pPacketSize->SetValue(optimalPacketSize);

#ifdef _DEBUG
                    std::cout << "GevSCPSPacketSize impostato a: " << optimalPacketSize
                        << " bytes" << std::endl;
#endif
                }
            }
            catch (...) {
                // Non critico, usa default
            }
        }

        // 2. Inter-Packet Delay per GigE (SFNC)
        if (isParameterAvailable("GevSCPD")) {
            try {
                // Calcola delay ottimale basato su bandwidth
                int64_t optimalDelay = calculateOptimalInterPacketDelay();
                setParameter("GevSCPD", std::to_string(optimalDelay));
            }
            catch (...) {
                // Non critico
            }
        }

        // 3. Gestione Buffer Mode secondo SFNC
        if (isParameterAvailable("StreamBufferHandlingMode")) {
            try {
                // "NewestOnly" è raccomandato per applicazioni real-time
                setParameter("StreamBufferHandlingMode", "NewestOnly");
            }
            catch (...) {
                try {
                    // Fallback a "OldestFirst" se NewestOnly non disponibile
                    setParameter("StreamBufferHandlingMode", "OldestFirst");
                }
                catch (...) {
                    // Usa default
                }
            }
        }

        // 4. Timeout configuration (SFNC)
        configureTimeouts();

        // 5. Abilita Event notification se supportato
        if (isParameterAvailable("EventNotification")) {
            try {
                setParameter("EventNotification", "On");
            }
            catch (...) {
                // Non critico
            }
        }
    }

    /**
     * @brief Calcola il packet size ottimale per GigE Vision
     */
    int64_t GenICamCamera::getOptimalPacketSize() const {
        // Default: Jumbo frames se supportati, altrimenti standard
        int64_t defaultSize = 1500; // Standard Ethernet MTU

        try {
            // Verifica se jumbo frames sono supportati
            if (isParameterAvailable("GevSCPSPacketSize")) {
                GenApi::CIntegerPtr pPacketSize = getIntegerNode("GevSCPSPacketSize");
                if (pPacketSize.IsValid()) {
                    int64_t max = pPacketSize->GetMax();
                    if (max >= 9000) {
                        // Jumbo frames supportati
                        return 8192; // Valore sicuro per jumbo frames
                    }
                }
            }
        }
        catch (...) {}

        return defaultSize;
    }

    /**
     * @brief Calcola l'inter-packet delay ottimale
     */
    int64_t GenICamCamera::calculateOptimalInterPacketDelay() const {
        // Formula base: delay = (packet_size * 8) / (bandwidth * factor)
        // dove factor < 1 per lasciare margine alla rete

        try {
            if (isParameterAvailable("GevSCPSPacketSize") &&
                isParameterAvailable("DeviceLinkSpeed")) {

                int64_t packetSize = std::stoll(getParameter("GevSCPSPacketSize"));
                int64_t linkSpeed = std::stoll(getParameter("DeviceLinkSpeed")); // in Mbps

                // Usa 70% della bandwidth disponibile
                double factor = 0.7;

                // Calcola delay in nanosecondi
                int64_t delay = static_cast<int64_t>((packetSize * 8.0) / (linkSpeed * factor));

                // Converti in tick del timestamp (di solito nanosecondi)
                return (std::max)(int64_t(0), delay);
            }
        }
        catch (...) {}

        // Default conservativo: 1000 ns
        return 1000;
    }

    /**
     * @brief Configura i timeout secondo SFNC
     */
    void GenICamCamera::configureTimeouts() {
        
        // Lista dei timeout SFNC con valori di default in microsecondi
        const std::vector<std::pair<std::string, int64_t>> timeouts = {
            {"GevSCFTD", 5000000},                  // Frame Timeout: 5 secondi
            {"GevSCPHostReceiveTimeout", 1000000},  // Host Receive: 1 secondo
            {"TransferTimeout", 5000000},           // Transfer: 5 secondi
            {"AcquisitionStatusTimeout", 10000000}  // Acquisition Status: 10 secondi
        };

        for (const auto& timeout : timeouts) {
            const std::string& param = timeout.first;
            const int64_t& value = timeout.second;

            if (isParameterAvailable(param) && isParameterWritable(param)) {
                try {
                    setParameter(param, std::to_string(value));
                }
                catch (...) {
                    // Usa default del dispositivo
                }
            }
        }
    }


    // === Gestione Linee I/O ===

    void GenICamCamera::setLineSelector(LineSelector line) {
        try {
            GenApi::CEnumerationPtr pLineSelector = getEnumerationNode("LineSelector");

            if (!GenApi::IsWritable(pLineSelector)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "LineSelector non scrivibile");
            }

            *pLineSelector = lineSelectorToString(line);
            notifyParameterChanged("LineSelector", lineSelectorToString(line));

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione LineSelector: ") + e.GetDescription());
        }
    }

    LineSelector GenICamCamera::getLineSelector() const {
        try {
            GenApi::CEnumerationPtr pLineSelector = getEnumerationNode("LineSelector");
            std::string value = pLineSelector->ToString().c_str();

            // Parse common line names
            if (value == "Line0") return LineSelector::Line0;
            if (value == "Line1") return LineSelector::Line1;
            if (value == "Line2") return LineSelector::Line2;
            if (value == "Line3") return LineSelector::Line3;
            if (value == "Line4") return LineSelector::Line4;
            if (value == "Line5") return LineSelector::Line5;
            if (value == "Line6") return LineSelector::Line6;
            if (value == "Line7") return LineSelector::Line7;
            if (value == "CC1") return LineSelector::CC1;
            if (value == "CC2") return LineSelector::CC2;
            if (value == "CC3") return LineSelector::CC3;
            if (value == "CC4") return LineSelector::CC4;

            return LineSelector::Line0; // Default

        }
        catch (...) {
            return LineSelector::Line0;
        }
    }

    std::vector<LineSelector> GenICamCamera::getAvailableLines() const {
        std::vector<LineSelector> lines;

        try {
            GenApi::CEnumerationPtr pLineSelector = getEnumerationNode("LineSelector");
            GenApi::NodeList_t entries;
            pLineSelector->GetEntries(entries);

            for (auto& entry : entries) {
                if (GenApi::IsAvailable(entry)) {
                    std::string name = entry->GetName().c_str();

                    if (name == "Line0") lines.push_back(LineSelector::Line0);
                    else if (name == "Line1") lines.push_back(LineSelector::Line1);
                    else if (name == "Line2") lines.push_back(LineSelector::Line2);
                    else if (name == "Line3") lines.push_back(LineSelector::Line3);
                    else if (name == "Line4") lines.push_back(LineSelector::Line4);
                    else if (name == "Line5") lines.push_back(LineSelector::Line5);
                    else if (name == "Line6") lines.push_back(LineSelector::Line6);
                    else if (name == "Line7") lines.push_back(LineSelector::Line7);
                    else if (name == "CC1") lines.push_back(LineSelector::CC1);
                    else if (name == "CC2") lines.push_back(LineSelector::CC2);
                    else if (name == "CC3") lines.push_back(LineSelector::CC3);
                    else if (name == "CC4") lines.push_back(LineSelector::CC4);
                }
            }
        }
        catch (...) {}

        return lines;
    }

    void GenICamCamera::setLineMode(LineMode mode) {
        try {
            GenApi::CEnumerationPtr pLineMode = getEnumerationNode("LineMode");

            if (!GenApi::IsWritable(pLineMode)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "LineMode non scrivibile");
            }

            *pLineMode = (mode == LineMode::Input) ? "Input" : "Output";
            notifyParameterChanged("LineMode", (mode == LineMode::Input) ? "Input" : "Output");

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione LineMode: ") + e.GetDescription());
        }
    }

    LineMode GenICamCamera::getLineMode() const {
        try {
            GenApi::CEnumerationPtr pLineMode = getEnumerationNode("LineMode");
            std::string value = pLineMode->ToString().c_str();
            return (value == "Input") ? LineMode::Input : LineMode::Output;
        }
        catch (...) {
            return LineMode::Input;
        }
    }

    bool GenICamCamera::getLineStatus() const {
        try {
            GenApi::CBooleanPtr pLineStatus = getBooleanNode("LineStatus");
            return pLineStatus->GetValue();
        }
        catch (...) {
            return false;
        }
    }

    void GenICamCamera::setLineInverter(bool invert) {
        try {
            GenApi::CBooleanPtr pLineInverter = getBooleanNode("LineInverter");

            if (!GenApi::IsWritable(pLineInverter)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "LineInverter non scrivibile");
            }

            pLineInverter->SetValue(invert);
            notifyParameterChanged("LineInverter", invert ? "true" : "false");

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione LineInverter: ") + e.GetDescription());
        }
    }

    bool GenICamCamera::getLineInverter() const {
        try {
            GenApi::CBooleanPtr pLineInverter = getBooleanNode("LineInverter");
            return pLineInverter->GetValue();
        }
        catch (...) {
            return false;
        }
    }

    void GenICamCamera::setLineSource(LineSource source) {
        try {
            GenApi::CEnumerationPtr pLineSource = getEnumerationNode("LineSource");

            if (!GenApi::IsWritable(pLineSource)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "LineSource non scrivibile");
            }

            *pLineSource = lineSourceToString(source);
            notifyParameterChanged("LineSource", lineSourceToString(source));

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione LineSource: ") + e.GetDescription());
        }
    }

    LineSource GenICamCamera::getLineSource() const {
        try {
            GenApi::CEnumerationPtr pLineSource = getEnumerationNode("LineSource");
            std::string value = pLineSource->ToString().c_str();

            // Conversione string to enum
            if (value == "Off") return LineSource::Off;
            if (value == "ExposureActive") return LineSource::ExposureActive;
            if (value == "FrameTriggerWait") return LineSource::FrameTriggerWait;
            if (value == "FrameActive") return LineSource::FrameActive;
            if (value == "FVAL") return LineSource::FVAL;
            if (value == "LVAL") return LineSource::LVAL;
            if (value == "UserOutput0") return LineSource::UserOutput0;
            if (value == "UserOutput1") return LineSource::UserOutput1;
            if (value == "UserOutput2") return LineSource::UserOutput2;
            if (value == "UserOutput3") return LineSource::UserOutput3;
            // ... altri casi ...

            return LineSource::Off;

        }
        catch (...) {
            return LineSource::Off;
        }
    }

    void GenICamCamera::setLineDebouncerTime(double timeUs) {
        try {
            GenApi::CFloatPtr pDebouncer = getFloatNode("LineDebouncerTime");

            if (!GenApi::IsWritable(pDebouncer)) {
                // Prova nome alternativo
                pDebouncer = getFloatNode("LineDebouncerTimeAbs");
                if (!GenApi::IsWritable(pDebouncer)) {
                    THROW_GENICAM_ERROR(ErrorType::ParameterError,
                        "LineDebouncerTime non scrivibile");
                }
            }

            double min = pDebouncer->GetMin();
            double max = pDebouncer->GetMax();

            if (timeUs < min || timeUs > max) {
                std::stringstream ss;
                ss << "Debouncer time fuori range [" << min << ", " << max << "]";
                THROW_GENICAM_ERROR(ErrorType::ParameterError, ss.str());
            }

            pDebouncer->SetValue(timeUs);
            notifyParameterChanged("LineDebouncerTime", std::to_string(timeUs));

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione LineDebouncerTime: ") + e.GetDescription());
        }
    }

    double GenICamCamera::getLineDebouncerTime() const {
        try {
            GenApi::CFloatPtr pDebouncer;
            try {
                pDebouncer = getFloatNode("LineDebouncerTime");
            }
            catch (...) {
                pDebouncer = getFloatNode("LineDebouncerTimeAbs");
            }
            return pDebouncer->GetValue();
        }
        catch (...) {
            return 0.0;
        }
    }

/*    LineStatus GenICamCamera::getLineFullStatus(LineSelector line) {
        LineStatus status;

        // Salva selezione corrente
        LineSelector currentSelection = getLineSelector();

        // Seleziona la linea richiesta
        setLineSelector(line);

        // Leggi tutti i parametri
        status.value = getLineStatus();
        status.mode = getLineMode();
        status.inverter = getLineInverter();

        if (status.mode == LineMode::Output) {
            status.source = getLineSource();
        }
        else {
            status.source = LineSource::Off;
        }

        status.debounceTime = getLineDebouncerTime();

        // Prova a leggere LineFormat se disponibile
        try {
            GenApi::CEnumerationPtr pLineFormat = getEnumerationNode("LineFormat");
            status.format = pLineFormat->ToString().c_str();
        }
        catch (...) {
            status.format = "Unknown";
        }

        // Ripristina selezione originale
        setLineSelector(currentSelection);

        return status;
    }*/

    LineStatus GenICamCamera::getLineFullStatus(LineSelector line) const {
        LineStatus status;

        // Salva selezione corrente  
        LineSelector currentSelection = getLineSelector();

        // Seleziona la linea richiesta  
        const_cast<GenICamCamera*>(this)->setLineSelector(line);

        // Leggi tutti i parametri  
        status.value = getLineStatus();
        status.mode = getLineMode();
        status.inverter = getLineInverter();

        if (status.mode == LineMode::Output) {
            status.source = getLineSource();
        }
        else {
            status.source = LineSource::Off;
        }

        status.debounceTime = getLineDebouncerTime();

        // Prova a leggere LineFormat se disponibile  
        try {
            GenApi::CEnumerationPtr pLineFormat = getEnumerationNode("LineFormat");
            status.format = pLineFormat->ToString().c_str();
        }
        catch (...) {
            status.format = "Unknown";
        }

        // Ripristina selezione originale  
        const_cast<GenICamCamera*>(this)->setLineSelector(currentSelection);

        return status;
    }

    // === User Output Control ===

    void GenICamCamera::setUserOutputSelector(UserOutputSelector output) {
        try {
            GenApi::CEnumerationPtr pSelector = getEnumerationNode("UserOutputSelector");

            if (!GenApi::IsWritable(pSelector)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "UserOutputSelector non scrivibile");
            }

            std::string outputStr;
            switch (output) {
            case UserOutputSelector::UserOutput0: outputStr = "UserOutput0"; break;
            case UserOutputSelector::UserOutput1: outputStr = "UserOutput1"; break;
            case UserOutputSelector::UserOutput2: outputStr = "UserOutput2"; break;
            case UserOutputSelector::UserOutput3: outputStr = "UserOutput3"; break;
            }

            *pSelector = outputStr.c_str();
            notifyParameterChanged("UserOutputSelector", outputStr);

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione UserOutputSelector: ") + e.GetDescription());
        }
    }

    void GenICamCamera::setUserOutputValue(bool value) {
        try {
            GenApi::CBooleanPtr pValue = getBooleanNode("UserOutputValue");

            if (!GenApi::IsWritable(pValue)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "UserOutputValue non scrivibile");
            }

            pValue->SetValue(value);
            notifyParameterChanged("UserOutputValue", value ? "true" : "false");

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione UserOutputValue: ") + e.GetDescription());
        }
    }

    bool GenICamCamera::getUserOutputValue() const {
        try {
            GenApi::CBooleanPtr pValue = getBooleanNode("UserOutputValue");
            return pValue->GetValue();
        }
        catch (...) {
            return false;
        }
    }

    // Versione compatibile C++14 per setAllUserOutputs

    void GenICamCamera::setAllUserOutputs(const std::map<UserOutputSelector, bool>& values) {
        // Salva selezione corrente
        UserOutputSelector currentSelection = getUserOutputSelector();

        try {
            // Versione C++14 - usa iteratore esplicito
            for (std::map<UserOutputSelector, bool>::const_iterator it = values.begin();
                it != values.end(); ++it) {
                setUserOutputSelector(it->first);   // output selector
                setUserOutputValue(it->second);     // value
            }
        }
        catch (...) {
            // Ripristina selezione anche in caso di errore
            setUserOutputSelector(currentSelection);
            throw;
        }

        // Ripristina selezione originale
        setUserOutputSelector(currentSelection);
    }

    // === Counter Control ===
    void GenICamCamera::setCounterSelector(CounterSelector counter) {
        try {
            GenApi::CEnumerationPtr pSelector = getEnumerationNode("CounterSelector");

            if (!GenApi::IsWritable(pSelector)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "CounterSelector non scrivibile");
            }

            std::string counterStr;
            switch (counter) {
            case CounterSelector::Counter0: counterStr = "Counter0"; break;
            case CounterSelector::Counter1: counterStr = "Counter1"; break;
            case CounterSelector::Counter2: counterStr = "Counter2"; break;
            case CounterSelector::Counter3: counterStr = "Counter3"; break;
            }

            *pSelector = counterStr.c_str();
            notifyParameterChanged("CounterSelector", counterStr);

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione CounterSelector: ") + e.GetDescription());
        }
    }

    uint64_t GenICamCamera::getCounterValue() const {
        try {
            GenApi::CIntegerPtr pValue = getIntegerNode("CounterValue");
            return static_cast<uint64_t>(pValue->GetValue());
        }
        catch (...) {
            return 0;
        }
    }

    void GenICamCamera::resetCounter() {
        try {
            GenApi::CCommandPtr pReset = getCommandNode("CounterReset");

            if (!GenApi::IsWritable(pReset)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "CounterReset non eseguibile");
            }

            pReset->Execute();
            while (!pReset->IsDone()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            notifyParameterChanged("CounterReset", "Executed");

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore reset counter: ") + e.GetDescription());
        }
    }

    // === Action Control ===

    void GenICamCamera::configureActionCommand(uint32_t actionIndex,
        uint32_t deviceKey,
        uint32_t groupKey,
        uint32_t groupMask) {
        try {
            // Seleziona l'action
            GenApi::CIntegerPtr pActionSelector = getIntegerNode("ActionSelector");
            if (GenApi::IsWritable(pActionSelector)) {
                pActionSelector->SetValue(actionIndex);
            }

            // Configura device key
            GenApi::CIntegerPtr pDeviceKey = getIntegerNode("ActionDeviceKey");
            if (GenApi::IsWritable(pDeviceKey)) {
                pDeviceKey->SetValue(deviceKey);
            }

            // Configura group key
            GenApi::CIntegerPtr pGroupKey = getIntegerNode("ActionGroupKey");
            if (GenApi::IsWritable(pGroupKey)) {
                pGroupKey->SetValue(groupKey);
            }

            // Configura group mask
            GenApi::CIntegerPtr pGroupMask = getIntegerNode("ActionGroupMask");
            if (GenApi::IsWritable(pGroupMask)) {
                pGroupMask->SetValue(groupMask);
            }

            std::stringstream ss;
            ss << "Action" << actionIndex << " configured";
            notifyParameterChanged("ActionCommand", ss.str());

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore configurazione action command: ") + e.GetDescription());
        }
    }

    // === Pulse Generator ===

    void GenICamCamera::configurePulseGenerator(LineSelector lineOutput,
        double frequencyHz,
        double dutyCycle,
        uint32_t pulseCount) {
        // Implementazione dipende dal modello di camera
        // Alcuni usano parametri dedicati, altri timer/counter configurati

        try {
            // Esempio generico usando timer
            setTimerSelector(TimerSelector::Timer0);

            // Calcola durata del timer
            double periodUs = 1000000.0 / frequencyHz;
            double highTimeUs = periodUs * dutyCycle;

            setTimerDuration(highTimeUs);
            setTimerDelay(periodUs - highTimeUs);

            // Configura linea output
            setLineSelector(lineOutput);
            setLineMode(LineMode::Output);
            setLineSource(LineSource::Timer0Active);

            // Se supportato, imposta conteggio impulsi
            if (pulseCount > 0) {
                try {
                    GenApi::CIntegerPtr pPulseCount = getIntegerNode("TimerPulseCount");
                    if (GenApi::IsWritable(pPulseCount)) {
                        pPulseCount->SetValue(pulseCount);
                    }
                }
                catch (...) {}
            }

            std::stringstream ss;
            ss << "PulseGen: " << frequencyHz << "Hz, "
                << (dutyCycle * 100) << "% duty";
            notifyParameterChanged("PulseGenerator", ss.str());

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore configurazione pulse generator: ") + e.GetDescription());
        }
    }

    // === Utility Functions ===

    std::string GenICamCamera::getIOStatusReport() const {
        std::stringstream report;

        report << "=== I/O Status Report ===" << std::endl;
        report << "Camera: " << getCameraModel() << std::endl;
        report << "Serial: " << getCameraSerialNumber() << std::endl;
        report << std::endl;

        // Report delle linee disponibili
        auto lines = getAvailableLines();
        report << "Available I/O Lines: " << lines.size() << std::endl;

        for (const auto& line : lines) {
            LineStatus status = getLineFullStatus(line);
            
            report << "\n" << lineSelectorToString(line) << ":" << std::endl;
            report << "  Mode: " << (status.mode == LineMode::Input ? "Input" : "Output") << std::endl;
            report << "  Status: " << (status.value ? "High" : "Low") << std::endl;
            report << "  Inverter: " << (status.inverter ? "Enabled" : "Disabled") << std::endl;

            if (status.mode == LineMode::Output) {
                report << "  Source: " << lineSourceToString(status.source) << std::endl;
            }
            else {
                report << "  Debounce: " << status.debounceTime << " us" << std::endl;
            }

            report << "  Format: " << status.format << std::endl;
        }

        // Report trigger
        report << "\nTrigger Configuration:" << std::endl;
        try {
            auto triggers = getAvailableTriggerSelectors();
            for (const auto& trigger : triggers) {
                const_cast<GenICamCamera*>(this)->setTriggerSelector(trigger);
                report << "\n" << triggerSelectorToString(trigger) << ":" << std::endl;
                report << "  Mode: ";
                switch (getTriggerMode()) {
                case TriggerMode::Off: report << "Off"; break;
                case TriggerMode::On: report << "On"; break;
                }
                report << std::endl;

                if (getTriggerMode() != TriggerMode::Off) {
                    report << "  Delay: " << getTriggerDelay() << " us" << std::endl;
                    report << "  Divider: " << getTriggerDivider() << std::endl;
                }
            }
        }
        catch (...) {}

        // Report counters
        report << "\nCounters:" << std::endl;
        try {
            for (int i = 0; i < 4; ++i) {
                try {
                    const_cast<GenICamCamera*>(this)->setCounterSelector(static_cast<CounterSelector>(i));
                    uint64_t value = getCounterValue();
                    report << "  Counter" << i << ": " << value << std::endl;
                }
                catch (...) {
                    break;
                }
            }
        }
        catch (...) {}

        return report.str();
    }

    std::string GenICamCamera::testIOLines() {
        std::stringstream report;

        report << "=== I/O Lines Test ===" << std::endl;

        auto lines = getAvailableLines();

        for (const auto& line : lines) {
            report << "\nTesting " << lineSelectorToString(line) << ":" << std::endl;

            setLineSelector(line);
            LineMode mode = getLineMode();

            if (mode == LineMode::Output) {
                // Test output toggling
                report << "  Output test: ";

                try {
                    // Set to user control
                    setLineSource(LineSource::UserOutput0);

                    // Test high
                    setUserOutputSelector(UserOutputSelector::UserOutput0);
                    setUserOutputValue(true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    bool highState = getLineStatus();

                    // Test low
                    setUserOutputValue(false);
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    bool lowState = getLineStatus();

                    if (highState && !lowState) {
                        report << "PASSED (Toggle OK)" << std::endl;
                    }
                    else {
                        report << "FAILED (No toggle detected)" << std::endl;
                    }
                }
                catch (...) {
                    report << "ERROR (Exception)" << std::endl;
                }
            }
            else {
                // Test input reading
                report << "  Input test: ";

                try {
                    bool status = getLineStatus();
                    report << "Current state = " << (status ? "High" : "Low") << std::endl;

                    // Test debouncer if available
                    try {
                        double oldDebounce = getLineDebouncerTime();
                        setLineDebouncerTime(1000.0); // 1ms
                        double newDebounce = getLineDebouncerTime();

                        if (abs(newDebounce - 1000.0) < 100.0) {
                            report << "  Debouncer: PASSED" << std::endl;
                        }
                        else {
                            report << "  Debouncer: FAILED" << std::endl;
                        }

                        setLineDebouncerTime(oldDebounce);
                    }
                    catch (...) {
                        report << "  Debouncer: Not available" << std::endl;
                    }
                }
                catch (...) {
                    report << "ERROR (Exception)" << std::endl;
                }
            }
        }

        return report.str();
    }

    // === Helper aggiuntivi per accesso nodi GenApi ===

    GenApi::CBooleanPtr GenICamCamera::getBooleanNode(const std::string& nodeName) const {
        GenApi::CNodePtr node = getNode(nodeName);
        GenApi::CBooleanPtr boolNode(node);

        if (!boolNode.IsValid()) {
            THROW_GENICAM_ERROR(ErrorType::ParameterError,
                "Il nodo " + nodeName + " non è di tipo Boolean");
        }

        return boolNode;
    }

    GenApi::CStringPtr GenICamCamera::getStringNode(const std::string& nodeName) const {
        GenApi::CNodePtr node = getNode(nodeName);
        GenApi::CStringPtr stringNode(node);

        if (!stringNode.IsValid()) {
            THROW_GENICAM_ERROR(ErrorType::ParameterError,
                "Il nodo " + nodeName + " non è di tipo String");
        }

        return stringNode;
    }

    // === Metodi Trigger mancanti ===

    void GenICamCamera::setTriggerOverlap(TriggerOverlap overlap) {
        try {
            GenApi::CEnumerationPtr pOverlap = getEnumerationNode("TriggerOverlap");

            if (!GenApi::IsWritable(pOverlap)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "TriggerOverlap non scrivibile");
            }

            switch (overlap) {
            case TriggerOverlap::Off:
                *pOverlap = "Off";
                break;
            case TriggerOverlap::ReadOut:
                *pOverlap = "ReadOut";
                break;
            case TriggerOverlap::PreviousFrame:
                *pOverlap = "PreviousFrame";
                break;
            }

            notifyParameterChanged("TriggerOverlap", pOverlap->ToString().c_str());

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione TriggerOverlap: ") + e.GetDescription());
        }
    }

    TriggerOverlap GenICamCamera::getTriggerOverlap() const {
        try {
            GenApi::CEnumerationPtr pOverlap = getEnumerationNode("TriggerOverlap");
            std::string value = pOverlap->ToString().c_str();

            if (value == "Off") return TriggerOverlap::Off;
            if (value == "ReadOut") return TriggerOverlap::ReadOut;
            if (value == "PreviousFrame") return TriggerOverlap::PreviousFrame;

            return TriggerOverlap::Off;

        }
        catch (...) {
            return TriggerOverlap::Off;
        }
    }

    void GenICamCamera::resetTriggerCounter() {
        try {
            GenApi::CCommandPtr pReset = getCommandNode("TriggerCounterReset");

            if (!GenApi::IsWritable(pReset)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "TriggerCounterReset non eseguibile");
            }

            pReset->Execute();
            while (!pReset->IsDone()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            notifyParameterChanged("TriggerCounterReset", "Executed");

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            // Prova nome alternativo
            try {
                GenApi::CIntegerPtr pCounter = getIntegerNode("TriggerCounter");
                if (GenApi::IsWritable(pCounter)) {
                    pCounter->SetValue(0);
                    notifyParameterChanged("TriggerCounter", "0");
                }
            }
            catch (...) {
                THROW_GENICAM_ERROR(ErrorType::GenApiError,
                    std::string("Errore reset trigger counter: ") + e.GetDescription());
            }
        }
    }

    uint64_t GenICamCamera::getTriggerCounter() const {
        try {
            GenApi::CIntegerPtr pCounter = getIntegerNode("TriggerCounter");
            return static_cast<uint64_t>(pCounter->GetValue());
        }
        catch (...) {
            return 0;
        }
    }

    // === User Output Selector ===

    UserOutputSelector GenICamCamera::getUserOutputSelector() const {
        try {
            GenApi::CEnumerationPtr pSelector = getEnumerationNode("UserOutputSelector");
            std::string value = pSelector->ToString().c_str();

            if (value == "UserOutput0") return UserOutputSelector::UserOutput0;
            if (value == "UserOutput1") return UserOutputSelector::UserOutput1;
            if (value == "UserOutput2") return UserOutputSelector::UserOutput2;
            if (value == "UserOutput3") return UserOutputSelector::UserOutput3;

            return UserOutputSelector::UserOutput0;

        }
        catch (...) {
            return UserOutputSelector::UserOutput0;
        }
    }

    // === Line Source available ===

    std::vector<LineSource> GenICamCamera::getAvailableLineSources() const {
        std::vector<LineSource> sources;

        try {
            GenApi::CEnumerationPtr pLineSource = getEnumerationNode("LineSource");
            GenApi::NodeList_t entries;
            pLineSource->GetEntries(entries);

            for (auto& entry : entries) {
                if (GenApi::IsAvailable(entry)) {
                    std::string name = entry->GetName().c_str();

                    if (name == "Off") sources.push_back(LineSource::Off);
                    else if (name == "ExposureActive") sources.push_back(LineSource::ExposureActive);
                    else if (name == "FrameTriggerWait") sources.push_back(LineSource::FrameTriggerWait);
                    else if (name == "FrameActive") sources.push_back(LineSource::FrameActive);
                    else if (name == "FVAL") sources.push_back(LineSource::FVAL);
                    else if (name == "LVAL") sources.push_back(LineSource::LVAL);
                    else if (name == "UserOutput0") sources.push_back(LineSource::UserOutput0);
                    else if (name == "UserOutput1") sources.push_back(LineSource::UserOutput1);
                    else if (name == "UserOutput2") sources.push_back(LineSource::UserOutput2);
                    else if (name == "UserOutput3") sources.push_back(LineSource::UserOutput3);
                    else if (name == "Counter0Active") sources.push_back(LineSource::Counter0Active);
                    else if (name == "Counter1Active") sources.push_back(LineSource::Counter1Active);
                    else if (name == "Timer0Active") sources.push_back(LineSource::Timer0Active);
                    else if (name == "Timer1Active") sources.push_back(LineSource::Timer1Active);
                    // ... altri casi se necessario
                }
            }
        }
        catch (...) {}

        return sources;
    }

    // === Strobe Control ===

    void GenICamCamera::setStrobeEnable(bool enable) {
        try {
            // Prima prova StrobeEnable
            GenApi::CBooleanPtr pStrobe = getBooleanNode("StrobeEnable");

            if (!GenApi::IsWritable(pStrobe)) {
                // Prova LineStrobeEnable
                pStrobe = getBooleanNode("LineStrobeEnable");
                if (!GenApi::IsWritable(pStrobe)) {
                    THROW_GENICAM_ERROR(ErrorType::ParameterError,
                        "StrobeEnable non scrivibile");
                }
            }

            pStrobe->SetValue(enable);
            notifyParameterChanged("StrobeEnable", enable ? "true" : "false");

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione StrobeEnable: ") + e.GetDescription());
        }
    }

    bool GenICamCamera::getStrobeEnable() const {
        try {
            GenApi::CBooleanPtr pStrobe;
            try {
                pStrobe = getBooleanNode("StrobeEnable");
            }
            catch (...) {
                pStrobe = getBooleanNode("LineStrobeEnable");
            }
            return pStrobe->GetValue();
        }
        catch (...) {
            return false;
        }
    }

    void GenICamCamera::setStrobeDuration(double durationUs) {
        try {
            GenApi::CFloatPtr pDuration;
            try {
                pDuration = getFloatNode("StrobeDuration");
            }
            catch (...) {
                pDuration = getFloatNode("LineStrobeDuration");
            }

            if (!GenApi::IsWritable(pDuration)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "StrobeDuration non scrivibile");
            }

            double min = pDuration->GetMin();
            double max = pDuration->GetMax();

            if (durationUs < min || durationUs > max) {
                std::stringstream ss;
                ss << "Strobe duration fuori range [" << min << ", " << max << "]";
                THROW_GENICAM_ERROR(ErrorType::ParameterError, ss.str());
            }

            pDuration->SetValue(durationUs);
            notifyParameterChanged("StrobeDuration", std::to_string(durationUs));

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione StrobeDuration: ") + e.GetDescription());
        }
    }

    double GenICamCamera::getStrobeDuration() const {
        try {
            GenApi::CFloatPtr pDuration;
            try {
                pDuration = getFloatNode("StrobeDuration");
            }
            catch (...) {
                pDuration = getFloatNode("LineStrobeDuration");
            }
            return pDuration->GetValue();
        }
        catch (...) {
            return 0.0;
        }
    }

    void GenICamCamera::setStrobeDelay(double delayUs) {
        try {
            GenApi::CFloatPtr pDelay;
            try {
                pDelay = getFloatNode("StrobeDelay");
            }
            catch (...) {
                pDelay = getFloatNode("LineStrobeDelay");
            }

            if (!GenApi::IsWritable(pDelay)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "StrobeDelay non scrivibile");
            }

            double min = pDelay->GetMin();
            double max = pDelay->GetMax();

            if (delayUs < min || delayUs > max) {
                std::stringstream ss;
                ss << "Strobe delay fuori range [" << min << ", " << max << "]";
                THROW_GENICAM_ERROR(ErrorType::ParameterError, ss.str());
            }

            pDelay->SetValue(delayUs);
            notifyParameterChanged("StrobeDelay", std::to_string(delayUs));

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione StrobeDelay: ") + e.GetDescription());
        }
    }

    double GenICamCamera::getStrobeDelay() const {
        try {
            GenApi::CFloatPtr pDelay;
            try {
                pDelay = getFloatNode("StrobeDelay");
            }
            catch (...) {
                pDelay = getFloatNode("LineStrobeDelay");
            }
            return pDelay->GetValue();
        }
        catch (...) {
            return 0.0;
        }
    }

    void GenICamCamera::setStrobePolarity(bool activeHigh) {
        try {
            GenApi::CEnumerationPtr pPolarity;
            try {
                pPolarity = getEnumerationNode("StrobeLinePolarity");
            }
            catch (...) {
                pPolarity = getEnumerationNode("LineStrobePolarity");
            }

            if (!GenApi::IsWritable(pPolarity)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "StrobePolarity non scrivibile");
            }

            *pPolarity = activeHigh ? "ActiveHigh" : "ActiveLow";
            notifyParameterChanged("StrobePolarity", activeHigh ? "ActiveHigh" : "ActiveLow");

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione StrobePolarity: ") + e.GetDescription());
        }
    }

    bool GenICamCamera::getStrobePolarity() const {
        try {
            GenApi::CEnumerationPtr pPolarity;
            try {
                pPolarity = getEnumerationNode("StrobeLinePolarity");
            }
            catch (...) {
                pPolarity = getEnumerationNode("LineStrobePolarity");
            }

            std::string value = pPolarity->ToString().c_str();
            return (value == "ActiveHigh");
        }
        catch (...) {
            return true; // Default active high
        }
    }

    // === Counter Control ===

    CounterSelector GenICamCamera::getCounterSelector() const {
        try {
            GenApi::CEnumerationPtr pSelector = getEnumerationNode("CounterSelector");
            std::string value = pSelector->ToString().c_str();

            if (value == "Counter0") return CounterSelector::Counter0;
            if (value == "Counter1") return CounterSelector::Counter1;
            if (value == "Counter2") return CounterSelector::Counter2;
            if (value == "Counter3") return CounterSelector::Counter3;

            return CounterSelector::Counter0;

        }
        catch (...) {
            return CounterSelector::Counter0;
        }
    }

    void GenICamCamera::setCounterEnable(bool enable) {
        try {
            GenApi::CBooleanPtr pEnable = getBooleanNode("CounterEnable");

            if (!GenApi::IsWritable(pEnable)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "CounterEnable non scrivibile");
            }

            pEnable->SetValue(enable);
            notifyParameterChanged("CounterEnable", enable ? "true" : "false");

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione CounterEnable: ") + e.GetDescription());
        }
    }

    bool GenICamCamera::getCounterEnable() const {
        try {
            GenApi::CBooleanPtr pEnable = getBooleanNode("CounterEnable");
            return pEnable->GetValue();
        }
        catch (...) {
            return false;
        }
    }

    void GenICamCamera::setCounterTriggerSource(LineSource source) {
        try {
            GenApi::CEnumerationPtr pSource = getEnumerationNode("CounterTriggerSource");

            if (!GenApi::IsWritable(pSource)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "CounterTriggerSource non scrivibile");
            }

            *pSource = lineSourceToString(source);
            notifyParameterChanged("CounterTriggerSource", lineSourceToString(source));

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione CounterTriggerSource: ") + e.GetDescription());
        }
    }

    LineSource GenICamCamera::getCounterTriggerSource() const {
        try {
            GenApi::CEnumerationPtr pSource = getEnumerationNode("CounterTriggerSource");
            std::string value = pSource->ToString().c_str();

            // Conversione string to enum (implementare tutti i casi)
            if (value == "Off") return LineSource::Off;
            if (value == "Line0") return LineSource::Off; // Map line to appropriate source
            // ... altri casi ...

            return LineSource::Off;

        }
        catch (...) {
            return LineSource::Off;
        }
    }

    // === Timer Control ===

    TimerSelector GenICamCamera::getTimerSelector() const {
        try {
            GenApi::CEnumerationPtr pSelector = getEnumerationNode("TimerSelector");
            std::string value = pSelector->ToString().c_str();

            if (value == "Timer0") return TimerSelector::Timer0;
            if (value == "Timer1") return TimerSelector::Timer1;
            if (value == "Timer2") return TimerSelector::Timer2;
            if (value == "Timer3") return TimerSelector::Timer3;

            return TimerSelector::Timer0;

        }
        catch (...) {
            return TimerSelector::Timer0;
        }
    }

    void GenICamCamera::setTimerSelector(TimerSelector timer) {
        try {
            GenApi::CEnumerationPtr pSelector = getEnumerationNode("TimerSelector");

            if (!GenApi::IsWritable(pSelector)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "TimerSelector non scrivibile");
            }

            std::string timerStr;
            switch (timer) {
            case TimerSelector::Timer0: timerStr = "Timer0"; break;
            case TimerSelector::Timer1: timerStr = "Timer1"; break;
            case TimerSelector::Timer2: timerStr = "Timer2"; break;
            case TimerSelector::Timer3: timerStr = "Timer3"; break;
            }

            *pSelector = timerStr.c_str();
            notifyParameterChanged("TimerSelector", timerStr);

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione TimerSelector: ") + e.GetDescription());
        }
    }

    void GenICamCamera::setTimerDuration(double durationUs) {
        try {
            GenApi::CFloatPtr pDuration = getFloatNode("TimerDuration");

            if (!GenApi::IsWritable(pDuration)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "TimerDuration non scrivibile");
            }

            double min = pDuration->GetMin();
            double max = pDuration->GetMax();

            if (durationUs < min || durationUs > max) {
                std::stringstream ss;
                ss << "Timer duration fuori range [" << min << ", " << max << "]";
                THROW_GENICAM_ERROR(ErrorType::ParameterError, ss.str());
            }

            pDuration->SetValue(durationUs);
            notifyParameterChanged("TimerDuration", std::to_string(durationUs));

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione TimerDuration: ") + e.GetDescription());
        }
    }

    double GenICamCamera::getTimerDuration() const {
        try {
            GenApi::CFloatPtr pDuration = getFloatNode("TimerDuration");
            return pDuration->GetValue();
        }
        catch (...) {
            return 0.0;
        }
    }

    void GenICamCamera::setTimerDelay(double delayUs) {
        try {
            GenApi::CFloatPtr pDelay = getFloatNode("TimerDelay");

            if (!GenApi::IsWritable(pDelay)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "TimerDelay non scrivibile");
            }

            double min = pDelay->GetMin();
            double max = pDelay->GetMax();

            if (delayUs < min || delayUs > max) {
                std::stringstream ss;
                ss << "Timer delay fuori range [" << min << ", " << max << "]";
                THROW_GENICAM_ERROR(ErrorType::ParameterError, ss.str());
            }

            pDelay->SetValue(delayUs);
            notifyParameterChanged("TimerDelay", std::to_string(delayUs));

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione TimerDelay: ") + e.GetDescription());
        }
    }

    double GenICamCamera::getTimerDelay() const {
        try {
            GenApi::CFloatPtr pDelay = getFloatNode("TimerDelay");
            return pDelay->GetValue();
        }
        catch (...) {
            return 0.0;
        }
    }

    void GenICamCamera::setTimerEnable(bool enable) {
        try {
            GenApi::CBooleanPtr pEnable = getBooleanNode("TimerEnable");

            if (!GenApi::IsWritable(pEnable)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "TimerEnable non scrivibile");
            }

            pEnable->SetValue(enable);
            notifyParameterChanged("TimerEnable", enable ? "true" : "false");

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione TimerEnable: ") + e.GetDescription());
        }
    }

    bool GenICamCamera::getTimerEnable() const {
        try {
            GenApi::CBooleanPtr pEnable = getBooleanNode("TimerEnable");
            return pEnable->GetValue();
        }
        catch (...) {
            return false;
        }
    }

    void GenICamCamera::resetTimer() {
        try {
            GenApi::CCommandPtr pReset = getCommandNode("TimerReset");

            if (!GenApi::IsWritable(pReset)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "TimerReset non eseguibile");
            }

            pReset->Execute();
            while (!pReset->IsDone()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            notifyParameterChanged("TimerReset", "Executed");

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore reset timer: ") + e.GetDescription());
        }
    }

    void GenICamCamera::setTimerTriggerSource(LineSource source) {
        try {
            GenApi::CEnumerationPtr pSource = getEnumerationNode("TimerTriggerSource");

            if (!GenApi::IsWritable(pSource)) {
                THROW_GENICAM_ERROR(ErrorType::ParameterError,
                    "TimerTriggerSource non scrivibile");
            }

            *pSource = lineSourceToString(source);
            notifyParameterChanged("TimerTriggerSource", lineSourceToString(source));

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione TimerTriggerSource: ") + e.GetDescription());
        }
    }

    LineSource GenICamCamera::getTimerTriggerSource() const {
        try {
            GenApi::CEnumerationPtr pSource = getEnumerationNode("TimerTriggerSource");
            std::string value = pSource->ToString().c_str();

            // Conversione string to enum
            if (value == "Off") return LineSource::Off;
            // ... altri casi ...

            return LineSource::Off;

        }
        catch (...) {
            return LineSource::Off;
        }
    }

    // === Action Command Enable ===

    void GenICamCamera::setActionCommandEnable(bool enable) {
        try {
            // Prima prova con il nome standard
            GenApi::CBooleanPtr pEnable = getBooleanNode("ActionUnconditionalMode");

            if (!GenApi::IsWritable(pEnable)) {
                // Prova nome alternativo
                pEnable = getBooleanNode("ActionCommandEnable");
                if (!GenApi::IsWritable(pEnable)) {
                    THROW_GENICAM_ERROR(ErrorType::ParameterError,
                        "ActionCommandEnable non scrivibile");
                }
            }

            pEnable->SetValue(enable);
            notifyParameterChanged("ActionCommandEnable", enable ? "true" : "false");

        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore impostazione ActionCommandEnable: ") + e.GetDescription());
        }
    }

    bool GenICamCamera::getActionCommandEnable() const {
        try {
            GenApi::CBooleanPtr pEnable;
            try {
                pEnable = getBooleanNode("ActionUnconditionalMode");
            }
            catch (...) {
                pEnable = getBooleanNode("ActionCommandEnable");
            }
            return pEnable->GetValue();
        }
        catch (...) {
            return false;
        }
    }

    // === Pulse Generator ===

    void GenICamCamera::startPulseGenerator() {
        try {
            // Abilita il timer usato per il pulse generator
            setTimerEnable(true);
            notifyParameterChanged("PulseGenerator", "Started");
        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore avvio pulse generator: ") + e.GetDescription());
        }
    }

    void GenICamCamera::stopPulseGenerator() {
        try {
            // Disabilita il timer
            setTimerEnable(false);
            notifyParameterChanged("PulseGenerator", "Stopped");
        }
        catch (const GENICAM_NAMESPACE::GenericException& e) {
            THROW_GENICAM_ERROR(ErrorType::GenApiError,
                std::string("Errore stop pulse generator: ") + e.GetDescription());
        }
    }

    // === Save/Load Configuration (semplificato senza JSON) ===

    std::string GenICamCamera::saveIOConfiguration() const {
        std::stringstream config;

        config << "# GenICam I/O Configuration\n";
        config << "# Camera: " << getCameraModel() << "\n";
        config << "# Date: " << std::chrono::system_clock::now().time_since_epoch().count() << "\n\n";

        // Salva configurazione linee
        auto lines = getAvailableLines();
        for (const auto& line : lines) {
            LineStatus status = getLineFullStatus(line);

            config << "[" << lineSelectorToString(line) << "]\n";
            config << "Mode=" << (status.mode == LineMode::Input ? "Input" : "Output") << "\n";
            config << "Inverter=" << (status.inverter ? "true" : "false") << "\n";
            config << "Source=" << lineSourceToString(status.source) << "\n";
            config << "DebounceTime=" << status.debounceTime << "\n";
            config << "Format=" << status.format << "\n\n";
        }

        // Salva configurazione trigger
        config << "[Trigger]\n";
        config << "Mode=";
        switch (getTriggerMode()) {
        case TriggerMode::Off: config << "Off"; break;
        case TriggerMode::On: config << "On"; break;
        }
        config << "\n";
        config << "Delay=" << getTriggerDelay() << "\n";
        config << "Divider=" << getTriggerDivider() << "\n\n";

        return config.str();
    }

    void GenICamCamera::loadIOConfiguration(const std::string& config) {
        // Implementazione semplificata
        // In produzione, usare un parser JSON o INI appropriato

        std::istringstream stream(config);
        std::string line;
        std::string currentSection;

        while (std::getline(stream, line)) {
            // Rimuovi spazi
            line.erase(0, line.find_first_not_of(" \t"));
            line.erase(line.find_last_not_of(" \t") + 1);

            // Skip commenti e linee vuote
            if (line.empty() || line[0] == '#') continue;

            // Sezione
            if (line[0] == '[' && line[line.length() - 1] == ']') {
                currentSection = line.substr(1, line.length() - 2);
                continue;
            }

            // Parse key=value
            size_t pos = line.find('=');
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                std::string value = line.substr(pos + 1);

                // Applica configurazione (implementazione base)
                if (currentSection == "Trigger") {
                    if (key == "Source" && value == "Software") {
                        setTriggerSource(TriggerSource::Software);
                    }
                    else if (key == "Delay") {
                        setTriggerDelay(std::stod(value));
                    }
                    else if (key == "Divider") {
                        setTriggerDivider(std::stoul(value));
                    }
                }
                // ... altri casi ...
            }
        }
    }
} // namespace GenICamWrapper