# Simulacro de defensa

Este documento reúne las preguntas que pueden hacerte en la defensa del
proyecto, agrupadas por tema, con respuestas modelo. La idea es que puedas
practicar en voz alta: lee la pregunta, responde sin mirar, y después compara
con la respuesta.

## Cómo usar este documento

1. Lee la **chuleta de números** hasta memorizarla. En una defensa, un número
   preciso vale más que una explicación larga.
2. Practica cada bloque en voz alta. Marca las que te cuesten.
3. Al final hay una sección de **preguntas trampa** y un **guion de demostración
   en vivo**.

> Regla de oro: si no sabes algo, dilo. "No lo medí, pero puedo razonarlo así…"
> es una respuesta honesta y fuerte. Inventar es lo único que te hunde.

## Chuleta de números clave

| Dato | Valor |
| --- | --- |
| Tamaño por jugada en disco | ≈ 180 B |
| Dataset de humo | 1 M de jugadas = 188 751 872 B (≈ 180 MB) |
| Escala | 10 M ≈ 1,57 GB · 100 M ≈ 18 GB |
| Sorteos del dataset de humo | 37 230 |
| Escritura ordenada (append) | 1,94 M reg/s · 192 MB/s · commit p50 11,6 ms |
| Escritura aleatoria | 226 K reg/s · 22,4 MB/s · commit p50 101 ms |
| Diferencia append vs aleatorio | ×8,6 rendimiento · ×3,4 espacio |
| Concurrencia: 1 taquilla | 1,34 M reg/s · commit p50 11,2 ms |
| Concurrencia: 16 taquillas | 943 K reg/s · commit p50 26,3 ms (×0,70) |
| Lookup tibio (1 M) | 1,16 M ops/s · p50 0,81 µs |
| Lookup frío (1 M) | 314 K ops/s · p99 4,3 µs (×3,7 · 68 fallos mayores) |
| Barrido tibio vs frío (1 M) | 8,4 M vs 1,15 M ops/s (×7,3) |
| Lookup frío (10 M) | 6 068 ops/s · 83 423 fallos mayores (×151) |
| Validación | 149/149 comprobaciones |

---

## Bloque A — Memoria virtual (el corazón del proyecto)

**¿Qué es la memoria virtual?**

Es el mecanismo por el cual el sistema operativo le da a cada proceso la ilusión
de tener un espacio de direcciones enorme, cuando en realidad los datos viven en
disco y se traen a RAM bajo demanda. El SO divide el espacio en **páginas** (4 KB
en Linux) y traduce direcciones virtuales a físicas con la **tabla de páginas**,
acelerada por la **TLB**. Está en el capítulo 5.4 del libro, pág. 492.

**¿Qué es un fallo de página?**

Cuando el proceso accede a una página que no está en memoria principal, el SO
interrumpe el proceso, trae la página desde el disco y reanuda la instrucción. Es
la **paginación por demanda**: el disco se comporta como extensión de la RAM,
con el costo de latencia que eso implica.

**¿Cuál es la diferencia entre un fallo de página mayor y uno menor?**

- **Menor**: la página ya está en el caché de páginas del SO; solo falta
  registrarla en la tabla de páginas del proceso. No hay I/O.
- **Mayor**: la página no está en RAM; hay que leerla del dispositivo. Es el
  costo caro.

En el proyecto medimos ambos con `/proc/self/stat` y la diferencia entre tibio y
frío es exactamente el costo del fallo mayor.

**¿Cómo "simula" LMDB la memoria virtual?**

No la simula: **la usa de verdad**. LMDB mapea el archivo de la base de datos al
espacio de direcciones del proceso con `mmap`. Cuando el programa lee una clave,
está leyendo memoria virtual; si la página está en RAM, es un acceso directo; si
no, el hardware dispara un fallo de página y el SO trae la página del disco. El
caché de páginas del SO **es** el caché de LMDB.

**¿Por qué el proyecto "demuestra" la memoria virtual y no solo la usa?**

Porque medimos el fenómeno de forma controlada: comparamos el mismo acceso con la
página en RAM (tibio) y con la página en disco (frío), y cuantificamos la
diferencia. En el dataset de 10 M, un lookup frío fue **151 veces más lento** y
generó 83 423 fallos de página mayores. Eso es la paginación por demanda hecha
medición.

**¿Qué pasa si el dataset cabe entero en la RAM?**

Que no hay paginación observable: todos los accesos son "tibios" y la diferencia
frío/tibio desaparece. Por eso el proyecto escala el dataset (1 M → 10 M → 100 M)
y por eso el experimento definitivo necesita un dataset mayor que la RAM. En una
máquina de 32 GB, un archivo de 18 GB cabe y no sirve para demostrar frío; haría
falta una máquina con menos RAM.

