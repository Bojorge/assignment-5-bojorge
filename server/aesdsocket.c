#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

#define PORT 9000
#define DATA_FILE "/var/tmp/aesdsocketdata"

int server_fd = -1;
int client_fd = -1;
volatile sig_atomic_t exit_requested = 0;

// Manejador de señales (SIGINT y SIGTERM)
void handle_signal(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        exit_requested = 1;
        syslog(LOG_INFO, "Caught signal, exiting");
    }
}

void setup_signals() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

int main(int argc, char *argv[]) {
    int is_daemon = 0;
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        is_daemon = 1;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);
    setup_signals();

    struct sockaddr_in server_addr;
    
    // 1. Crear socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        syslog(LOG_ERR, "Socket creation failed");
        return -1;
    }

    // Permitir reutilizar la dirección si el programa se reinicia rápido
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    // 2. Bind
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        syslog(LOG_ERR, "Bind failed");
        close(server_fd);
        return -1;
    }

    // 3. Modo Daemon (-d)
    if (is_daemon) {
        pid_t pid = fork();
        if (pid < 0) {
            syslog(LOG_ERR, "Fork failed");
            close(server_fd);
            return -1;
        }
        if (pid > 0) {
            // El padre sale exitosamente
            return 0;
        }
        // El hijo continúa como daemon
        setsid();
        // Redirigir entrada/salida estándar a /dev/null
        int devnull = open("/dev/null", O_RDWR);
        if (devnull != -1) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
    }

    // 4. Listen
    if (listen(server_fd, 10) == -1) {
        syslog(LOG_ERR, "Listen failed");
        close(server_fd);
        return -1;
    }

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    // 5. Bucle principal de aceptación
    while (!exit_requested) {
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd == -1) {
            if (exit_requested) break;
            continue;
        }

        // Obtener IP del cliente
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
        syslog(LOG_INFO, "Accepted connection from %s", ip_str);

        // Abrir archivo de datos
        int file_fd = open(DATA_FILE, O_RDWR | O_CREAT | O_APPEND, 0644);
        if (file_fd == -1) {
            syslog(LOG_ERR, "Failed to open data file");
            close(client_fd);
            continue;
        }

        char *recv_buffer = NULL;
        size_t buffer_size = 0;
        char temp_buf[1024];
        ssize_t bytes_received;

        // 6. Recibir datos del cliente
        while ((bytes_received = recv(client_fd, temp_buf, sizeof(temp_buf), 0)) > 0) {
            // Ampliar el búfer dinámicamente
            char *new_buffer = realloc(recv_buffer, buffer_size + bytes_received + 1);
            if (!new_buffer) {
                syslog(LOG_ERR, "Memory allocation failed");
                break;
            }
            recv_buffer = new_buffer;
            memcpy(recv_buffer + buffer_size, temp_buf, bytes_received);
            buffer_size += bytes_received;
            recv_buffer[buffer_size] = '\0';

            // Procesar paquetes (separados por \n)
            char *newline;
            while ((newline = memchr(recv_buffer, '\n', buffer_size)) != NULL) {
                size_t packet_len = newline - recv_buffer + 1; // +1 para incluir el \n
                
                // Escribir el paquete en el archivo
                write(file_fd, recv_buffer, packet_len);

                // Leer el archivo completo y enviarlo de vuelta
                lseek(file_fd, 0, SEEK_SET);
                char send_buf[1024];
                ssize_t file_bytes;
                while ((file_bytes = read(file_fd, send_buf, sizeof(send_buf))) > 0) {
                    send(client_fd, send_buf, file_bytes, 0);
                }
                // Volver al final del archivo para la próxima escritura
                lseek(file_fd, 0, SEEK_END);

                // Mover los datos sobrantes al inicio del búfer
                size_t remaining = buffer_size - packet_len;
                memmove(recv_buffer, newline + 1, remaining);
                buffer_size = remaining;
            }
        }

        // Limpieza de la conexión
        free(recv_buffer);
        close(file_fd);
        close(client_fd);
        client_fd = -1;
        
        syslog(LOG_INFO, "Closed connection from %s", ip_str);
    }

    // 7. Limpieza final
    if (server_fd != -1) {
        close(server_fd);
    }
    unlink(DATA_FILE); // Borrar el archivo al salir
    syslog(LOG_INFO, "Exited cleanly");
    closelog();

    return 0;
}

