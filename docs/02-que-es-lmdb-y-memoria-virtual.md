# Qué es LMDB y qué es la memoria virtual

Este documento explica los conceptos que dan sentido al proyecto: qué es una
base de datos clave-valor, qué hace especial a LMDB, cómo el sistema operativo
convierte un archivo en memoria accesible, y cómo se conecta todo con el
capítulo 5.4 del libro de Patterson y Hennessy. Los detalles concretos de este
proyecto están en [03-lmdb-en-este-proyecto.md](03-lmdb-en-este-proyecto.md).

## 1. Base de datos clave-valor

Una base de datos **clave-valor** es, en esencia, un diccionario gigante que
vive en disco. Guardas un par `clave → valor` y después lo recuperas buscando
la clave. No hay tablas, ni columnas, ni SQL: solo pares de bytes.

Piensa en un archivador: la clave es la etiqueta del cajón y el valor es lo que
hay dentro. El sistema de archivos no entiende de "jugadas" ni de "animalitos":
solo ve claves y valores como secuencias de bytes. Toda la semántica del dominio
(aquí, apuestas de lotería) la pone el programa: la capa `keys.h` del proyecto.

Las operaciones típicas son: **put** (insertar o reemplazar), **get** (buscar
una clave exacta), **del** (borrar) y **cursor/range** (recorrer claves en
orden a partir de un punto).

## 2. Qué es LMDB

**LMDB** son las siglas de *Lightning Memory-Mapped Database*. Es una
biblioteca de base de datos clave-valor escrita en C, diminuta y muy rápida, que
organiza los datos como un **árbol B+** dentro de un archivo.

Lo que la hace distinta de una base de datos tradicional:

- **Un archivo, mapeado a memoria.** Todo el contenido vive en un archivo, y el
  proceso lo accede a través de `mmap`. No hay un servidor separado escuchando
  en un puerto: LMDB es una biblioteca que se enlaza en tu programa (en este
  proyecto, `/usr/lib/liblmdb.so`).
- **No tiene su propio caché.** Este es el punto central.
- **Árbol B+ copia-en-escritura**: las páginas no se modifican en su sitio.
- **MVCC**: los lectores no bloquean al escritor ni viceversa.
- **Un solo escritor a la vez**.
- **Transacciones ACID**.

## 3. La técnica de caché de LMDB: el caché del sistema operativo

Esta es la idea que hay que saber explicar en la defensa.

LMDB **no implementa un caché de páginas propio**. En lugar de reservar memoria
y copiar páginas ahí, LMDB pide al kernel que **mapee el archivo** en el espacio
de direcciones virtuales del proceso con `mmap`. A partir de ese momento, leer
una página del B+tree es, literalmente, leer de una dirección de memoria.

Ese truco delega el caché al **caché de páginas del sistema operativo**, que ya
existe y es compartido por todos los procesos. Las consecuencias:

- Si la página está **residente en RAM** (en la caché de páginas), el acceso es
  un acierto a memoria: del orden de submicrosegundos. No hay copia ni syscall.
- Si la página **no está residente**, el acceso provoca un **fallo de página**:
  el kernel interrumpe el programa, va al dispositivo de almacenamiento, trae la
  página y reanuda la ejecución. Ese acceso paga la latencia del dispositivo
  (microsegundos a milisegundos según el medio).

Por eso el rendimiento de LMDB es, en gran medida, el rendimiento de la memoria
virtual del sistema operativo. El proyecto mide justamente esa frontera: la
diferencia entre una corrida "tibia" (páginas en RAM) y una "fría" (páginas
expulsadas y medidas con `/proc/self/stat`) es el costo real de la paginación
por demanda.

## 4. `mmap`, tablas de páginas, TLB y fallos de página

### 4.1 Qué hace `mmap`

`mmap` (memory map) es la llamada al sistema que pide al kernel: "asocia este
rango de mi espacio de direcciones virtuales con este archivo". A partir de
ahí, el programa usa direcciones virtuales normales y el hardware se encarga de
traducirlas.

