#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <conio.h>  // Per _kbhit() su Windows
#include <opencv2/opencv.hpp>
#include <opencv2/highgui.hpp>

#include "GenICamCamera.h"
#include "GenICamException.h"
#include "GenTLLoader.h"
#include "CameraEventListener.h"
#include "ChunkDataManager.h"
#include "ChunkDataVerifier.h"

using namespace GenICamWrapper;
using namespace std;

// Classe per gestire i callback della camera con thread safety
class TestEventCallback : public CameraEventListener {
private:
    cv::Mat m_lastFrame;
    mutable mutex m_frameMutex;
    atomic<uint64_t> m_frameCount{ 0 };
    atomic<uint64_t> m_errorCount{ 0 };
    chrono::steady_clock::time_point m_startTime;
    bool m_displayEnabled;
    string m_windowName;

public:
    TestEventCallback(bool enableDisplay = true)
        : m_displayEnabled(enableDisplay)
        , m_windowName("Camera View") {
        m_startTime = chrono::steady_clock::now();
        if (m_displayEnabled) {
            cv::namedWindow(m_windowName, cv::WINDOW_NORMAL);
        }
    }

    ~TestEventCallback() {
        if (m_displayEnabled) {
            cv::destroyWindow(m_windowName);
        }
    }

    void OnFrameReady(const ImageData* imageData, cv::Mat immagine) override {
        if (!imageData) return;

        m_frameCount++;

        // Converti ImageData in cv::Mat
        cv::Mat frame = imageData->toCvMat();

        {
            lock_guard<mutex> lock(m_frameMutex);
            //m_lastFrame = frame.clone();
            m_lastFrame = immagine.clone();
        }

        // Calcola FPS
        auto now = chrono::steady_clock::now();
        auto elapsed = chrono::duration_cast<chrono::seconds>(now - m_startTime).count();
        if (elapsed > 0) {
            double fps = static_cast<double>(m_frameCount) / elapsed;

            cout << "\rFrame: " << imageData->frameID
                << " | FPS: " << fixed << setprecision(1) << fps
                << " | Exposure: " << imageData->exposureTime << " us"
                << " | Gain: " << imageData->gain
                << " | Size: " << imageData->width << "x" << imageData->height
                << "     " << flush;
        }

        // Visualizza frame se abilitato
        if (m_displayEnabled && !frame.empty()) {
            cv::Mat displayFrame = frame.clone();

            // Aggiungi informazioni sul frame
            string info = "Frame: " + to_string(imageData->frameID) +
                " Exp: " + to_string(static_cast<int>(imageData->exposureTime)) + "us";
            cv::putText(displayFrame, info, cv::Point(10, 30),
                cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 0), 2);

            cv::imshow(m_windowName, displayFrame);
            cv::waitKey(1);
        }
    }

    void OnError(int errorCode, const string& errorMessage) override {
        m_errorCount++;
        cerr << "\n[ERRORE CAMERA] Code: " << errorCode
            << " - " << errorMessage << endl;
    }

    void OnConnectionLost(const string& errorMessage) override {
        cerr << "\n[CONNESSIONE PERSA] " << errorMessage << endl;
    }

    void OnAcquisitionStarted() override {
        cout << "\n[ACQUISIZIONE AVVIATA]" << endl;
        resetFrameCount();
    }

    void OnAcquisitionStopped() override {
        cout << "\n[ACQUISIZIONE FERMATA] Frame totali: " << m_frameCount
            << ", Errori: " << m_errorCount << endl;
    }

    void OnParameterChanged(const string& parameterName, const string& newValue) override {
        cout << "\n[PARAMETRO MODIFICATO] " << parameterName
            << " = " << newValue << endl;
    }

    cv::Mat getLastFrame() const {
        lock_guard<mutex> lock(m_frameMutex);
        return m_lastFrame.clone();
    }

    uint64_t getFrameCount() const { return m_frameCount; }
    uint64_t getErrorCount() const { return m_errorCount; }

    void resetFrameCount() {
        m_frameCount = 0;
        m_errorCount = 0;
        m_startTime = chrono::steady_clock::now();
    }
};