---

## Bloque B — LMDB

**¿Qué es LMDB?**

Lightning Memory-Mapped Database: una base de datos **clave-valor** (un
"diccionario" gigante en disco) implementada como un **árbol B+** dentro de un
archivo, que el proceso mapea a memoria con `mmap`. Es una biblioteca, no un
servidor: no hay un proceso escuchando en un puerto.

**¿Por qué LMDB y no SQLite, Redis o PostgreSQL?**

- **Redis** guarda todo en RAM: no sirve para demostrar paginación a disco.
- **PostgreSQL** es cliente-servidor con su propio gestor de caché: el caché no
  es el del sistema operativo, así que no se ve la memoria virtual "desnuda".
- **SQLite** es una alternativa válida, pero LMDB deja el `mmap` y el caché de
  páginas del SO al descubierto, que es justo lo que el proyecto quiere medir.

**¿LMDB tiene caché propio?**

No. Ese es el punto central del proyecto. Otros motores administran su propio
caché en memoria; LMDB delega en el **caché de páginas del sistema operativo** a
través del `mmap`. Ventaja: no duplica memoria y el SO decide qué páginas
mantener. Consecuencia: el rendimiento depende del tamaño de la RAM y de la
localidad de los accesos.

**¿Qué es MVCC y qué ventaja da?**

Control de concurrencia multiversión: los lectores ven una foto consistente de la
base sin bloquear al escritor, y el escritor no bloquea a los lectores. LMDB lo
logra porque el B+tree es **copia-en-escritura**: modificar no sobrescribe
páginas, crea páginas nuevas. La ventaja es lecturas sin bloqueos.

**¿Cuántos escritores admite LMDB a la vez?**

**Uno solo.** Es una restricción de diseño que garantiza consistencia. En el
benchmark concurrente lo modelamos con un `std::mutex`, y por eso el rendimiento
cae al añadir taquillas.

**¿Qué es `MDB_APPEND` y por qué importa?**

Es una operación de inserción optimizada para cuando las claves llegan en orden
**estrictamente ascendente**: coloca el registro en el borde derecho del B+tree
sin buscar su posición. Como el generador escribe las jugadas en orden
cronológico (claves ascendentes), aprovecha `MDB_APPEND` y evita rebalanceos y
divisiones de páginas. Eso explica el ×8,6 frente a la escritura aleatoria.

---

## Bloque C — Diseño del proyecto

**¿Por qué eligieron el dominio de apuestas y no el de datos sísmicos que sugería
el enunciado?**

Porque el objetivo del proyecto es agnóstico al dominio: lo que importa es que
los datos no quepan en RAM. Las apuestas tienen un perfil ideal: **millones de
registros pequeños** con consultas puntuales y por rango, que ejercitan
exactamente lo que LMDB hace bien (búsquedas por clave, cursores, barridos). Los
datos sísmicos son pocos archivos enormes con acceso casi secuencial, que
ejercitan menos el B+tree. El profesor aprobó el cambio de dominio.

**¿Por qué las claves van en big-endian?**

Porque LMDB compara claves con `memcmp`, byte a byte, como texto. Si los enteros
se guardaran en little-endian, el orden de los bytes no coincidiría con el orden
numérico y los barridos por rango de fecha devolverían resultados desordenados.
Con big-endian, el orden lexicográfico de los bytes **es** el orden numérico, y
un `SET_RANGE` desde una fecha recorre todas las jugadas posteriores.

**¿Por qué cinco sub-bases y no una sola?**

Porque cada consulta del negocio tiene un patrón de acceso distinto, y una base
por patrón evita recorrer toda la tabla:

| Base | Pregunta de negocio |
| --- | --- |
| `bets` | registro maestro, consultas por fecha |
| `i_animalito` | "¿cuál es el animalito más jugado?" |
| `i_terminal` | "¿qué terminal tiene más actividad?" |
| `i_taquilla` | "replay completo de la taquilla 7" |
| `draws` | resultados de los sorteos |

Es el principio de los **índices secundarios**: pagar espacio y tiempo de
escritura para ganar velocidad de lectura.

**¿Cómo garantizan que el dataset es reproducible?**

El generador usa un PRNG determinista (`SplitMix64`) sembrado con `--seed`. Con
los mismos parámetros y la misma semilla, el archivo sale **byte a byte
idéntico**. Lo verificamos regenerando tras refactorizar el código: mismo tamaño,
188 751 872 B. Además hay una auto-prueba que camina el B+tree y confirma que las
marcas de tiempo no descienden dentro de cada lotería.

**¿Cómo validan que las consultas son correctas?**

El generador escribe un **manifiesto** JSON con la verdad absoluta del dataset
(conteos por lotería, tipo, animalito, terminal). El comando `validate` recalcula
todos los agregados leyendo la base y los compara contra el manifiesto:
**149/149 comprobaciones aprobadas**.

