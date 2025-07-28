#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <atomic>
#include <thread>
#include <opencv2/opencv.hpp>
#include <GenICam.h>
#include <GenTL/GenTL.h>
#include "GenTLLoader.h"
#include "ImageTypes.h"
#include "CameraEventListener.h"

namespace GenICamWrapper {

    // Enumerazioni aggiuntive per I/O
    enum class LineSelector {
        Line0,
        Line1,
        Line2,
        Line3,
        Line4,
        Line5,
        Line6,
        Line7,
        CC1,
        CC2,
        CC3,
        CC4
    };

    enum class LineMode {
        Input,
        Output
    };

    enum class LineSource {
        Off,
        ExposureActive,
        FrameTriggerWait,
        FrameActive,
        FVAL,
        LVAL,
        UserOutput0,
        UserOutput1,
        UserOutput2,
        UserOutput3,
        Counter0Active,
        Counter1Active,
        Timer0Active,
        Timer1Active,
        Encoder0,
        Encoder1,
        SoftwareSignal0,
        SoftwareSignal1,
        Action0,
        Action1
    };

    enum class TriggerSelector {
        FrameStart,
        FrameEnd,
        FrameBurstStart,
        FrameBurstEnd,
        LineStart,
        ExposureStart,
        ExposureEnd,
        AcquisitionStart,
        AcquisitionEnd,
        Action0,
        Action1
    };

    enum class TriggerOverlap {
        Off,
        ReadOut,
        PreviousFrame
    };

    enum class CounterSelector {
        Counter0,
        Counter1,
        Counter2,
        Counter3
    };

    enum class TimerSelector {
        Timer0,
        Timer1,
        Timer2,
        Timer3
    };

    enum class UserOutputSelector {
        UserOutput0,
        UserOutput1,
        UserOutput2,
        UserOutput3
    };

    /**
     * @brief Modalità di acquisizione secondo SFNC
     */
    enum class AcquisitionMode {
        SingleFrame,    // Acquisisce un singolo frame
        MultiFrame,     // Acquisisce un numero predefinito di frame
        Continuous      // Acquisizione continua
    };

    // tempi di esposizione
    enum class ExposureMode {
        Off,           // Nessun controllo esposizione
        Timed,         // Esposizione temporizzata
        TriggerWidth,  // La larghezza del trigger determina l'esposizione
        TriggerControlled // Controllata da segnali trigger
    };

    enum class ExposureAuto {
        Off,        // Controllo manuale
        Once,       // Esegue auto-exposure una volta
        Continuous  // Auto-exposure continuo
    };

    /**
 * @brief Modalità di trigger supportate
 */
    enum class TriggerMode {
        Off,    // Trigger disabilitato
        On      // Trigger abilitato
    };

    /**
     * @brief Sorgenti trigger supportate
     */
    enum class TriggerSource {
        Software,       // Trigger software
        Line0,          // Linea hardware 0
        Line1,          // Linea hardware 1  
        Line2,          // Linea hardware 2
        Line3,          // Linea hardware 3
        Line4,          // Linea hardware 4
        Line5,          // Linea hardware 5
        Line6,          // Linea hardware 6
        Line7,          // Linea hardware 7
        Counter0End,    // Fine contatore 0
        Counter1End,    // Fine contatore 1
		  Counter2End,	 // Fine contatore 2
        Timer0End,      // Fine timer 0
        Timer1End,      // Fine timer 1
		  Timer2End,		// Fine timer 2
        UserOutput0,    // User output 0
        UserOutput1,    // User output 1
        UserOutput2,    // User output 2
        UserOutput3,    // User output 3
        Action0,        // Action command 0
        Action1,         // Action command 1
        Encoder0,
        Encoder1,
        FrameTriggerWait,
        ExposureActive
    };

    /**
     * @brief Attivazione trigger (edge/level)
     */
    enum class TriggerActivation {
        RisingEdge,     // Fronte di salita
        FallingEdge,    // Fronte di discesa
        AnyEdge,        // Qualsiasi fronte
        LevelHigh,      // Livello alto
        LevelLow        // Livello basso
    };
    /**
     * @brief Stato della telecamera
     */
    enum class CameraState {
        Disconnected,
        Connected,
        Acquiring,
        Error
    };