// Funzioni di utilità
void clearScreen() {
    system("cls");  // Windows
    // system("clear");  // Linux/Mac
}

void pauseAndWait() {
    cout << "\nPremi un tasto per continuare...";
    _getch();
}

void printSeparator() {
    cout << "\n" << string(60, '=') << "\n";
}

// Test del GenTL Loader
void testGenTLLoader() {
    cout << "\n=== TEST GENTL LOADER ===\n" << endl;

    // 1. Enumera tutti i producer disponibili
    cout << "1. Ricerca producer GenTL nel sistema..." << endl;
    auto producers = GenTLLoader::EnumerateProducersInStandardPaths();

    if (producers.empty()) {
        cout << "   ATTENZIONE: Nessun producer GenTL trovato!" << endl;
        cout << "   Assicurarsi che i file .cti siano presenti nei percorsi standard." << endl;
        return;
    }

    cout << "   Trovati " << producers.size() << " producer(s):" << endl;
    for (size_t i = 0; i < producers.size(); ++i) {
        cout << "   " << (i + 1) << ". " << producers[i] << endl;
    }

    // 2. Test caricamento/scaricamento
    cout << "\n2. Test caricamento del producer ..." << endl;
    GenTLLoader& loader = GenTLLoaderSingleton::GetInstance();

    if (!loader.IsLoaded()) {
        if (loader.LoadProducer(producers[4])) {
            cout << "   OK Producer caricato con successo!" << endl;
            cout << "   Path: " << loader.GetProducerPath() << endl;
        }
        else {
            cout << "   KO Errore caricamento: " << loader.GetLastError() << endl;
        }
    }
    else {
        cout << "   OK Producer già caricato: " << loader.GetProducerPath() << endl;
    }

    // Verifica che le funzioni siano state caricate
    cout << "\n3. Verifica funzioni GenTL caricate:" << endl;
    cout << "   GCInitLib: " << (loader.GCInitLib ? "OK" : "KO") << endl;
    cout << "   TLOpen: " << (loader.TLOpen ? "OK" : "KO") << endl;
    cout << "   IFOpenDevice: " << (loader.IFOpenDevice ? "OK" : "KO") << endl;
    cout << "   DSStartAcquisition: " << (loader.DSStartAcquisition ? "OK" : "KO") << endl;
}

// Test enumerazione camere
void testCameraEnumeration(GenICamCamera& camera) {
    cout << "\n=== TEST ENUMERAZIONE CAMERE ===\n" << endl;

    try {
        auto cameras = camera.enumerateCameras();

        if (cameras.empty()) {
            cout << "Nessuna camera trovata nel sistema." << endl;
            return;
        }

        cout << "Trovate " << cameras.size() << " camera(s):" << endl;
        for (size_t i = 0; i < cameras.size(); ++i) {
            cout << (i + 1) << ". " << cameras[i].nomeConModello << endl;
        }
    }
    catch (const GenICamException& e) {
        cerr << "Errore enumerazione: " << e.what() << endl;
    }
}

// Test connessione e informazioni camera
void testCameraConnection(GenICamCamera& camera) {
    cout << "\n=== TEST CONNESSIONE CAMERA ===\n" << endl;

    try {
        // Connetti alla prima camera disponibile
        cout << "Connessione alla prima camera disponibile..." << endl;
        camera.connectFirst("Sigla_01");
        //camera.connectFirst("H2632510");
        cout << "Connesso con successo!" << endl;

        // Mostra informazioni camera
        cout << "\nInformazioni camera:" << endl;
        cout << camera.getCameraInfo() << endl;

        // Test parametri disponibili
        cout << "\nParametri disponibili (primi 20):" << endl;
        auto params = camera.getAvailableParameters();
        int count = 0;
        for (const auto& param : params) {
            cout << "  - " << param;
            if (camera.isParameterReadable(param)) cout << " [R]";
            if (camera.isParameterWritable(param)) cout << " [W]";
            cout << endl;
            /*
            if (++count >= 20) {
                cout << "  ... e altri " << (params.size() - 20) << " parametri" << endl;
                break;
            }*/
        }
        cout << " Totale " << params.size() << " parametri" << endl;
    }
    catch (const GenICamException& e) {
        cerr << "Errore connessione: " << e.what() << endl;
    }
}

