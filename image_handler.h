#ifndef F1_TRACK_LOADER_H
#define F1_TRACK_LOADER_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <TJpg_Decoder.h>
#include <lvgl.h>

// ==========================================
// CONFIGURAZIONE
// ==========================================
const bool JPG_SWAP_BYTES = true; // Se i colori sono invertiti, metti false
const int IMG_REQ_WIDTH = 200;    // Larghezza richiesta

// ==========================================
// VARIABILI GLOBALI
// ==========================================
uint16_t* track_raw_buffer = nullptr;

// Variabili per il passaggio dati alla callback
static uint16_t* _decode_ptr = nullptr;
static int _decode_width = 0;   // Larghezza reale buffer
static int _decode_height = 0;  // Altezza reale buffer

lv_image_dsc_t track_img_dsc;
lv_obj_t* ui_TrackImage = nullptr; 

// ==========================================
// HELPERS
// ==========================================

// 1. Mapping Circuit ID -> URL wsrv.nl
String getTrackImageUrl(String circuitId) {
    String originalUrl = "";

    if (circuitId == "bahrain") originalUrl = "https://upload.wikimedia.org/wikipedia/commons/thumb/2/29/Bahrain_International_Circuit--Grand_Prix_Layout.svg/1024px-Bahrain_International_Circuit--Grand_Prix_Layout.svg.png";
    else if (circuitId == "werace") originalUrl = "https://www.trackscapes.com/cdn/shop/products/Circuit-de-Barcelona-Catalunya-Spain-Racing-Track-Art-Sculpture-Layout.jpeg?v=1747217816";
    else if (circuitId == "jeddah") originalUrl = "https://upload.wikimedia.org/wikipedia/commons/thumb/0/05/Jeddah_Street_Circuit_2021.svg/1024px-Jeddah_Street_Circuit_2021.svg.png";
    else if (circuitId == "albert_park") originalUrl = "https://upload.wikimedia.org/wikipedia/commons/thumb/5/52/Albert_Park_Circuit_2021.svg/1024px-Albert_Park_Circuit_2021.svg.png";
    else if (circuitId == "suzuka") originalUrl = "https://upload.wikimedia.org/wikipedia/commons/thumb/2/22/Suzuka_circuit_map_2005.svg/1024px-Suzuka_circuit_map_2005.svg.png";
    else if (circuitId == "shanghai") originalUrl = "https://upload.wikimedia.org/wikipedia/commons/thumb/0/0e/Shanghai_International_Racing_Circuit_track_map.svg/1024px-Shanghai_International_Racing_Circuit_track_map.svg.png";
    else if (circuitId == "miami") originalUrl = "https://upload.wikimedia.org/wikipedia/commons/thumb/4/41/Miami_International_Autodrome_2022.svg/1024px-Miami_International_Autodrome_2022.svg.png";
    else if (circuitId == "imola") originalUrl = "https://upload.wikimedia.org/wikipedia/commons/thumb/2/23/Imola_2009.svg/1024px-Imola_2009.svg.png";
    else if (circuitId == "monaco") originalUrl = "https://upload.wikimedia.org/wikipedia/commons/thumb/3/30/Monte_Carlo_Formula_1_track_map.svg/1024px-Monte_Carlo_Formula_1_track_map.svg.png";
    else if (circuitId == "villeneuve") originalUrl = "https://upload.wikimedia.org/wikipedia/commons/thumb/e/e0/Circuit_Gilles_Villeneuve.svg/1024px-Circuit_Gilles_Villeneuve.svg.png";
    else if (circuitId == "catalunya") originalUrl = "https://upload.wikimedia.org/wikipedia/commons/thumb/a/a2/Catalunya_2021.svg/1024px-Catalunya_2021.svg.png";
    else if (circuitId == "red_bull_ring") originalUrl = "https://upload.wikimedia.org/wikipedia/commons/thumb/4/4b/Spielberg_E2_2022.svg/1024px-Spielberg_E2_2022.svg.png";
    else if (circuitId == "silverstone") originalUrl = "https://upload.wikimedia.org/wikipedia/commons/thumb/1/15/Silverstone_Circuit_2022.svg/1024px-Silverstone_Circuit_2022.svg.png";
    else if (circuitId == "hungaroring") originalUrl = "https://upload.wikimedia.org/wikipedia/commons/thumb/e/e4/Hungaroring.svg/1024px-Hungaroring.svg.png";
    else if (circuitId == "spa") originalUrl = "https://upload.wikimedia.org/wikipedia/commons/thumb/5/54/Spa-Francorchamps_of_Belgium.svg/1024px-Spa-Francorchamps_of_Belgium.svg.png";
    else if (circuitId == "zandvoort") originalUrl = "https://upload.wikimedia.org/wikipedia/commons/thumb/e/e5/Circuit_Zandvoort_2020.svg/1024px-Circuit_Zandvoort_2020.svg.png";
    else if (circuitId == "monza") originalUrl = "https://upload.wikimedia.org/wikipedia/commons/thumb/5/56/Monza_track_map.svg/1024px-Monza_track_map.svg.png";
    else if (circuitId == "baku") originalUrl = "https://upload.wikimedia.org/wikipedia/commons/thumb/6/6d/Baku_Formula_One_circuit_map.svg/1024px-Baku_Formula_One_circuit_map.svg.png";
    else if (circuitId == "marina_bay") originalUrl = "https://upload.wikimedia.org/wikipedia/commons/thumb/b/b8/Marina_Bay_Street_Circuit_2023.svg/1024px-Marina_Bay_Street_Circuit_2023.svg.png";
    else if (circuitId == "americas") originalUrl = "https://upload.wikimedia.org/wikipedia/commons/thumb/a/a5/Austin_circuit.svg/1024px-Austin_circuit.svg.png";
    else if (circuitId == "rodriguez") originalUrl = "https://upload.wikimedia.org/wikipedia/commons/thumb/2/2b/Aut%C3%B3dromo_Hermanos_Rodr%C3%ADguez_2015.svg/1024px-Aut%C3%B3dromo_Hermanos_Rodr%C3%ADguez_2015.svg.png";
    else if (circuitId == "interlagos") originalUrl = "https://upload.wikimedia.org/wikipedia/commons/thumb/5/5c/Interlagos_2014.svg/1024px-Interlagos_2014.svg.png";
    else if (circuitId == "las_vegas") originalUrl = "https://upload.wikimedia.org/wikipedia/commons/thumb/8/87/Las_Vegas_Strip_Circuit_2023.svg/1024px-Las_Vegas_Strip_Circuit_2023.svg.png";
    else if (circuitId == "losail") originalUrl = "https://upload.wikimedia.org/wikipedia/commons/thumb/e/ee/Losail_International_Circuit_2023.svg/1024px-Losail_International_Circuit_2023.svg.png";
    else if (circuitId == "yas_marina") originalUrl = "https://upload.wikimedia.org/wikipedia/commons/thumb/2/2c/Yas_Marina_Circuit_2021.svg/1024px-Yas_Marina_Circuit_2021.svg.png";
    else return "";

    // Costruiamo l'URL proxy wsrv.nl
    // output=jpg: forza JPG per evitare PNG trasparenti
    // we=1: scala mantenendo aspect ratio
    // bg=white: mette sfondo bianco (se UI è chiara) o black (se UI scura)
    return "https://wsrv.nl/?url=" + originalUrl + "&w=" + String(IMG_REQ_WIDTH) + "&output=jpg&we=1&bg=white";
}

