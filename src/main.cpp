#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ESPAsyncWiFiManager.h>
#include <ESPmDNS.h>
#include <JPEGDEC.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <Adafruit_NeoPixel.h>
#include <esp_nn.h>

#include "victor2162-project-1_inferencing.h"

#define PIN_RGB 48
#define mdnsName "esp32"

Adafruit_NeoPixel led(1, PIN_RGB, NEO_GRB + NEO_KHZ800);

DNSServer dns;
AsyncWebServer server(80);
AsyncWiFiManager wm(&server, &dns);
JPEGDEC jpeg;

// ---------------------------------------------------------------------------
// Comunicação entre a task de inferência (Core 0) e o loop (Core 1)
// ---------------------------------------------------------------------------
static char inferenceResultJson[768] = {0};
static bool inferenceRunning = false;

// Flags e ponteiros para resposta assíncrona
volatile bool inferenceComplete = false;
TaskHandle_t InferenceTaskHandle = nullptr;

// ---------------------------------------------------------------------------
// Gestão de memória (PSRAM)
// ---------------------------------------------------------------------------
uint8_t* imageBuffer = nullptr;
size_t currentImageSize = 0;
const size_t MAX_IMAGE_SIZE = 3 * 1024 * 1024;

float* features = nullptr;

struct {
  int width;
  int height;
  int target_w = EI_CLASSIFIER_INPUT_WIDTH;
  int target_h = EI_CLASSIFIER_INPUT_HEIGHT;
} image_info;

// ---------------------------------------------------------------------------
// Protótipos
// ---------------------------------------------------------------------------
int JPEGDraw(JPEGDRAW* pDraw);
int get_feature_data(size_t offset, size_t length, float* out_ptr);
void inference_task(void* pvParameters);
void imprimirMetricasMemoria() {
  Serial.println("\n--- STATUS DE MEMÓRIA ---");

  // 1. Memória RAM Interna (SRAM)
  // Heap total disponível para alocação dinâmica
  uint32_t freeRAM = ESP.getFreeHeap();
  // O maior bloco contínuo (importante para evitar erros de alocação)
  uint32_t maxBlockRAM = ESP.getMaxAllocHeap();
  Serial.printf("RAM Interna Livre: %u bytes (Maior bloco: %u)\n", freeRAM,
                maxBlockRAM);

  // 2. PSRAM (RAM Externa de 8MB no seu caso)
  if (psramFound()) {
    uint32_t freePSRAM = ESP.getFreePsram();
    uint32_t maxBlockPSRAM = ESP.getMaxAllocPsram();
    Serial.printf("PSRAM Livre: %u bytes (Maior bloco: %u)\n", freePSRAM,
                  maxBlockPSRAM);
    Serial.printf("PSRAM máxima: %u MB\n", ESP.getPsramSize() / (1024 * 1024));
  } else {
    Serial.println("PSRAM não detectada ou não inicializada!");
  }

  // 3. Memória Flash (Onde fica o código e arquivos)
  uint32_t flashSize = ESP.getFlashChipSize();
  uint32_t sketchSize = ESP.getSketchSize();
  uint32_t freeSketchSpace = ESP.getFreeSketchSpace();

  Serial.printf("Flash Total: %u MB\n", flashSize / (1024 * 1024));
  Serial.printf("Sketch usado: %u bytes (Disponível: %u bytes)\n", sketchSize,
                freeSketchSpace);

  Serial.println("-------------------------\n");
}

// ---------------------------------------------------------------------------
// Callback do JPEGDEC
// ---------------------------------------------------------------------------
int JPEGDraw(JPEGDRAW* pDraw) {
  uint32_t original_w = image_info.width;
  uint32_t original_h = image_info.height;
  uint32_t target_w = image_info.target_w;
  uint32_t target_h = image_info.target_h;

  for (int y = 0; y < pDraw->iHeight; y++) {
    //disableCore0WDT();
    for (int x = 0; x < pDraw->iWidth; x++) {
      uint32_t src_x = pDraw->x + x;
      uint32_t src_y = pDraw->y + y;
      uint32_t dst_x = (src_x * target_w) / original_w;
      uint32_t dst_y = (src_y * target_h) / original_h;

      if (dst_x < target_w && dst_y < target_h) {
        uint16_t pixel = pDraw->pPixels[y * pDraw->iWidth + x];
        uint8_t r = (pixel & 0xF800) >> 8;
        uint8_t g = (pixel & 0x07E0) >> 3;
        uint8_t b = (pixel & 0x001F) << 3;
        uint32_t rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        features[(dst_y * target_w) + dst_x] = (float)rgb;
      }
    }
  }
  return 1;
}