// Test acquisizione singola
void testSingleFrameAcquisition(GenICamCamera& camera) {
    cout << "\n=== TEST ACQUISIZIONE FRAME SINGOLO ===\n" << endl;

    try {
        cout << "Acquisizione frame singolo..." << endl;

        auto startTime = chrono::high_resolution_clock::now();
        cv::Mat frame = camera.grabSingleFrame(5000);
        auto endTime = chrono::high_resolution_clock::now();

        auto duration = chrono::duration_cast<chrono::milliseconds>(endTime - startTime).count();

        if (!frame.empty()) {
            cout << "✓ Frame acquisito con successo!" << endl;
            cout << "  Dimensioni: " << frame.cols << "x" << frame.rows << endl;
            cout << "  Canali: " << frame.channels() << endl;
            cout << "  Tipo: " << frame.type() << endl;
            cout << "  Tempo acquisizione: " << duration << " ms" << endl;

            // Salva il frame
            string filename = "test_single_frame.png";
            cv::imwrite(filename, frame);
            cout << "  Frame salvato in: " << filename << endl;

            // Mostra il frame
            //cv::namedWindow("Window", cv::WINDOW_NORMAL);
            //cv::imshow("Window", frame);
            //cout << "\nPremi un tasto sulla finestra per continuare..." << endl;
            //std::cin.get();
            //cv::waitKey(0);
            //cv::destroyWindow("Window");
        }
        else {
            cout << "✗ Frame vuoto ricevuto!" << endl;
        }
    }
    catch (const GenICamException& e) {
        cerr << "Errore acquisizione: " << e.what() << endl;
    }
}

// Test streaming continuo
void testContinuousAcquisition(GenICamCamera& camera) {
    cout << "\n=== TEST ACQUISIZIONE CONTINUA ===\n" << endl;

    try {
        TestEventCallback callback(false);  // Con visualizzazione
        camera.setEventListener(&callback);

        cout << "Avvio acquisizione continua (10 buffer)..." << endl;
        camera.startAcquisition(10);

        cout << "\nAcquisizione in corso. Comandi disponibili:" << endl;
        cout << "  [SPAZIO] - Cattura e salva frame corrente" << endl;
        cout << "  [P] - Mostra/modifica parametri" << endl;
        cout << "  [ESC] - Termina acquisizione" << endl;
        cout << "\nStatistiche:" << endl;

        bool running = true;
        int savedFrames = 0;

        while (running) {
            if (_kbhit()) {
                char key = _getch();

                if (key == 27) {  // ESC
                    running = false;
                }
                else if (key == ' ') {  // SPAZIO
                    cv::Mat frame = callback.getLastFrame();
                    if (!frame.empty()) {
                        string filename = "captured_frame_" + to_string(++savedFrames) + ".png";
                        cv::imwrite(filename, frame);
                        cout << "\n✓ Frame salvato: " << filename << endl;
                    }
                }
                else if (key == 'p' || key == 'P') {  // P
                    cout << "\n\nParametri correnti:" << endl;
                    cout << "  Exposure: " << camera.getExposureTime() << " us" << endl;
                    cout << "  Gain: " << camera.getGain() << endl;
                    cout << "  Frame Rate: " << camera.getFrameRate() << " fps" << endl;
                }
            }

            this_thread::sleep_for(chrono::milliseconds(100));
        }

        cout << "\n\nArresto acquisizione..." << endl;
        camera.stopAcquisition();
        cout << "✓ Acquisizione terminata" << endl;
        cout << "  Frame totali acquisiti: " << callback.getFrameCount() << endl;
        cout << "  Errori: " << callback.getErrorCount() << endl;

    }
    catch (const GenICamException& e) {
        cerr << "Errore streaming: " << e.what() << endl;
    }
}