El kernel no trae el archivo a memoria de golpe: monta la asociación y **carga
las páginas cuando se tocan** (paginación por demanda). Esa carga diferida es
lo que permite abrir un archivo de 180 MB o de 1,57 GB sin leerlo entero.

### 4.2 Tabla de páginas

La **tabla de páginas** es la estructura del kernel que guarda la traducción
`dirección virtual → dirección física`, página por página, con los permisos y
los bits de presencia. La MMU (unidad de gestión de memoria del procesador) la
consulta en cada acceso.

Cuando pides abrir un archivo con `mmap`, el kernel crea entradas de tabla de
páginas para ese rango, pero **sin marcar las páginas como presentes**. El
primer acceso a una página ausente es un fallo de página.

### 4.3 TLB

La **TLB** (*Translation Lookaside Buffer*) es una caché pequeña y rapidísima
dentro de la CPU que guarda las traducciones de página usadas recientemente.
Sin TLB, cada acceso a memoria exigiría recorrer la tabla de páginas (varios
accesos más). Con TLB, la traducción casi siempre sale en uno o dos ciclos.

La TLB tiene pocas entradas (decenas). Cuando el programa salta por muchas
páginas distintas, las traducciones se expulsan y hay **fallos de TLB**: aunque
la página esté en RAM, hay que volver a la tabla de páginas. Por eso un acceso
secuencial (pocas páginas, buena localidad) es más barato que uno aleatorio.

### 4.4 Fallos de página mayores y menores

Un **fallo de página** ocurre cuando el acceso pide una página que no está
marcada como presente. Hay dos tipos, y la diferencia importa muchísimo:

- **Fallo menor** (*minor fault*): la página ya está en memoria (en la caché de
  páginas) y solo hay que instalarla en la tabla de páginas del proceso. No hay
  E/S. Es barato.
- **Fallo mayor** (*major fault*): la página no está en memoria y hay que
  leerla del dispositivo (disco, SSD, etc.). Es caro: paga la latencia del
  almacenamiento.

La distinción es exactamente lo que el proyecto mide. El benchmark lee los
contadores `minflt` y `majflt` de `/proc/self/stat` antes y después de cada
corrida; la diferencia son los fallos de ese tipo ocurridos durante la medición.
Un barrido secuencial frío provoca mayoritariamente **fallos mayores agrupados**
(una página trae muchas jugadas contiguas); un acceso aleatorio provoca **un
fallo por casi cada consulta** y arruina el rendimiento.

### 4.5 Tamaño de página

El kernel divide la memoria (y el archivo mapeado) en **páginas**, típicamente
de **4 KB** en Linux. Cada fallo de página trae 4 KB, no un byte. Esto tiene una
consecuencia de localidad:

- Leer **secuencialmente** es barato: un fallo trae 4 KB y las siguientes
  jugadas salen de la misma página, sin fallo.
- Leer **aleatoriamente** es caro: dos consultas rara vez caen en la misma
  página; cada una puede disparar su propio fallo. Y un lookup en un B+tree
  toca varias páginas (raíz, rama, hoja) porque desciende por los niveles del
  árbol.

Esta es la razón física de que, en los resultados del proyecto, el barrido
secuencial frío rinda mucho mejor que el lookup aleatorio frío.

## 5. Conceptos internos de LMDB

### 5.1 Árbol B+ copia-en-escritura (copy-on-write)

Un **árbol B+** es una estructura de búsqueda balanceada de gran abanico,
optimizada para bloques: las claves se mantienen ordenadas y las hojas se
encadenan, lo que hace que tanto "buscar una clave" como "recorrer un rango"
sean eficientes. Un árbol B+ de pocos niveles indexa millones de entradas.

"**Copia-en-escritura**" significa que cuando una transacción modifica una
página, LMDB **no la sobreescribe**: escribe una página nueva y actualiza los
punteros de las páginas padre. Las páginas viejas siguen intactas y siguen
sirviendo a los lectores que estaban usando la versión anterior. Cuando la
transacción se confirma, una raíz nueva "apunta" al árbol nuevo.

