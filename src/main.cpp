#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ESPAsyncWiFiManager.h>
#include <ESPmDNS.h>
#include <JPEGDEC.h>
#include <LittleFS.h>
#include <WiFi.h>

#include "victor2162-project-1_inferencing.h"

#define PIN_RGB 48
#define mdnsName "esp32"

Adafruit_NeoPixel led(1, PIN_RGB, NEO_GRB + NEO_KHZ800);

// é necessário para usar AyncWifiManager
DNSServer dns;
AsyncWebServer server(80);
AsyncWiFiManager wm(&server, &dns);
JPEGDEC jpeg;

// --- GESTÃO DE MEMÓRIA (PSRAM) ---
uint8_t* imageBuffer = nullptr;  // image memory adress
size_t currentImageSize = 0;
const size_t MAX_IMAGE_SIZE = 3 * 1024 * 1024;

float* features = nullptr;  // array rede neural

struct {
  int width;
  int height;
  int target_w = EI_CLASSIFIER_INPUT_WIDTH;
  int target_h = EI_CLASSIFIER_INPUT_HEIGHT;
} image_info;

int JPEGDraw(JPEGDRAW* pDraw) {
  // Aqui poderíamos fazer o redimensionamento (resizing) em tempo real.
  // Para simplificar e garantir performance na S3, vamos focar na captura dos
  // dados.
  return 1;
}

// Função que o Edge Impulse usa para ler os pixels do buffer decodificado
int get_feature_data(size_t offset, size_t length, float* out_ptr) {
  // Esta função será expandida para converter os pixels do buffer para o
  // formato do modelo Geralmente: (pixel_rgb >> 16 & 0xFF) / 255.0f etc.
  return 0;
}

void run_inference(AsyncWebServerRequest* request) {
  Serial.println("Iniciando Inferência...");

  unsigned long start_time = millis();

  // 1. Abrir o JPEG da PSRAM
  if (jpeg.openRAM(imageBuffer, currentImageSize, JPEGDraw)) {
    image_info.width = jpeg.getWidth();
    image_info.height = jpeg.getHeight();

    Serial.printf("Imagem: %dx%d\n", image_info.width, image_info.height);
    jpeg.close();
  }

  // 2. Preparar o sinal para o classificador
  ei_impulse_result_t result = {0};
  signal_t signal;

  // Aqui ligamos o buffer de pixels à função de extração do Edge Impulse
  // Nota: Implementaremos a lógica de preenchimento de 'signal' na próxima
  // etapa após validarmos o tempo de resposta básico.

  // 3. Executar o Classificador
  // EI_IMPULSE_OK significa sucesso
  EI_IMPULSE_ERROR res = run_classifier(&signal, &result, false);

  unsigned long end_time = millis();

  // 4. Construir resposta JSON dinâmica
  String jsonResponse = "{\"status\": \"sucesso\", \"tempo_ms\": " +
                        String(end_time - start_time) + ", \"resultados\": [";

  for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
    jsonResponse +=
        "{\"label\": \"" + String(result.classification[ix].label) + "\", ";
    jsonResponse +=
        "\"score\": " + String(result.classification[ix].value, 4) + "}";
    if (ix < EI_CLASSIFIER_LABEL_COUNT - 1) jsonResponse += ",";
  }
  jsonResponse += "]}";

  request->send(200, "application/json", jsonResponse);
}

void setupDefaultRoutes() {
  // Rota OPTIONS para o Preflight do CORS (O navegador pergunta isso antes do
  // POST)
  server.on("/api/upload", HTTP_OPTIONS, [](AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = request->beginResponse(204);
    response->addHeader("Access-Control-Allow-Origin", "*");
    response->addHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
    response->addHeader("Access-Control-Allow-Headers", "Content-Type");
    request->send(response);
  });

  // 4. Configura o Endpoint de Upload (Recebendo POST em /api/upload)
  server.on(
      "/api/upload", HTTP_POST,
      [](AsyncWebServerRequest* request) {
        // Agora chamamos a inferência após o upload terminar
        run_inference(request);
      },
      [](AsyncWebServerRequest* request, String filename, size_t index,
         uint8_t* data, size_t len, bool final) {
        if (index == 0) {
          currentImageSize = 0;
          memset(imageBuffer, 0, MAX_IMAGE_SIZE);
        }
        if (currentImageSize + len <= MAX_IMAGE_SIZE) {
          memcpy(imageBuffer + currentImageSize, data, len);
          currentImageSize += len;
        }
        if (final) {
          Serial.printf("Upload concluido: %u bytes\n", currentImageSize);
        }
      });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(LittleFS, "/index.html", "text/html");
  });
}

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

