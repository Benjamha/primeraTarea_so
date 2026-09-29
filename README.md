# El Planificador Dieciochero

Tarea 1 — Sistemas Operativos  
Procesos, Tuberías y Señales

## Compilación

Para compilar el programa:

```bash
make
```

También se puede compilar directamente con:

```bash
g++ -Wall -Wextra -std=c++17 -o planificador planificador.cpp -lpthread
```

El programa no utiliza threads.

## Uso

```bash
./planificador plan.txt K
```

Donde `K` corresponde al número máximo de procesos que pueden ejecutarse al mismo tiempo.

## Formato de `plan.txt`

Cada actividad se escribe con el siguiente formato:

```text
ID : Nombre : tiempo_ms : dependencias
```

Por ejemplo:

```text
1 : prender_carbon : 500 :
2 : comprar_carne : 1200 :
4 : asar_longaniza : 800 : 1, 2
```

Las dependencias deben terminar antes de que pueda comenzar la actividad.

Si una actividad no tiene dependencias, el último campo queda vacío.

Si no se indica un tiempo, se genera uno aleatorio entre 100 y 5000 ms.

## Funcionamiento

El programa lee las actividades del archivo y construye sus relaciones de dependencia.

Las actividades que no tienen dependencias pueden comenzar directamente. Cuando una actividad termina correctamente, se actualizan las actividades que dependen de ella. Cuando una actividad tiene todas sus dependencias terminadas, queda disponible para ejecutarse.

Cada actividad se ejecuta como un proceso creado mediante `fork()`. El valor de `K` limita la cantidad de procesos que pueden estar ejecutándose simultáneamente.

## Comunicación mediante pipes

Se utilizan pipes para comunicar el proceso principal con los procesos de las actividades.

Cuando una actividad termina, se envía un mensaje de texto que puede ser recibido por las actividades que dependen de ella.

## Aislamiento de errores

Si una actividad falla, se marca como fallida y las actividades que dependen de ella se abortan.

Las demás ramas del plan pueden continuar ejecutándose de forma independiente.

Para probar fallos se puede incluir `_FALLA` en el nombre de una actividad. También existe una pequeña probabilidad de fallo aleatorio.

## Ctrl+C

Al presionar `Ctrl+C`, el programa termina las actividades que se encuentran ejecutándose y marca como abortadas las actividades que todavía no habían comenzado.

## Prueba de carga

El programa puede utilizarse con planes de gran tamaño, incluyendo planes de hasta 10000 actividades.

Por ejemplo:

```bash
./planificador plan_stress.txt 200
```

## Archivos del proyecto

- `planificador.cpp`: código fuente del planificador.
- `Makefile`: archivo para compilar el programa.
- `plan.txt`: plan de ejemplo.
- `plan_stress.txt`: plan para pruebas de carga.
- `README.md`: documentación del proyecto.
