#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#define STB_EASY_FONT_IMPLEMENTATION
#include "stb_easy_font.h"

#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <map>
#include <algorithm>
#include <cmath>

// =====================================================
// CONFIGURACION GENERAL
// =====================================================

const unsigned int ANCHO_VENTANA = 1280;
const unsigned int ALTO_VENTANA = 720;

// Proteus usa COM5.
// OpenGL usa COM6.
const std::string PUERTO_SERIAL = "\\\\.\\COM6";

// =====================================================
// DATOS RECIBIDOS DESDE ARDUINO
// =====================================================

struct DatosArduino {
    int velocidad = 0;
    int pwmMotor = 0;
    int cajas = 0;
    int tiempoCajaMs = 0;

    int botonAtasco = 0;
    int atascoActivo = 0;

    int potenciometro = 0;

    std::string control = "SIN_CONTROL";
    std::string aviso = "SIN_AVISO";
    std::string modo = "HIBRIDO";
    std::string estado = "SIN_SERIAL";
    std::string ultimaLinea = "";

    bool conectado = false;
};

// =====================================================
// FUNCIONES AUXILIARES
// =====================================================

float limitarFloat(float valor, float minimo, float maximo) {
    if (valor < minimo) return minimo;
    if (valor > maximo) return maximo;
    return valor;
}

int limitarInt(int valor, int minimo, int maximo) {
    if (valor < minimo) return minimo;
    if (valor > maximo) return maximo;
    return valor;
}

std::string limpiarEspacios(const std::string& texto) {
    size_t inicio = texto.find_first_not_of(" \t\r\n");
    if (inicio == std::string::npos) return "";

    size_t fin = texto.find_last_not_of(" \t\r\n");
    return texto.substr(inicio, fin - inicio + 1);
}

bool esEstadoFuncionando(const std::string& estado) {
    return estado == "FUNCIONANDO" || estado == "RUN";
}

bool esEstadoBajaVelocidad(const std::string& estado) {
    return estado == "BAJA_VELOCIDAD" || estado == "LOW_SPEED";
}

bool esEstadoAtascoSimulado(const std::string& estado) {
    return estado == "ATASCO_SIMULADO" ||
        estado == "ATASCO" ||
        estado == "OBJECT_STOPPED";
}

bool esEstadoPosibleAtasco(const std::string& estado) {
    return estado == "POSIBLE_ATASCO" || estado == "POSSIBLE_JAM";
}

bool esEstadoEmergencia(const std::string& estado) {
    return estado == "EMERGENCIA" || estado == "EMERGENCY";
}

bool esControlFisicoPrioritario(const std::string& aviso) {
    return aviso == "CONTROL_FISICO_PRIORITARIO";
}

// =====================================================
// CALCULO LOCAL PARA MODO DEMO
// =====================================================

unsigned long calcularTiempoCajaDemo(int velocidad) {
    const int VELOCIDAD_MINIMA = 15;
    const int VELOCIDAD_MEDIA = 50;
    const int VELOCIDAD_ALTA = 90;

    const unsigned long TIEMPO_15 = 5000;
    const unsigned long TIEMPO_50 = 3000;
    const unsigned long TIEMPO_90 = 1500;

    if (velocidad < VELOCIDAD_MINIMA) return 0;

    auto interpolar = [](float x, float x1, float y1, float x2, float y2) {
        return y1 + ((x - x1) * (y2 - y1)) / (x2 - x1);
        };

    if (velocidad >= VELOCIDAD_MINIMA && velocidad <= VELOCIDAD_MEDIA) {
        return (unsigned long)interpolar(
            (float)velocidad,
            (float)VELOCIDAD_MINIMA,
            (float)TIEMPO_15,
            (float)VELOCIDAD_MEDIA,
            (float)TIEMPO_50
        );
    }

    if (velocidad > VELOCIDAD_MEDIA && velocidad <= VELOCIDAD_ALTA) {
        return (unsigned long)interpolar(
            (float)velocidad,
            (float)VELOCIDAD_MEDIA,
            (float)TIEMPO_50,
            (float)VELOCIDAD_ALTA,
            (float)TIEMPO_90
        );
    }

    return TIEMPO_90;
}

// =====================================================
// COMUNICACION SERIAL WINDOWS
// =====================================================

class ComunicacionSerial {
private:
    HANDLE puerto = INVALID_HANDLE_VALUE;
    std::thread hiloLectura;
    std::atomic<bool> ejecutando{ false };

    std::mutex mutexDatos;
    std::mutex mutexPuerto;

    DatosArduino datos;

public:
    ComunicacionSerial(const std::string& nombrePuerto) {
        abrir(nombrePuerto);
    }

    ~ComunicacionSerial() {
        cerrar();
    }

    bool abrir(const std::string& nombrePuerto) {
        puerto = CreateFileA(
            nombrePuerto.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );

        if (puerto == INVALID_HANDLE_VALUE) {
            std::cout << "No se pudo abrir " << nombrePuerto << std::endl;
            std::cout << "Revisa VSPE COM5 <-> COM6 y cierra PuTTY." << std::endl;
            return false;
        }

        DCB configuracion = { 0 };
        configuracion.DCBlength = sizeof(configuracion);

        if (!GetCommState(puerto, &configuracion)) {
            std::cout << "No se pudo obtener configuracion serial." << std::endl;
            CloseHandle(puerto);
            puerto = INVALID_HANDLE_VALUE;
            return false;
        }

        configuracion.BaudRate = CBR_9600;
        configuracion.ByteSize = 8;
        configuracion.StopBits = ONESTOPBIT;
        configuracion.Parity = NOPARITY;

        if (!SetCommState(puerto, &configuracion)) {
            std::cout << "No se pudo configurar puerto serial." << std::endl;
            CloseHandle(puerto);
            puerto = INVALID_HANDLE_VALUE;
            return false;
        }

        COMMTIMEOUTS timeouts = { 0 };
        timeouts.ReadIntervalTimeout = 50;
        timeouts.ReadTotalTimeoutConstant = 50;
        timeouts.ReadTotalTimeoutMultiplier = 10;
        timeouts.WriteTotalTimeoutConstant = 50;
        timeouts.WriteTotalTimeoutMultiplier = 10;

        SetCommTimeouts(puerto, &timeouts);
        PurgeComm(puerto, PURGE_RXCLEAR | PURGE_TXCLEAR);

        {
            std::lock_guard<std::mutex> lock(mutexDatos);
            datos.conectado = true;
            datos.estado = "ESPERANDO_DATOS";
            datos.control = "ESPERANDO";
            datos.aviso = "SIN_AVISO";
        }

        ejecutando = true;
        hiloLectura = std::thread(&ComunicacionSerial::bucleLectura, this);

        std::cout << "Puerto serial abierto correctamente: " << nombrePuerto << std::endl;

        return true;
    }