// 2. CALLBACK CORRETTA CON CLIPPING (CRITICO!!)
bool tjpg_output_callback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (y >= _decode_height) return 0; // Fuori altezza

    // Disegna riga per riga
    for (int16_t i = 0; i < h; i++) {
        int16_t current_y = y + i;
        if(current_y >= _decode_height) break;

        // Calcola quanti pixel copiare realmente
        // Se x + w supera la larghezza immagine, taglia!
        int16_t pixels_to_copy = w;
        if (x + w > _decode_width) {
            pixels_to_copy = _decode_width - x;
        }

        if (pixels_to_copy > 0) {
            // Copia sicura
            memcpy(
                &_decode_ptr[(current_y * _decode_width) + x], 
                &bitmap[i * w], 
                pixels_to_copy * sizeof(uint16_t)
            );
        }
    }
    return 1;
}

// 3. Download
uint8_t* downloadImageToBuffer(String url, int &len) {
    HTTPClient http;
    http.setReuse(false);
    http.setUserAgent("Mozilla/5.0 (ESP32)"); // Aggiunto per evitare blocchi 403
    http.begin(url);
    
    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[TrackLoader] HTTP Error: %d\n", httpCode);
        http.end();
        return nullptr;
    }

    len = http.getSize();
    // Controllo Magic Bytes (è davvero un JPG?)
    if (len < 10) { 
        Serial.println("File troppo piccolo"); 
        http.end(); return nullptr; 
    }

    uint8_t* buf = (uint8_t*)ps_malloc(len);
    if (!buf) buf = (uint8_t*)malloc(len);

    if (!buf) {
        Serial.println("[TrackLoader] RAM insufficiente");
        http.end();
        return nullptr;
    }

    WiFiClient *stream = http.getStreamPtr();
    int total_read = 0;
    unsigned long start_time = millis();
    
    while (http.connected() && total_read < len && (millis() - start_time < 5000)) { 
        size_t available = stream->available();
        if (available) {
            int c = stream->readBytes(buf + total_read, available);
            total_read += c;
            start_time = millis();
        } else {
            delay(5);
        }
    }
    http.end();

    if (total_read != len) {
        Serial.printf("[TrackLoader] Errore Download incompleto\n");
        free(buf);
        return nullptr;
    }

    // CHECK MAGIC BYTES JPG (FF D8)
    if (buf[0] != 0xFF || buf[1] != 0xD8) {
        Serial.printf("[TrackLoader] ERRORE: Il file scaricato NON è un JPG! (Header: %02X %02X)\n", buf[0], buf[1]);
        free(buf);
        return nullptr;
    }

    return buf;
}