// Test parametri camera con accesso uniforme GenApi
void testCameraParameters(GenICamCamera& camera) {
    cout << "\n=== TEST PARAMETRI CAMERA (GENAPI) ===\n" << endl;

    try {
        // Test esposizione
        cout << "1. Test tempo di esposizione:" << endl;
        if (camera.isExposureTimeAvailable()) {
            double minExp, maxExp;
            camera.getExposureTimeRange(minExp, maxExp);
            double currentExp = camera.getExposureTime();

            cout << "   Range: " << minExp << " - " << maxExp << " us" << endl;
            cout << "   Valore corrente: " << currentExp << " us" << endl;

            // Prova a impostare alcuni valori
            vector<double> testExposures = { minExp, (minExp + maxExp) / 2, maxExp };
            for (double exp : testExposures) {
                if (exp >= minExp && exp <= maxExp) {
                    camera.setExposureTime(exp);
                    double readback = camera.getExposureTime();
                    cout << "   Set " << exp << " us -> Read " << readback << " us ";
                    cout << (abs(exp - readback) < 1.0 ? "✓" : "✗") << endl;
                }
            }
            camera.setExposureTime(currentExp);  // Ripristina
        }
        else {
            cout << "   Exposure time non disponibile" << endl;
        }

        // Test gain
        cout << "\n2. Test gain:" << endl;
        if (camera.isGainAvailable()) {
            double minGain, maxGain;
            camera.getGainRange(minGain, maxGain);
            double currentGain = camera.getGain();

            cout << "   Range: " << minGain << " - " << maxGain << endl;
            cout << "   Valore corrente: " << currentGain << endl;
        }
        else {
            cout << "   Gain non disponibile" << endl;
        }

        // Test ROI
        cout << "\n3. Test ROI:" << endl;
        uint32_t maxWidth, maxHeight;
        camera.getSensorSize(maxWidth, maxHeight);
        ROI currentROI = camera.getROI();

        cout << "   Dimensioni sensore: " << maxWidth << "x" << maxHeight << endl;
        cout << "   ROI corrente: " << currentROI.width << "x" << currentROI.height
            << " @ (" << currentROI.x << "," << currentROI.y << ")" << endl;

        // Test formato pixel
        cout << "\n4. Test formato pixel:" << endl;
        PixelFormat currentFormat = camera.getPixelFormat();
        cout << "   Formato corrente: ";
        switch (currentFormat) {
        case PixelFormat::Mono8: cout << "Mono8"; break;
        case PixelFormat::Mono10: cout << "Mono10"; break;
        case PixelFormat::Mono12: cout << "Mono12"; break;
        case PixelFormat::Mono16: cout << "Mono16"; break;
        case PixelFormat::RGB8: cout << "RGB8"; break;
        case PixelFormat::BGR8: cout << "BGR8"; break;
        case PixelFormat::BayerRG8: cout << "BayerRG8"; break;
        case PixelFormat::BayerGB8: cout << "BayerGB8"; break;
        case PixelFormat::BayerGR8: cout << "BayerGR8"; break;
        case PixelFormat::BayerBG8: cout << "BayerBG8"; break;
        case PixelFormat::YUV422_8: cout << "YUV422_8"; break;
        default: cout << "Altro"; break;
        }
        cout << endl;

        auto formats = camera.getAvailablePixelFormats();
        cout << "   Formati disponibili: " << formats.size() << endl;
        for (auto fmt : formats) {
            cout << "     - ";
            switch (fmt) {
            case PixelFormat::Mono8: cout << "Mono8"; break;
            case PixelFormat::Mono10: cout << "Mono10"; break;
            case PixelFormat::Mono12: cout << "Mono12"; break;
            case PixelFormat::Mono16: cout << "Mono16"; break;
            case PixelFormat::RGB8: cout << "RGB8"; break;
            case PixelFormat::BGR8: cout << "BGR8"; break;
            case PixelFormat::BayerRG8: cout << "BayerRG8"; break;
            case PixelFormat::BayerGB8: cout << "BayerGB8"; break;
            case PixelFormat::BayerGR8: cout << "BayerGR8"; break;
            case PixelFormat::BayerBG8: cout << "BayerBG8"; break;
            case PixelFormat::YUV422_8: cout << "YUV422_8"; break;
            default: cout << "Unknown"; break;
            }
            cout << endl;
        }

        // Test trigger mode
        cout << "\n5. Test modalità trigger:" << endl;
        if (camera.isTriggerModeAvailable()) {
            TriggerMode currentTrigger = camera.getTriggerMode();
            cout << "   Modalità corrente: ";
            switch (currentTrigger) {
            case TriggerMode::Off: cout << "Off (Free Running)"; break;
            case TriggerMode::On: cout << "On"; break;
            }
            cout << endl;
        }
        else {
            cout << "   Trigger mode non disponibile" << endl;
        }

        // Test frame rate
        cout << "\n6. Test frame rate:" << endl;
        if (camera.isFrameRateAvailable()) {
            double minFps, maxFps;
            camera.getFrameRateRange(minFps, maxFps);
            double currentFps = camera.getFrameRate();

            cout << "   Range: " << minFps << " - " << maxFps << " fps" << endl;
            cout << "   Valore corrente: " << currentFps << " fps" << endl;
        }
        else {
            cout << "   Frame rate control non disponibile" << endl;
        }

    }
    catch (const GenICamException& e) {
        cerr << "Errore test parametri: " << e.what() << endl;
    }
}

