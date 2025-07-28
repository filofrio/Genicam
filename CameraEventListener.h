#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include "ImageTypes.h"

namespace GenICamWrapper {

    /**
     * @brief Interfaccia per la gestione degli eventi della telecamera
     *
     * Classe astratta che definisce i callback per gli eventi asincroni
     * generati dalla telecamera durante l'acquisizione.
     *
     * Thread Safety: I metodi di callback possono essere chiamati da thread diversi.
     * L'implementazione deve gestire la sincronizzazione se necessario.
     */
    class CameraEventListener {
    public:
        virtual ~CameraEventListener() = default;

        /**
         * @brief Callback chiamato quando un nuovo frame è disponibile
         * @param imageData Puntatore ai dati dell'immagine acquisita
         * @note Questo metodo viene chiamato dal thread di acquisizione
         */
        virtual void OnFrameReady(const ImageData* imageData, cv::Mat) = 0;

        /**
         * @brief Callback chiamato quando la connessione con la camera viene persa
         * @param errorMessage Messaggio descrittivo dell'errore
         */
        virtual void OnConnectionLost(const std::string& errorMessage) = 0;

        /**
         * @brief Callback opzionale per errori asincroni durante l'acquisizione
         * @param errorCode Codice dell'errore
         * @param errorMessage Descrizione dell'errore
         */
        virtual void OnError(int errorCode, const std::string& errorMessage) {
            // Implementazione di default vuota - opzionale per le classi derivate
        }

        /**
         * @brief Callback opzionale per notificare l'inizio dell'acquisizione
         */
        virtual void OnAcquisitionStarted() {
            // Implementazione di default vuota
        }

        /**
         * @brief Callback opzionale per notificare la fine dell'acquisizione
         */
        virtual void OnAcquisitionStopped() {
            // Implementazione di default vuota
        }

        /**
         * @brief Callback opzionale per notificare un cambio di parametro
         * @param parameterName Nome del parametro modificato
         * @param newValue Nuovo valore del parametro
         */
        virtual void OnParameterChanged(const std::string& parameterName, const std::string& newValue) {
            // Implementazione di default vuota
        }
    };

} // namespace GenICamWrapper