### 5.2 MVCC: lecturas sin bloqueos

**MVCC** (*Multi-Version Concurrency Control*) es la técnica por la que cada
lector ve **una instantánea consistente** de la base de datos, congelada en el
momento en que empezó su transacción. Nunca ve cambios a medias ni necesita
esperar a nadie. En LMDB esto sale gratis de la copia-en-escritura: el lector
recorre el árbol de su versión, mientras el escritor construye el nuevo.

### 5.3 Un solo escritor a la vez

LMDB permite **muchos lectores concurrentes, pero un único escritor**. Dos
transacciones de escritura no pueden estar activas al mismo tiempo: la segunda
espera. No es un defecto, es una decisión de diseño que simplifica la
durabilidad y la consistencia. En este proyecto tiene una consecuencia de
negocio muy concreta: simular muchas taquillas escribiendo en paralelo hace que
todas compitan por ese único escritor, y el rendimiento **se degrada** aunque
haya más hilos (medido: ×0,70 con 16 taquillas). Eso lleva a las estrategias de
escalado que se discuten en
[03-lmdb-en-este-proyecto.md](03-lmdb-en-este-proyecto.md).

### 5.4 Transacciones ACID

LMDB ofrece transacciones con las propiedades **ACID**:

- **Atomicidad**: una transacción se aplica entera o no se aplica.
- **Consistencia**: la base pasa de un estado válido a otro válido.
- **Aislamiento**: las transacciones no ven los cambios de otras a medias
  (gracias a MVCC).
- **Durabilidad**: al confirmar (`commit`), los datos quedan persistidos aunque
  el proceso o la máquina fallen. El costo de eso es el `fsync` en el commit, y
  por eso el proyecto agrupa escrituras en lotes: cada commit fuerza a disco, y
  amortizar commits es clave para el rendimiento.

### 5.5 MDB_APPEND

`MDB_APPEND` es una opción de inserción que le dice a LMDB: "esta clave es
**mayor que todas las existentes**, insértala directamente en el borde derecho
del árbol". Cuando eso se cumple, LMDB se ahorra la búsqueda y el rebalanceo
del árbol: es el camino rápido de escritura masiva. El proyecto lo usa en la
base `bets` porque el generador produce las claves en orden estrictamente
ascendente. A cambio, `MDB_APPEND` **exige** que las claves lleguen ordenadas;
si no, falla.

### 5.6 Claves comparadas con `memcmp` y la obligación del big-endian

LMDB no sabe si tu clave es un número, una fecha o un texto: compara los bytes
crudos con `memcmp`, es decir, en orden lexicográfico. Eso crea una trampa
clásica:

- Si un entero de 64 bits se guarda en **little-endian**, el byte menos
  significativo va primero. Para el número `1` y el número `256`, la
  comparación de bytes daría un orden que **no** coincide con el orden numérico,
  y los barridos por rango de fecha devolverían resultados incorrectos.
- Si se guarda en **big-endian**, el byte más significativo va primero, y el
  orden lexicográfico de los bytes coincide **exactamente** con el orden
  numérico.

Por eso la regla crítica del proyecto: **toda clave con enteros multi-byte va
en big-endian**. Los valores, en cambio, nunca se comparan, así que pueden ir en
little-endian. En [03-lmdb-en-este-proyecto.md](03-lmdb-en-este-proyecto.md)
hay un ejemplo byte a byte.

## 6. Conexión con el libro (Patterson y Hennessy)

El capítulo central del proyecto es el **§5.4, Memoria virtual** (página 492).
El resto le da contexto. Este es el mapa:

| Sección | Tema | Página aprox. |
| --- | --- | --- |
| §5.2–5.3 | Caches y localidad | ~470 y siguientes |
| §5.4 | Memoria virtual | 492 (central) |
| §5.6 | Aclaratoria: máquinas virtuales ≠ memoria virtual | 525 |
| §6.3 | Discos magnéticos | 575 |
| §6.4 | Memorias Flash | 580 |
| §6.7 | Medición de prestaciones de E/S | 596 |

