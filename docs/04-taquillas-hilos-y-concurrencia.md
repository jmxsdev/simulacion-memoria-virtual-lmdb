# Taquillas, hilos y concurrencia

Este documento responde a una pregunta concreta: **¿cómo se justifica el uso de
hilos en el benchmark `write-concurrent`, y cómo se relacionan las taquillas del
negocio con esos hilos?** Todo está respaldado por el libro de Patterson y
Hennessy, con las secciones y páginas exactas.

## 1. El problema: muchas taquillas, un solo programa

En el negocio real hay miles de taquillas (puntos de venta) registrando jugadas
al mismo tiempo. Para estudiar ese escenario en un solo programa hay que
representar esas fuentes de trabajo simultáneas. La herramienta natural para eso
es el **hilo** (thread): cada taquilla del benchmark es un hilo del proceso.

El benchmark `write-concurrent` de `src/bench.cpp` hace exactamente eso: con
`--taquillas 16` lanza 16 hilos, y cada hilo genera y escribe las jugadas de
"su" taquilla.

## 2. Qué es un hilo a nivel de arquitectura

El libro lo define en la sección **7.5, "Ejecución multihilo en hardware",
pág. 645**:

> La ejecución multihilo en hardware permite que varios hilos (*threads*)
> compartan las unidades funcionales del procesador y se ejecuten
> simultáneamente. Para facilitar la compartición de los recursos del
> procesador, cada hilo debe tener su propio estado; por ejemplo, cada hilo
> debería tener una copia del banco de registros y el PC. **La memoria puede
> compartirse gracias a la memoria virtual**, que ya incluye soporte para
> multiprogramación.

De esa definición salen las dos ideas que necesitas para la defensa:

| Recurso | ¿Propio de cada hilo o compartido? |
| --- | --- |
| Banco de registros | Propio |
| Contador de programa (PC) | Propio |
| Pila (stack) | Propio |
| Espacio de direcciones | Compartido |
| El `mmap` de LMDB | Compartido |
| El caché de páginas del sistema | Compartido |

**La analogía del negocio:** cada taquilla (hilo) tiene su propia caja
registradora y su propio empleado (registros, PC, pila), pero todas comparten el
mismo almacén (el espacio de direcciones del proceso, donde vive el `mmap` de
LMDB). No hay 16 copias de la base de datos: hay **una sola**, y 16 hilos
mirándola al mismo tiempo.

## 3. Por qué hace falta sincronizar: el lock

Como los hilos comparten el mismo almacén, hay que coordinar quién escribe. El
libro lo explica dos veces:

**Sección 2.11, "Paralelismo e instrucciones: sincronización", pág. 137:**

> La programación paralela es más fácil cuando las tareas son independientes,
> pero a menudo es necesaria una cooperación entre ellas. […] Si no hay esta
> sincronización existe el peligro de una **carrera de datos** (*data race*), en
> la que los resultados de un programa pueden variar dependiendo del orden en el
> que ocurran ciertos sucesos.
>
> […] nos centraremos en la implementación de las operaciones de sincronización
> **lock** (bloquear) y **unlock** (desbloquear). Estas operaciones pueden
> utilizarse de forma directa para crear regiones a las que sólo puede acceder un
> procesador, llamadas **regiones de exclusión mutua**.

**Sección 7.3, "Multiprocesadores de memoria compartida", pág. 638:**

> Como normalmente los procesadores que operan en paralelo comparten datos, es
> necesario introducir una coordinación en la operación sobre los datos
> compartidos; de lo contrario, un procesador podría comenzar a operar sobre un
> dato antes de que otro procesador terminase de modificar ese mismo dato. Esta
> coordinación se llama **sincronización**. […] Una de las alternativas es el uso
> de **bloqueos** (*lock*) para una variable compartida. **En cada instante un
> solo procesador puede hacerse con el bloqueo, y el resto de los procesadores
> interesados en acceder a la variable compartida deben esperar hasta que el
> primero desbloquea la variable.**

Ese "en cada instante un solo procesador puede hacerse con el bloqueo" es
exactamente la regla de LMDB: **un solo escritor a la vez**. Y el mecanismo que
la implementa en el benchmark es el `std::mutex`:

```cpp
std::mutex mtx;                        // el lock compartido (bench.cpp)

// Dentro del hilo de cada taquilla:
std::lock_guard<std::mutex> lock(mtx); // adquirir el lock
// ... escribir el lote en LMDB ...
                                       // el lock se libera al salir del bloque
```

`std::lock_guard` es la versión C++ de "adquirir al entrar, liberar al salir":
garantiza que el lock se suelte aunque ocurra un error, evitando que una taquilla
se quede con la llave para siempre y bloquee a las demás.

## 4. Las lecturas y el caché: lo que comparten las taquillas

Aquí está la conexión más importante con la memoria virtual.

LMDB **no tiene caché propio**: mapea el archivo al espacio de direcciones del
proceso con `mmap`. Como todos los hilos comparten ese espacio de direcciones
(sección 7.5: "la memoria puede compartirse gracias a la memoria virtual"), todos
los hilos **comparten el mismo `mmap` y, por lo tanto, el mismo caché de páginas
del sistema operativo**.

Consecuencias prácticas:

1. **Una página que trae una taquilla la aprovechan las demás.** Si el hilo de la
   taquilla 3 provoca un fallo de página y el kernel trae una página del B+tree a
   RAM, esa página queda residente para todos los hilos. Es la **localidad
   compartida**: el trabajo de traer datos del disco se amortiza entre taquillas.