// ==========================================
// FUNZIONE PRINCIPALE
// ==========================================
void displayTrackLayout(String circuitId) {
    if (WiFi.status() != WL_CONNECTED) return;

    String imageUrl = getTrackImageUrl(circuitId);
    if (imageUrl == "") return;

    Serial.println("[TrackLoader] URL: " + imageUrl);

    int jpg_len = 0;
    uint8_t* jpg_buffer = downloadImageToBuffer(imageUrl, jpg_len);
    if (!jpg_buffer) return; 

    uint16_t w = 0, h = 0;
    TJpgDec.getJpgSize(&w, &h, jpg_buffer, jpg_len);
    Serial.printf("[TrackLoader] Size: %dx%d\n", w, h);

    size_t raw_size = w * h * 2;
    
    // --- GESTIONE MEMORIA ---
    if (track_raw_buffer != nullptr) {
        free(track_raw_buffer);
        track_raw_buffer = nullptr;
    }

    track_raw_buffer = (uint16_t*)ps_malloc(raw_size);
    if (!track_raw_buffer) track_raw_buffer = (uint16_t*)malloc(raw_size);

    if (!track_raw_buffer) {
        Serial.println("[TrackLoader] OOM (Out Of Memory)");
        free(jpg_buffer);
        return;
    }

    // *** FIX RUMORE STATICO ***
    // Riempiamo tutto di Nero (0x0000) PRIMA di decodificare.
    // Se la decodifica fallisce, vedrai un box nero, non neve colorata.
    memset(track_raw_buffer, 0, raw_size);

    // Configurazione Callback
    _decode_ptr = track_raw_buffer;
    _decode_width = w;
    _decode_height = h;

    TJpgDec.setJpgScale(1);
    TJpgDec.setSwapBytes(JPG_SWAP_BYTES);
    TJpgDec.setCallback(tjpg_output_callback);
    
    // Decodifica
    TJpgDec.drawJpg(0, 0, jpg_buffer, jpg_len);
    free(jpg_buffer);

    // LVGL Setup
    track_img_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    track_img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    track_img_dsc.header.flags = 0;
    track_img_dsc.header.w = w;
    track_img_dsc.header.h = h;
    track_img_dsc.header.stride = w * 2; 
    track_img_dsc.data_size = raw_size;
    track_img_dsc.data = (const uint8_t*)track_raw_buffer;

    if (ui_TrackImage == nullptr) {
        ui_TrackImage = lv_image_create(lv_screen_active());
        lv_obj_align(ui_TrackImage, LV_ALIGN_CENTER, 0, 0);
    }
    
    lv_image_set_src(ui_TrackImage, &track_img_dsc);
    lv_obj_invalidate(ui_TrackImage);
}

#endif