    // Struttura per stato linea I/O
    struct LineStatus {
        bool value;
        LineMode mode;
        bool inverter;
        LineSource source;
        std::string format;
        double debounceTime;
    };

    // Custom deleter per memoria allineata
    struct AlignedBufferDeleter {
        void operator()(void* p) const {
            if (p) {
                #ifdef _WIN32
                    _aligned_free(p);
                #else
                    free(p);
                #endif
            }
        }
    };

    struct PixelFormatInfo {
       PixelFormat format = PixelFormat::Undefined;
       std::string name;           // Nome simbolico (es. "Mono8")
       std::string displayName;    // Nome del parametro (es. "Pixel Format")
       std::string description;    // Descrizione/tooltip se disponibile
       uint64_t pfncValue = 0;     // Valore PFNC
       double bytesPerPixel = 0.0; // Bytes per pixel
       int bitsPerPixel = 0;       // Bits per pixel
       bool isPacked = false;      // Se è un formato packed
       bool isBayer = false;       // Se è un formato Bayer
       bool isColor = false;       // Se è un formato a colori
       bool isValid = false;       // Se le informazioni sono valide
    };

    struct infoCamere{
       std::string nomeConModello;
       std::string userID;
    };

    /**
     * @brief Classe principale per la gestione della telecamera GenICam
     *
     * Thread Safety: Questa classe è thread-safe. Tutti i metodi pubblici possono
     * essere chiamati da thread diversi senza sincronizzazione esterna.
     */
    class GenICamCamera {
    public:
        GenICamCamera(std::string fileProducer);
        ~GenICamCamera();

        // Non copiabile
        GenICamCamera(const GenICamCamera&) = delete;
        GenICamCamera& operator=(const GenICamCamera&) = delete;

        // === Gestione Connessione ===
        /**
         * @brief Enumera tutte le telecamere disponibili nel sistema
         * @return Vector con ID e descrizione delle telecamere
         * @throws GenICamException in caso di errore
         */
        std::vector<infoCamere> enumerateCameras();

        /**
         * @brief Connette a una telecamera specifica
         * @param cameraID ID della telecamera da connettere
         * @throws GenICamException se la connessione fallisce
         */
        void connect(const std::string& cameraID);

        /**
         * @brief Connette alla prima telecamera disponibile
         * @throws GenICamException se nessuna telecamera è disponibile
         */
        void connectFirst(const std::string& cameraUserID);

        /**
         * @brief Disconnette dalla telecamera corrente
         */
        void disconnect();

        /**
         * @brief Verifica se la telecamera è connessa
         * @return true se connessa
         */
        bool isConnected() const;

        /**
         * @brief Ottiene lo stato corrente della telecamera
         * @return Stato corrente
         */
        CameraState getState() const;

        // === Controllo Acquisizione ===
        /**
         * @brief Avvia l'acquisizione continua
         * @param bufferCount Numero di buffer da allocare (default 10)
         * @throws GenICamException in caso di errore
         */
        void startAcquisition(size_t bufferCount = 10);

        /**
         * @brief Ferma l'acquisizione
         */
        void stopAcquisition();

        /**
         * @brief Acquisisce un singolo frame
         * @param timeoutMs Timeout in millisecondi
         * @return Frame acquisito come cv::Mat
         * @throws GenICamException in caso di timeout o errore
         */
        cv::Mat grabSingleFrame(uint32_t timeoutMs = 5000);
        void configureHikrobotGigE();
        void debugAcquisitionStart();

        // === Parametri Camera - Accesso Uniforme via GenApi ===

        // === Acquisition Mode ===
        /**
         * @brief Imposta la modalità di acquisizione
         * @param mode Modalità di acquisizione desiderata
         * @throws GenICamException se il parametro non è disponibile o scrivibile
         */
        void setAcquisitionMode(AcquisitionMode mode);

        /**
         * @brief Ottiene la modalità di acquisizione corrente
         * @return Modalità di acquisizione attuale
         * @throws GenICamException se il parametro non è disponibile o leggibile
         */
        AcquisitionMode getAcquisitionMode() const;

        /**
         * @brief Verifica se AcquisitionMode è disponibile
         * @return true se il parametro è disponibile
         */
        bool isAcquisitionModeAvailable() const;