// Callback do JPEGDEC: Intercepta cada bloco decodificado
int JPEGDraw(JPEGDRAW *pDraw) {
    int original_w = image_info.width;
    int original_h = image_info.height;
    int target_w = image_info.target_w;
    int target_h = image_info.target_h;

    // Iterar sobre todos os pixels deste bloco MCU (geralmente 16x16 ou 8x8)
    for (int y = 0; y < pDraw->iHeight; y++) {
        for (int x = 0; x < pDraw->iWidth; x++) {
            
            // Coordenadas reais do pixel na imagem original (Gigante)
            int src_x = pDraw->x + x;
            int src_y = pDraw->y + y;

            // Mapeia para a coordenada de destino na IA (Ex: 96x96) usando Nearest Neighbor
            int dst_x = (src_x * target_w) / original_w;
            int dst_y = (src_y * target_h) / original_h;

            // Garante que não vamos escrever fora do array da IA
            if (dst_x < target_w && dst_y < target_h) {
                
                // Pega o pixel em formato RGB565 (16 bits)
                uint16_t pixel = pDraw->pPixels[y * pDraw->iWidth + x];

                // Extrai os canais R, G e B
                uint8_t r = (pixel & 0xF800) >> 8;
                uint8_t g = (pixel & 0x07E0) >> 3;
                uint8_t b = (pixel & 0x001F) << 3;

                // O Edge Impulse espera os canais empacotados em um int de 32 bits (0x00RRGGBB)
                uint32_t rgb = (r << 16) | (g << 8) | b;

                // Salva o pixel empacotado no array (convertido para float)
                int dst_index = (dst_y * target_w) + dst_x;
                features[dst_index] = (float)rgb;
            }
        }
    }
    return 1; // 1 = Continua a decodificação
}

// Função que entrega o array processado para o modelo
int get_feature_data(size_t offset, size_t length, float *out_ptr) {
    memcpy(out_ptr, features + offset, length * sizeof(float));
    return 0;
}

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
    Serial.println("ERRO CRITICO: PSRAM não inicializou!");
    while (1);  // Trava aqui se não houver memória
  }

  // 2. Aloca o buffer na PSRAM de forma segura
  // MALLOC_CAP_SPIRAM força a alocação na RAM externa (8MB) e não na interna
  // (SRAM)
  imageBuffer = (uint8_t*)heap_caps_malloc(MAX_IMAGE_SIZE, MALLOC_CAP_SPIRAM);

  if (imageBuffer == nullptr) {
    Serial.println("ERRO: Falha ao alocar buffer na PSRAM!");
    while (1);
  }
  Serial.printf("Buffer de 3MB alocado na PSRAM. PSRAM Livre: %u bytes\n", ESP.getFreePsram());

  features = (float*)heap_caps_malloc(
    EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);

  if (features == nullptr) {
    Serial.println("ERRO: Falha ao alocar buffer de features na PSRAM!");
    while (1);
  }

  // Tenta conectar, se não conseguir, cria o AP "ESP32_Config"
  if (!wm.autoConnect("ESP32_S3_Config")) {
    Serial.println("Falha na conexão e tempo esgotado");
    ESP.restart();
  }

  Serial.print("Conectado! IP: ");
  Serial.println(WiFi.localIP());

  // mDNS
  if (MDNS.begin(mdnsName)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Acesse: http://%s.local\n", mdnsName);
  }

  setupDefaultRoutes();
  server.begin();
  imprimirMetricasMemoria();
  Serial.println("Modelo do chip: " + (String)ESP.getChipModel());
  Serial.println("Núcleos do chip: " + (String)ESP.getChipCores());
}

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