Orden de lectura recomendado: **§5.2 → §5.3 → §5.4 → §6.3 → §6.4 → §6.7**.

Cómo enlaza con el proyecto:

- **§5.2–5.3 (cachés y localidad)**: explican por qué el barrido secuencial
  frío rinde mucho más que el lookup aleatorio frío. La localidad espacial
  determina cuántos fallos de página (y de TLB) provoca cada patrón de acceso.
- **§5.4 (memoria virtual)**: es la teoría de `mmap`, la tabla de páginas, los
  fallos de página y la traducción de direcciones. El proyecto es, en la
  práctica, un experimento sobre esta sección.
- **§5.6**: aclara un error común de vocabulario. Las **máquinas virtuales**
  (VMware, VirtualBox, hypervisores) no son lo mismo que la **memoria virtual**
  (paginación, traducción de direcciones). En la defensa conviene no
  confundirlas.
- **§6.3 y §6.4 (discos y flash)**: explican de dónde sale la latencia que se
  paga en un fallo de página mayor, y por qué el medio (HDD vs. SSD) cambia la
  magnitud.
- **§6.7 (medición de E/S)**: es la referencia para justificar *cómo* medir:
  latencias, percentiles y contadores del sistema, que es lo que hace
  `bench.cpp`.

## 7. Preguntas de defensa

**¿LMDB es una base de datos en memoria?**
No. El dato vive en un archivo en disco. Lo que es en memoria es el **mapeo**
del archivo (`mmap`) en el espacio de direcciones del proceso. El caché real es
el caché de páginas del sistema operativo.

**Si el dataset cabe en RAM, ¿LMDB lo tendrá entero en memoria?**
No necesariamente de entrada, pero a medida que se accede a las páginas, el
caché de páginas las va reteniendo. Si el dataset cabe holgadamente en RAM y se
recorre con frecuencia, la mayoría de los accesos serán aciertos y se verá
rendimiento tibio. Si no cabe (como el dataset de 10M, de 1,57 GB), habrá
expulsiones y fallos mayores.

**¿Por qué no usar Redis, que es en memoria?**
Porque el objetivo del proyecto es justamente simular **memoria secundaria** y
medir la paginación. Redis mantiene los datos en RAM y no expone el mismo
fenómeno de fallos de página. Además LMDB no requiere un servidor ni replicar
todo el dataset en memoria, y ofrece transacciones ACID duraderas sobre un
archivo único.

**¿Qué es un fallo de página mayor?**
Es el acceso a una página que no está en memoria: hay que leerla del dispositivo
de almacenamiento. Es caro porque paga la latencia de E/S. Un fallo menor, en
cambio, encuentra la página ya en memoria y solo la instala en la tabla de
páginas.

**¿LMDB tiene su propio caché de páginas?**
No. Delega el caché al sistema operativo vía `mmap`. Esa es su decisión de
diseño más característica.

**¿Qué es MVCC y para qué sirve aquí?**
Es el control de concurrencia multiversión: cada lector ve una instantánea
consistente sin bloquear al escritor. Gracias a la copia-en-escritura, las
consultas del proyecto (`query`) pueden correr sin interferir con escrituras.

**¿Por qué LMDB solo permite un escritor?**
Porque simplifica la consistencia y la durabilidad sin sacrificar la
concurrencia de lectura. El proyecto lo demuestra empíricamente con
`write-concurrent`: con 16 taquillas el throughput cae a ×0,70 respecto de una.

**¿Por qué las claves van en big-endian y los valores en little-endian?**
Porque LMDB compara las **claves** con `memcmp` y necesita que el orden
lexicográfico de bytes coincida con el orden numérico (para que los rangos de
fecha funcionen). Los **valores** no se comparan, así que su endianness es una
decisión libre; el proyecto eligió little-endian de forma explícita para que el
resultado sea idéntico en cualquier máquina.