    bool estaAbierto() const {
        return puerto != INVALID_HANDLE_VALUE;
    }

    void cerrar() {
        ejecutando = false;

        if (hiloLectura.joinable()) {
            hiloLectura.join();
        }

        if (puerto != INVALID_HANDLE_VALUE) {
            CloseHandle(puerto);
            puerto = INVALID_HANDLE_VALUE;
        }
    }

    DatosArduino obtenerDatos() {
        std::lock_guard<std::mutex> lock(mutexDatos);
        return datos;
    }

    void enviarComando(const std::string& comando) {
        if (puerto == INVALID_HANDLE_VALUE) {
            std::cout << "Puerto cerrado. No se envio: " << comando << std::endl;
            return;
        }

        std::string mensaje = comando + "\n";
        DWORD bytesEscritos = 0;

        std::lock_guard<std::mutex> lock(mutexPuerto);

        BOOL ok = WriteFile(
            puerto,
            mensaje.c_str(),
            (DWORD)mensaje.size(),
            &bytesEscritos,
            NULL
        );

        if (ok) {
            std::cout << "COMANDO ENVIADO: " << comando << std::endl;
        }
        else {
            std::cout << "ERROR ENVIANDO COMANDO: " << comando << std::endl;
        }
    }

private:
    void bucleLectura() {
        char buffer[256];
        DWORD bytesLeidos = 0;
        std::string linea;

        while (ejecutando) {
            bool lecturaOk = false;

            {
                std::lock_guard<std::mutex> lock(mutexPuerto);

                if (puerto != INVALID_HANDLE_VALUE) {
                    lecturaOk = ReadFile(
                        puerto,
                        buffer,
                        sizeof(buffer) - 1,
                        &bytesLeidos,
                        NULL
                    );
                }
            }

            if (lecturaOk && bytesLeidos > 0) {
                buffer[bytesLeidos] = '\0';

                for (DWORD i = 0; i < bytesLeidos; i++) {
                    char c = buffer[i];

                    if (c == '\n') {
                        parsearLinea(linea);
                        linea.clear();
                    }
                    else if (c != '\r') {
                        linea += c;
                    }
                }
            }

            Sleep(5);
        }
    }

    void parsearLinea(const std::string& lineaOriginal) {
        std::string linea = limpiarEspacios(lineaOriginal);

        if (linea.empty()) return;

        DatosArduino nuevosDatos;

        {
            std::lock_guard<std::mutex> lock(mutexDatos);
            nuevosDatos = datos;
        }

        nuevosDatos.ultimaLinea = linea;
        nuevosDatos.conectado = true;

        std::stringstream ss(linea);
        std::string token;

        while (std::getline(ss, token, ';')) {
            size_t posIgual = token.find('=');
            if (posIgual == std::string::npos) continue;

            std::string clave = limpiarEspacios(token.substr(0, posIgual));
            std::string valor = limpiarEspacios(token.substr(posIgual + 1));

            try {
                if (clave == "VELOCIDAD") {
                    nuevosDatos.velocidad = std::stoi(valor);
                }
                else if (clave == "PWM_MOTOR") {
                    nuevosDatos.pwmMotor = std::stoi(valor);
                }
                else if (clave == "CAJAS") {
                    nuevosDatos.cajas = std::stoi(valor);
                }
                else if (clave == "TIEMPO_CAJA_MS") {
                    nuevosDatos.tiempoCajaMs = std::stoi(valor);
                }
                else if (clave == "BOTON_ATASCO") {
                    nuevosDatos.botonAtasco = std::stoi(valor);
                }
                else if (clave == "ATASCO_ACTIVO") {
                    nuevosDatos.atascoActivo = std::stoi(valor);
                }
                else if (clave == "POTENCIOMETRO") {
                    nuevosDatos.potenciometro = std::stoi(valor);
                }
                else if (clave == "CONTROL") {
                    nuevosDatos.control = valor;
                }
                else if (clave == "AVISO") {
                    nuevosDatos.aviso = valor;
                }
                else if (clave == "MODO") {
                    nuevosDatos.modo = valor;
                }
                else if (clave == "ESTADO") {
                    nuevosDatos.estado = valor;
                }

                // Compatibilidad con nombres anteriores
                else if (clave == "VEL") {
                    nuevosDatos.velocidad = std::stoi(valor);
                }
                else if (clave == "PWM") {
                    nuevosDatos.pwmMotor = std::stoi(valor);
                }
                else if (clave == "COUNT") {
                    nuevosDatos.cajas = std::stoi(valor);
                }
                else if (clave == "INTERVAL") {
                    nuevosDatos.tiempoCajaMs = std::stoi(valor);
                }
                else if (clave == "JAM_BUTTON") {
                    nuevosDatos.botonAtasco = std::stoi(valor);
                }
                else if (clave == "JAM_ACTIVE") {
                    nuevosDatos.atascoActivo = std::stoi(valor);
                }
                else if (clave == "ATASCO") {
                    nuevosDatos.atascoActivo = std::stoi(valor);
                }
                else if (clave == "MODE") {
                    nuevosDatos.modo = valor;
                }
                else if (clave == "STATE") {
                    nuevosDatos.estado = valor;
                }
            }
            catch (...) {
                // Ignorar datos corruptos
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutexDatos);
            datos = nuevosDatos;
        }

        std::cout << "SERIAL: " << linea << std::endl;
    }
};

// =====================================================
// SHADERS INTERNOS
// =====================================================

const char* vertexShaderSource = R"(
#version 330 core

layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;

out vec3 FragPos;
out vec3 Normal;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

void main()
{
    FragPos = vec3(model * vec4(aPos, 1.0));
    Normal = mat3(transpose(inverse(model))) * aNormal;
    gl_Position = projection * view * vec4(FragPos, 1.0);
}
)";

const char* fragmentShaderSource = R"(
#version 330 core

out vec4 FragColor;

in vec3 FragPos;
in vec3 Normal;

uniform vec3 objectColor;
uniform vec3 lightPos;
uniform vec3 viewPos;