2. **Pero las lecturas y las escrituras compiten por el mismo caché.** El escritor
   modifica páginas (las "ensucia"), y esas páginas ocupan espacio que otro hilo
   podría estar usando. Con un dataset que no cabe en RAM, los hilos se desalojan
   páginas entre sí.
3. **El caché compartido es también el punto de contención.** Todos los hilos
   pasan por las mismas páginas del B+tree para llegar a sus datos; no hay un
   caché por taquilla que aísle a unas de otras.

Esto conecta directamente con la **sección 5.4, "Memoria virtual", pág. 492**
(cubierta en [02-que-es-lmdb-y-memoria-virtual.md](02-que-es-lmdb-y-memoria-virtual.md)):
el `mmap` es el mecanismo que hace que "compartir memoria" entre hilos sea
literalmente compartir páginas del mismo archivo.

## 5. Por qué el rendimiento cae: la ley de Amdahl

Con 16 taquillas el throughput **baja** a ×0,70 respecto a una sola, y el commit
p50 se duplica. ¿Por qué añadir hilos no acelera el sistema? Porque el recurso
crítico no se multiplica: **sigue habiendo un solo escritor**. Los hilos pasan
tiempo esperando el lock, y ese tiempo de espera es trabajo perdido.

Esto es un caso de la **ley de Amdahl** (sección 1.8 del libro, "Falacias y
errores habituales"): la mejora de un aspecto del sistema no se traduce en una
mejora proporcional del total si una parte del trabajo es intrínsecamente
secuencial. Si la fracción serializable es alta, el límite de aceleración es
bajo por más hilos que añadas.

La sección **7.2, "La dificultad de crear programas de procesamiento paralelo",
pág. 634**, desarrolla esta misma idea: no basta con repartir el trabajo entre
más unidades; hay que lidiar con la sincronización, la sobrecarga de las
comunicaciones y las partes que no se pueden paralelizar.

La conclusión para el negocio (y para la defensa) es directa: **para escalar a
10 000 taquillas no se compra más hilos, se cambia la arquitectura de datos**.
De ahí salen las estrategias de particionado (varios entornos LMDB por rango de
tiempo o por taquilla) y de agrupación de escrituras en lotes.

## 6. Sobre `-lpthread`: qué enlaza y por qué

`std::thread` y `std::mutex` de C++ son envoltorios sobre la API de **hilos
POSIX** (*pthreads*). En Linux, esa API se enlaza con `-lpthread`:

```make
$(BENCH): $(BENCH_SRCS) $(COMMON_HDRS) | $(BIN)
	$(CXX) $(CXXFLAGS) $(BENCH_SRCS) -o $@ $(LDLIBS) -lpthread
```

`generator` y `query` no usan hilos, por eso no llevan el flag; `bench` sí,
porque su modo `write-concurrent` los usa. Cada binario enlaza solo lo que
necesita.

**Matiz técnico importante para la defensa:** en versiones modernas de glibc
(2.34 y posteriores), la implementación de pthreads se integró dentro de `libc`,
así que `bin/bench` no aparece enlazado a una `libpthread.so` separada (puedes
comprobarlo con `ldd bin/bench`). El flag `-lpthread` se mantiene por
**compatibilidad y portabilidad**: en sistemas con glibc más antigua, o en otras
bibliotecas C, sí se necesita. Dejarlo es la práctica recomendada.

## 7. Guion para la defensa

Si te preguntan "¿por qué usas hilos y cómo se relaciona con las taquillas?",
puedes responder en cuatro pasos:

1. **Qué representa cada hilo.** "Cada hilo simula una taquilla del negocio real.
   Un hilo tiene su propio estado (registros, PC, pila) pero comparte el espacio
   de direcciones del proceso, tal como lo describe la sección 7.5 del libro."
2. **Qué comparten.** "Como todos los hilos comparten el `mmap` de LMDB, todos
   comparten el caché de páginas del sistema operativo. Una página que trae un
   hilo la aprovechan los demás: eso es localidad compartida."
3. **Por qué se sincronizan.** "LMDB admite un solo escritor a la vez. En el
   benchmark lo implemento con un `std::mutex`, que es el *lock* de la sección
   7.3 del libro: en cada instante solo una taquilla escribe y el resto espera."
4. **Por qué no acelera.** "Añadir taquillas no multiplica el escritor, así que
   el throughput baja a ×0,70 con 16 hilos. Es la ley de Amdahl: la parte
   serializable pone el límite. La solución real es particionar los datos, no
   añadir hilos."

## 8. Referencias del libro

| Sección | Página | Qué aporta |
| --- | --- | --- |
| §2.11 Paralelismo e instrucciones: sincronización | 137 | Carrera de datos, `lock`/`unlock`, exclusión mutua |
| §5.4 Memoria virtual | 492 | Páginas, fallos de página, base del `mmap` compartido |
| §7.2 La dificultad de crear programas paralelos | 634 | Sincronización, sobrecarga, límites del paralelismo |
| §7.3 Multiprocesadores de memoria compartida | 638 | Bloqueos: un solo procesador a la vez |
| §7.5 Ejecución multihilo en hardware | 645 | Qué es un hilo, estado propio y memoria compartida |
| §1.8 Falacias y errores habituales | — | Ley de Amdahl |

> Nota: las citas textuales de este documento fueron tomadas del PDF del libro
> que acompaña al curso, y las páginas corresponden a la 4.ª edición en español
> (Reverté).