---

## Bloque D — Resultados y metodología

**¿Cómo midieron los fallos de página?**

Leyendo `/proc/self/stat` antes y después de cada corrida y restando los
contadores: `minflt` (fallos menores) y `majflt` (fallos mayores). No usamos
`perf stat` porque requiere permisos especiales en muchos sistemas; `/proc` es
portable y no necesita privilegios.

**¿Cómo fuerzan la caché fría sin ser root?**

Con `madvise(MADV_DONTNEED)` sobre la región `mmap` del archivo LMDB, que
localizamos leyendo `/proc/self/maps`. La alternativa clásica
(`echo 3 > /proc/sys/vm/drop_caches`) exige root. Descubrimos además que
`posix_fadvise(DONTNEED)` **no basta**: no invalida las páginas que ya están
mapeadas; hay que aplicar `madvise` sobre la región mmap.

**¿Por qué el barrido secuencial casi no sufre en frío?**

Porque el kernel detecta el acceso secuencial y activa la **relectura
anticipada** (*readahead*): trae páginas por delante de que se necesiten. Por eso
el barrido frío solo generó 11 fallos mayores y cayó apenas ×1,8, mientras los
lookups aleatorios (que no pueden anticiparse) cayeron ×151.

**¿Qué demuestra el experimento con los hints de `madvise`?**

Que una "optimización" puede ser contraproducente. `MADV_RANDOM` antes de los
lookups empeoró el rendimiento frío ×6,3 (de 314 K a 50 K ops/s) porque desactiva
la relectura anticipada del kernel, que estaba ayudando. `MADV_SEQUENTIAL` apenas
mejoró +12% porque el kernel ya detecta el acceso secuencial solo. La lección: no
interferir con las heurísticas del kernel es en sí mismo una optimización.

**¿Por qué la escritura ordenada es 8,6 veces más rápida?**

Con claves ascendentes cada inserción cae en el borde derecho del B+tree y una
misma página se llena de corrido. Con claves aleatorias cada inserción abre o
divide páginas dispersas: el archivo final queda ×3,4 más grande (398 MB vs
118 MB) y el rendimiento cae ×8,6. El orden de las claves no es cosmético.

---

## Bloque E — Concurrencia e hilos

**¿Por qué usaron hilos y cómo se relacionan con las taquillas?**

Cada hilo del benchmark `write-concurrent` simula una taquilla del negocio real.
El libro lo respalda en la sección 7.5, pág. 645: varios hilos comparten las
unidades funcionales del procesador, cada hilo tiene su propio estado (banco de
registros y PC) y **la memoria puede compartirse gracias a la memoria virtual**.
Las taquillas tienen su propia "caja" (registros, pila) pero comparten el mismo
almacén (el `mmap` de LMDB).

**¿Qué comparten exactamente las taquillas-hilo?**

El espacio de direcciones del proceso y, por lo tanto, el `mmap` de LMDB y el
caché de páginas del sistema operativo. Una página que trae una taquilla la
aprovechan las demás: es localidad compartida. No hay 16 copias de la base: hay
una sola que todos miran.

**¿Por qué necesitan un lock?**

Porque comparten datos y hay riesgo de **carrera de datos** (sección 2.11, pág.
137). La sección 7.3, pág. 638, lo dice: "en cada instante un solo procesador
puede hacerse con el bloqueo, y el resto […] deben esperar". Ese bloqueo es el
`std::mutex` del benchmark, y refleja la regla de **un solo escritor** de LMDB.

**¿Por qué el rendimiento cae al añadir taquillas?**

Porque el recurso crítico no se multiplica: sigue habiendo un solo escritor. Los
hilos pasan tiempo esperando el lock. Con 16 taquillas el throughput cae a ×0,70 y
el commit p50 se duplica (11,2 → 26,3 ms). Es la **ley de Amdahl** (sección 1.8):
la parte serializable pone el límite, por más hilos que añadas.

**¿Cuál sería la solución para escalar a 10 000 taquillas?**

No comprar más hilos: cambiar la arquitectura de datos. Particionar en varios
entornos LMDB (por rango de tiempo o por taquilla) para que cada partición tenga
su propio escritor, agrupar escrituras de varias taquillas en lotes grandes antes
de persistir, y separar el proceso de lectura del de escritura.

**¿Por qué `-lpthread` solo en `bench`?**

Porque `std::thread` y `std::mutex` son envoltorios de hilos POSIX. `generator` y
`query` no usan hilos, así que no lo necesitan. Matiz: en glibc 2.34+ pthreads
está dentro de `libc`, así que el flag es no-op en sistemas modernos, pero se
mantiene por portabilidad.

