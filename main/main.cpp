#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi.h"

static const char *TAG = "http_server";

/* Настройки SPIFFS */
#define SPIFFS_BASE_PATH "/spiffs"
#define FALLBACK_PATH "/spiffs/index.html"
#define SPIFFS_PART_LABEL NULL
#define SPIFFS_MAX_FILES 5
#define SPIFFS_FORMAT_IF_MOUNT_FAILED true

/* HTTP */
#define SERVER_PORT 80
#define RECV_BUF_LEN 1024
#define SEND_BUF_LEN 1024
#define FILE_CHUNK 1024

/* Возвращаем mime по расширению */
static const char * get_mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    ext++; // skip '.'
    if (strcasecmp(ext, "html") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(ext, "htm") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(ext, "css") == 0) return "text/css";
    if (strcasecmp(ext, "js") == 0) return "application/javascript";
    if (strcasecmp(ext, "json") == 0) return "application/json";
    if (strcasecmp(ext, "png") == 0) return "image/png";
    if (strcasecmp(ext, "jpg") == 0) return "image/jpeg";
    if (strcasecmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, "gif") == 0) return "image/gif";
    if (strcasecmp(ext, "svg") == 0) return "image/svg+xml";
    if (strcasecmp(ext, "ico") == 0) return "image/x-icon";
    if (strcasecmp(ext, "txt") == 0) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

/* Убираем возможные `../` в пути и возвращаем безопасный путь в `buf` (buflen bytes) */
static void sanitize_path(const char *req_path, char *buf, size_t buflen) {
    // Если root или "/", то index.html
    if (!req_path || strcmp(req_path, "/") == 0) {
        snprintf(buf, buflen, "%s/%s", SPIFFS_BASE_PATH, "index.html");
        return;
    }

    // Обрезаем ведущий '/'
    const char *p = req_path;
    if (p[0] == '/') p++;

    // Убираем ".." сегменты
    char tmp[256] = {0};
    size_t ti = 0;
    const char *seg = p;
    while (*seg && ti + 1 < sizeof(tmp)) {
        // взять следующий сегмент
        const char *next = strchr(seg, '/');
        size_t len = next ? (size_t)(next - seg) : strlen(seg);
        if (len == 0) {
            // двойной '/', пропустить
            seg = next ? next + 1 : seg + len;
            continue;
        }
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            // попытка подняться выше — игнорируем (не добавляем)
        } else {
            if (ti + len + 1 >= sizeof(tmp)) break;
            if (ti != 0) tmp[ti++] = '/';
            memcpy(&tmp[ti], seg, len);
            ti += len;
            tmp[ti] = 0;
        }
        seg = next ? next + 1 : seg + len;
    }

    // Собираем полный путь
    snprintf(buf, buflen, "%s/%s", SPIFFS_BASE_PATH, tmp[0] ? tmp : "index.html");
}

/* Извлекаем path в pathbuf */
static void parse_request_path(const char *req, char *pathbuf, size_t pathbuflen) {
    // Ожидаем что первая строка: "GET /some/path HTTP/1.1"
    const char *sp1 = strchr(req, ' ');
    if (!sp1) { strncpy(pathbuf, "/", pathbuflen); return; }
    const char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2) { strncpy(pathbuf, "/", pathbuflen); return; }
    size_t len = sp2 - (sp1 + 1);
    if (len >= pathbuflen) len = pathbuflen - 1;
    memcpy(pathbuf, sp1 + 1, len);
    pathbuf[len] = 0;
}

/* Отправка error страницы */
static void send_404(int sock) {
    const char *body = "<html><body><h1>404 Not Found</h1></body></html>";
    char header[256];
    int body_len = strlen(body);
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 404 Not Found\r\n"
                     "Content-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: %d\r\n"
                     "Connection: close\r\n"
                     "\r\n", body_len);
    send(sock, header, n, 0);
    send(sock, body, body_len, 0);
}