void main()
{
    float ambientStrength = 0.35;
    vec3 ambient = ambientStrength * objectColor;

    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(lightPos - FragPos);

    float diff = max(dot(norm, lightDir), 0.0);
    vec3 diffuse = diff * objectColor;

    vec3 result = ambient + diffuse;

    FragColor = vec4(result, 1.0);
}
)";

GLuint compilarShader(GLenum tipo, const char* fuente) {
    GLuint shader = glCreateShader(tipo);
    glShaderSource(shader, 1, &fuente, NULL);
    glCompileShader(shader);

    int exito;
    char infoLog[1024];

    glGetShaderiv(shader, GL_COMPILE_STATUS, &exito);

    if (!exito) {
        glGetShaderInfoLog(shader, 1024, NULL, infoLog);
        std::cout << "ERROR COMPILANDO SHADER:\n" << infoLog << std::endl;
    }

    return shader;
}

GLuint crearProgramaShader() {
    GLuint vertexShader = compilarShader(GL_VERTEX_SHADER, vertexShaderSource);
    GLuint fragmentShader = compilarShader(GL_FRAGMENT_SHADER, fragmentShaderSource);

    GLuint programa = glCreateProgram();
    glAttachShader(programa, vertexShader);
    glAttachShader(programa, fragmentShader);
    glLinkProgram(programa);

    int exito;
    char infoLog[1024];

    glGetProgramiv(programa, GL_LINK_STATUS, &exito);

    if (!exito) {
        glGetProgramInfoLog(programa, 1024, NULL, infoLog);
        std::cout << "ERROR LINKING SHADER PROGRAM:\n" << infoLog << std::endl;
    }

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    return programa;
}

// =====================================================
// CUBO CON NORMALES
// =====================================================

float verticesCubo[] = {
    -0.5f,-0.5f,-0.5f,  0.0f, 0.0f,-1.0f,
     0.5f,-0.5f,-0.5f,  0.0f, 0.0f,-1.0f,
     0.5f, 0.5f,-0.5f,  0.0f, 0.0f,-1.0f,
     0.5f, 0.5f,-0.5f,  0.0f, 0.0f,-1.0f,
    -0.5f, 0.5f,-0.5f,  0.0f, 0.0f,-1.0f,
    -0.5f,-0.5f,-0.5f,  0.0f, 0.0f,-1.0f,

    -0.5f,-0.5f, 0.5f,  0.0f, 0.0f, 1.0f,
     0.5f,-0.5f, 0.5f,  0.0f, 0.0f, 1.0f,
     0.5f, 0.5f, 0.5f,  0.0f, 0.0f, 1.0f,
     0.5f, 0.5f, 0.5f,  0.0f, 0.0f, 1.0f,
    -0.5f, 0.5f, 0.5f,  0.0f, 0.0f, 1.0f,
    -0.5f,-0.5f, 0.5f,  0.0f, 0.0f, 1.0f,

    -0.5f, 0.5f, 0.5f, -1.0f, 0.0f, 0.0f,
    -0.5f, 0.5f,-0.5f, -1.0f, 0.0f, 0.0f,
    -0.5f,-0.5f,-0.5f, -1.0f, 0.0f, 0.0f,
    -0.5f,-0.5f,-0.5f, -1.0f, 0.0f, 0.0f,
    -0.5f,-0.5f, 0.5f, -1.0f, 0.0f, 0.0f,
    -0.5f, 0.5f, 0.5f, -1.0f, 0.0f, 0.0f,

     0.5f, 0.5f, 0.5f,  1.0f, 0.0f, 0.0f,
     0.5f, 0.5f,-0.5f,  1.0f, 0.0f, 0.0f,
     0.5f,-0.5f,-0.5f,  1.0f, 0.0f, 0.0f,
     0.5f,-0.5f,-0.5f,  1.0f, 0.0f, 0.0f,
     0.5f,-0.5f, 0.5f,  1.0f, 0.0f, 0.0f,
     0.5f, 0.5f, 0.5f,  1.0f, 0.0f, 0.0f,

    -0.5f,-0.5f,-0.5f,  0.0f,-1.0f, 0.0f,
     0.5f,-0.5f,-0.5f,  0.0f,-1.0f, 0.0f,
     0.5f,-0.5f, 0.5f,  0.0f,-1.0f, 0.0f,
     0.5f,-0.5f, 0.5f,  0.0f,-1.0f, 0.0f,
    -0.5f,-0.5f, 0.5f,  0.0f,-1.0f, 0.0f,
    -0.5f,-0.5f,-0.5f,  0.0f,-1.0f, 0.0f,

    -0.5f, 0.5f,-0.5f,  0.0f, 1.0f, 0.0f,
     0.5f, 0.5f,-0.5f,  0.0f, 1.0f, 0.0f,
     0.5f, 0.5f, 0.5f,  0.0f, 1.0f, 0.0f,
     0.5f, 0.5f, 0.5f,  0.0f, 1.0f, 0.0f,
    -0.5f, 0.5f, 0.5f,  0.0f, 1.0f, 0.0f,
    -0.5f, 0.5f,-0.5f,  0.0f, 1.0f, 0.0f
};

void dibujarCubo(
    GLuint shader,
    GLuint VAO,
    glm::vec3 posicion,
    glm::vec3 escala,
    glm::vec3 color
) {
    glm::mat4 modelo = glm::mat4(1.0f);
    modelo = glm::translate(modelo, posicion);
    modelo = glm::scale(modelo, escala);

    glUniformMatrix4fv(
        glGetUniformLocation(shader, "model"),
        1,
        GL_FALSE,
        glm::value_ptr(modelo)
    );

    glUniform3fv(
        glGetUniformLocation(shader, "objectColor"),
        1,
        glm::value_ptr(color)
    );

    glBindVertexArray(VAO);
    glDrawArrays(GL_TRIANGLES, 0, 36);
}

// =====================================================
// TEXTO 2D
// =====================================================

struct VerticeTexto {
    float x, y, z;
    unsigned char color[4];
};

class RenderTexto2D {
private:
    GLuint VAO = 0;
    GLuint VBO = 0;
    GLuint shader = 0;

public:
    void inicializar() {
        const char* vs = R"(
        #version 330 core

        layout (location = 0) in vec2 aPos;

        uniform vec2 pantalla;

        void main()
        {
            float x = (aPos.x / pantalla.x) * 2.0 - 1.0;
            float y = 1.0 - (aPos.y / pantalla.y) * 2.0;
            gl_Position = vec4(x, y, 0.0, 1.0);
        }
        )";

        const char* fs = R"(
        #version 330 core