int get_feature_data(size_t offset, size_t length, float* out_ptr) {
  memcpy(out_ptr, features + offset, length * sizeof(float));
  return 0;
}

// ---------------------------------------------------------------------------
// Task de inferência — roda no Core 0
// ---------------------------------------------------------------------------
void inference_task(void* pvParameters) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    Serial.println("Core 0: iniciando inferência...");
    esp_task_wdt_init(90, false); 
    unsigned long decode_start_time = millis();
    //disableCore0WDT();

    bool decodeOk = false;
    if (jpeg.openRAM(imageBuffer, currentImageSize, JPEGDraw)) {
      image_info.width = jpeg.getWidth();
      image_info.height = jpeg.getHeight();
      Serial.printf("Decodificando %dx%d -> %dx%d\n", image_info.width,
                    image_info.height, image_info.target_w,
                    image_info.target_h);
      jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
      jpeg.decode(0, 0, 0);
      jpeg.close();
      decodeOk = true;
    }
    unsigned long decode_end_time = millis();

    if (!decodeOk) {
      snprintf(inferenceResultJson, sizeof(inferenceResultJson),
               "{\"status\":\"erro\",\"mensagem\":\"Falha ao ler o JPEG\"}");
      inferenceComplete = true;
      esp_task_wdt_init(5, false);
      continue;
    }

    Serial.println("Executando a rede neural...");
    ei_impulse_result_t result = {0};
    signal_t signal;
    signal.total_length = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE;
    signal.get_data = &get_feature_data;
    unsigned long inference_start_time = millis();

    // Desliga o WDT do Core 0 durante a matemática pesada
    EI_IMPULSE_ERROR res = run_classifier(&signal, &result, false);
    //enableCore0WDT();
    unsigned long inference_end_time = millis();
    esp_task_wdt_init(5, false);

    if (res != EI_IMPULSE_OK) {
      snprintf(inferenceResultJson, sizeof(inferenceResultJson),
               "{\"status\":\"erro\",\"mensagem\":\"Falha no classificador\"}");
      inferenceComplete = true;
      continue;
    }

    int written =
        snprintf(inferenceResultJson, sizeof(inferenceResultJson),
                 "{\"status\":\"sucesso\",\"tempo_ms\":%lu,\"resultados\":[",
                 inference_end_time - inference_start_time);

    for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
      written += snprintf(
          inferenceResultJson + written, sizeof(inferenceResultJson) - written,
          "{\"label\":\"%s\",\"score\":%.4f}%s\n",
          result.classification[ix].label, result.classification[ix].value,
          (ix < EI_CLASSIFIER_LABEL_COUNT - 1) ? "," : "");
    }
    snprintf(inferenceResultJson + written,
             sizeof(inferenceResultJson) - written, "]}");

    Serial.printf("Core 0: decodificação finalizada em %lu ms\n",
                  decode_end_time - decode_start_time);
    Serial.printf("Core 0: inferencia finalizada em %lu ms\n",
                  inference_end_time - inference_start_time);

    // Avisa ao loop() que o JSON está pronto para envio
    inferenceComplete = true;
  }
}