// Test trigger software
void testSoftwareTrigger(GenICamCamera& camera) {
    cout << "\n=== TEST TRIGGER SOFTWARE SFNC ===\n" << endl;

    try {
        // Mostra configurazione iniziale
        cout << camera.getTriggerConfiguration() << endl;

        // Seleziona trigger per FrameStart
        cout << "\nImpostazione trigger per FrameStart..." << endl;
        camera.setTriggerSelector(TriggerSelector::FrameStart);

        // Abilita trigger software
        cout << "Abilitazione trigger software..." << endl;
        camera.enableSoftwareTrigger(true);

        // Mostra configurazione aggiornata
        cout << "\n" << camera.getTriggerConfiguration() << endl;

        TestEventCallback callback(false);
        camera.setEventListener(&callback);

        cout << "\nAvvio acquisizione con trigger software..." << endl;
        camera.startAcquisition(5);

        cout << "\nEsecuzione 10 trigger software:" << endl;
        for (int i = 1; i <= 10; ++i) {
            cout << "  Trigger " << i << "... ";
            camera.executeTriggerSoftware();

            this_thread::sleep_for(chrono::milliseconds(100));
            cout << "Frame ricevuti: " << callback.getFrameCount() << endl;
        }

        cout << "\nArresto acquisizione..." << endl;
        camera.stopAcquisition();

        cout << "✓ Test completato. Frame totali: " << callback.getFrameCount() << endl;

        // Ripristina free running
        camera.enableSoftwareTrigger(false);

    }
    catch (const GenICamException& e) {
        cerr << "Errore test trigger: " << e.what() << endl;
    }
}

// Aggiungere nuovo test per trigger hardware:

void testHardwareTrigger(GenICamCamera& camera) {
    cout << "\n=== TEST TRIGGER HARDWARE SFNC ===\n" << endl;

    try {
        cout << "Sorgenti trigger disponibili:" << endl;
        auto sources = camera.getAvailableTriggerSources();
        for (const auto& source : sources) {
            cout << "  - ";
            switch (source) {
            case TriggerSource::Software: cout << "Software"; break;
            case TriggerSource::Line0: cout << "Line0"; break;
            case TriggerSource::Line1: cout << "Line1"; break;
            case TriggerSource::Line2: cout << "Line2"; break;
            case TriggerSource::Line3: cout << "Line3"; break;
                // ... altri casi
            default: cout << "Unknown";
            }
            cout << endl;
        }

        // Test trigger su Line1
        cout << "\nConfigurazione trigger hardware su Line1..." << endl;
        camera.setTriggerSelector(TriggerSelector::FrameStart);
        camera.enableHardwareTrigger(TriggerSource::Line1, TriggerActivation::RisingEdge);

        // Imposta delay
        cout << "Impostazione trigger delay a 1000 µs..." << endl;
        camera.setTriggerDelay(1000.0);

        cout << "\n" << camera.getTriggerConfiguration() << endl;

        cout << "\nPer testare il trigger hardware, applicare un segnale" << endl;
        cout << "alla Line1 della telecamera." << endl;

        // Ripristina
        camera.enableSoftwareTrigger(false);

    }
    catch (const GenICamException& e) {
        cerr << "Errore test trigger hardware: " << e.what() << endl;
    }
}