        out vec4 FragColor;

        uniform vec4 colorTexto;

        void main()
        {
            FragColor = colorTexto;
        }
        )";

        GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vertexShader, 1, &vs, NULL);
        glCompileShader(vertexShader);

        GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fragmentShader, 1, &fs, NULL);
        glCompileShader(fragmentShader);

        shader = glCreateProgram();
        glAttachShader(shader, vertexShader);
        glAttachShader(shader, fragmentShader);
        glLinkProgram(shader);

        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);

        glGenVertexArrays(1, &VAO);
        glGenBuffers(1, &VBO);
    }

    void dibujarTexto(
        const std::string& texto,
        float x,
        float y,
        float escala,
        glm::vec4 color,
        int anchoPantalla,
        int altoPantalla
    ) {
        char buffer[99999];

        int quads = stb_easy_font_print(
            0.0f,
            0.0f,
            (char*)texto.c_str(),
            NULL,
            buffer,
            sizeof(buffer)
        );

        if (quads <= 0) return;

        VerticeTexto* vertices = (VerticeTexto*)buffer;

        std::vector<float> triangulos;
        triangulos.reserve(quads * 6 * 2);

        for (int i = 0; i < quads; i++) {
            VerticeTexto v0 = vertices[i * 4 + 0];
            VerticeTexto v1 = vertices[i * 4 + 1];
            VerticeTexto v2 = vertices[i * 4 + 2];
            VerticeTexto v3 = vertices[i * 4 + 3];

            auto agregar = [&](VerticeTexto v) {
                triangulos.push_back(x + v.x * escala);
                triangulos.push_back(y + v.y * escala);
                };

            agregar(v0);
            agregar(v1);
            agregar(v2);

            agregar(v0);
            agregar(v2);
            agregar(v3);
        }

        glUseProgram(shader);

        glUniform2f(
            glGetUniformLocation(shader, "pantalla"),
            (float)anchoPantalla,
            (float)altoPantalla
        );

        glUniform4f(
            glGetUniformLocation(shader, "colorTexto"),
            color.r,
            color.g,
            color.b,
            color.a
        );

        glBindVertexArray(VAO);
        glBindBuffer(GL_ARRAY_BUFFER, VBO);

        glBufferData(
            GL_ARRAY_BUFFER,
            triangulos.size() * sizeof(float),
            triangulos.data(),
            GL_DYNAMIC_DRAW
        );

        glVertexAttribPointer(
            0,
            2,
            GL_FLOAT,
            GL_FALSE,
            2 * sizeof(float),
            (void*)0
        );

        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(triangulos.size() / 2));
    }

    void dibujarRectangulo(
        float x,
        float y,
        float ancho,
        float alto,
        glm::vec4 color,
        int anchoPantalla,
        int altoPantalla
    ) {
        float vertices[] = {
            x,         y,
            x + ancho, y,
            x + ancho, y + alto,

            x,         y,
            x + ancho, y + alto,
            x,         y + alto
        };

        glUseProgram(shader);

        glUniform2f(
            glGetUniformLocation(shader, "pantalla"),
            (float)anchoPantalla,
            (float)altoPantalla
        );

        glUniform4f(
            glGetUniformLocation(shader, "colorTexto"),
            color.r,
            color.g,
            color.b,
            color.a
        );

        glBindVertexArray(VAO);
        glBindBuffer(GL_ARRAY_BUFFER, VBO);

        glBufferData(
            GL_ARRAY_BUFFER,
            sizeof(vertices),
            vertices,
            GL_DYNAMIC_DRAW
        );

        glVertexAttribPointer(
            0,
            2,
            GL_FLOAT,
            GL_FALSE,
            2 * sizeof(float),
            (void*)0
        );

        glEnableVertexAttribArray(0);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
};

// =====================================================
// HUD / MONITOR INDUSTRIAL
// =====================================================