// ---------------------------------------------------------------------------
// Rotas HTTP
// ---------------------------------------------------------------------------
void setupDefaultRoutes() {
  server.on("/api/upload", HTTP_OPTIONS, [](AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = request->beginResponse(204);
    response->addHeader("Access-Control-Allow-Origin", "*");
    response->addHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
    response->addHeader("Access-Control-Allow-Headers", "Content-Type");
    request->send(response);
  });

  // CORS Preflight para a rota de resultados
  server.on("/api/resultado", HTTP_OPTIONS, [](AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = request->beginResponse(204);
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
  });

  // 1. ROTA DE UPLOAD (Responde rápido para não dar Timeout)
  server.on(
      "/api/upload", HTTP_POST,
      [](AsyncWebServerRequest* request) {
        if (inferenceRunning) {
          request->send(429, "application/json",
                        "{\"status\":\"erro\",\"mensagem\":\"Ocupado.\"}");
          return;
        }

        inferenceRunning = true;
        inferenceComplete = false;

        // RESPONDE IMEDIATAMENTE! O navegador não morre de tédio esperando.
        AsyncWebServerResponse* response = request->beginResponse(
            200, "application/json", "{\"status\":\"processando\"}");
        response->addHeader("Access-Control-Allow-Origin", "*");
        request->send(response);

        // Acorda o Core 0 para trabalhar
        xTaskNotifyGive(InferenceTaskHandle);
      },
      // Callback dos bytes da imagem (Mantém exatamente o mesmo)
      [](AsyncWebServerRequest* request, String filename, size_t index,
         uint8_t* data, size_t len, bool final) {
        if (index == 0) {
          currentImageSize = 0;
          Serial.printf("\nRecebendo: %s\n", filename.c_str());
        }
        if (currentImageSize + len <= MAX_IMAGE_SIZE) {
          memcpy(imageBuffer + currentImageSize, data, len);
          currentImageSize += len;
        }
        if (final) {
          Serial.printf("Upload concluido: %u bytes\n", currentImageSize);
        }
      });

  // 2. NOVA ROTA DE CONSULTA (O Vue.js vai bater aqui a cada 1 segundo)
  server.on("/api/resultado", HTTP_GET, [](AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response;

    if (inferenceComplete) {
      // A IA terminou! Devolve o JSON real e reseta o sistema
      response =
          request->beginResponse(200, "application/json", inferenceResultJson);
      inferenceComplete = false;
      inferenceRunning = false;
    } else if (inferenceRunning) {
      // A IA ainda está suando no Core 0
      response = request->beginResponse(200, "application/json",
                                        "{\"status\":\"processando\"}");
    } else {
      response = request->beginResponse(200, "application/json",
                                        "{\"status\":\"ocioso\"}");
    }

    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
  });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(LittleFS, "/index.html", "text/html");
  });
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  led.begin();
  led.setBrightness(5);

  if (!LittleFS.begin(true)) {
    Serial.println("Erro ao montar LittleFS");
    return;
  }

  if (!psramInit()) {
    Serial.println("ERRO CRITICO: PSRAM nao inicializou!");
    while (1);
  }

  imageBuffer = (uint8_t*)heap_caps_malloc(MAX_IMAGE_SIZE, MALLOC_CAP_SPIRAM);
  features = (float*)heap_caps_malloc(
      EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);

  if (!wm.autoConnect("ESP32_S3_Config")) {
    Serial.println("Falha na conexao e tempo esgotado");
    ESP.restart();
  }
  Serial.print("Conectado! IP: ");
  Serial.println(WiFi.localIP());

  if (MDNS.begin(mdnsName)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Acesse: http://%s.local\n", mdnsName);
  }

  setupDefaultRoutes();
  server.begin();
  imprimirMetricasMemoria();

  xTaskCreatePinnedToCore(inference_task, "InferenceTask", 8192 * 2, nullptr, 1,
                          &InferenceTaskHandle,
                          0  // Core 0
  );
}

// ---------------------------------------------------------------------------
// Loop — O Responsável pelo Envio
// ---------------------------------------------------------------------------
void loop() {
  static unsigned long lastUpdate = 0;
  static long hue = 0;

  if (millis() - lastUpdate >= 10) {
    lastUpdate = millis();
    led.setPixelColor(0, led.ColorHSV(hue, 255, 255));
    led.show();
    hue = (hue + 256) % 65536;
  }
}
/*
void loop() {
  for (long hue = 0; hue < 65536; hue += 256) {
    // Converte HSV para RGB
    // Parâmetros: (Matiz, Saturação 255, Brilho 255)
    uint32_t color = led.ColorHSV(hue, 255, 255);

    led.setPixelColor(0, color);
    led.show();

    delay(10);
  }
}
*/