        /**
         * @brief Ottiene le modalità di acquisizione disponibili
         * @return Vector con le modalità supportate dalla camera
         */
        std::vector<AcquisitionMode> getAvailableAcquisitionModes() const;

        std::string getAcquisitionModeString(AcquisitionMode mode);

        // === Controllo Esposizione SFNC compliant ===
        void setExposureMode(ExposureMode mode);
        ExposureMode getExposureMode() const;
        bool isExposureModeAvailable() const;
        std::vector<ExposureMode> getAvailableExposureModes() const;

        // ExposureAuto
        void setExposureAuto(ExposureAuto mode);
        ExposureAuto getExposureAuto() const;
        bool isExposureAutoAvailable() const;

        // ExposureTime (già esistente ma da aggiornare)
        void setExposureTime(double microseconds);
        double getExposureTime() const;
        void getExposureTimeRange(double& min, double& max) const;
        bool isExposureTimeAvailable() const;

        // ExposureTimeSelector (per multi-esposizione)
        void setExposureTimeSelector(const std::string& selector);
        std::string getExposureTimeSelector() const;
        std::vector<std::string> getAvailableExposureTimeSelectors() const;
        std::string getExposureConfiguration() const;
        std::string getExposureInfo() const;
        std::vector<ExposureMode> getAvailableExposureModesFiltered() const;
        std::string exposureModeToString(ExposureMode mode) const;

        // Guadagno
        void setGain(double gain);
        double getGain() const;
        void getGainRange(double& min, double& max) const;
        bool isGainAvailable() const;

        // ROI
        void setROI(const ROI& roi);
        ROI getROI() const;
        void getSensorSize(uint32_t& width, uint32_t& height) const;

        // Trigger
        void setTriggerMode(TriggerMode mode);
        TriggerMode getTriggerMode() const;
        bool isTriggerModeAvailable() const;
        std::vector<TriggerMode> getAvailableTriggerModes() const;

        // Formato Pixel
        void setPixelFormat(PixelFormat format);
        PixelFormat getPixelFormat() const;
        std::vector<PixelFormat> getAvailablePixelFormats() const;

        // Frame Rate
        void setFrameRate(double fps);
        double getFrameRate() const;
        void getFrameRateRange(double& min, double& max) const;
        bool isFrameRateAvailable() const;

        // === Callback ===
        /**
         * @brief Imposta il listener per gli eventi
         * @param listener Puntatore al listener (può essere nullptr)
         * @note Il listener deve rimanere valido per tutta la durata dell'uso
         */
        void setEventListener(CameraEventListener* listener);

        // === Informazioni ===
        std::string getCameraInfo() const;
        std::string getCameraModel() const;
        std::string getCameraSerialNumber() const;
        std::string getCameraVendor() const;
        std::string getCameraVersion() const;
        std::string getCameraUserID() const;

        // === Parametri Generici GenApi ===
        /**
         * @brief Ottiene il valore di un parametro generico
         * @param parameterName Nome del parametro GenApi
         * @return Valore del parametro come stringa
         * @throws GenICamException se il parametro non esiste o non è leggibile
         */
        std::string getParameter(const std::string& parameterName) const;

        /**
         * @brief Imposta il valore di un parametro generico
         * @param parameterName Nome del parametro GenApi
         * @param value Valore da impostare
         * @throws GenICamException se il parametro non esiste o non è scrivibile
         */
        void setParameter(const std::string& parameterName, const std::string& value);

        /**
         * @brief Ottiene la lista di tutti i parametri disponibili
         * @return Vector con i nomi dei parametri
         */
        std::vector<std::string> getAvailableParameters() const;

        /**
         * @brief Verifica se un parametro è disponibile
         * @param parameterName Nome del parametro
         * @return true se il parametro esiste ed è implementato
         */
        bool isParameterAvailable(const std::string& parameterName) const;

        /**
         * @brief Verifica se un parametro è leggibile
         * @param parameterName Nome del parametro
         * @return true se il parametro è leggibile
         */
        bool isParameterReadable(const std::string& parameterName) const;

        /**
         * @brief Verifica se un parametro è scrivibile
         * @param parameterName Nome del parametro
         * @return true se il parametro è scrivibile
         */
        bool isParameterWritable(const std::string& parameterName) const;