// Test accesso parametri generici
void testGenericParameters(GenICamCamera& camera) {
    cout << "\n=== TEST PARAMETRI GENERICI GENAPI ===\n" << endl;

    try {
        // Test lettura di alcuni parametri comuni
        vector<string> testParams = {
            "DeviceVendorName",
            "DeviceModelName",
            "DeviceVersion",
            "DeviceFirmwareVersion",
            "DeviceSerialNumber",
            "DeviceUserID",
            "PixelFormat",
            "Width",
            "Height",
            "ExposureTime",
            "Gain"
        };

        cout << "Lettura parametri comuni:" << endl;
        for (const auto& param : testParams) {
            if (camera.isParameterAvailable(param)) {
                try {
                    string value = camera.getParameter(param);
                    cout << "  " << param << " = " << value;

                    if (camera.isParameterWritable(param)) {
                        cout << " [R/W]";
                    }
                    else {
                        cout << " [R]";
                    }
                    cout << endl;
                }
                catch (const exception& e) {
                    cout << "  " << param << " - Errore: " << e.what() << endl;
                }
            }
            else {
                cout << "  " << param << " - Non disponibile" << endl;
            }
        }

        // Test scrittura parametro generico
        cout << "\nTest scrittura parametro UserDefinedName (se disponibile):" << endl;
        if (camera.isParameterAvailable("DeviceUserID") &&
            camera.isParameterWritable("DeviceUserID")) {

            string oldValue = camera.getParameter("DeviceUserID");
            cout << "  Valore corrente: " << oldValue << endl;

            string newValue = "TestCamera_xx";
            camera.setParameter("DeviceUserID", newValue);
            cout << "  Nuovo valore impostato: " << newValue << endl;

            string readback = camera.getParameter("DeviceUserID");
            cout << "  Valore riletto: " << readback << endl;
            cout << "  Test " << (readback == newValue ? "PASSED ✓" : "FAILED ✗") << endl;

            // Ripristina
            camera.setParameter("DeviceUserID", oldValue);
        }
        else {
            cout << "  DeviceUserID non disponibile o non scrivibile" << endl;
        }

    }
    catch (const GenICamException& e) {
        cerr << "Errore test parametri generici: " << e.what() << endl;
    }
}

// Menu principale
void showMainMenu() {
    //clearScreen();
    cout << "╔══════════════════════════════════════════════════╗" << endl;
    cout << "║        TEST GENICAM CAMERA WRAPPER               ║" << endl;
    cout << "╚══════════════════════════════════════════════════╝" << endl;
    cout << "\nMenu principale:\n" << endl;
    cout << "  1. Test GenTL Loader" << endl;
    cout << "  2. Enumera camere disponibili" << endl;
    cout << "  3. Test connessione camera" << endl;
    cout << "  4. Test acquisizione frame singolo" << endl;
    cout << "  5. Test acquisizione continua (streaming)" << endl;
    cout << "  6. Test parametri camera (GenApi)" << endl;
    cout << "  7. Test trigger software" << endl;
    cout << "  8. Test parametri generici" << endl;
    cout << "  9. Test completo automatico" << endl;
    cout << "  0. Esci" << endl;
    cout << "\nScegli un'opzione: ";
}