void dibujarMonitorIndustrial(
    RenderTexto2D& texto,
    const DatosArduino& datos,
    bool modoDemo,
    int anchoPantalla,
    int altoPantalla
) {
    glm::vec4 fondo = glm::vec4(0.02f, 0.03f, 0.04f, 0.86f);
    glm::vec4 borde = glm::vec4(0.0f, 0.80f, 1.0f, 1.0f);
    glm::vec4 blanco = glm::vec4(0.92f, 0.96f, 1.0f, 1.0f);
    glm::vec4 celeste = glm::vec4(0.0f, 0.85f, 1.0f, 1.0f);
    glm::vec4 verde = glm::vec4(0.0f, 1.0f, 0.25f, 1.0f);
    glm::vec4 amarillo = glm::vec4(1.0f, 0.85f, 0.0f, 1.0f);
    glm::vec4 rojo = glm::vec4(1.0f, 0.10f, 0.10f, 1.0f);

    glm::vec4 colorEstado = verde;

    if (esEstadoBajaVelocidad(datos.estado)) {
        colorEstado = rojo;
    }
    else if (esEstadoAtascoSimulado(datos.estado)) {
        colorEstado = amarillo;
    }
    else if (esEstadoPosibleAtasco(datos.estado) || esEstadoEmergencia(datos.estado)) {
        colorEstado = rojo;
    }

    glm::vec4 colorControl = blanco;

    if (datos.control == "OPENGL") {
        colorControl = celeste;
    }
    else if (datos.control == "POTENCIOMETRO") {
        colorControl = verde;
    }

    glm::vec4 colorAviso = blanco;

    if (datos.aviso == "CONTROL_FISICO_PRIORITARIO") {
        colorAviso = rojo;
    }
    else if (datos.aviso == "CONTROL_OPENGL_ACTIVO") {
        colorAviso = celeste;
    }
    else if (datos.aviso == "POTENCIOMETRO_ACTIVO") {
        colorAviso = verde;
    }

    std::stringstream l1, l2, l3, l4, l5, l6, l7, l8, l9, l10;

    l1 << "MONITOR DE PRODUCCION";
    l2 << "Velocidad usada: " << datos.velocidad << "%";
    l3 << "Potenciometro: " << datos.potenciometro << "%";
    l4 << "PWM motor: " << datos.pwmMotor;
    l5 << "Cajas contadas: " << datos.cajas;
    l6 << "Tiempo por caja: " << datos.tiempoCajaMs << " ms";
    l7 << "Control: " << datos.control;
    l8 << "Aviso: " << datos.aviso;
    l9 << "Modo: " << datos.modo << (modoDemo ? " / DEMO" : " / SERIAL");
    l10 << "Estado: " << datos.estado;

    texto.dibujarRectangulo(20, 20, 500, 300, fondo, anchoPantalla, altoPantalla);
    texto.dibujarRectangulo(20, 20, 500, 4, borde, anchoPantalla, altoPantalla);

    texto.dibujarTexto(l1.str(), 35, 38, 1.55f, celeste, anchoPantalla, altoPantalla);
    texto.dibujarTexto(l2.str(), 35, 75, 1.18f, blanco, anchoPantalla, altoPantalla);
    texto.dibujarTexto(l3.str(), 35, 100, 1.18f, blanco, anchoPantalla, altoPantalla);
    texto.dibujarTexto(l4.str(), 35, 125, 1.18f, blanco, anchoPantalla, altoPantalla);
    texto.dibujarTexto(l5.str(), 35, 150, 1.18f, blanco, anchoPantalla, altoPantalla);
    texto.dibujarTexto(l6.str(), 35, 175, 1.18f, blanco, anchoPantalla, altoPantalla);
    texto.dibujarTexto(l7.str(), 35, 200, 1.18f, colorControl, anchoPantalla, altoPantalla);
    texto.dibujarTexto(l8.str(), 35, 225, 1.18f, colorAviso, anchoPantalla, altoPantalla);
    texto.dibujarTexto(l9.str(), 35, 250, 1.18f, amarillo, anchoPantalla, altoPantalla);
    texto.dibujarTexto(l10.str(), 35, 275, 1.18f, colorEstado, anchoPantalla, altoPantalla);

    float x = 20.0f;
    float y = (float)altoPantalla - 195.0f;

    texto.dibujarRectangulo(x, y, 575, 170, fondo, anchoPantalla, altoPantalla);
    texto.dibujarRectangulo(x, y, 575, 4, borde, anchoPantalla, altoPantalla);

    texto.dibujarTexto("CONTROLES", x + 15, y + 18, 1.45f, celeste, anchoPantalla, altoPantalla);
    texto.dibujarTexto("F1  -> Cambiar DEMO / SERIAL", x + 15, y + 50, 1.10f, blanco, anchoPantalla, altoPantalla);
    texto.dibujarTexto("R   -> Reiniciar contador de cajas", x + 15, y + 72, 1.10f, blanco, anchoPantalla, altoPantalla);
    texto.dibujarTexto("J   -> Activar / limpiar atasco", x + 15, y + 94, 1.10f, blanco, anchoPantalla, altoPantalla);
    texto.dibujarTexto("E   -> Paro de emergencia", x + 15, y + 116, 1.10f, blanco, anchoPantalla, altoPantalla);
    texto.dibujarTexto("C   -> Limpiar alarmas", x + 15, y + 138, 1.10f, blanco, anchoPantalla, altoPantalla);
    texto.dibujarTexto("UP/DOWN -> Enviar velocidad desde OpenGL", x + 15, y + 158, 1.10f, blanco, anchoPantalla, altoPantalla);

    if (datos.aviso == "CONTROL_FISICO_PRIORITARIO") {
        texto.dibujarRectangulo(360, 335, 610, 55, glm::vec4(0.45f, 0.0f, 0.0f, 0.88f), anchoPantalla, altoPantalla);
        texto.dibujarTexto(
            "CONTROL FISICO PRIORITARIO: la maquina fisica controla los motores",
            380,
            355,
            1.20f,
            glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
            anchoPantalla,
            altoPantalla
        );
    }
}

// =====================================================
// CAJA VISUAL
// =====================================================

struct CajaVisual {
    float x;
    float z;
};

// =====================================================
// CALLBACKS
// =====================================================

void framebufferSizeCallback(GLFWwindow* window, int width, int height) {
    glViewport(0, 0, width, height);
}

bool teclaPresionadaUnaVez(GLFWwindow* window, int tecla) {
    static std::map<int, bool> estadoAnterior;

    bool estadoActual = glfwGetKey(window, tecla) == GLFW_PRESS;
    bool disparo = estadoActual && !estadoAnterior[tecla];

    estadoAnterior[tecla] = estadoActual;

    return disparo;
}

// =====================================================
// DEMO LOCAL
// =====================================================

void actualizarDemoLocal(DatosArduino& datosDemo, float deltaTime) {
    static float acumuladorCajaMs = 0.0f;
    static float acumuladorAtasco = 0.0f;

    datosDemo.conectado = true;
    datosDemo.pwmMotor = (int)(datosDemo.velocidad * 255.0f / 100.0f);
    datosDemo.tiempoCajaMs = (int)calcularTiempoCajaDemo(datosDemo.velocidad);
    datosDemo.modo = "HIBRIDO_DEMO";

    if (esEstadoEmergencia(datosDemo.estado)) {
        datosDemo.pwmMotor = 0;
        return;
    }

    if (datosDemo.atascoActivo == 1) {
        acumuladorAtasco += deltaTime;

        if (acumuladorAtasco >= 2.0f) {
            datosDemo.estado = "POSIBLE_ATASCO";
        }
        else {
            datosDemo.estado = "ATASCO_SIMULADO";
        }

        return;
    }
    else {
        acumuladorAtasco = 0.0f;
    }

    if (datosDemo.velocidad < 15) {
        datosDemo.estado = "BAJA_VELOCIDAD";
        datosDemo.pwmMotor = 0;
        acumuladorCajaMs = 0.0f;
        return;
    }

    datosDemo.estado = "FUNCIONANDO";

    if (datosDemo.tiempoCajaMs > 0) {
        acumuladorCajaMs += deltaTime * 1000.0f;

        if (acumuladorCajaMs >= datosDemo.tiempoCajaMs) {
            datosDemo.cajas++;
            acumuladorCajaMs = 0.0f;
        }
    }
}

// =====================================================
// MAIN
// =====================================================