        // === Gestione Trigger Avanzata ===

        /**
         * @brief Seleziona quale trigger configurare
         * @param selector Tipo di trigger da configurare
         */
        void setTriggerSelector(TriggerSelector selector);
        TriggerSelector getTriggerSelector() const;
        std::vector<TriggerSelector> getAvailableTriggerSelectors() const;

        /**
         * @brief Imposta la sorgente del trigger
         * @param source Sorgente del trigger (Software, Line1, Line3, etc.)
         */
        void setTriggerSource(TriggerSource source);
        TriggerSource getTriggerSource() const;
        std::vector<TriggerSource> getAvailableTriggerSources() const;

        /**
         * @brief Esegue un trigger software
         * @note Funziona solo se TriggerSource è impostato su Software
         */
        void executeTriggerSoftware();

        /**
         * @brief Imposta il ritardo del trigger in microsecondi
         * @param delayUs Ritardo in microsecondi
         */
        void setTriggerDelay(double delayUs);
        double getTriggerDelay() const;
        void getTriggerDelayRange(double& min, double& max) const;

        /**
         * @brief Helper per configurazione rapida trigger software
         * @param enable true per abilitare trigger software, false per free-running
         */
        void enableSoftwareTrigger(bool enable = true);

        /**
         * @brief Helper per configurazione rapida trigger hardware
         * @param line Linea da usare come trigger (Line1, Line3, etc.)
         * @param activation Tipo di attivazione (default RisingEdge)
         */
        void enableHardwareTrigger(TriggerSource line,
            TriggerActivation activation = TriggerActivation::RisingEdge);

        /**
         * @brief Verifica se il trigger è configurato e attivo
         * @return true se un trigger è abilitato
         */
        bool isTriggerEnabled() const;

        /**
         * @brief Ottiene informazioni complete sulla configurazione trigger
         * @return Stringa con la configurazione corrente
         */
        std::string getTriggerConfiguration() const;

        void setTriggerActivation(TriggerActivation activation);
        TriggerActivation getTriggerActivation() const;

        /**
         * @brief Imposta il divisore del trigger
         * @param divider Valore del divisore (1 = ogni trigger, 2 = ogni 2 trigger, ecc.)
         */
        void setTriggerDivider(uint32_t divider);
        uint32_t getTriggerDivider() const;

        /**
         * @brief Imposta la modalità overlap del trigger
         * @param overlap Modalità overlap
         */
        void setTriggerOverlap(TriggerOverlap overlap);
        TriggerOverlap getTriggerOverlap() const;

        /**
         * @brief Resetta il contatore del trigger
         */
        void resetTriggerCounter();
        uint64_t getTriggerCounter() const;

        // Gestione del Transport Layer....serve per la sua corretta gestione prima del Data Streaming
        bool setTransportLayerLock(bool lock);
        void prepareTransportLayerForAcquisition();
        int64_t getOptimalPacketSize() const;
        int64_t calculateOptimalInterPacketDelay() const;
        void configureTimeouts();

        // === Gestione Linee I/O ===

        /**
         * @brief Seleziona quale linea I/O configurare
         * @param line Linea da configurare
         */
        void setLineSelector(LineSelector line);
        LineSelector getLineSelector() const;
        std::vector<LineSelector> getAvailableLines() const;

        /**
         * @brief Configura la modalità della linea (input/output)
         * @param mode Modalità della linea
         */
        void setLineMode(LineMode mode);
        LineMode getLineMode() const;

        /**
         * @brief Legge lo stato corrente della linea
         * @return true se alta, false se bassa
         */
        bool getLineStatus() const;

        /**
         * @brief Inverte la logica della linea
         * @param invert true per invertire
         */
        void setLineInverter(bool invert);
        bool getLineInverter() const;

        /**
         * @brief Imposta la sorgente per una linea di output
         * @param source Sorgente del segnale
         */
        void setLineSource(LineSource source);
        LineSource getLineSource() const;
        std::vector<LineSource> getAvailableLineSources() const;

        /**
         * @brief Imposta il tempo di debounce per una linea di input
         * @param timeUs Tempo in microsecondi
         */
        void setLineDebouncerTime(double timeUs);
        double getLineDebouncerTime() const;