---

## Bloque F — Preguntas trampa

**"LMDB es una base de datos en memoria, ¿verdad?"**
No. LMDB **mapea** el archivo a memoria, pero los datos viven en disco. Lo que
está en RAM son las páginas que el sistema operativo decide mantener en el caché.
Si la RAM se llena, las páginas se desalojan y hay que traerlas del disco.

**"Entonces el caché de LMDB es muy bueno, ¿no?"**
LMDB no tiene caché propio: usa el del sistema operativo. Por eso el rendimiento
depende del tamaño de la RAM y de la localidad de los accesos, no de un
algoritmo de caché interno.

**"Si ordenar las claves es tan bueno, ¿por qué no lo hace la base de datos
sola?"**
Porque el orden lo decide quien inserta. LMDB no puede reordenar tus claves: solo
aprovecha que lleguen ordenadas. La decisión de diseño (big-endian + flujo
cronológico) es del proyecto, no de LMDB.

**"¿Por qué no usaron `drop_caches` para la caché fría?"**
Porque requiere root. `madvise(MADV_DONTNEED)` sobre la región mmap logra el mismo
efecto sin privilegios, lo que hace el experimento reproducible en cualquier
máquina de estudiante.

**"El ×151 en frío, ¿no será un artefacto de la medición?"**
No: lo respalda el conteo de fallos de página. En modo frío se registraron 83 423
fallos **mayores** (I/O real al dispositivo), frente a 0 en modo tibio. El tiempo
extra corresponde exactamente a esos accesos a disco.

**"¿Qué pasa con el `-lmdb` del Makefile?"**
El flag correcto es `-llmdb`, porque `-l` antepone "lib" y agrega ".so": `-llmdb`
busca `liblmdb.so` y `-lmdb` buscaría `libmdb.so`, que no existe. Al principio el
Makefile tenía esa confusión; la corregimos al verificarlo.

**"¿Por qué el proyecto tiene 5 sub-bases si solo usa 1 en los benchmarks?"**
Porque el diseño del modelo de datos cubre las consultas del negocio (frecuencia,
rachas, replay por taquilla), y los benchmarks de lectura usan `bets` e
`i_animalito` para medir dos patrones distintos: búsqueda puntual y barrido de
índice.

---

## Bloque G — Demostración en vivo

Si te piden ejecutar algo, este es el guion más seguro (y rápido):

```sh
# 1. Compilar (debe salir limpio, sin advertencias)
make clean && make all

# 2. Generar un dataset pequeño y ver la validación
make smoke
./bin/query --db data/smoke.lmdb validate      # debe decir 149/149

# 3. Mostrar el orden de claves y el tamaño
./bin/query --db data/smoke.lmdb info

# 4. Demostrar tibio vs frío (el resultado estrella)
./bin/bench read-lookup --db data/smoke.lmdb --n 200000
./bin/bench read-lookup --db data/smoke.lmdb --n 200000 --cold
```

Si te preguntan por qué el frío del dataset de 1 M no es tan dramático, responde:
"porque 180 MB caben en RAM; el efecto se dispara en el dataset de 10 M, donde
medimos ×151 y 83 423 fallos mayores".

**Consejo:** si la defensa es en tu máquina, ten el dataset de humo ya generado
para no perder tiempo. Si es en otra, ten el repositorio clonado y las
dependencias instaladas (`lmdb`, `g++`, `make`).

---

## Bloque H — Si no sabes algo

Frases honestas y fuertes:

- "No medí ese caso, pero puedo razonarlo desde la teoría: …"
- "Ese detalle no lo controlo de memoria; está documentado en `docs/` y te lo
  confirmo."
- "No lo optimizamos porque el objetivo era medir, no acelerar. Pero la medición
  sugiere que la vía sería …"

Lo que **no** debes hacer: inventar un número, inventar una función del código, o
afirmar algo sobre el libro sin la sección a mano. Es mejor decir "no lo sé" que
quedar atrapado en una invención.

## Referencias del libro

| Sección | Página | Tema |
| --- | --- | --- |
| §1.8 | — | Ley de Amdahl |
| §2.11 | 137 | Sincronización, carrera de datos, `lock`/`unlock` |
| §5.2–5.3 | — | Caches y localidad |
| §5.4 | 492 | Memoria virtual (capítulo central) |
| §5.6 | 525 | Máquinas virtuales ≠ memoria virtual |
| §6.3 | 575 | Discos magnéticos |
| §6.4 | 580 | Memorias Flash |
| §6.7 | 596 | Medición de prestaciones de E/S |
| §7.2 | 634 | Dificultad de la programación paralela |
| §7.3 | 638 | Multiprocesadores de memoria compartida, bloqueos |
| §7.5 | 645 | Ejecución multihilo en hardware |
