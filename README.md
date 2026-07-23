\# Cinta Transportadora - Industria 4.0



\## Descripción



Proyecto desarrollado utilizando OpenGL, Arduino y Proteus para simular una línea de producción automatizada.



El sistema integra una simulación electrónica con una representación gráfica tridimensional en tiempo real mediante comunicación serial, permitiendo visualizar el comportamiento de una cinta transportadora industrial.



\---



\## Características



\- Simulación 3D en OpenGL.

\- Comunicación serial entre Arduino y OpenGL.

\- Simulación electrónica en Proteus.

\- Control mediante potenciómetro.

\- Conteo automático de cajas.

\- Monitor de producción en tiempo real.

\- Semáforo de estados.

\- Paro de emergencia.

\- Simulación de atascos.



\---



\## Tecnologías utilizadas



\- C++

\- OpenGL

\- GLFW

\- GLAD

\- Arduino UNO

\- Proteus 8 Professional



\---



\## Arquitectura



Arduino controla la lógica del sistema.



↓



Proteus simula el circuito electrónico.



↓



Comunicación Serial



↓



OpenGL representa la cinta transportadora en tiempo real.



\---



\## Capturas



\### Simulación en Proteus



!\[Proteus](docs/images/proteus.png)



\---



\### Integración Proteus + OpenGL



!\[OpenGL](docs/images/opengl.png)



\---



\### Monitor de Producción



!\[Monitor](docs/images/monitor.png)



\---



\## Estructura del proyecto



```

Cinta-Transportadora-Industria-4.0

│

├── Arduino/

├── OpenGL/

├── Proteus/

├── docs/

└── README.md

```



\---



\## Funcionamiento



1\. El operador modifica la velocidad mediante el potenciómetro.

2\. Arduino procesa la información.

3\. Proteus simula el circuito electrónico.

4\. Los datos se envían mediante comunicación serial.

5\. OpenGL actualiza la velocidad de la cinta.

6\. Se realiza el conteo automático de cajas.

7\. El monitor muestra el estado del sistema.



\---



\## Funcionalidades



\- Control manual.

\- Conteo de cajas.

\- Semáforo industrial.

\- Emergencia.

\- Atasco.

\- Comunicación serial.

\- Monitor de producción.



\---



\## Autor



Oscar Fierro



Escuela Politécnica Nacional

Ingeniería en Software