        /**
         * @brief Ottiene lo stato completo di una linea
         * @param line Linea da interrogare
         * @return Struttura con tutti i parametri della linea
         */
        LineStatus getLineFullStatus(LineSelector line) const;

        // === User Output Control ===

        /**
         * @brief Seleziona quale user output configurare
         * @param output User output da configurare
         */
        void setUserOutputSelector(UserOutputSelector output);
        UserOutputSelector getUserOutputSelector() const;

        /**
         * @brief Imposta il valore di un user output
         * @param value true per alto, false per basso
         */
        void setUserOutputValue(bool value);
        bool getUserOutputValue() const;

        /**
         * @brief Imposta tutti i user output con una singola chiamata
         * @param values Mappa output->valore
         */
        void setAllUserOutputs(const std::map<UserOutputSelector, bool>& values);

        // === Strobe Output ===

        /**
         * @brief Abilita/disabilita lo strobe output
         * @param enable true per abilitare
         */
        void setStrobeEnable(bool enable);
        bool getStrobeEnable() const;

        /**
         * @brief Imposta la durata dello strobe
         * @param durationUs Durata in microsecondi
         */
        void setStrobeDuration(double durationUs);
        double getStrobeDuration() const;

        /**
         * @brief Imposta il ritardo dello strobe
         * @param delayUs Ritardo in microsecondi
         */
        void setStrobeDelay(double delayUs);
        double getStrobeDelay() const;

        /**
         * @brief Imposta la polarità dello strobe
         * @param activeHigh true per attivo alto
         */
        void setStrobePolarity(bool activeHigh);
        bool getStrobePolarity() const;

        // === Counter Control ===

        /**
         * @brief Seleziona quale counter configurare
         * @param counter Counter da configurare
         */
        void setCounterSelector(CounterSelector counter);
        CounterSelector getCounterSelector() const;

        /**
         * @brief Abilita/disabilita il counter
         * @param enable true per abilitare
         */
        void setCounterEnable(bool enable);
        bool getCounterEnable() const;

        /**
         * @brief Legge il valore corrente del counter
         * @return Valore del counter
         */
        uint64_t getCounterValue() const;

        /**
         * @brief Resetta il counter
         */
        void resetCounter();

        /**
         * @brief Imposta la sorgente di trigger per il counter
         * @param source Sorgente trigger
         */
        void setCounterTriggerSource(LineSource source);
        LineSource getCounterTriggerSource() const;

        void configureTriggerOptions();

        // === Timer Control ===

        /**
         * @brief Seleziona quale timer configurare
         * @param timer Timer da configurare
         */
        void setTimerSelector(TimerSelector timer);
        TimerSelector getTimerSelector() const;

        /**
         * @brief Imposta la durata del timer
         * @param durationUs Durata in microsecondi
         */
        void setTimerDuration(double durationUs);
        double getTimerDuration() const;

        /**
         * @brief Imposta il ritardo del timer
         * @param delayUs Ritardo in microsecondi
         */
        void setTimerDelay(double delayUs);
        double getTimerDelay() const;

        /**
         * @brief Abilita/disabilita il timer
         * @param enable true per abilitare
         */
        void setTimerEnable(bool enable);
        bool getTimerEnable() const;

        /**
         * @brief Resetta il timer
         */
        void resetTimer();

        /**
         * @brief Imposta la sorgente di trigger per il timer
         * @param source Sorgente trigger
         */
        void setTimerTriggerSource(LineSource source);
        LineSource getTimerTriggerSource() const;

        // === Action Control (per Action Commands) ===

        /**
         * @brief Configura un action command
         * @param actionIndex Indice dell'action (0-15)
         * @param deviceKey Device key per l'action
         * @param groupKey Group key per l'action
         * @param groupMask Group mask per l'action
         */
        void configureActionCommand(uint32_t actionIndex,
            uint32_t deviceKey,
            uint32_t groupKey,
            uint32_t groupMask);

        /**
         * @brief Abilita/disabilita la ricezione di action commands
         * @param enable true per abilitare
         */
        void setActionCommandEnable(bool enable);
        bool getActionCommandEnable() const;