// Test automatico completo
void runAutomaticTests(GenICamCamera& camera) {
    cout << "\n=== ESECUZIONE TEST AUTOMATICO COMPLETO ===\n" << endl;

    // 1. Test loader
    cout << "[1/8] Test GenTL Loader..." << endl;
    testGenTLLoader();
    pauseAndWait();

    // 2. Enumerazione
    cout << "\n[2/8] Enumerazione camere..." << endl;
    testCameraEnumeration(camera);
    pauseAndWait();

    // 3. Connessione
    cout << "\n[3/8] Test connessione..." << endl;
    testCameraConnection(camera);
    pauseAndWait();

    if (!camera.isConnected()) {
        cout << "\nImpossibile proseguire senza connessione camera." << endl;
        return;
    }

    // 4. Frame singolo
    cout << "\n[4/8] Test frame singolo..." << endl;
    testSingleFrameAcquisition(camera);
    pauseAndWait();

    // 5. Parametri
    cout << "\n[5/8] Test parametri..." << endl;
    testCameraParameters(camera);
    pauseAndWait();

    // 6. Parametri generici
    cout << "\n[6/8] Test parametri generici..." << endl;
    testGenericParameters(camera);
    pauseAndWait();

    // 7. Trigger software
    cout << "\n[7/8] Test trigger software..." << endl;
    testSoftwareTrigger(camera);
    pauseAndWait();

    // 8. Streaming (breve)
    cout << "\n[8/8] Test streaming breve (5 secondi)..." << endl;
    TestEventCallback callback(true);
    camera.setEventListener(&callback);
    camera.startAcquisition(10);

    cout << "Acquisizione in corso..." << endl;
    this_thread::sleep_for(chrono::seconds(5));

    camera.stopAcquisition();
    cout << "✓ Test completato. Frame acquisiti: " << callback.getFrameCount() << endl;

    cout << "\n=== TEST AUTOMATICO COMPLETATO ===" << endl;
}

// Main
int main() {
    // Imposta encoding console per caratteri speciali
    system("chcp 65001 > nul");

    cout << "Inizializzazione GenICam Camera Wrapper..." << endl;
    cout << "Thread-safe version with unified GenApi access" << endl;

    //GenICamCamera camera("MvProducerGEV.cti");
	 auto camera = std::make_unique<GenICamCamera>("MvProducerGEV.cti");
    //auto chunkManager = std::make_unique<ChunkDataManager>();
    bool exitProgram = false;

    while (!exitProgram) {
        showMainMenu();

        int choice;
        cin >> choice;
        cin.ignore(); // Pulisce il buffer

        clearScreen();

        try {
            switch (choice) {
            case 1:
                testGenTLLoader();
                break;

            case 2:
                testCameraEnumeration(*camera);
                break;

            case 3:
                testCameraConnection(*camera);

                //chunkManager->Initialize(camera->getNodeMap(), camera->getStreamNodeMap());
                break;

            case 4:
                if (!camera->isConnected()) {
                    cout << "Prima connetti una camera (opzione 3)" << endl;
                }
                else {
                    camera->debugAcquisitionParameters();
                    testSingleFrameAcquisition(*camera);
                }
                break;

            case 5:
                if (!camera->isConnected()) {
                    cout << "Prima connetti una camera (opzione 3)" << endl;
                }
                else {
                    testContinuousAcquisition(*camera);
                }
                break;

            case 6:
                if (!camera->isConnected()) {
                    cout << "Prima connetti una camera (opzione 3)" << endl;
                }
                else {
                    testCameraParameters(*camera);
                }
                break;

            case 7:
                if (!camera->isConnected()) {
                    cout << "Prima connetti una camera (opzione 3)" << endl;
                }
                else {
                    testSoftwareTrigger(*camera);
                }
                break;

            case 8:
                if (!camera->isConnected()) {
                    cout << "Prima connetti una camera (opzione 3)" << endl;
                }
                else {
                    testGenericParameters(*camera);
                }
                break;

            case 9:
                runAutomaticTests(*camera);
                break;

            case 0:
                exitProgram = true;
                cout << "Uscita dal programma..." << endl;
                break;

            default:
                cout << "Opzione non valida!" << endl;
            }

            if (!exitProgram && choice != 0) {
                pauseAndWait();
            }

        }
        catch (const GenICamException& e) {
            cerr << "\n[ECCEZIONE GENICAM] " << e.what() << endl;
            cerr << "Tipo errore: " << static_cast<int>(e.getType()) << endl;
            if (e.getErrorCode() != GenTL::GC_ERR_SUCCESS) {
                cerr << "Codice GenTL: " << e.getErrorCode() << endl;
            }
            pauseAndWait();
        }
        catch (const exception& e) {
            cerr << "\n[ECCEZIONE] " << e.what() << endl;
            pauseAndWait();
        }
    }

    // Cleanup
    if (camera->isConnected()) {
        cout << "\nDisconnessione camera..." << endl;
        camera->disconnect();
    }

    cout << "Programma terminato." << endl;
    return 0;
}