int main() {
    ComunicacionSerial serial(PUERTO_SERIAL);

    bool modoDemo = !serial.estaAbierto();

    DatosArduino datosDemo;
    datosDemo.conectado = true;
    datosDemo.velocidad = 60;
    datosDemo.pwmMotor = 153;
    datosDemo.cajas = 0;
    datosDemo.tiempoCajaMs = 3000;
    datosDemo.botonAtasco = 0;
    datosDemo.atascoActivo = 0;
    datosDemo.potenciometro = 60;
    datosDemo.control = "POTENCIOMETRO";
    datosDemo.aviso = "POTENCIOMETRO_ACTIVO";
    datosDemo.modo = "HIBRIDO_DEMO";
    datosDemo.estado = "FUNCIONANDO";

    if (!glfwInit()) {
        std::cout << "Error inicializando GLFW." << std::endl;
        return -1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(
        ANCHO_VENTANA,
        ALTO_VENTANA,
        "Cinta Transportadora - Industria 4.0 / Metaverso",
        NULL,
        NULL
    );

    if (!window) {
        std::cout << "No se pudo crear ventana GLFW." << std::endl;
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(window);
    glfwSetFramebufferSizeCallback(window, framebufferSizeCallback);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cout << "Error cargando GLAD." << std::endl;
        glfwTerminate();
        return -1;
    }

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    RenderTexto2D renderTexto;
    renderTexto.inicializar();

    GLuint shader = crearProgramaShader();

    GLuint VAO, VBO;

    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);

    glBindVertexArray(VAO);

    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verticesCubo), verticesCubo, GL_STATIC_DRAW);

    glVertexAttribPointer(
        0,
        3,
        GL_FLOAT,
        GL_FALSE,
        6 * sizeof(float),
        (void*)0
    );
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(
        1,
        3,
        GL_FLOAT,
        GL_FALSE,
        6 * sizeof(float),
        (void*)(3 * sizeof(float))
    );
    glEnableVertexAttribArray(1);

    // =====================================================
    // CAJAS VISUALES FIJAS
    // El contador no crea cajas.
    // =====================================================

    std::vector<CajaVisual> cajasVisuales;
    cajasVisuales.push_back({ -3.5f, 0.0f });
    cajasVisuales.push_back({ 1.5f, 0.0f });

    float tiempoAnterior = (float)glfwGetTime();
    float desplazamientoBanda = 0.0f;

    int velocidadOpenGLTemporal = 60;

    while (!glfwWindowShouldClose(window)) {
        float tiempoActual = (float)glfwGetTime();
        float deltaTime = tiempoActual - tiempoAnterior;
        tiempoAnterior = tiempoActual;

        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(window, true);
        }

        DatosArduino datos = modoDemo ? datosDemo : serial.obtenerDatos();

        // =================================================
        // CONTROLES
        // =================================================

        if (teclaPresionadaUnaVez(window, GLFW_KEY_F1)) {
            modoDemo = !modoDemo;

            if (modoDemo) {
                std::cout << "MODO DEMO LOCAL ACTIVADO" << std::endl;
            }
            else {
                std::cout << "MODO SERIAL ACTIVADO" << std::endl;
            }
        }

        if (teclaPresionadaUnaVez(window, GLFW_KEY_R)) {
            if (modoDemo) {
                datosDemo.cajas = 0;
            }
            else {
                serial.enviarComando("RESET");
            }
        }

        if (teclaPresionadaUnaVez(window, GLFW_KEY_J)) {
            if (modoDemo) {
                datosDemo.atascoActivo = datosDemo.atascoActivo ? 0 : 1;
                datosDemo.estado = datosDemo.atascoActivo ? "ATASCO_SIMULADO" : "FUNCIONANDO";
            }
            else {
                if (datos.atascoActivo == 1 ||
                    esEstadoAtascoSimulado(datos.estado) ||
                    esEstadoPosibleAtasco(datos.estado)) {
                    serial.enviarComando("ATASCO=0");
                }
                else {
                    serial.enviarComando("ATASCO=1");
                }
            }
        }

        if (teclaPresionadaUnaVez(window, GLFW_KEY_E)) {
            if (modoDemo) {
                datosDemo.estado = "EMERGENCIA";
            }
            else {
                serial.enviarComando("EMERGENCIA");
            }
        }

        if (teclaPresionadaUnaVez(window, GLFW_KEY_C)) {
            if (modoDemo) {
                datosDemo.estado = "FUNCIONANDO";
                datosDemo.atascoActivo = 0;
                datosDemo.aviso = "SIN_AVISO";
            }
            else {
                serial.enviarComando("LIMPIAR_ATASCO");
                serial.enviarComando("LIMPIAR_EMERGENCIA");
            }
        }

        // A = forzar control fisico del potenciometro
        if (teclaPresionadaUnaVez(window, GLFW_KEY_A)) {
            if (modoDemo) {
                datosDemo.control = "POTENCIOMETRO";
                datosDemo.aviso = "POTENCIOMETRO_ACTIVO";
                datosDemo.velocidad = datosDemo.potenciometro;
            }
            else {
                serial.enviarComando("CONTROL=POTENCIOMETRO");
            }
        }

        // M = declarar control OpenGL
        if (teclaPresionadaUnaVez(window, GLFW_KEY_M)) {
            if (modoDemo) {
                datosDemo.control = "OPENGL";
                datosDemo.aviso = "CONTROL_OPENGL_ACTIVO";
            }
            else {
                serial.enviarComando("MODO=MANUAL");
            }
        }

        // Flecha arriba = enviar velocidad desde OpenGL
        if (teclaPresionadaUnaVez(window, GLFW_KEY_UP)) {
            velocidadOpenGLTemporal += 5;
            velocidadOpenGLTemporal = limitarInt(velocidadOpenGLTemporal, 0, 100);

            if (modoDemo) {
                datosDemo.velocidad = velocidadOpenGLTemporal;
                datosDemo.control = "OPENGL";
                datosDemo.aviso = "CONTROL_OPENGL_ACTIVO";
            }
            else {
                serial.enviarComando("VELOCIDAD=" + std::to_string(velocidadOpenGLTemporal));
            }
        }

        // Flecha abajo = enviar velocidad desde OpenGL
        if (teclaPresionadaUnaVez(window, GLFW_KEY_DOWN)) {
            velocidadOpenGLTemporal -= 5;
            velocidadOpenGLTemporal = limitarInt(velocidadOpenGLTemporal, 0, 100);

            if (modoDemo) {
                datosDemo.velocidad = velocidadOpenGLTemporal;
                datosDemo.control = "OPENGL";
                datosDemo.aviso = "CONTROL_OPENGL_ACTIVO";
            }
            else {
                serial.enviarComando("VELOCIDAD=" + std::to_string(velocidadOpenGLTemporal));
            }
        }

        if (modoDemo) {
            actualizarDemoLocal(datosDemo, deltaTime);
            datos = datosDemo;
        }
        else {
            datos = serial.obtenerDatos();
        }

        // Sincronizar velocidad temporal OpenGL con lo que realmente usa Arduino.
        // Así, al presionar UP/DOWN no salta desde un valor viejo.
        velocidadOpenGLTemporal = limitarInt(datos.velocidad, 0, 100);

        // =================================================
        // ESTADOS
        // =================================================

        bool estadoEmergencia = esEstadoEmergencia(datos.estado);
        bool estadoPosibleAtasco = esEstadoPosibleAtasco(datos.estado);
        bool estadoAtasco = esEstadoAtascoSimulado(datos.estado) || datos.atascoActivo == 1;
        bool estadoBajaVelocidad = esEstadoBajaVelocidad(datos.estado);

        bool cintaMoviendose =
            esEstadoFuncionando(datos.estado) &&
            !estadoEmergencia &&
            !estadoPosibleAtasco &&
            !estadoAtasco &&
            datos.velocidad >= 15;

        float velocidadNormalizada = limitarFloat(datos.velocidad / 100.0f, 0.0f, 1.0f);
        float velocidadVisual = velocidadNormalizada * 3.0f;

        if (cintaMoviendose) {
            desplazamientoBanda += velocidadVisual * deltaTime;
        }

        // =================================================
        // MOVIMIENTO VISUAL DE CAJAS
        // =================================================

        if (cintaMoviendose) {
            for (auto& caja : cajasVisuales) {
                caja.x += velocidadVisual * deltaTime;

                if (caja.x > 4.6f) {
                    caja.x = -4.6f;
                }
            }
        }

        // =================================================
        // TITULO DE VENTANA
        // =================================================

        std::stringstream titulo;
        titulo << "Cinta Transportadora | "
            << (modoDemo ? "[DEMO] " : "[SERIAL] ")
            << "VEL=" << datos.velocidad << "% "
            << "POT=" << datos.potenciometro << "% "
            << "PWM=" << datos.pwmMotor << " "
            << "CAJAS=" << datos.cajas << " "
            << "CONTROL=" << datos.control << " "
            << "AVISO=" << datos.aviso << " "
            << "ESTADO=" << datos.estado;

        glfwSetWindowTitle(window, titulo.str().c_str());

        // =================================================
        // RENDER 3D
        // =================================================

        glClearColor(0.06f, 0.07f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glUseProgram(shader);

        glm::mat4 projection = glm::perspective(
            glm::radians(45.0f),
            (float)ANCHO_VENTANA / (float)ALTO_VENTANA,
            0.1f,
            100.0f
        );

        glm::mat4 view = glm::lookAt(
            glm::vec3(6.5f, 4.5f, 7.0f),
            glm::vec3(0.0f, 0.5f, 0.0f),
            glm::vec3(0.0f, 1.0f, 0.0f)
        );

        glUniformMatrix4fv(
            glGetUniformLocation(shader, "view"),
            1,
            GL_FALSE,
            glm::value_ptr(view)
        );

        glUniformMatrix4fv(
            glGetUniformLocation(shader, "projection"),
            1,
            GL_FALSE,
            glm::value_ptr(projection)
        );

        glUniform3f(glGetUniformLocation(shader, "lightPos"), 3.5f, 6.0f, 4.0f);
        glUniform3f(glGetUniformLocation(shader, "viewPos"), 6.5f, 4.5f, 7.0f);

        // Piso
        dibujarCubo(shader, VAO, glm::vec3(0.0f, -0.15f, 0.0f), glm::vec3(12.0f, 0.12f, 7.0f), glm::vec3(0.18f, 0.18f, 0.20f));

        // Base cinta
        dibujarCubo(shader, VAO, glm::vec3(0.0f, 0.20f, 0.0f), glm::vec3(9.5f, 0.25f, 1.6f), glm::vec3(0.06f, 0.06f, 0.07f));

        // Banda
        glm::vec3 colorBanda = cintaMoviendose ? glm::vec3(0.10f, 0.10f, 0.11f) : glm::vec3(0.05f, 0.05f, 0.06f);
        dibujarCubo(shader, VAO, glm::vec3(0.0f, 0.42f, 0.0f), glm::vec3(9.0f, 0.12f, 1.25f), colorBanda);

        // Lineas de banda
        for (int i = 0; i < 18; i++) {
            float x = -4.3f + i * 0.55f;

            if (cintaMoviendose) {
                x += std::fmod(desplazamientoBanda, 0.55f);
                if (x > 4.4f) x -= 9.0f;
            }

            dibujarCubo(shader, VAO, glm::vec3(x, 0.51f, 0.0f), glm::vec3(0.06f, 0.04f, 1.15f), glm::vec3(0.35f, 0.35f, 0.37f));
        }

        // Rieles
        dibujarCubo(shader, VAO, glm::vec3(0.0f, 0.75f, 0.78f), glm::vec3(9.7f, 0.12f, 0.10f), glm::vec3(0.55f, 0.55f, 0.58f));
        dibujarCubo(shader, VAO, glm::vec3(0.0f, 0.75f, -0.78f), glm::vec3(9.7f, 0.12f, 0.10f), glm::vec3(0.55f, 0.55f, 0.58f));

        // Rodillos
        dibujarCubo(shader, VAO, glm::vec3(-4.55f, 0.46f, 0.0f), glm::vec3(0.35f, 0.35f, 1.45f), glm::vec3(0.45f, 0.45f, 0.48f));
        dibujarCubo(shader, VAO, glm::vec3(4.55f, 0.46f, 0.0f), glm::vec3(0.35f, 0.35f, 1.45f), glm::vec3(0.45f, 0.45f, 0.48f));

        // Patas
        for (float x : { -4.0f, -1.5f, 1.5f, 4.0f }) {
            dibujarCubo(shader, VAO, glm::vec3(x, -0.55f, 0.65f), glm::vec3(0.16f, 0.80f, 0.16f), glm::vec3(0.40f, 0.40f, 0.43f));
            dibujarCubo(shader, VAO, glm::vec3(x, -0.55f, -0.65f), glm::vec3(0.16f, 0.80f, 0.16f), glm::vec3(0.40f, 0.40f, 0.43f));
        }

        // Cajas visuales
        for (const auto& caja : cajasVisuales) {
            dibujarCubo(shader, VAO, glm::vec3(caja.x, 0.88f, caja.z), glm::vec3(0.55f, 0.55f, 0.55f), glm::vec3(0.72f, 0.43f, 0.18f));
            dibujarCubo(shader, VAO, glm::vec3(caja.x, 1.18f, caja.z), glm::vec3(0.50f, 0.04f, 0.50f), glm::vec3(0.95f, 0.75f, 0.45f));
        }

        // Panel de control 3D
        dibujarCubo(shader, VAO, glm::vec3(-3.2f, 1.35f, -2.2f), glm::vec3(2.4f, 1.45f, 0.18f), glm::vec3(0.10f, 0.14f, 0.20f));

        // Barra de velocidad
        dibujarCubo(shader, VAO, glm::vec3(-3.2f, 1.55f, -2.05f), glm::vec3(1.7f, 0.12f, 0.08f), glm::vec3(0.03f, 0.03f, 0.04f));

        float anchoBarra = std::max(0.02f, velocidadNormalizada * 1.7f);
        glm::vec3 colorBarra = glm::vec3(0.0f, 0.55f, 1.0f);

        if (datos.control == "POTENCIOMETRO") colorBarra = glm::vec3(0.0f, 1.0f, 0.25f);
        if (datos.control == "OPENGL") colorBarra = glm::vec3(0.0f, 0.55f, 1.0f);
        if (estadoBajaVelocidad) colorBarra = glm::vec3(0.80f, 0.10f, 0.10f);
        if (estadoAtasco) colorBarra = glm::vec3(1.0f, 0.75f, 0.0f);
        if (estadoPosibleAtasco || estadoEmergencia) colorBarra = glm::vec3(1.0f, 0.0f, 0.0f);

        dibujarCubo(shader, VAO, glm::vec3(-4.05f + anchoBarra / 2.0f, 1.55f, -1.95f), glm::vec3(anchoBarra, 0.10f, 0.10f), colorBarra);

        // Indicador 3D de control
        glm::vec3 colorControl3D = glm::vec3(0.35f, 0.35f, 0.35f);

        if (datos.control == "OPENGL") {
            colorControl3D = glm::vec3(0.0f, 0.55f, 1.0f);
        }
        else if (datos.control == "POTENCIOMETRO") {
            colorControl3D = glm::vec3(0.0f, 1.0f, 0.25f);
        }

        if (datos.aviso == "CONTROL_FISICO_PRIORITARIO") {
            colorControl3D = glm::vec3(1.0f, 0.0f, 0.0f);
        }

        dibujarCubo(shader, VAO, glm::vec3(-3.2f, 0.82f, -1.95f), glm::vec3(1.1f, 0.18f, 0.10f), colorControl3D);

        // Botones visuales
        glm::vec3 colorBotonAtasco = datos.atascoActivo ? glm::vec3(1.0f, 0.75f, 0.0f) : glm::vec3(0.15f, 0.15f, 0.15f);
        glm::vec3 colorBotonEmergencia = estadoEmergencia ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.35f, 0.0f, 0.0f);

        dibujarCubo(shader, VAO, glm::vec3(-3.75f, 1.05f, -1.95f), glm::vec3(0.30f, 0.20f, 0.10f), colorBotonAtasco);
        dibujarCubo(shader, VAO, glm::vec3(-2.65f, 1.05f, -1.95f), glm::vec3(0.30f, 0.20f, 0.10f), colorBotonEmergencia);

        // Semaforo
        dibujarCubo(shader, VAO, glm::vec3(3.4f, 1.2f, -2.0f), glm::vec3(0.25f, 2.0f, 0.25f), glm::vec3(0.25f, 0.25f, 0.27f));
        dibujarCubo(shader, VAO, glm::vec3(3.4f, 2.3f, -2.0f), glm::vec3(0.75f, 1.25f, 0.35f), glm::vec3(0.08f, 0.08f, 0.09f));

        glm::vec3 colorVerde = glm::vec3(0.0f, 0.25f, 0.0f);
        glm::vec3 colorAmarillo = glm::vec3(0.25f, 0.20f, 0.0f);
        glm::vec3 colorRojo = glm::vec3(0.25f, 0.0f, 0.0f);

        if (cintaMoviendose) {
            colorVerde = glm::vec3(0.0f, 1.0f, 0.15f);
        }

        if (estadoAtasco) {
            colorAmarillo = glm::vec3(1.0f, 0.85f, 0.0f);
        }

        if (estadoPosibleAtasco || estadoEmergencia || estadoBajaVelocidad) {
            colorRojo = glm::vec3(1.0f, 0.0f, 0.0f);
        }

        if (estadoPosibleAtasco) {
            colorAmarillo = glm::vec3(1.0f, 0.85f, 0.0f);
            colorRojo = glm::vec3(1.0f, 0.0f, 0.0f);
        }

        dibujarCubo(shader, VAO, glm::vec3(3.4f, 2.65f, -1.78f), glm::vec3(0.38f, 0.25f, 0.12f), colorVerde);
        dibujarCubo(shader, VAO, glm::vec3(3.4f, 2.30f, -1.78f), glm::vec3(0.38f, 0.25f, 0.12f), colorAmarillo);
        dibujarCubo(shader, VAO, glm::vec3(3.4f, 1.95f, -1.78f), glm::vec3(0.38f, 0.25f, 0.12f), colorRojo);

        // Alerta superior 3D
        if (estadoEmergencia) {
            dibujarCubo(shader, VAO, glm::vec3(0.0f, 2.35f, 0.0f), glm::vec3(4.5f, 0.22f, 0.22f), glm::vec3(1.0f, 0.0f, 0.0f));
        }
        else if (estadoPosibleAtasco) {
            dibujarCubo(shader, VAO, glm::vec3(0.0f, 2.35f, 0.0f), glm::vec3(4.5f, 0.22f, 0.22f), glm::vec3(1.0f, 0.55f, 0.0f));
        }
        else if (estadoAtasco) {
            dibujarCubo(shader, VAO, glm::vec3(0.0f, 2.35f, 0.0f), glm::vec3(3.5f, 0.18f, 0.18f), glm::vec3(1.0f, 0.85f, 0.0f));
        }
        else if (datos.aviso == "CONTROL_FISICO_PRIORITARIO") {
            dibujarCubo(shader, VAO, glm::vec3(0.0f, 2.35f, 0.0f), glm::vec3(4.8f, 0.18f, 0.18f), glm::vec3(1.0f, 0.0f, 0.0f));
        }

        // =================================================
        // HUD 2D
        // =================================================

        glDisable(GL_DEPTH_TEST);

        dibujarMonitorIndustrial(
            renderTexto,
            datos,
            modoDemo,
            ANCHO_VENTANA,
            ALTO_VENTANA
        );

        glEnable(GL_DEPTH_TEST);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    serial.cerrar();

    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
    glDeleteProgram(shader);

    glfwTerminate();

    return 0;
}