        // === Pulse Generator ===

        /**
         * @brief Configura un generatore di impulsi
         * @param lineOutput Linea di output per gli impulsi
         * @param frequencyHz Frequenza in Hz
         * @param dutyCycle Duty cycle (0.0-1.0)
         * @param pulseCount Numero di impulsi (0 = continuo)
         */
        void configurePulseGenerator(LineSelector lineOutput,
            double frequencyHz,
            double dutyCycle,
            uint32_t pulseCount = 0);

        /**
         * @brief Avvia il generatore di impulsi
         */
        void startPulseGenerator();

        /**
         * @brief Ferma il generatore di impulsi
         */
        void stopPulseGenerator();

        // === Utility Functions ===

        /**
         * @brief Salva la configurazione I/O corrente
         * @return Stringa JSON con la configurazione
         */
        std::string saveIOConfiguration() const;

        /**
         * @brief Carica una configurazione I/O
         * @param jsonConfig Configurazione in formato JSON
         */
        void loadIOConfiguration(const std::string& jsonConfig);

        /**
         * @brief Test automatico di tutte le linee I/O
         * @return Report del test
         */
        std::string testIOLines();

        /**
         * @brief Ottiene un report dettagliato dello stato I/O
         * @return Report testuale
         */
        std::string getIOStatusReport() const;

        std::string getGainInfo() const;
        void debugAcquisitionParameters() const;

        /**
         * @brief Ottiene il puntatore al NodeMap del dispositivo
         *
         * Questo metodo è necessario per inizializzare componenti esterni
         * come ChunkDataManager che necessitano accesso diretto al NodeMap.
         *
         * @return Puntatore al NodeMap del dispositivo, nullptr se non connesso
         * @throws GenICamException se la camera non è connessa
         */
        GenApi::INodeMap* getNodeMap() const;

        /**
         * @brief Ottiene il puntatore al NodeMap dello stream (se disponibile)
         *
         * Alcune implementazioni GenTL forniscono un NodeMap separato per lo stream.
         * Questo è particolarmente utile per la gestione dei chunk data.
         *
         * @return Puntatore al NodeMap dello stream, può essere nullptr
         * @note Non tutte le implementazioni supportano uno stream NodeMap separato
         */
        GenApi::INodeMap* getStreamNodeMap() const;

        std::vector<infoCamere> m_infoTelecamere;    // 1o elemento marca e tipo di telecamera, 2o elemento e' il device user ID


    private:
        // === Thread Safety ===
        mutable std::shared_mutex m_connectionMutex;  // Per stato connessione
        mutable std::shared_mutex m_parameterMutex;   // Per accesso parametri
        std::mutex m_acquisitionMutex;                // Per controllo acquisizione
        std::mutex m_callbackMutex;                   // Per gestione callback

        // === Handles GenTL ===
        GenTL::TL_HANDLE m_tlHandle;
        GenTL::IF_HANDLE m_ifHandle;
        GenTL::DEV_HANDLE m_devHandle;
        GenTL::DS_HANDLE m_dsHandle;
        GenTL::PORT_HANDLE m_portHandle;
        GenTL::EVENT_HANDLE m_eventHandle;

        GenTL::EVENT_HANDLE m_featureEventHandle;

        // === GenApi ===
        std::unique_ptr<GenApi::CNodeMapRef> m_pNodeMap;

        // === Buffer Management ===
        std::vector<GenTL::BUFFER_HANDLE> m_bufferHandles;
        std::vector<std::unique_ptr<void, AlignedBufferDeleter>> m_alignedBuffers;
        size_t m_bufferSize;

        // === Stato ===
        std::atomic<CameraState> m_state;
        std::atomic<bool> m_isAcquiring;
        std::string m_cameraID;

        // === Thread Acquisizione ===
        std::thread m_acquisitionThread;
        std::atomic<bool> m_stopAcquisition{ false };
        std::condition_variable m_stopCondition;
        std::mutex m_stopMutex;

        // === Callback ===
        CameraEventListener* m_eventListener;

        // === Cache Parametri (per performance) ===
        mutable std::map<std::string, std::pair<std::string, std::chrono::steady_clock::time_point>> m_parameterCache;
        static constexpr std::chrono::milliseconds CACHE_TIMEOUT{ 100 };