/* Отправляем файл по пути (полный путь в файловой системе) */
static void send_file(int sock, const char *fullpath, bool isFallback = false) {
    FILE *f = fopen(fullpath, "rb");
    if (!f) {
        ESP_LOGW(TAG, "File not found: %s", fullpath);
        // Если файл не найден, реализуем fallback до корневого файла. Нужно, когда серверуем SPA приложения
        if (!isFallback) {
            send_file(sock, FALLBACK_PATH, true);
            return;
        } else {
            send_404(sock);
            return;
        }
    }

    // Определим размер файла
    fseek(f, 0, SEEK_END);
    long filesize = ftell(f);
    // Возвращаем указатель на место
    fseek(f, 0, SEEK_SET);

    const char *mime = get_mime_type(fullpath);
    char header[256];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %ld\r\n"
                              "Connection: close\r\n"
                              "\r\n", mime, filesize);
    send(sock, header, header_len, 0);

    // Отправляем тело чанками
    uint8_t buf[FILE_CHUNK];

    /* Счетчик прочитанных байтов */
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
        /* Счетчик отправленных байтов */
        size_t sent = 0;
        // Отправляем данные в сокет, пока чанк не кончится
        while (sent < r) {
            ssize_t s = send(sock, buf + sent, r - sent, 0);
            if (s < 0) {
                ESP_LOGW(TAG, "send error");
                fclose(f);
                return;
            }
            sent += s;
        }
    }
    fclose(f);
}

/* Обработка одного соединения */
static void handle_client(int client_sock) {
    char recv_buf[RECV_BUF_LEN + 1];
    memset(recv_buf, 0, sizeof(recv_buf));
    int r = recv(client_sock, recv_buf, RECV_BUF_LEN, 0);

    // Поскольку мы реализуем сервер, который просто отдает статичные файлы,
    // нам достаточно читать первые 1024 байта, чтобы вытянуть оттуда адрес.
    // Если бы предполагалась обработка POST запросов, то нужно было бы
    // Читать буфер через while на манер того, как это реализовано в функции
    // send_file
    if (r <= 0) {
        close(client_sock);
        return;
    }
    recv_buf[r] = 0;

    char req_path[256];
    parse_request_path(recv_buf, req_path, sizeof(req_path));
    ESP_LOGI(TAG, "Requested: %s", req_path);

    char safe_path[256];
    sanitize_path(req_path, safe_path, sizeof(safe_path));
    ESP_LOGI(TAG, "Serving file: %s", safe_path);

    send_file(client_sock, safe_path);

    shutdown(client_sock, SHUT_RDWR);
    close(client_sock);
}

/* Серверная задача */
static void http_server_task(void *pv) {
    struct sockaddr_in server_addr;
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

    // Не удалось создать сервер - удаляем задачу, чтобы не тратить на задачу ресурсы
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    // Разрешаем переиспользование адреса
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(SERVER_PORT);

    // Привязываем сокет к адресу сервера
    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    // Начинаем слушать сокет
    if (listen(listen_sock, 5) != 0) {
        ESP_LOGE(TAG, "Error during listen: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "HTTP server listening on port %d", SERVER_PORT);

    while (1) {
        struct sockaddr_in6 client_addr;
        u32_t addr_len = sizeof(client_addr);
        int client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client_sock < 0) {
            ESP_LOGW(TAG, "Unable to accept connection: errno %d", errno);
            continue;
        }
        handle_client(client_sock);
        // Даем контроллеру решать другие задачи (опционально)
        // vTaskDelay(pdMS_TO_TICKS(300));
    }

    // В любом адекватном сценарии сюда нельзя добраться.
    close(listen_sock);
    vTaskDelete(NULL);
}

/* Монтируем SPIFFS */
static esp_err_t init_spiffs(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_BASE_PATH,
        .partition_label = SPIFFS_PART_LABEL,
        .max_files = SPIFFS_MAX_FILES,
        .format_if_mount_failed = SPIFFS_FORMAT_IF_MOUNT_FAILED
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to register SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);
    ESP_LOGI(TAG, "SPIFFS mounted. total: %d, used: %d", (int)total, (int)used);
    return ESP_OK;
}

extern "C" void app_main(void) {
    // Инициализируем Wi-Fi. Здесь я использую типовой для своих проектов заголовочный файл
    // Если за пять попыток не удается подключиться, то контроллер перестанет стучаться, и нужно
    // перезагружать девайс программно или руками. 
    // Для обратной связи есть флаги `wifi_conection_established` и `wifi_conection_failed` - для управления обратной связью
    wifi_init_sta();
    esp_err_t r = init_spiffs();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS init failed");
        // можно продолжить, но сервер не будет отдавать файлы. Можно сделать ребут
        // устройства через `esp_restart` на прод девайсе. Ошибка может исчезнуть при
        // перезагрузке.
        // esp_restart();
    }

    xTaskCreate(http_server_task, "http_server", 8192, NULL, 5, NULL);
}