        // === Metodi Helper Privati ===
        void initializeGenTL(std::string fileProducer);
        void cleanupGenTL();
        void allocateBuffers(size_t count);
        void freeBuffers();
        void acquisitionThreadFunction();
        void loadXMLFromDevice();
        void parseAndLoadXMLFromURL(const std::string& urlString);
        bool setupGainSelector() const;

        cv::Mat convertBufferToMat(void* buffer, size_t size,
            uint32_t width, uint32_t height,
            PixelFormat format) const;

        PixelFormat convertFromGenICamPixelFormat(uint64_t genICamFormat) const;
        uint64_t convertToGenICamPixelFormat(PixelFormat format) const;
        void unpackMono10Packed(const uint8_t* src, uint16_t* dst, uint32_t width, uint32_t height) const;
        void unpackMono12Packed(const uint8_t* src, uint16_t* dst, uint32_t width, uint32_t height) const;
        PixelFormat getPixelFormatFromSymbolicName(const std::string& name) const;
        PixelFormatInfo getPixelFormatInfo() const;
        std::string toHexString(uint64_t value) const;

        // Helper GenApi thread-safe
        GenApi::CNodePtr getNode(const std::string& nodeName) const;
        GenApi::CFloatPtr getFloatNode(const std::string& nodeName) const;
        GenApi::CIntegerPtr getIntegerNode(const std::string& nodeName) const;
        GenApi::CEnumerationPtr getEnumerationNode(const std::string& nodeName) const;
        GenApi::CStringPtr getStringNode(const std::string& nodeName) const;
        GenApi::CBooleanPtr getBooleanNode(const std::string& nodeName) const;
        GenApi::CCommandPtr getCommandNode(const std::string& nodeName) const;

        // === NodeMap Management ===
        std::atomic<bool> m_nodeMapValid{ false };
        std::mutex m_nodeMapRefreshMutex;
        std::chrono::steady_clock::time_point m_lastNodeMapRefresh;
        static constexpr std::chrono::seconds NODEMAP_REFRESH_INTERVAL{ 30 };

        // Metodi per gestione NodeMap
        void validateNodeMap() const;
        void refreshNodeMap();
        void handleFeatureInvalidation(const std::string& featureName);
        bool isNodeMapStale() const;

        // Cache per i selettori supportati
        mutable std::vector<TriggerSelector> m_cachedTriggerSelectors;
        mutable bool m_triggerSelectorsCached = false;
        std::string triggerSelectorToString(TriggerSelector selector) const;
        // Cache per le sorgenti trigger supportate
        mutable std::map<TriggerSource, std::string> m_triggerSourceMap;
        mutable bool m_triggerSourceMapCached = false;

        // Helper per conversione
        std::string triggerSourceToString(TriggerSource source) const;
        TriggerSource stringToTriggerSource(const std::string& str) const;
        void cacheTriggerSourceMappings() const;

        // Event handler per invalidazione features
        void registerFeatureInvalidationEvents();
        void unregisterFeatureInvalidationEvents();

        void exploreNode(GenApi::CNodePtr pNode, std::vector<std::string>& parameters) const;
        void notifyParameterChanged(const std::string& parameterName, const std::string& value);

        std::string getGenTLErrorString(GenTL::GC_ERROR error) const;

        // === Classe Port per GenApi ===
        class CameraPort : public GenApi::IPort {
        private:
            GenTL::PORT_HANDLE m_portHandle;
            mutable std::mutex m_portMutex;

        public:
            CameraPort(GenTL::PORT_HANDLE handle) : m_portHandle(handle) {}

            virtual void Read(void* pBuffer, int64_t Address, int64_t Length) override;
            virtual void Write(const void* pBuffer, int64_t Address, int64_t Length) override;
            virtual GenApi::EAccessMode GetAccessMode() const override { return GenApi::RW; }
        };

        std::unique_ptr<CameraPort> m_pCameraPort;

        // Timeout per operazioni critiche
        static constexpr auto ACQUISITION_STOP_TIMEOUT = std::chrono::seconds(5);
        static constexpr auto BUFFER_WAIT_TIMEOUT = std::chrono::milliseconds(100);
    };

} // namespace GenICamWrapper